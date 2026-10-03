// Ore framework - application skeleton: window + context + renderer + main loop.
//
//   class MyGame final : public ore::Application {
//       void on_start() override { ... }
//       void on_update(f32 dt) override { ... }
//       void on_render(ore::RenderFrame& frame) override { ... }
//   };
//   ORE_APP_MAIN(MyGame)
#pragma once

#include <ore/core/cli.h>
#include <ore/core/log.h>
#include <ore/core/time.h>
#include <ore/core/types.h>
#include <ore/platform/input.h>
#include <ore/platform/window.h>
#include <ore/renderer/renderer.h>
#include <ore/rhi/context.h>
#include <ore/rhi/shader_compiler.h>

#include <string>
#include <vector>

namespace ore {

struct AppConfig {
    std::string name = "Ore Application";
    WindowDesc window{};
    /// Render offscreen without a window (screenshots, tests, servers).
    bool headless = false;
    /// Render exactly N frames then exit; 0 runs until the window is closed.
    u32 frames = 0;
    bool vsync = true;
    bool validation = ORE_ENABLE_VALIDATION != 0;
    /// When set, the first frame (or the last one with --frames) is written to this PNG path.
    std::string screenshot_path;
    u32 frames_in_flight = 2;
    /// Colour format for the swapchain and the offscreen target. The default (undefined) resolves to
    /// a non-sRGB format, so colours authored as sRGB bytes appear exactly as authored and a window
    /// looks like an offscreen screenshot; name an _SRGB format when correct 3D lighting matters more.
    VkFormat color_format = VK_FORMAT_UNDEFINED;
    VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
    bool offscreen_sampled = false;
    bool enable_hot_reload = ORE_ENABLE_HOT_RELOAD != 0;
    bool quit_on_escape = true;
    /// When the windowed Vulkan context cannot be created (no presentation support, broken WSI),
    /// keep the window for input but render offscreen instead of exiting.
    bool allow_headless_fallback = true;
    u32 device_index = 0;
    bool dump_gpu_memory = false;
    /// Seconds between automatic log lines with frame statistics (0 disables them).
    f32 stats_interval = 0.0f;
};

class Application {
public:
    virtual ~Application() = default;

    /// Parses the command line, boots the framework, runs the loop and shuts everything down.
    int run(int argc, char** argv);

    [[nodiscard]] const AppConfig& config() const { return config_; }
    [[nodiscard]] const CommandLine& cli() const { return cli_; }
    [[nodiscard]] Renderer& renderer() const { return *renderer_; }
    [[nodiscard]] rhi::GraphicsContext& context() const { return *context_; }
    [[nodiscard]] Window* window() const { return window_.get(); }
    [[nodiscard]] InputState& input() const;
    [[nodiscard]] InputMap& input_map() { return input_map_; }
    [[nodiscard]] const FrameTimer& frame_timer() const { return timer_; }
    [[nodiscard]] u64 frame_count() const { return frame_count_; }
    [[nodiscard]] bool running() const { return !quit_requested_; }
    void quit() { quit_requested_ = true; }

    /// Resolves a compiled shader (e.g. "cube.vert.spv") to a path on disk. Search order:
    /// --shader-dir, $ORE_SHADER_DIR, the target's build shader directory, <exe>/shaders,
    /// <exe>, ./shaders, ., <source>/shaders. Returns the name unchanged when nothing matches,
    /// which keeps shader-load errors readable.
    [[nodiscard]] std::string shader_path(std::string_view name) const;

    /// Registers a GLSL file for hot reload; on_shader_reload() receives the changed paths.
    void watch_shader(std::string path);
    /// Compiles a GLSL file at runtime (requires shaderc) - used with hot reload.
    [[nodiscard]] rhi::ShaderCompileResult recompile_shader(std::string_view path) const;

protected:
    /// Last chance to tweak the configuration before anything is created (CLI not applied yet).
    virtual void on_configure(AppConfig& config) { (void)config; }
    /// Called once the renderer exists.
    virtual void on_start() {}
    /// Simulation step; \p delta_seconds is the wall clock delta of this frame.
    virtual void on_update(f32 delta_seconds) { (void)delta_seconds; }
    /// Draw here: use renderer().begin_pass()/end_pass() around your command recording.
    virtual void on_render(RenderFrame& frame) = 0;
    /// Framebuffer size changed.
    virtual void on_resize(u32 width, u32 height) {
        (void)width;
        (void)height;
    }
    /// Called after the frame was submitted and presented.
    virtual void on_frame_end() {}
    /// Shader files changed (only when hot reload is enabled and a watched file was touched).
    virtual void on_shader_reload(ConstSpan<std::string> changed_paths) { (void)changed_paths; }
    /// Tear down GPU resources here; called before the device is destroyed.
    virtual void on_shutdown() {}
    /// Extra command line options added to --help.
    [[nodiscard]] virtual ConstSpan<CliOption> cli_options() const { return {}; }

private:
    void apply_command_line();
    void print_help() const;
    void log_summary() const;

    AppConfig config_{};
    CommandLine cli_{};
    Scope<Window> window_;
    Scope<rhi::GraphicsContext> context_;
    Scope<Renderer> renderer_;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    InputMap input_map_{};
    FrameTimer timer_{};
    rhi::ShaderHotReloader hot_reloader_{};
    u64 frame_count_ = 0;
    bool quit_requested_ = false;
    f64 stats_accumulator_ = 0.0;
};

} // namespace ore

#define ORE_APP_MAIN(AppType)                                                                        \
    int main(int argc, char** argv) {                                                                \
        AppType application;                                                                          \
        return application.run(argc, argv);                                                           \
    }
