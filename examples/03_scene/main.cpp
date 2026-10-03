// Ore example 03 - a small ECS scene: sun, planets and moons on a textured grid.
//
// Shows: ore::World (entities, components, hierarchy, each<>), transform propagation, several
// meshes sharing one pipeline, textured draws, the orbit camera controller and loading a PNG
// texture from disk with a procedural fallback.
//
// Controls: left mouse drag orbits, middle mouse drag pans, scroll wheel zooms, Esc quits.
// Headless: --headless --frames 3 --screenshot scene.png
#include <ore/ore.h>

using namespace ore;

namespace {

struct SceneUniforms {
    Mat4 view_projection{1.0f};
    Vec4 camera_position{0.0f};
    Vec4 light_direction{0.0f, -1.0f, 0.0f, 0.0f};
};

struct ObjectUniforms {
    Mat4 model{1.0f};
    Mat4 normal_matrix{1.0f};
    Vec4 base_color{1.0f};
};

struct PushConstants {
    Vec4 tint{1.0f};
    f32 uv_scale = 1.0f;
    i32 use_texture = 0;
};

/// Example specific component: rotates its entity around \p axis.
struct Spin {
    f32 radians_per_second = 0.5f;
    Vec3 axis{0.0f, 1.0f, 0.0f};
};

constexpr Vec4 kPalette[] = {
    {0.92f, 0.55f, 0.24f, 1.0f}, {0.35f, 0.62f, 0.88f, 1.0f}, {0.55f, 0.82f, 0.45f, 1.0f},
    {0.88f, 0.36f, 0.42f, 1.0f}, {0.72f, 0.56f, 0.90f, 1.0f}, {0.90f, 0.82f, 0.40f, 1.0f},
};

void spin_system(World& world, f32 delta_seconds) {
    world.each<Transform, Spin>([delta_seconds](Entity, Transform& transform, Spin& spin) {
        transform.rotation = glm::normalize(glm::angleAxis(spin.radians_per_second * delta_seconds, spin.axis) *
                                            transform.rotation);
    });
}

} // namespace

class SceneApp final : public Application {
protected:
    void on_configure(AppConfig& config) override {
        config.name = "Ore example 03 - ECS scene";
        config.window.title = config.name;
        config.window.width = 1280;
        config.window.height = 720;
        config.stats_interval = 2.0f;
    }

    void on_start() override {
        rhi::GraphicsContext& ctx = context();

        cube_ = Mesh::cube(ctx, 1.0f);
        sphere_ = Mesh::sphere(ctx, 0.5f, 48, 24);
        grid_ = Mesh::grid(ctx, 40.0f, 40);
        meshes_ = {cube_.get(), sphere_.get()};

        // A PNG on disk wins, otherwise the checkerboard is generated procedurally.
        Image albedo;
        const std::string albedo_path = find_data_file("assets/textures/checker.png");
        if (const auto loaded = Image::load_png(albedo_path); loaded.has_value()) {
            albedo = *loaded;
            ORE_INFO("loaded {} ({}x{})", albedo_path, albedo.width, albedo.height);
        } else {
            albedo = make_checkerboard(256, 8, make_rgba(235, 235, 240), make_rgba(120, 130, 150));
            ORE_INFO("assets/textures/checker.png not found, using a procedural checkerboard");
        }
        albedo_texture_ = ctx.create_texture(albedo, true, "checker");

        vertex_shader_ = rhi::ShaderModule::load(ctx.device(), shader_path("mesh.vert.spv"));
        fragment_shader_ = rhi::ShaderModule::load(ctx.device(), shader_path("mesh.frag.spv"));
        grid_vertex_shader_ = rhi::ShaderModule::load(ctx.device(), shader_path("grid.vert.spv"));
        grid_fragment_shader_ = rhi::ShaderModule::load(ctx.device(), shader_path("grid.frag.spv"));
        if (vertex_shader_ == nullptr || fragment_shader_ == nullptr || grid_vertex_shader_ == nullptr ||
            grid_fragment_shader_ == nullptr) {
            ORE_FATAL("shaders missing - run the build so that glslc produces the .spv files");
        }

        const rhi::DescriptorBinding scene_bindings[] = {
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0},
            {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, 0},
        };
        scene_layout_ = rhi::DescriptorSetLayout::create(ctx.device(), scene_bindings, "scene.layout");

