// Mine - the half of a thing that receives clicks.
//
// A click is an event the game's own logic gets told about (the screen turns a mouse press into "this
// cell was clicked", app.cpp), and what a thing does with one is its own: a **click** is that side of it,
// and it is a component because everything that shows a panel needs the same two rules - a click **on it**
// opens (or closes) its panel, and a click **somewhere else** closes it, because a panel that stayed open
// while the pointer went elsewhere would be lying about what is selected (docs/GAME_DESIGN.md section
// 1.27).
//
// It holds no idea of what the panel shows: that is the container's (types/container.h), and drawing it
// is the screen's job.
#pragma once

#include <t2d/core/types.h>

namespace mine::types {

using t2d::u32;

class Click {
public:
    /// A click arrived. \p on_it is whether it landed on the thing this belongs to. Returns whether that
    /// thing took it - true when it did, which is also what opened or closed its panel.
    bool receive(bool on_it);
    /// Whether its panel is showing.
    [[nodiscard]] bool open() const { return open_; }
    /// Shows or hides the panel by hand (a screen that opens one for its own reason).
    void set_open(bool open) { open_ = open; }
    /// How many clicks landed on it so far.
    [[nodiscard]] u32 clicks() const { return clicks_; }

private:
    bool open_ = false;
    u32 clicks_ = 0;
};

} // namespace mine::types
