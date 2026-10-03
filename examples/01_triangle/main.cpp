// Ore example 01 - the smallest possible frame.
//
// Shows: application lifecycle, one graphics pipeline with push constants, dynamic rendering
// and the headless screenshot path (try: --headless --screenshot triangle.png).
#include <ore/ore.h>

using namespace ore;

namespace {

struct PushConstants {
    Vec4 color{1.0f};
    f32 time = 0.0f;
};

} // namespace

class TriangleApp final : public Application {
protected:
    void on_configure(AppConfig& config) override {
        config.name = "Ore example 01 - triangle";
        config.window.title = config.name;
        config.window.width = 1280;
        config.window.height = 720;
        config.depth_format = VK_FORMAT_UNDEFINED; // a triangle needs no depth buffer
    }

    void on_start() override {
        rhi::GraphicsContext& ctx = context();

        vertex_shader_ = rhi::ShaderModule::load(ctx.device(), shader_path("triangle.vert.spv"));
        fragment_shader_ = rhi::ShaderModule::load(ctx.device(), shader_path("triangle.frag.spv"));
        if (vertex_shader_ == nullptr || fragment_shader_ == nullptr) {
            ORE_FATAL("shaders missing - run the build so that glslc produces the .spv files");
        }

        const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                        sizeof(PushConstants)};
        pipeline_layout_ = rhi::PipelineLayout::create(ctx.device(), {},
                                                       ConstSpan<VkPushConstantRange>(&range, 1), "triangle.layout");

        rhi::GraphicsPipelineDesc desc;
        desc.vertex_shader = vertex_shader_.get();
        desc.fragment_shader = fragment_shader_.get();
        desc.layout = pipeline_layout_.get();
        desc.color_formats = {renderer().color_format()};
        desc.depth_format = VK_FORMAT_UNDEFINED;
        desc.depth_test = false;
        desc.depth_write = false;
        desc.cull_mode = VK_CULL_MODE_NONE;
        desc.debug_name = "triangle.pipeline";
        pipeline_ = rhi::GraphicsPipeline::create(ctx.device(), desc);
    }

    void on_update(f32 delta_seconds) override { time_ += delta_seconds; }

    void on_render(RenderFrame& frame) override {
        VkClearValue clear{};
        clear.color = {{0.06f, 0.07f, 0.09f, 1.0f}};

        renderer().begin_pass(clear);
        frame.cmd->bind_pipeline(*pipeline_);
        frame.cmd->set_viewport(frame.width, frame.height);
        frame.cmd->set_scissor_full(frame.width, frame.height);

        PushConstants constants;
        constants.time = time_;
        constants.color = Vec4(1.0f, 1.0f, 1.0f, 1.0f);
        frame.cmd->push_constants(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, constants);
        frame.cmd->draw(3);

        frame.counters.draw_calls = 1;
        frame.counters.vertices = 3;
        frame.counters.triangles = 1;
        renderer().end_pass();
    }

private:
    Scope<rhi::ShaderModule> vertex_shader_;
    Scope<rhi::ShaderModule> fragment_shader_;
    Scope<rhi::PipelineLayout> pipeline_layout_;
    Scope<rhi::GraphicsPipeline> pipeline_;
    f32 time_ = 0.0f;
};

ORE_APP_MAIN(TriangleApp)
