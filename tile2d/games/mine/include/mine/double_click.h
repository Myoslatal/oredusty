// Mine - what makes two presses one double click.
//
// A double click is the game's own idea of "these two presses were one gesture", and the game needs it
// because the designer put a second meaning on the pointer: a **double click** selects a plot that has a
// container, and what it holds shows at the top of the screen (docs/GAME_DESIGN.md section 1.31). A
// single click keeps its own meaning (types/click.h), so the two are told apart here rather than by the
// window layer: what a window system calls a double click is that system's setting, and a game that read
// it would behave differently on every desktop.
//
// The rule is two presses on the **same cell** inside a **window of time**, and nothing else: the pointer
// may wander between them (a hand that shakes must not turn one double click into two), while a press
// somewhere else starts the count again.
#pragma once

#include <mine/types/tile.h>

#include <t2d/core/types.h>

namespace mine {

using t2d::f32;

/// How long the second press may take and still count as a double click, in seconds. A code constant and
/// not content data: the designer asked for double click selection, not for a number to tune.
inline constexpr f32 kDoubleClickSeconds = 0.4f;

class DoubleClick {
public:
    /// Waits \p window seconds for the second press.
    explicit DoubleClick(f32 window = kDoubleClickSeconds) : window_(window) {}

    /// Called once a frame with the time that passed. A first press is forgotten when the window ends, so
    /// a press a minute later is a first press again.
    void advance(f32 seconds);

    /// A press at \p cell. True when this press **completes** a double click - the same cell, still inside
    /// the window - and starts the window again, so a third press can pair with it.
    bool press(GridPos cell);

    /// Forgets a pending first press: a layer changed, a screen did, nothing is being double clicked.
    void reset();

    /// Whether a first press is still waiting for its second.
    [[nodiscard]] bool pending() const { return pending_; }
    /// The window, in seconds.
    [[nodiscard]] f32 window() const { return window_; }

private:
    f32 window_ = kDoubleClickSeconds;
    f32 left_ = 0.0f;   ///< how much of the window is left
    GridPos cell_{};
    bool pending_ = false;
};

} // namespace mine