        const rhi::DescriptorBinding texture_bindings[] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 0},
        };
        texture_layout_ = rhi::DescriptorSetLayout::create(ctx.device(), texture_bindings, "texture.layout");

        const rhi::DescriptorSetLayout* set_layouts[] = {scene_layout_.get(), texture_layout_.get()};
        const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                        sizeof(PushConstants)};
        pipeline_layout_ = rhi::PipelineLayout::create(ctx.device(), set_layouts,
                                                      ConstSpan<VkPushConstantRange>(&range, 1), "scene.layout");

        rhi::GraphicsPipelineDesc mesh_desc;
        mesh_desc.vertex_shader = vertex_shader_.get();
        mesh_desc.fragment_shader = fragment_shader_.get();
        mesh_desc.layout = pipeline_layout_.get();
        mesh_desc.vertex_layout = Vertex::layout();
        mesh_desc.color_formats = {renderer().color_format()};
        mesh_desc.depth_format = renderer().depth_format();
        mesh_desc.debug_name = "mesh.pipeline";
        pipeline_ = rhi::GraphicsPipeline::create(ctx.device(), mesh_desc);

        rhi::GraphicsPipelineDesc grid_desc = mesh_desc;
        grid_desc.vertex_shader = grid_vertex_shader_.get();
        grid_desc.fragment_shader = grid_fragment_shader_.get();
        grid_desc.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        grid_desc.blend = rhi::BlendMode::Alpha;
        grid_desc.depth_write = false;
        grid_desc.debug_name = "grid.pipeline";
        grid_pipeline_ = rhi::GraphicsPipeline::create(ctx.device(), grid_desc);

        const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4};
        persistent_pool_ = rhi::DescriptorPool::create(ctx.device(), ConstSpan<VkDescriptorPoolSize>(&pool_size, 1), 4);
        sampler_ = rhi::Sampler::create(ctx.device(), rhi::SamplerDesc::linear());
        texture_set_ = rhi::DescriptorSet(ctx.device(), persistent_pool_->allocate(*texture_layout_, "albedo.set"),
                                          *texture_layout_);
        texture_set_.write_texture(0, *albedo_texture_, sampler_->handle());

        build_scene();

        camera_.fov_y_degrees = 55.0f;
        camera_.near_plane = 0.1f;
        camera_.far_plane = 250.0f;
        orbit_camera_ = make_scope<OrbitCameraController>(camera_, Vec3(0.0f), 16.0f);
        orbit_camera_->set_limits(-1.4f, 1.4f, 2.0f, 90.0f);
        orbit_camera_->rotate(0.6f, 0.42f);
        orbit_camera_->update(0.0f);
    }

    void on_update(f32 delta_seconds) override {
        if (Window* window = this->window(); window != nullptr) {
            const InputState& input = window->input();
            if (input.mouse_down(MouseButton::Left)) {
                orbit_camera_->rotate(input.mouse_delta_x() * 0.006f, input.mouse_delta_y() * 0.006f);
            }
            if (input.mouse_down(MouseButton::Middle)) {
                orbit_camera_->pan(input.mouse_delta_x() * 0.01f, input.mouse_delta_y() * 0.01f);
            }
            if (f32 scroll = input.scroll_y(); scroll != 0.0f) orbit_camera_->zoom(scroll);
        } else {
            // Headless runs still animate so screenshots are not static.
            orbit_camera_->rotate(delta_seconds * 0.15f, 0.0f);
        }
        orbit_camera_->update(delta_seconds);

        spin_system(world_, delta_seconds);
        update_transforms(world_);
        elapsed_ += delta_seconds;
    }

    void on_render(RenderFrame& frame) override {
        camera_.set_aspect_from_size(frame.width, frame.height);

        const u64 alignment = frame.ring->alignment();
        const auto scene_slice = frame.ring->allocate(sizeof(SceneUniforms), alignment);
        const auto grid_slice = frame.ring->allocate(sizeof(ObjectUniforms), alignment);
        if (!scene_slice.valid() || !grid_slice.valid()) return;

        auto* scene = static_cast<SceneUniforms*>(scene_slice.mapped);
        scene->view_projection = camera_.view_projection();
        scene->camera_position = Vec4(camera_.position, 1.0f);
        scene->light_direction = glm::normalize(Vec4(-0.4f, -0.75f, -0.5f, 0.0f));

        auto* grid_object = static_cast<ObjectUniforms*>(grid_slice.mapped);
        grid_object->model = Mat4(1.0f);
        grid_object->normal_matrix = Mat4(1.0f);
        grid_object->base_color = Vec4(1.0f);

        rhi::DescriptorSet scene_set = renderer().allocate_descriptor_set(*scene_layout_, "frame.scene");
        scene_set.write_buffer(0, frame.ring->buffer(), scene_slice.offset, sizeof(SceneUniforms));
        scene_set.write_buffer(1, frame.ring->buffer(), 0, sizeof(ObjectUniforms));

        VkClearValue clear{};
        clear.color = {{0.03f, 0.035f, 0.05f, 1.0f}};

        renderer().begin_pass(clear, 1.0f);
        frame.cmd->set_viewport(frame.width, frame.height);
        frame.cmd->set_scissor_full(frame.width, frame.height);

        PushConstants constants;

        // --- ground grid (untextured, alpha blended) ----------------------------------
        const u32 grid_offset = static_cast<u32>(grid_slice.offset);
        frame.cmd->bind_pipeline(*grid_pipeline_);
        frame.cmd->bind_descriptor_set(0, scene_set, ConstSpan<u32>(&grid_offset, 1));
        frame.cmd->bind_descriptor_set(1, texture_set_);
        frame.cmd->push_constants(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, constants);
        grid_->draw_non_indexed(*frame.cmd);
        ++frame.counters.draw_calls;

        // --- every visible entity, driven by the ECS ----------------------------------
        frame.cmd->bind_pipeline(*pipeline_);
        frame.cmd->bind_descriptor_set(1, texture_set_);
        constants.use_texture = 1;
        constants.uv_scale = 2.0f;

        world_.each<Transform, MeshRenderer>([&](Entity entity, Transform& transform, MeshRenderer& renderer_component) {
            if (!renderer_component.visible || renderer_component.mesh >= meshes_.size()) return;

            const auto slice = frame.ring->allocate(sizeof(ObjectUniforms), alignment);
            if (!slice.valid()) return;
            auto* object = static_cast<ObjectUniforms*>(slice.mapped);
            object->model = transform.world;
            object->normal_matrix = glm::transpose(glm::inverse(transform.world));
            const usize material = renderer_component.material < std::size(kPalette) ? renderer_component.material : 0;
            object->base_color = kPalette[material];

            const u32 dynamic_offset = static_cast<u32>(slice.offset);
            frame.cmd->bind_descriptor_set(0, scene_set, ConstSpan<u32>(&dynamic_offset, 1));
            frame.cmd->push_constants(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, constants);

            const Mesh* mesh = meshes_[renderer_component.mesh];
            mesh->draw(*frame.cmd);
            ++frame.counters.draw_calls;
            frame.counters.vertices += mesh->vertex_count();
            frame.counters.triangles += mesh->index_count() / 3;
            (void)entity;
        });

        renderer().end_pass();
    }

