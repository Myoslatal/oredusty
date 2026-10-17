#include <mine/double_click.h>

namespace mine {

void DoubleClick::advance(f32 seconds) {
    if (!pending_ || seconds <= 0.0f) return;
    left_ -= seconds;
    // Running out of window is what makes the next press a first press again.
    if (left_ <= 0.0f) reset();
}

bool DoubleClick::press(GridPos cell) {
    const bool again = pending_ && cell.x == cell_.x && cell.y == cell_.y;
    pending_ = true;
    cell_ = cell;
    left_ = window_;
    return again;
}

void DoubleClick::reset() {
    pending_ = false;
    left_ = 0.0f;
}

} // namespace mine
