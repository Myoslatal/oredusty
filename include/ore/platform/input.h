// Ore framework - keyboard/mouse state and named action mapping.
//
// The window layer fills an InputState every frame; game code either queries it directly or
// binds named actions/axes through InputMap so key choices stay out of gameplay code.
#pragma once

#include <ore/core/types.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace ore {

/// Key codes. The values intentionally mirror GLFW's key codes so the platform layer can forward
/// them without a translation table; treat them as opaque identifiers.
enum class Key : i32 {
    Unknown = -1,
    Space = 32, Apostrophe = 39, Comma = 44, Minus = 45, Period = 46, Slash = 47,
    Num0 = 48, Num1 = 49, Num2 = 50, Num3 = 51, Num4 = 52, Num5 = 53, Num6 = 54, Num7 = 55, Num8 = 56, Num9 = 57,
    Semicolon = 59, Equal = 61,
    A = 65, B = 66, C = 67, D = 68, E = 69, F = 70, G = 71, H = 72, I = 73, J = 74, K = 75, L = 76, M = 77,
    N = 78, O = 79, P = 80, Q = 81, R = 82, S = 83, T = 84, U = 85, V = 86, W = 87, X = 88, Y = 89, Z = 90,
    LeftBracket = 91, Backslash = 92, RightBracket = 93, GraveAccent = 96,
    Escape = 256, Enter = 257, Tab = 258, Backspace = 259, Insert = 260, Delete = 261,
    Right = 262, Left = 263, Down = 264, Up = 265, PageUp = 266, PageDown = 267, Home = 268, End = 269,
    CapsLock = 280, ScrollLock = 281, NumLock = 282, PrintScreen = 283, Pause = 284,
    F1 = 290, F2 = 291, F3 = 292, F4 = 293, F5 = 294, F6 = 295, F7 = 296, F8 = 297,
    F9 = 298, F10 = 299, F11 = 300, F12 = 301,
    Kp0 = 320, Kp1 = 321, Kp2 = 322, Kp3 = 323, Kp4 = 324, Kp5 = 325, Kp6 = 326, Kp7 = 327,
    Kp8 = 328, Kp9 = 329, KpDecimal = 330, KpDivide = 331, KpMultiply = 332, KpSubtract = 333,
    KpAdd = 334, KpEnter = 335, KpEqual = 336,
    LeftShift = 340, LeftControl = 341, LeftAlt = 342, LeftSuper = 343,
    RightShift = 344, RightControl = 345, RightAlt = 346, RightSuper = 347, Menu = 348,
    Count = 349,
};

enum class MouseButton : u8 { Left = 0, Right = 1, Middle = 2, Button4 = 3, Button5 = 4, Button6 = 5, Button7 = 6, Count = 7 };

enum class CursorMode : u8 { Normal, Hidden, Disabled };

/// The window system reports the pointer in *screen coordinates* (the window's content size, e.g.
/// 512x384), while an Ore app draws and hit-tests in *framebuffer pixels* (1024x768 for that same
/// window on a 2x display). This is the factor between the two, one per axis because a fractional
/// scale rounds the width and the height independently.
struct PixelScale {
    f32 x = 1.0f;
    f32 y = 1.0f;

    /// The factor from the two sizes the window layer knows. A window with no size (minimised) or no
    /// framebuffer yields 1.0 instead of dividing by zero.
    [[nodiscard]] static PixelScale of(u32 window_width, u32 window_height, u32 framebuffer_width,
                                       u32 framebuffer_height);
    [[nodiscard]] bool identity() const { return x == 1.0f && y == 1.0f; }
    [[nodiscard]] f32 to_pixels_x(f32 screen_x) const { return screen_x * x; }
    [[nodiscard]] f32 to_pixels_y(f32 screen_y) const { return screen_y * y; }
};

class InputState {
public:
    static constexpr usize kKeyCount = static_cast<usize>(Key::Count);
    static constexpr usize kMouseButtonCount = static_cast<usize>(MouseButton::Count);

    void begin_frame();  ///< clears per-frame edges and deltas

    // --- keyboard ---
    [[nodiscard]] bool key_down(Key key) const;
    [[nodiscard]] bool key_pressed(Key key) const;
    [[nodiscard]] bool key_released(Key key) const;
    /// True when any shift/control/alt/super key is held.
    [[nodiscard]] bool shift_down() const;
    [[nodiscard]] bool control_down() const;
    [[nodiscard]] bool alt_down() const;

