#include <ore/platform/window.h>

#include <ore/core/assert.h>
#include <ore/core/log.h>

#include <vulkan/vulkan.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstring>

namespace ore {
namespace {

u32 g_window_count = 0;
bool g_glfw_ready = false;

void glfw_error_callback(int code, const char* description) {
    ORE_ERROR("glfw error {}: {}", code, description != nullptr ? description : "unknown");
}

[[nodiscard]] int to_glfw_cursor_mode(CursorMode mode) {
    switch (mode) {
        case CursorMode::Hidden: return GLFW_CURSOR_HIDDEN;
        case CursorMode::Disabled: return GLFW_CURSOR_DISABLED;
        default: return GLFW_CURSOR_NORMAL;
    }
}

} // namespace

struct Window::Impl {
    GLFWwindow* handle = nullptr;
    f64 last_cursor_x = 0.0;
    f64 last_cursor_y = 0.0;
    bool first_cursor_event = true;
};

Scope<Window> Window::create(const WindowDesc& desc) {
    if (!g_glfw_ready) {
        glfwSetErrorCallback(&glfw_error_callback);
        if (glfwInit() != GLFW_TRUE) {
            ORE_ERROR("failed to initialise the window system (no display?). "
                      "Use --headless to render offscreen.");
            return nullptr;
        }
        g_glfw_ready = true;
        const char* platform_name = "unknown";
        switch (glfwGetPlatform()) {
            case GLFW_PLATFORM_WIN32: platform_name = "win32"; break;
            case GLFW_PLATFORM_COCOA: platform_name = "cocoa"; break;
            case GLFW_PLATFORM_WAYLAND: platform_name = "wayland"; break;
            case GLFW_PLATFORM_X11: platform_name = "x11"; break;
            case GLFW_PLATFORM_NULL: platform_name = "null"; break;
            default: break;
        }
        ORE_INFO("window system: GLFW {} ({})", glfwGetVersionString(), platform_name);
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // Vulkan only
    glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, desc.visible ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_DECORATED, desc.decorated ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_MAXIMIZED, desc.maximized ? GLFW_TRUE : GLFW_FALSE);

    Scope<Window> window(new Window());
    window->impl_ = make_scope<Impl>();
    window->impl_->handle = glfwCreateWindow(static_cast<int>(desc.width), static_cast<int>(desc.height),
                                            desc.title.c_str(), nullptr, nullptr);
    if (window->impl_->handle == nullptr) {
        ORE_ERROR("failed to create a {}x{} window", desc.width, desc.height);
        return nullptr;
    }
    ++g_window_count;
    window->vsync_ = desc.vsync;

    glfwSetWindowUserPointer(window->impl_->handle, window.get());
    GLFWwindow* handle = window->impl_->handle;

    glfwSetFramebufferSizeCallback(handle, [](GLFWwindow* source, int width, int height) {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(source));
        if (self == nullptr) return;
        self->width_ = static_cast<u32>(std::max(width, 0));
        self->height_ = static_cast<u32>(std::max(height, 0));
        self->resized_ = true;
        if (self->resize_callback_) self->resize_callback_(self->width_, self->height_);
    });
    glfwSetKeyCallback(handle, [](GLFWwindow* source, int key, int, int action, int) {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(source));
        if (self == nullptr) return;
        if (action == GLFW_PRESS) self->input_.set_key(static_cast<Key>(key), true);
        else if (action == GLFW_RELEASE) self->input_.set_key(static_cast<Key>(key), false);
    });
    glfwSetCharCallback(handle, [](GLFWwindow* source, unsigned int codepoint) {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(source));
        if (self == nullptr) return;
        char utf8[5] = {};
        const u32 value = codepoint;
        if (value < 0x80u) {
            utf8[0] = static_cast<char>(value);
        } else if (value < 0x800u) {
            utf8[0] = static_cast<char>(0xC0u | (value >> 6));
            utf8[1] = static_cast<char>(0x80u | (value & 0x3Fu));
        } else if (value < 0x10000u) {
            utf8[0] = static_cast<char>(0xE0u | (value >> 12));
            utf8[1] = static_cast<char>(0x80u | ((value >> 6) & 0x3Fu));
            utf8[2] = static_cast<char>(0x80u | (value & 0x3Fu));
        } else {
            utf8[0] = static_cast<char>(0xF0u | (value >> 18));
            utf8[1] = static_cast<char>(0x80u | ((value >> 12) & 0x3Fu));
            utf8[2] = static_cast<char>(0x80u | ((value >> 6) & 0x3Fu));
            utf8[3] = static_cast<char>(0x80u | (value & 0x3Fu));
        }
        self->input_.add_text(utf8);
    });
    glfwSetMouseButtonCallback(handle, [](GLFWwindow* source, int button, int action, int) {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(source));
        if (self == nullptr || button < 0 || button >= static_cast<int>(MouseButton::Count)) return;
        self->input_.set_mouse_button(static_cast<MouseButton>(button), action == GLFW_PRESS);
    });
    glfwSetCursorPosCallback(handle, [](GLFWwindow* source, double x, double y) {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(source));
        if (self == nullptr) return;
        if (!self->impl_->first_cursor_event) {
            self->input_.add_mouse_delta(static_cast<f32>(x - self->impl_->last_cursor_x),
                                         static_cast<f32>(y - self->impl_->last_cursor_y));
        }
        self->impl_->first_cursor_event = false;
        self->impl_->last_cursor_x = x;
        self->impl_->last_cursor_y = y;
        self->input_.set_mouse_position(static_cast<f32>(x), static_cast<f32>(y));
    });
    glfwSetCursorEnterCallback(handle, [](GLFWwindow* source, int entered) {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(source));
        if (self == nullptr) return;
        self->input_.set_cursor_inside(entered == GLFW_TRUE);
        self->impl_->first_cursor_event = true;
    });
    glfwSetScrollCallback(handle, [](GLFWwindow* source, double dx, double dy) {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(source));
        if (self == nullptr) return;
        self->input_.add_scroll(static_cast<f32>(dx), static_cast<f32>(dy));
    });

    int framebuffer_width = 0;
    int framebuffer_height = 0;
    glfwGetFramebufferSize(handle, &framebuffer_width, &framebuffer_height);
    window->width_ = static_cast<u32>(std::max(framebuffer_width, 1));
    window->height_ = static_cast<u32>(std::max(framebuffer_height, 1));

    // Wayland compositors decide window placement themselves and GLFW errors out if asked.
    const bool platform_supports_positioning = glfwGetPlatform() == GLFW_PLATFORM_X11 ||
                                               glfwGetPlatform() == GLFW_PLATFORM_WIN32 ||
                                               glfwGetPlatform() == GLFW_PLATFORM_COCOA;
    if (desc.center && platform_supports_positioning) {
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        if (monitor != nullptr) {
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);
            if (mode != nullptr) {
                int monitor_x = 0;
                int monitor_y = 0;
                glfwGetMonitorPos(monitor, &monitor_x, &monitor_y);
                glfwSetWindowPos(handle, monitor_x + (mode->width - static_cast<int>(desc.width)) / 2,
                                 monitor_y + (mode->height - static_cast<int>(desc.height)) / 2);
            }
        }
    }

    float scale_x = 1.0f;
    float scale_y = 1.0f;
    glfwGetWindowContentScale(handle, &scale_x, &scale_y);
    window->dpi_scale_ = std::max(scale_x, 0.5f);

    ORE_INFO("window: \"{}\" {}x{} (dpi scale {:.2f})", desc.title, window->width_, window->height_,
             static_cast<f64>(window->dpi_scale_));
    return window;
}

