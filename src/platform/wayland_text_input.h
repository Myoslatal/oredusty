// Ore - the Wayland text-input-v3 client.
//
// GLFW's Wayland backend has no text-input protocol, so without this an IME such as fcitx5 cannot type
// into a window. The object lives beside the window on the Wayland platform only: window.cpp creates
// it, turns it on while a text field has the focus and gives it the caret's rectangle. Compiled only
// when the build found wayland-client and the protocol XML (ORE_HAS_WAYLAND_TEXT_INPUT).
#pragma once

#include <ore/core/types.h>

#include <string>

struct wl_display;
struct wl_registry;
struct wl_seat;
struct wl_surface;
struct zwp_text_input_manager_v3;
struct zwp_text_input_v3;

namespace ore {

class InputState;

class WaylandTextInput {
public:
    /// Binds the registry, takes the manager and the seat and creates the text input. Never fails: a
    /// compositor without zwp_text_input_manager_v3, or one that announces no seat, logs a warning and
    /// leaves every call a no-op (the build with the protocol missing does not compile this file at
    /// all).
    [[nodiscard]] static Scope<WaylandTextInput> create(InputState& input, wl_display* display,
                                                        wl_surface* surface);
    ~WaylandTextInput();
    ORE_NON_MOVABLE(WaylandTextInput);

    /// Asks the compositor for text input. Until the surface has an enter the request is remembered
    /// and applied then, because the protocol allows requests only while the surface has the focus.
    void set_enabled(bool enabled);
    /// The caret's rectangle, in surface coordinates: an IME places its candidate window there. The
    /// rectangle is kept and sent with the next enable; while enabled a change is sent at once.
    void set_cursor_rect(i32 x, i32 y, i32 w, i32 h);

private:
    WaylandTextInput(InputState& input, wl_display* display, wl_surface* surface);

    void bind_globals();
    void ensure_text_input();
    void apply_enable();
    void apply_disable();
    void send_cursor_rect();

    static void on_global(void* data, wl_registry* registry, u32 name, const char* interface,
                          u32 version);
    static void on_global_remove(void* data, wl_registry* registry, u32 name);
    static void on_enter(void* data, zwp_text_input_v3* text_input, wl_surface* surface);
    static void on_leave(void* data, zwp_text_input_v3* text_input, wl_surface* surface);
    static void on_preedit(void* data, zwp_text_input_v3* text_input, const char* text,
                           i32 cursor_begin, i32 cursor_end);
    static void on_commit(void* data, zwp_text_input_v3* text_input, const char* text);
    static void on_delete(void* data, zwp_text_input_v3* text_input, u32 before_length,
                          u32 after_length);
    static void on_done(void* data, zwp_text_input_v3* text_input, u32 serial);

    InputState* input_ = nullptr;
    wl_display* display_ = nullptr;
    wl_surface* surface_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_seat* seat_ = nullptr;
    zwp_text_input_manager_v3* manager_ = nullptr;
    zwp_text_input_v3* text_input_ = nullptr;
    u32 manager_version_ = 0;
    bool manager_bound_ = false;
    /// What set_enabled() asked for (not necessarily what the compositor has heard yet).
    bool enabled_requested_ = false;
    /// enable has been committed and not disabled since.
    bool enabled_ = false;
    /// The compositor says the text input has the surface's focus.
    bool focused_ = false;

    i32 cursor_x_ = 0;
    i32 cursor_y_ = 0;
    i32 cursor_w_ = 0;
    i32 cursor_h_ = 0;
    bool has_cursor_rect_ = false;
    /// The rectangle last put on the wire, so a screen that hands the same one every frame does not
    /// commit it every frame.
    i32 sent_cursor_x_ = 0;
    i32 sent_cursor_y_ = 0;
    i32 sent_cursor_w_ = 0;
    i32 sent_cursor_h_ = 0;

    /// The double-buffered part of the protocol: events fill these, done applies them.
    std::string pending_preedit_;
    std::string pending_commit_;
    bool pending_preedit_changed_ = false;
    bool pending_commit_changed_ = false;
};

} // namespace ore
