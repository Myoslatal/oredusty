// Ore framework - GLFW backed window with an input state.
#pragma once

#include <ore/core/types.h>
#include <ore/platform/input.h>

#include <vulkan/vulkan.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ore {

struct WindowDesc {
    std::string title = "Ore Application";
    u32 width = 1280;
    u32 height = 720;
    bool resizable = true;
    bool visible = true;
    bool decorated = true;
    bool maximized = false;
    bool vsync = true;
    bool center = true;
};

class Window {
public:
    /// Creates the window and initialises the window system. Returns nullptr when no display is
    /// available (headless machines) - the caller should then fall back to offscreen rendering.
    [[nodiscard]] static Scope<Window> create(const WindowDesc& desc);
    ~Window();
    ORE_NON_MOVABLE(Window);

    /// Instance extensions required to create a surface for this window system.
    [[nodiscard]] static std::vector<std::string> required_instance_extensions();
    [[nodiscard]] static bool platform_initialised();

    [[nodiscard]] VkSurfaceKHR create_surface(VkInstance instance) const;

    [[nodiscard]] bool should_close() const;
    void request_close();
    void poll_events();
    /// Sleeps until an event arrives or the timeout expires (0 = return immediately).
    void wait_events(f32 timeout_seconds = 0.0f);

    [[nodiscard]] u32 width() const { return width_; }
    [[nodiscard]] u32 height() const { return height_; }
    [[nodiscard]] f32 aspect_ratio() const {
        return height_ > 0 ? static_cast<f32>(width_) / static_cast<f32>(height_) : 1.0f;
    }
    [[nodiscard]] bool minimized() const { return width_ == 0 || height_ == 0; }
    /// True once after the framebuffer size changed; consumes the flag.
    [[nodiscard]] bool consume_resized();
    void set_resize_callback(std::function<void(u32, u32)> callback);

    void set_title(std::string_view title);
    void set_cursor_mode(CursorMode mode);
    [[nodiscard]] CursorMode cursor_mode() const { return cursor_mode_; }
    [[nodiscard]] bool vsync() const { return vsync_; }
    void set_vsync(bool enabled) { vsync_ = enabled; }
    [[nodiscard]] f32 dpi_scale() const { return dpi_scale_; }
    [[nodiscard]] f64 time() const;

    [[nodiscard]] const InputState& input() const { return input_; }
    [[nodiscard]] InputState& input() { return input_; }

    /// Native GLFWwindow* - for extensions that need the raw handle.
    [[nodiscard]] void* native_handle() const;

private:
    Window() = default;

    struct Impl;
    Scope<Impl> impl_;
    InputState input_{};
    u32 width_ = 0;
    u32 height_ = 0;
    f32 dpi_scale_ = 1.0f;
    bool resized_ = false;
    bool vsync_ = true;
    CursorMode cursor_mode_ = CursorMode::Normal;
    std::function<void(u32, u32)> resize_callback_;
};

} // namespace ore
