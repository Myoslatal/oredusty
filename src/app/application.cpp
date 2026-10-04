#include <ore/app/application.h>

#include <ore/core/assert.h>
#include <ore/core/file.h>

#include <algorithm>
#include <cstdlib>
#include <format>

namespace ore {
namespace {

constexpr CliOption kBuiltinOptions[] = {
    {"help", "", "Show this message and exit"},
    {"headless", "", "Render offscreen without creating a window"},
    {"platform", "<wayland|x11|null>", "Which window system to use (default: whatever GLFW finds)"},
    {"input-server", "<port>", "Listen on 127.0.0.1 for debug input commands (key, mouse, scroll, shot)"},
    {"frames", "<count>", "Render exactly N frames, then exit"},
    {"size", "<WxH>", "Window / offscreen resolution (default 1280x720)"},
    {"screenshot", "<path>", "Write the rendered frame to a PNG file"},
    {"vsync", "<0|1>", "Enable or disable vertical sync"},
    {"validation", "<0|1>", "Enable Vulkan validation layers"},
    {"device", "<index>", "Use the Vulkan device with this index"},
    {"list-devices", "", "List the available Vulkan devices and exit"},
    {"log-level", "<level>", "trace | debug | info | warn | error"},
    {"hot-reload", "<0|1>", "Watch shader files and reload them at runtime"},
    {"dump-gpu-memory", "", "Print GPU allocator statistics on exit"},
    {"shader-dir", "<path>", "Directory searched for compiled .spv shaders"},
};

[[nodiscard]] bool parse_size(std::string_view text, u32& width, u32& height) {
    const usize separator = text.find_first_of("xX*");
    if (separator == std::string_view::npos) return false;
    try {
        const int parsed_width = std::stoi(std::string(text.substr(0, separator)));
        const int parsed_height = std::stoi(std::string(text.substr(separator + 1)));
        if (parsed_width <= 0 || parsed_height <= 0) return false;
        width = static_cast<u32>(parsed_width);
        height = static_cast<u32>(parsed_height);
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] LogLevel parse_log_level(std::string_view text) {
    if (text == "trace") return LogLevel::Trace;
    if (text == "debug") return LogLevel::Debug;
    if (text == "info") return LogLevel::Info;
    if (text == "warn" || text == "warning") return LogLevel::Warn;
    if (text == "error") return LogLevel::Error;
    if (text == "off" || text == "none") return LogLevel::Off;
    return LogLevel::Info;
}

[[nodiscard]] std::string device_type_label(VkPhysicalDeviceType type) {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "software";
        default: return "other";
    }
}

} // namespace

void Application::print_help() const {
    std::string text = cli_.help("[options]", kBuiltinOptions);
    const ConstSpan<CliOption> extra = cli_options();
    if (!extra.empty()) {
        text.append("\napplication options:\n");
        for (const CliOption& option : extra) {
            std::string left = std::format("  --{}", option.name);
            if (!option.value_hint.empty()) std::format_to(std::back_inserter(left), " {}", option.value_hint);
            std::format_to(std::back_inserter(text), "{:<28} {}\n", left, option.description);
        }
    }
    ORE_INFO("\n{}", text);
}

void Application::apply_command_line() {
    if (const auto headless = cli_.bool_value("headless"); headless.value_or(false)) config_.headless = true;
    if (const auto platform = cli_.value("platform"); platform.has_value()) {
        if (*platform == "wayland") config_.window.platform = WindowPlatform::Wayland;
        else if (*platform == "x11") config_.window.platform = WindowPlatform::X11;
        else if (*platform == "null") config_.window.platform = WindowPlatform::Null;
        else ORE_WARN("ignoring unknown --platform '{}' (wayland, x11, null)", *platform);
    }
    if (const auto frames = cli_.uint_value("frames"); frames.has_value()) config_.frames = *frames;
    if (const auto vsync = cli_.bool_value("vsync"); vsync.has_value()) config_.vsync = *vsync;
    if (const auto validation = cli_.bool_value("validation"); validation.has_value()) {
        config_.validation = *validation;
    }
    if (const auto hot_reload = cli_.bool_value("hot-reload"); hot_reload.has_value()) {
        config_.enable_hot_reload = *hot_reload;
    }
    if (const auto device = cli_.uint_value("device"); device.has_value()) config_.device_index = *device;
    if (const auto port = cli_.uint_value("input-server"); port.has_value()) {
        if (*port > 0 && *port <= 65535) config_.input_server_port = static_cast<u16>(*port);
        else ORE_WARN("ignoring --input-server {}: a port is 1..65535", *port);
    }
    if (cli_.has("dump-gpu-memory")) config_.dump_gpu_memory = true;
    if (const auto path = cli_.value("screenshot"); path.has_value()) config_.screenshot_path = *path;
    if (const auto level = cli_.value("log-level"); level.has_value()) set_log_level(parse_log_level(*level));
    if (const auto size = cli_.value("size"); size.has_value()) {
        u32 width = 0;
        u32 height = 0;
        if (parse_size(*size, width, height)) {
            config_.window.width = width;
            config_.window.height = height;
        } else {
            ORE_WARN("ignoring malformed --size '{}' (expected WxH)", *size);
        }
    }
    config_.window.vsync = config_.vsync;
}

int Application::run(int argc, char** argv) {
    cli_ = CommandLine::parse(argc, argv);

    config_ = AppConfig{};
    on_configure(config_);
    apply_command_line();

    if (cli_.has("help") || cli_.has("h")) {
        print_help();
        return 0;
    }

    if (cli_.has("list-devices")) {
        rhi::InstanceDesc instance_desc;
        instance_desc.application_name = config_.name + " (device query)";
        instance_desc.enable_validation = false;
        auto instance = rhi::Instance::create(instance_desc);
        if (instance == nullptr) {
            ORE_ERROR("no Vulkan instance could be created; is a driver installed?");
            return 2;
        }
        const auto devices = rhi::enumerate_physical_devices(*instance);
        if (devices.empty()) {
            ORE_INFO("no Vulkan device found");
            return 1;
        }
        for (const rhi::PhysicalDeviceInfo& info : devices) {
            ORE_INFO("device {}: {} [{}] api {} | dynamic rendering: {} | queue families: {}", info.index, info.name,
                     device_type_label(info.type), rhi::api_version_string(info.api_version),
                     info.features.meets_requirements() ? "yes" : "no", info.queue_families.size());
        }
        return 0;
    }

    ORE_INFO("Ore {} - {}", ORE_VERSION_STRING, config_.name);

    // ------------------------------------------------------------- window ---
    std::vector<std::string> instance_extensions;
    if (!config_.headless) {
        window_ = Window::create(config_.window);
        if (window_ == nullptr) {
            ORE_ERROR("could not open a window; re-run with --headless for offscreen rendering");
            return 2;
        }
        instance_extensions = Window::required_instance_extensions();
    }

    // ------------------------------------------------------------ context ---
    rhi::ContextDesc context_desc;
    context_desc.application_name = config_.name;
    context_desc.enable_validation = config_.validation;
    context_desc.headless = config_.headless;
    context_desc.instance_extensions = instance_extensions;
    context_desc.device_index = config_.device_index;
    if (!config_.headless) {
        // The surface needs the instance the context is about to create, and the device choice
        // needs the surface, so the context calls back into the window here.
        context_desc.surface_factory = [this](VkInstance instance) -> VkSurfaceKHR {
            return window_ != nullptr ? window_->create_surface(instance) : VK_NULL_HANDLE;
        };
    }
    context_ = rhi::GraphicsContext::create(context_desc);
    if (context_ == nullptr && !config_.headless && config_.allow_headless_fallback) {
        ORE_WARN("no windowed Vulkan context (drivers without presentation support, or a broken "
                 "window system integration); continuing with offscreen rendering - the window only "
                 "provides input now");
        rhi::ContextDesc offscreen_desc = context_desc;
        offscreen_desc.headless = true;
        offscreen_desc.surface = VK_NULL_HANDLE;
        offscreen_desc.surface_factory = nullptr;
        context_ = rhi::GraphicsContext::create(offscreen_desc);
        config_.headless = true;
    }
    if (context_ == nullptr) {
        ORE_ERROR("failed to create the Vulkan context");
        return 3;
    }
    surface_ = context_->surface();

    // ----------------------------------------------------------- renderer ---
    Renderer::Desc renderer_desc;
    renderer_desc.context = context_.get();
    renderer_desc.surface = surface_;
    renderer_desc.width = window_ != nullptr ? window_->width() : config_.window.width;
    renderer_desc.height = window_ != nullptr ? window_->height() : config_.window.height;
    renderer_desc.vsync = config_.vsync;
    renderer_desc.color_format = config_.color_format;
    renderer_desc.depth_format = config_.depth_format;
    renderer_desc.frames_in_flight = config_.frames_in_flight;
    renderer_desc.upload_segment_size = config_.upload_segment_size;
    renderer_desc.offscreen_sampled = config_.offscreen_sampled;
    renderer_ = Renderer::create(renderer_desc);
    if (renderer_ == nullptr) {
        ORE_ERROR("failed to create the renderer");
        return 4;
    }

    if (config_.headless && config_.frames == 0 && window_ == nullptr) {
        config_.frames = 1;
        ORE_INFO("headless run without --frames: rendering a single frame");
    }

    if (window_ != nullptr) {
        window_->set_resize_callback([this](u32 width, u32 height) {
            if (renderer_ != nullptr) renderer_->resize(width, height, config_.vsync);
            on_resize(width, height);
        });
    }

    // The debug input server is started before the application is: a scripted run may well send its
    // first command while on_start() is still loading content, and those commands wait in the queue.
    if (config_.input_server_port != 0) {
        std::string error;
        input_server_ = debug::InputServer::create(config_.input_server_port, &error);
        if (input_server_ == nullptr) ORE_WARN("debug input server: {}", error);
        else {
            ORE_INFO("debug input server: listening on 127.0.0.1:{} (one command per line: key, mouse, "
                     "scroll, text, shot, quit)",
                     input_server_->port());
        }
    }

    on_start();

    const bool capture_requested = !config_.screenshot_path.empty();
    bool capture_done = false;
    timer_.reset();
    f64 previous_time = Clock::now_seconds();

    while (!quit_requested_) {
        // ---------------------------------------------------------- input ---
        if (window_ == nullptr) {
            // A headless run has an input state too - the debug input server feeds it - and it needs
            // the same per-frame edges a window's does, or a press would read as a press forever.
            // Window::poll_events() does this for the windowed path.
            input().begin_frame();
        }
        if (window_ != nullptr) {
            window_->poll_events();
            if (window_->consume_resized()) {
                if (renderer_ != nullptr) renderer_->resize(window_->width(), window_->height(), config_.vsync);
                on_resize(window_->width(), window_->height());
            }
            if (window_->should_close()) break;
            if (window_->minimized()) {
                window_->wait_events(0.05f);
                continue;
            }
            input_map_.update(window_->input());
            if (config_.quit_on_escape && window_->input().key_pressed(Key::Escape)) break;
        }
        // After the window was polled - so begin_frame() has already cleared last frame's edges, and a
        // press applied here is the press on_update() is about to see - and outside the window check,
        // because a headless run has an input state too.
        pump_debug_input();

        // ------------------------------------------------------- hot reload -
        if (config_.enable_hot_reload && hot_reloader_.watched_count() > 0) {
            const std::vector<std::string> changed = hot_reloader_.poll();
            if (!changed.empty()) on_shader_reload(changed);
        }

        // ----------------------------------------------------------- frame --
        const f32 delta_seconds = timer_.tick();
        const f64 frame_start = Clock::now_seconds();
        on_update(delta_seconds);

        RenderFrame& frame = renderer_->begin_frame(delta_seconds);
        on_render(frame);

        // Screenshots are captured from the last rendered frame (or the first, in interactive runs).
        const bool last_frame = config_.frames > 0 && frame_count_ + 1 >= config_.frames;
        if (capture_requested && !capture_done && (last_frame || config_.frames == 0)) {
            renderer_->request_screenshot(config_.screenshot_path);
            capture_done = true;
        }

        renderer_->end_frame();
        on_frame_end();

        ++frame_count_;
        const f64 now = Clock::now_seconds();
        previous_time = now;
        (void)previous_time;
        if (config_.stats_interval > 0.0f) {
            stats_accumulator_ += static_cast<f64>(delta_seconds);
            if (stats_accumulator_ >= static_cast<f64>(config_.stats_interval)) {
                stats_accumulator_ = 0.0;
                ORE_INFO("frame {}: {:.2f} ms ({:.0f} fps), {} draw call(s), {}", frame_count_,
                         static_cast<f64>(timer_.average_ms()), static_cast<f64>(timer_.average_fps()),
                         renderer_->stats().last_frame.draw_calls, renderer_->ring().dump_stats());
            }
        }
        (void)frame_start;

        if (config_.frames > 0 && frame_count_ >= config_.frames) break;
    }

    // The GPU goes idle *before* the application tears down: on_shutdown() is where an application
    // destroys its own buffers, textures and pipelines, and destroying anything a frame still in
    // flight references loses the device (the frame's fence is not waited on by end_frame()).
    renderer_->wait_idle();
    on_shutdown();
    log_summary();
    return 0;
}

std::string Application::shader_path(std::string_view name) const {
    std::vector<std::string> candidates;
    if (const auto directory = cli_.value("shader-dir"); directory.has_value()) candidates.push_back(*directory);
    if (const char* from_env = std::getenv("ORE_SHADER_DIR"); from_env != nullptr && from_env[0] != '\0') {
        candidates.emplace_back(from_env);
    }
#if defined(ORE_SHADER_DIR)
    candidates.emplace_back(ORE_SHADER_DIR);
#endif
    candidates.push_back(path_join(executable_dir(), "shaders"));
    candidates.push_back(executable_dir());
    candidates.push_back(path_join(current_dir(), "shaders"));
    candidates.push_back(current_dir());
    candidates.push_back(builtin_shader_dir());

    for (const std::string& directory : candidates) {
        const std::string candidate = path_join(directory, name);
        if (path_exists(candidate)) return candidate;
    }
    return std::string(name);
}

InputState& Application::input() const {
    static InputState headless_input;
    return window_ != nullptr ? window_->input() : headless_input;
}

void Application::pump_debug_input() {
    if (input_server_ == nullptr) return;
    for (const debug::InputEvent& action : input_server_->pump(input())) {
        switch (action.kind) {
            case debug::InputEvent::Kind::Quit: quit(); break;
            case debug::InputEvent::Kind::Screenshot:
                // Requested before the frame is rendered, so the file holds the frame this command
                // asked for rather than the one before it.
                renderer_->request_screenshot(action.text);
                break;
            default: break;   // the rest was applied to the input state by pump()
        }
    }
}

void Application::watch_shader(std::string path) { hot_reloader_.watch(path); }

rhi::ShaderCompileResult Application::recompile_shader(std::string_view path) const {
    return rhi::compile_glsl_file(path, std::nullopt, {});
}

void Application::log_summary() const {
    if (renderer_ != nullptr) {
        ORE_INFO("{}", renderer_->dump_stats());
        ORE_INFO("{}", renderer_->ring().dump_stats());
    }
    if (context_ != nullptr) {
        ORE_INFO("frames: {}, average {:.2f} ms ({:.1f} fps), min {:.2f} ms, max {:.2f} ms", frame_count_,
                 static_cast<f64>(timer_.average_ms()), static_cast<f64>(timer_.average_fps()),
                 static_cast<f64>(timer_.min_ms()), static_cast<f64>(timer_.max_ms()));
        if (config_.dump_gpu_memory) {
            ORE_INFO("{}", context_->allocator().dump_stats());
        }
    }
}

} // namespace ore
