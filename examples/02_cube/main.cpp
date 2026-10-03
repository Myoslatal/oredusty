// Ore example 02 - a lit, spinning cube on a grid with a fly camera.
//
// Shows: depth buffering, per-frame scene uniforms and per-object dynamic uniforms from the
// upload ring, descriptor sets, push constants, procedural meshes, two pipelines and mouse/keyboard
// input. Controls: WASD/arrows move, right mouse button looks, scroll wheel changes speed, Esc quits.
#include <ore/ore.h>

using namespace ore;

namespace {

/// Matches SceneUniforms in shaders/mesh.vert (std140).
struct SceneUniforms {
    Mat4 view_projection{1.0f};
    Vec4 camera_position{0.0f};
    Vec4 light_direction{0.0f, -1.0f, 0.0f, 0.0f};
};

/// Matches ObjectUniforms in shaders/mesh.vert (std140).
struct ObjectUniforms {
    Mat4 model{1.0f};
    Mat4 normal_matrix{1.0f};
    Vec4 base_color{1.0f};
};

/// Matches PushConstants in the mesh/grid shaders.
struct PushConstants {
    Vec4 tint{1.0f};
    f32 uv_scale = 1.0f;
    i32 use_texture = 0;
};

} // namespace

class CubeApp final : public Application {
protected:
    void on_configure(AppConfig& config) override {
        config.name = "Ore example 02 - spinning cube";
        config.window.title = config.name;
        config.window.width = 1280;
        config.window.height = 720;
        config.stats_interval = 2.0f;
    }

    void on_start() override {
        rhi::GraphicsContext& ctx = context();

        cube_ = Mesh::cube(ctx, 1.0f);
        grid_ = Mesh::grid(ctx, 24.0f, 24);
        white_texture_ = ctx.create_solid_texture(make_rgba(255, 255, 255, 255), "white");

        vertex_shader_ = rhi::ShaderModule::load(ctx.device(), shader_path("mesh.vert.spv"));
        fragment_shader_ = rhi::ShaderModule::load(ctx.device(), shader_path("mesh.frag.spv"));
        grid_vertex_shader_ = rhi::ShaderModule::load(ctx.device(), shader_path("grid.vert.spv"));
        grid_fragment_shader_ = rhi::ShaderModule::load(ctx.device(), shader_path("grid.frag.spv"));
        if (vertex_shader_ == nullptr || fragment_shader_ == nullptr || grid_vertex_shader_ == nullptr ||
            grid_fragment_shader_ == nullptr) {
            ORE_FATAL("shaders missing - run the build so that glslc produces the .spv files");
        }

        // Set 0: scene uniforms (binding 0, updated once per frame) + object uniforms
        // (binding 1, a dynamic uniform buffer sliced from the per-frame upload ring).
        const rhi::DescriptorBinding scene_bindings[] = {
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0},
            {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, 0},
        };
        scene_layout_ = rhi::DescriptorSetLayout::create(ctx.device(), scene_bindings, "scene.layout");