private:
    /// Builds the solar-system-ish hierarchy: a spinning sun, planets orbiting it and moons
    /// orbiting the planets. Parent transforms drive the children through update_transforms().
    void build_scene() {
        const Entity sun = world_.create();
        world_.emplace<Name>(sun, Name{"sun"});
        auto& sun_transform = world_.emplace<Transform>(sun);
        sun_transform.scale = Vec3(2.2f);
        world_.emplace<MeshRenderer>(sun, MeshRenderer{1, 5u, true});
        world_.emplace<Spin>(sun, Spin{0.25f, Vec3(0.0f, 1.0f, 0.0f)});

        constexpr u32 kPlanets = 6;
        for (u32 i = 0; i < kPlanets; ++i) {
            const f32 angle = static_cast<f32>(i) / static_cast<f32>(kPlanets) * 2.0f * kPi;
            const f32 radius = 5.0f + static_cast<f32>(i) * 1.6f;

            const Entity planet = world_.create();
            world_.emplace<Name>(planet, Name{"planet " + std::to_string(i)});
            auto& transform = world_.emplace<Transform>(planet);
            transform.position = Vec3(std::cos(angle) * radius, 0.0f, std::sin(angle) * radius);
            transform.scale = Vec3(0.9f + static_cast<f32>(i) * 0.12f);
            world_.emplace<MeshRenderer>(planet,
                                         MeshRenderer{1, static_cast<u32>(i % std::size(kPalette)), true});
            world_.emplace<Spin>(planet, Spin{0.6f + static_cast<f32>(i) * 0.1f, Vec3(0.0f, 1.0f, 0.0f)});
            world_.set_parent(planet, sun);

            const Entity moon = world_.create();
            world_.emplace<Name>(moon, Name{"moon " + std::to_string(i)});
            auto& moon_transform = world_.emplace<Transform>(moon);
            moon_transform.position = Vec3(1.9f, 0.4f, 0.0f);
            moon_transform.scale = Vec3(0.35f);
            world_.emplace<MeshRenderer>(
                moon, MeshRenderer{0, static_cast<u32>((i + 3) % std::size(kPalette)), true});
            world_.emplace<Spin>(moon, Spin{1.4f, Vec3(0.2f, 1.0f, 0.1f)});
            world_.set_parent(moon, planet);
        }

        update_transforms(world_);
        ORE_INFO("scene: {} entities", world_.entity_count());
    }

    World world_{};
    std::vector<Mesh*> meshes_;
    Scope<Mesh> cube_;
    Scope<Mesh> sphere_;
    Scope<Mesh> grid_;
    Scope<rhi::Texture> albedo_texture_;
    Scope<rhi::ShaderModule> vertex_shader_;
    Scope<rhi::ShaderModule> fragment_shader_;
    Scope<rhi::ShaderModule> grid_vertex_shader_;
    Scope<rhi::ShaderModule> grid_fragment_shader_;
    Scope<rhi::DescriptorSetLayout> scene_layout_;
    Scope<rhi::DescriptorSetLayout> texture_layout_;
    Scope<rhi::PipelineLayout> pipeline_layout_;
    Scope<rhi::GraphicsPipeline> pipeline_;
    Scope<rhi::GraphicsPipeline> grid_pipeline_;
    Scope<rhi::DescriptorPool> persistent_pool_;
    Scope<rhi::Sampler> sampler_;
    rhi::DescriptorSet texture_set_;
    PerspectiveCamera camera_;
    Scope<OrbitCameraController> orbit_camera_;
    f32 elapsed_ = 0.0f;
};

ORE_APP_MAIN(SceneApp)
