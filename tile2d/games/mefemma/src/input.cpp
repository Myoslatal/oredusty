#include <mefemma/input.h>

namespace mefemma {

void InputState::begin_frame() {
    pressed_.fill(false);
    released_.fill(false);
    mouse_pressed_.fill(false);
    mouse_released_.fill(false);
    mouse_dx_ = 0.0f;
    mouse_dy_ = 0.0f;
    scroll_x_ = 0.0f;
    scroll_y_ = 0.0f;
    text_.clear();
}

void InputState::set_key(Key key, bool down) {
    const usize index = static_cast<usize>(key);
    if (index >= kKeyCount) return;
    if (down == down_[index]) return;
    down_[index] = down;
    if (down) pressed_[index] = true;
    else released_[index] = true;
}

void InputState::set_mouse_button(MouseButton button, bool down) {
    const usize index = static_cast<usize>(button);
    if (index >= kMouseButtonCount) return;
    if (down == mouse_down_[index]) return;
    mouse_down_[index] = down;
    if (down) mouse_pressed_[index] = true;
    else mouse_released_[index] = true;
}

void InputState::set_mouse_position(f32 x, f32 y) {
    if (position_known_) {
        mouse_dx_ += x - last_x_;
        mouse_dy_ += y - last_y_;
    }
    position_known_ = true;
    last_x_ = x;
    last_y_ = y;
    mouse_x_ = x;
    mouse_y_ = y;
}

void InputState::add_scroll(f32 x, f32 y) {
    scroll_x_ += x;
    scroll_y_ += y;
}

void InputState::set_cursor_inside(bool inside) {
    if (inside == cursor_inside_) return;
    cursor_inside_ = inside;
    // The pointer's last position belongs to the window it was in. Forgetting it is what keeps a pointer
    // that re-enters somewhere else from dragging the view there.
    if (!inside) position_known_ = false;
}

void InputState::set_text(std::string_view text) { text_ = std::string(text); }

bool InputState::key_down(Key key) const {
    const usize index = static_cast<usize>(key);
    return index < kKeyCount && down_[index];
}

bool InputState::key_pressed(Key key) const {
    const usize index = static_cast<usize>(key);
    return index < kKeyCount && pressed_[index];
}

bool InputState::key_released(Key key) const {
    const usize index = static_cast<usize>(key);
    return index < kKeyCount && released_[index];
}

bool InputState::mouse_down(MouseButton button) const {
    const usize index = static_cast<usize>(button);
    return index < kMouseButtonCount && mouse_down_[index];
}

bool InputState::mouse_pressed(MouseButton button) const {
    const usize index = static_cast<usize>(button);
    return index < kMouseButtonCount && mouse_pressed_[index];
}

bool InputState::mouse_released(MouseButton button) const {
    const usize index = static_cast<usize>(button);
    return index < kMouseButtonCount && mouse_released_[index];
}

} // namespace mefemma
