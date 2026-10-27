// Mefemma - what a frame is handed about the keys and the pointer.
//
// The shell does not give a scene the platform's input state: it gives it this one, filled from the
// platform once a frame. That makes a scene's input a value a test can build, and it keeps a game's code
// from depending on which window system the engine happened to use.
//
// The state is **edge aware**: key_down() is the situation, key_pressed()/key_released() are the events of
// this frame. Both are cleared at the start of a frame by begin_frame(), which is the one call a host has
// to remember.
#pragma once

#include <t2d/core/types.h>

#include <array>
#include <string>
#include <string_view>

namespace mefemma {

using t2d::f32;
using t2d::u16;
using t2d::u32;
using t2d::u8;
using t2d::usize;

/// Keyboard codes. The values mirror GLFW's, the same way the engine's own do, so a platform layer can
/// hand them over without a translation table. They are opaque identifiers, not characters: text a player
/// types arrives through text_input().
enum class Key : u16 {
    Unknown = 0,
    Space = 32, Apostrophe = 39, Comma = 44, Minus = 45, Period = 46, Slash = 47,
    D0 = 48, D1 = 49, D2 = 50, D3 = 51, D4 = 52, D5 = 53, D6 = 54, D7 = 55, D8 = 56, D9 = 57,
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

enum class MouseButton : u8 { Left = 0, Right = 1, Middle = 2, Button4 = 3, Button5 = 4, Count = 5 };

/// One frame of input, in framebuffer pixels (the same pixels the renderer draws in).
class InputState {
public:
    static constexpr usize kKeyCount = static_cast<usize>(Key::Count);
    static constexpr usize kMouseButtonCount = static_cast<usize>(MouseButton::Count);

    /// Clears the per-frame edges, deltas, wheel and typed text. The keys that are held stay held, and the
    /// pointer's last position stays known, so the next position still has a delta.
    void begin_frame();

    void set_key(Key key, bool down);
    void set_mouse_button(MouseButton button, bool down);
    /// The pointer's position this frame. The delta accumulates from the previous position; the first
    /// position after begin_frame() (or after the pointer re-entered the window) has no delta.
    void set_mouse_position(f32 x, f32 y);
    void add_scroll(f32 x, f32 y);
    /// Leaving the window forgets where the pointer was, so re-entering does not drag the view across the
    /// screen.
    void set_cursor_inside(bool inside);
    void set_text(std::string_view text);

    [[nodiscard]] bool key_down(Key key) const;
    [[nodiscard]] bool key_pressed(Key key) const;
    [[nodiscard]] bool key_released(Key key) const;

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
    /// UTF-8 characters typed since begin_frame().
    [[nodiscard]] std::string_view text_input() const { return text_; }

private:
    std::array<bool, kKeyCount> down_{};
    std::array<bool, kKeyCount> pressed_{};
    std::array<bool, kKeyCount> released_{};
    std::array<bool, kMouseButtonCount> mouse_down_{};
    std::array<bool, kMouseButtonCount> mouse_pressed_{};
    std::array<bool, kMouseButtonCount> mouse_released_{};
    f32 mouse_x_ = 0.0f;
    f32 mouse_y_ = 0.0f;
    f32 mouse_dx_ = 0.0f;
    f32 mouse_dy_ = 0.0f;
    f32 scroll_x_ = 0.0f;
    f32 scroll_y_ = 0.0f;
    f32 last_x_ = 0.0f;
    f32 last_y_ = 0.0f;
    bool position_known_ = false;
    bool cursor_inside_ = false;
    std::string text_;
};

} // namespace mefemma
