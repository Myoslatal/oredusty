#include <ore/platform/input.h>

#include <ore/core/assert.h>

#include <algorithm>

namespace ore {
namespace {

[[nodiscard]] constexpr usize index_of(Key key) { return static_cast<usize>(key); }
[[nodiscard]] constexpr usize index_of(MouseButton button) { return static_cast<usize>(button); }

} // namespace

PixelScale PixelScale::of(u32 window_width, u32 window_height, u32 framebuffer_width,
                          u32 framebuffer_height) {
    PixelScale scale;
    // A minimised window reports a zero content size and a hidden one can report a zero framebuffer;
    // there is no factor to compute then, and 1.0 keeps every caller's arithmetic finite.
    if (window_width == 0 || window_height == 0) return scale;
    if (framebuffer_width == 0 || framebuffer_height == 0) return scale;
    scale.x = static_cast<f32>(framebuffer_width) / static_cast<f32>(window_width);
    scale.y = static_cast<f32>(framebuffer_height) / static_cast<f32>(window_height);
    return scale;
}

void InputState::begin_frame() {
    key_pressed_.fill(false);
    key_released_.fill(false);
    mouse_pressed_.fill(false);
    mouse_released_.fill(false);
    mouse_dx_ = 0.0f;
    mouse_dy_ = 0.0f;
    scroll_x_ = 0.0f;
    scroll_y_ = 0.0f;
    text_.clear();
}

bool InputState::key_down(Key key) const {
    const usize index = index_of(key);
    return index < kKeyCount ? key_down_[index] : false;
}

bool InputState::key_pressed(Key key) const {
    const usize index = index_of(key);
    return index < kKeyCount ? key_pressed_[index] : false;
}

bool InputState::key_released(Key key) const {
    const usize index = index_of(key);
    return index < kKeyCount ? key_released_[index] : false;
}

bool InputState::shift_down() const {
    return key_down(Key::LeftShift) || key_down(Key::RightShift);
}

bool InputState::control_down() const {
    return key_down(Key::LeftControl) || key_down(Key::RightControl);
}

bool InputState::alt_down() const { return key_down(Key::LeftAlt) || key_down(Key::RightAlt); }

bool InputState::mouse_down(MouseButton button) const {
    const usize index = index_of(button);
    return index < kMouseButtonCount ? mouse_down_[index] : false;
}

bool InputState::mouse_pressed(MouseButton button) const {
    const usize index = index_of(button);
    return index < kMouseButtonCount ? mouse_pressed_[index] : false;
}

bool InputState::mouse_released(MouseButton button) const {
    const usize index = index_of(button);
    return index < kMouseButtonCount ? mouse_released_[index] : false;
}

void InputState::set_key(Key key, bool down) {
    const usize index = index_of(key);
    if (index >= kKeyCount) return;
    if (down && !key_down_[index]) key_pressed_[index] = true;
    if (!down && key_down_[index]) key_released_[index] = true;
    key_down_[index] = down;
}

void InputState::set_mouse_button(MouseButton button, bool down) {
    const usize index = index_of(button);
    if (index >= kMouseButtonCount) return;
    if (down && !mouse_down_[index]) mouse_pressed_[index] = true;
    if (!down && mouse_down_[index]) mouse_released_[index] = true;
    mouse_down_[index] = down;
}

void InputState::set_mouse_position(f32 x, f32 y) {
    mouse_x_ = x;
    mouse_y_ = y;
}

void InputState::add_mouse_delta(f32 dx, f32 dy) {
    mouse_dx_ += dx;
    mouse_dy_ += dy;
}

void InputState::add_pointer_event(f32 screen_x, f32 screen_y, PixelScale scale) {
    const f32 x = scale.to_pixels_x(screen_x);
    const f32 y = scale.to_pixels_y(screen_y);
    // A delta is only meaningful between two positions the pointer actually travelled through: the
    // first event after entering the window would otherwise look like a jump from wherever the
    // pointer was last seen.
    if (pointer_tracked_) add_mouse_delta(x - pointer_x_, y - pointer_y_);
    pointer_tracked_ = true;
    pointer_x_ = x;
    pointer_y_ = y;
    set_mouse_position(x, y);
}

void InputState::add_scroll(f32 x, f32 y) {
    scroll_x_ += x;
    scroll_y_ += y;
}

void InputState::set_cursor_inside(bool inside) {
    cursor_inside_ = inside;
    pointer_tracked_ = false;
}

void InputState::add_text(std::string_view utf8) { text_.append(utf8); }

// ---------------------------------------------------------------- InputMap ---

void InputMap::bind(std::string_view action, Key key) {
    for (Action& existing : actions_) {
        if (existing.name == action) {
            if (std::find(existing.keys.begin(), existing.keys.end(), key) == existing.keys.end()) {
                existing.keys.push_back(key);
            }
            return;
        }
    }
    Action entry;
    entry.name = std::string(action);
    entry.keys.push_back(key);
    actions_.push_back(std::move(entry));
}

void InputMap::bind(std::string_view action, MouseButton button) {
    for (Action& existing : actions_) {
        if (existing.name == action) {
            if (std::find(existing.buttons.begin(), existing.buttons.end(), button) == existing.buttons.end()) {
                existing.buttons.push_back(button);
            }
            return;
        }
    }
    Action entry;
    entry.name = std::string(action);
    entry.buttons.push_back(button);
    actions_.push_back(std::move(entry));
}

void InputMap::bind_axis(std::string_view axis, Key positive, Key negative) {
    for (Axis& existing : axes_) {
        if (existing.name == axis) {
            existing.positive = positive;
            existing.negative = negative;
            return;
        }
    }
    axes_.push_back(Axis{std::string(axis), positive, negative});
}

void InputMap::unbind(std::string_view name) {
    actions_.erase(std::remove_if(actions_.begin(), actions_.end(),
                                  [name](const Action& action) { return action.name == name; }),
                   actions_.end());
    axes_.erase(std::remove_if(axes_.begin(), axes_.end(),
                               [name](const Axis& axis) { return axis.name == name; }),
                axes_.end());
}

void InputMap::clear() {
    actions_.clear();
    axes_.clear();
}

const InputMap::Action* InputMap::find_action(std::string_view name) const {
    for (const Action& action : actions_) {
        if (action.name == name) return &action;
    }
    return nullptr;
}

const InputMap::Axis* InputMap::find_axis(std::string_view name) const {
    for (const Axis& axis : axes_) {
        if (axis.name == name) return &axis;
    }
    return nullptr;
}

bool InputMap::action_down(std::string_view action) const {
    if (state_ == nullptr) return false;
    const Action* entry = find_action(action);
    if (entry == nullptr) return false;
    for (Key key : entry->keys) {
        if (state_->key_down(key)) return true;
    }
    for (MouseButton button : entry->buttons) {
        if (state_->mouse_down(button)) return true;
    }
    return false;
}

bool InputMap::action_pressed(std::string_view action) const {
    if (state_ == nullptr) return false;
    const Action* entry = find_action(action);
    if (entry == nullptr) return false;
    for (Key key : entry->keys) {
        if (state_->key_pressed(key)) return true;
    }
    for (MouseButton button : entry->buttons) {
        if (state_->mouse_pressed(button)) return true;
    }
    return false;
}

bool InputMap::action_released(std::string_view action) const {
    if (state_ == nullptr) return false;
    const Action* entry = find_action(action);
    if (entry == nullptr) return false;
    for (Key key : entry->keys) {
        if (state_->key_released(key)) return true;
    }
    for (MouseButton button : entry->buttons) {
        if (state_->mouse_released(button)) return true;
    }
    return false;
}

f32 InputMap::axis(std::string_view name) const {
    if (state_ == nullptr) return 0.0f;
    const Axis* entry = find_axis(name);
    if (entry == nullptr) return 0.0f;
    f32 value = 0.0f;
    if (entry->positive != Key::Unknown && state_->key_down(entry->positive)) value += 1.0f;
    if (entry->negative != Key::Unknown && state_->key_down(entry->negative)) value -= 1.0f;
    return value;
}

ConstSpan<Key> InputMap::keys_of(std::string_view action) const {
    const Action* entry = find_action(action);
    if (entry == nullptr) return {};
    return ConstSpan<Key>(entry->keys.data(), entry->keys.size());
}

} // namespace ore