    // --- mouse ---
    // Every pointer value is a framebuffer pixel, never a screen coordinate: a game lays out its UI
    // and its hit tests in the same pixels the renderer draws in, so a scaled display cannot shift
    // the pointer away from what is under it.
    [[nodiscard]] bool mouse_down(MouseButton button) const;
    [[nodiscard]] bool mouse_pressed(MouseButton button) const;
    [[nodiscard]] bool mouse_released(MouseButton button) const;
    [[nodiscard]] f32 mouse_x() const { return mouse_x_; }
    [[nodiscard]] f32 mouse_y() const { return mouse_y_; }
    [[nodiscard]] f32 mouse_delta_x() const { return mouse_dx_; }
    [[nodiscard]] f32 mouse_delta_y() const { return mouse_dy_; }
    [[nodiscard]] f32 scroll_x() const { return scroll_x_; }
    [[nodiscard]] f32 scroll_y() const { return scroll_y_; }
    [[nodiscard]] bool cursor_inside_window() const { return cursor_inside_; }
    /// UTF-8 characters typed since the last frame (text fields, debug consoles).
    [[nodiscard]] std::string_view text_input() const { return text_; }

    // --- used by the window layer ---
    void set_key(Key key, bool down);
    void set_mouse_button(MouseButton button, bool down);
    void set_mouse_position(f32 x, f32 y);
    void add_mouse_delta(f32 dx, f32 dy);
    /// One pointer event from the window layer, in screen coordinates. Stores the position and the
    /// delta in framebuffer pixels; the first event after the pointer entered the window carries no
    /// delta, so a pointer that reappears somewhere else does not drag the view with it.
    void add_pointer_event(f32 screen_x, f32 screen_y, PixelScale scale);
    void add_scroll(f32 x, f32 y);
    /// Leaving the content area clears the pointer tracking, so the next event starts a fresh delta.
    void set_cursor_inside(bool inside);
    void add_text(std::string_view utf8);

private:
    std::array<bool, kKeyCount> key_down_{};
    std::array<bool, kKeyCount> key_pressed_{};
    std::array<bool, kKeyCount> key_released_{};
    std::array<bool, kMouseButtonCount> mouse_down_{};
    std::array<bool, kMouseButtonCount> mouse_pressed_{};
    std::array<bool, kMouseButtonCount> mouse_released_{};
    f32 mouse_x_ = 0.0f;
    f32 mouse_y_ = 0.0f;
    f32 pointer_x_ = 0.0f;  ///< the last pointer event, in framebuffer pixels
    f32 pointer_y_ = 0.0f;
    bool pointer_tracked_ = false;
    f32 mouse_dx_ = 0.0f;
    f32 mouse_dy_ = 0.0f;
    f32 scroll_x_ = 0.0f;
    f32 scroll_y_ = 0.0f;
    bool cursor_inside_ = false;
    std::string text_;
};

/// Binds friendly action names to keys/buttons, then evaluates them against an InputState.
class InputMap {
public:
    void bind(std::string_view action, Key key);
    void bind(std::string_view action, MouseButton button);
    void bind_axis(std::string_view axis, Key positive, Key negative);
    /// Removes every binding of \p action or \p axis.
    void unbind(std::string_view name);
    void clear();

    [[nodiscard]] bool action_down(std::string_view action) const;
    [[nodiscard]] bool action_pressed(std::string_view action) const;
    [[nodiscard]] bool action_released(std::string_view action) const;
    /// -1, 0 or +1 depending on the bound keys.
    [[nodiscard]] f32 axis(std::string_view axis) const;

    [[nodiscard]] usize action_count() const { return actions_.size(); }
    [[nodiscard]] usize axis_count() const { return axes_.size(); }
    [[nodiscard]] ConstSpan<Key> keys_of(std::string_view action) const;

    /// The window layer calls this once per frame with the state it just filled in.
    void update(const InputState& state) { state_ = &state; }

private:
    struct Action {
        std::string name;
        std::vector<Key> keys;
        std::vector<MouseButton> buttons;
    };
    struct Axis {
        std::string name;
        Key positive = Key::Unknown;
        Key negative = Key::Unknown;
    };

    [[nodiscard]] const Action* find_action(std::string_view name) const;
    [[nodiscard]] const Axis* find_axis(std::string_view name) const;

    std::vector<Action> actions_;
    std::vector<Axis> axes_;
    const InputState* state_ = nullptr;
};

} // namespace ore