Window::~Window() {
    if (impl_ != nullptr && impl_->handle != nullptr) {
        glfwDestroyWindow(impl_->handle);
        impl_->handle = nullptr;
        if (g_window_count > 0) --g_window_count;
        if (g_window_count == 0 && g_glfw_ready) {
            glfwTerminate();
            g_glfw_ready = false;
        }
    }
}

std::vector<std::string> Window::required_instance_extensions() {
    std::vector<std::string> extensions;
    if (!g_glfw_ready && glfwInit() != GLFW_TRUE) return extensions;
    g_glfw_ready = true;
    u32 count = 0;
    const char** required = glfwGetRequiredInstanceExtensions(&count);
    for (u32 i = 0; i < count; ++i) extensions.emplace_back(required[i]);
    return extensions;
}

bool Window::platform_initialised() { return g_glfw_ready; }

VkSurfaceKHR Window::create_surface(VkInstance instance) const {
    if (impl_ == nullptr || impl_->handle == nullptr) return VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const VkResult result = glfwCreateWindowSurface(instance, impl_->handle, nullptr, &surface);
    if (result != VK_SUCCESS) {
        ORE_ERROR("glfwCreateWindowSurface failed: {}", static_cast<int>(result));
        return VK_NULL_HANDLE;
    }
    return surface;
}

bool Window::should_close() const {
    return impl_ == nullptr || impl_->handle == nullptr || glfwWindowShouldClose(impl_->handle) == GLFW_TRUE;
}

void Window::request_close() {
    if (impl_ != nullptr && impl_->handle != nullptr) glfwSetWindowShouldClose(impl_->handle, GLFW_TRUE);
}

void Window::poll_events() {
    input_.begin_frame();
    glfwPollEvents();
}

void Window::wait_events(f32 timeout_seconds) {
    input_.begin_frame();
    if (timeout_seconds <= 0.0f) {
        glfwPollEvents();
    } else {
        glfwWaitEventsTimeout(static_cast<double>(timeout_seconds));
    }
}

bool Window::consume_resized() {
    const bool value = resized_;
    resized_ = false;
    return value;
}

void Window::set_resize_callback(std::function<void(u32, u32)> callback) {
    resize_callback_ = std::move(callback);
}

void Window::set_title(std::string_view title) {
    if (impl_ == nullptr || impl_->handle == nullptr) return;
    const std::string owned(title);
    glfwSetWindowTitle(impl_->handle, owned.c_str());
}

void Window::set_cursor_mode(CursorMode mode) {
    if (impl_ == nullptr || impl_->handle == nullptr) return;
    cursor_mode_ = mode;
    glfwSetInputMode(impl_->handle, GLFW_CURSOR, to_glfw_cursor_mode(mode));
    if (mode == CursorMode::Disabled) {
        if (glfwRawMouseMotionSupported() == GLFW_TRUE) {
            glfwSetInputMode(impl_->handle, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        }
    }
}

f64 Window::time() const { return glfwGetTime(); }

void* Window::native_handle() const { return impl_ != nullptr ? static_cast<void*>(impl_->handle) : nullptr; }

} // namespace ore