        // Set 1: the albedo texture.
        const rhi::DescriptorBinding texture_bindings[] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 0},
        };
        texture_layout_ = rhi::DescriptorSetLayout::create(ctx.device(), texture_bindings, "texture.layout");

        const rhi::DescriptorSetLayout* set_layouts[] = {scene_layout_.get(), texture_layout_.get()};
        const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                        sizeof(PushConstants)};
        pipeline_layout_ = rhi::PipelineLayout::create(ctx.device(), set_layouts,
                                                      ConstSpan<VkPushConstantRange>(&range, 1), "mesh.layout");

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

        // The texture set is stable for the lifetime of the app, so it lives in its own pool
        // instead of the per-frame pools the renderer manages.
        const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4};
        persistent_pool_ = rhi::DescriptorPool::create(ctx.device(), ConstSpan<VkDescriptorPoolSize>(&pool_size, 1), 4);
        sampler_ = rhi::Sampler::create(ctx.device(), rhi::SamplerDesc::linear());
        texture_set_ = rhi::DescriptorSet(ctx.device(), persistent_pool_->allocate(*texture_layout_, "white.set"),
                                          *texture_layout_);
        texture_set_.write_texture(0, *white_texture_, sampler_->handle());

        camera_.position = Vec3(3.4f, 2.4f, 4.6f);
        camera_.orientation = glm::quatLookAt(glm::normalize(-camera_.position), Vec3(0.0f, 1.0f, 0.0f));
        fly_camera_ = make_scope<FlyCameraController>(camera_, 4.0f);
    }

    void on_update(f32 delta_seconds) override {
        CameraInputState state;
        if (Window* window = this->window(); window != nullptr) {
            const InputState& input = window->input();
            state.move_forward = input.key_down(Key::W) || input.key_down(Key::Up);
            state.move_backward = input.key_down(Key::S) || input.key_down(Key::Down);
            state.move_left = input.key_down(Key::A) || input.key_down(Key::Left);
            state.move_right = input.key_down(Key::D) || input.key_down(Key::Right);
            state.move_up = input.key_down(Key::Space);
            state.move_down = input.key_down(Key::LeftControl);
            state.look_active = input.mouse_down(MouseButton::Right);
            state.look_delta_x = input.mouse_delta_x();
            state.look_delta_y = input.mouse_delta_y();
            state.scroll_delta = input.scroll_y();
            state.speed_multiplier = input.shift_down() ? 3.0f : 1.0f;

            if (input.key_pressed(Key::Tab)) {
                cursor_disabled_ = !cursor_disabled_;
                window->set_cursor_mode(cursor_disabled_ ? CursorMode::Disabled : CursorMode::Normal);
            }
        }
        fly_camera_->update(state, delta_seconds);
        time_ += delta_seconds;
    }

    void on_render(RenderFrame& frame) override {
        camera_.set_aspect_from_size(frame.width, frame.height);

        // --- per-frame scene uniforms -------------------------------------------------
        const u64 alignment = frame.ring->alignment();
        const auto scene_slice = frame.ring->allocate(sizeof(SceneUniforms), alignment);
        const auto grid_slice = frame.ring->allocate(sizeof(ObjectUniforms), alignment);
        if (!scene_slice.valid() || !grid_slice.valid()) return;

        auto* scene = static_cast<SceneUniforms*>(scene_slice.mapped);
        scene->view_projection = camera_.view_projection();
        scene->camera_position = Vec4(camera_.position, 1.0f);
        scene->light_direction = glm::normalize(Vec4(-0.45f, -0.8f, -0.35f, 0.0f));

        auto* grid_object = static_cast<ObjectUniforms*>(grid_slice.mapped);
        grid_object->model = Mat4(1.0f);
        grid_object->normal_matrix = Mat4(1.0f);
        grid_object->base_color = Vec4(1.0f);

        rhi::DescriptorSet scene_set = renderer().allocate_descriptor_set(*scene_layout_, "frame.scene");
        scene_set.write_buffer(0, frame.ring->buffer(), scene_slice.offset, sizeof(SceneUniforms));
        scene_set.write_buffer(1, frame.ring->buffer(), 0, sizeof(ObjectUniforms));

        VkClearValue clear{};
        clear.color = {{0.045f, 0.05f, 0.065f, 1.0f}};

        renderer().begin_pass(clear, 1.0f);

        PushConstants constants;

        // --- grid ---------------------------------------------------------------------
        const u32 grid_offset = static_cast<u32>(grid_slice.offset);
        frame.cmd->bind_pipeline(*grid_pipeline_);
        frame.cmd->set_viewport(frame.width, frame.height);
        frame.cmd->set_scissor_full(frame.width, frame.height);
        frame.cmd->bind_descriptor_set(0, scene_set, ConstSpan<u32>(&grid_offset, 1));
        frame.cmd->bind_descriptor_set(1, texture_set_);
        frame.cmd->push_constants(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, constants);
        grid_->draw_non_indexed(*frame.cmd);
        ++frame.counters.draw_calls;

        // --- cube ---------------------------------------------------------------------
        const auto object_slice = frame.ring->allocate(sizeof(ObjectUniforms), alignment);
        if (!object_slice.valid()) return;
        auto* object = static_cast<ObjectUniforms*>(object_slice.mapped);
        const f32 angle = 0.7f + time_ * 0.8f;
        object->model = glm::rotate(Mat4(1.0f), angle, glm::normalize(Vec3(0.35f, 1.0f, 0.15f)));
        object->normal_matrix = glm::transpose(glm::inverse(object->model));
        object->base_color = Vec4(0.85f, 0.55f, 0.25f, 1.0f);

        const u32 object_offset = static_cast<u32>(object_slice.offset);
        frame.cmd->bind_pipeline(*pipeline_);
        frame.cmd->bind_descriptor_set(0, scene_set, ConstSpan<u32>(&object_offset, 1));
        frame.cmd->bind_descriptor_set(1, texture_set_);
        constants.uv_scale = 1.0f;
        constants.use_texture = 0;
        frame.cmd->push_constants(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, constants);
        cube_->draw(*frame.cmd);
        ++frame.counters.draw_calls;
        frame.counters.vertices += cube_->vertex_count();
        frame.counters.triangles += cube_->index_count() / 3;

        renderer().end_pass();
    }

private:
    Scope<Mesh> cube_;
    Scope<Mesh> grid_;
    Scope<rhi::Texture> white_texture_;
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
    Scope<FlyCameraController> fly_camera_;
    f32 time_ = 0.0f;
    bool cursor_disabled_ = false;
};

ORE_APP_MAIN(CubeApp)
