// Mine - the start screen as data.
//
// Everything the screen shows and does lives here: no window, no GPU, no clock. The application maps
// platform key presses to MenuKey, calls handle() and draws whatever the model reports, which is what
// makes the whole start screen testable in milliseconds.
#pragma once

#include <mine/session.h>

#include <t2d/core/types.h>

namespace mine {

using t2d::i32;
using t2d::usize;

/// What the start screen asks the application to do.
enum class MenuAction : u8 { None, StartSession, Quit };

/// One key press, translated from the platform so the model never sees a window.
enum class MenuKey : u8 { Up, Down, Left, Right, Confirm, Back, Randomise };

enum class RowKind : u8 { World, Seed, Action };

struct MenuRow {
    RowKind kind = RowKind::Action;
    MenuAction action = MenuAction::None;
    Role role = Role::Single;
    const char* label = "";
};

class MenuModel {
public:
    /// World, seed (endless only) and four actions.
    static constexpr usize kMaxRows = 6;
    /// Seeds are kept in a range that stays readable on screen.
    static constexpr u32 kMinSeed = 1;
    static constexpr u32 kMaxSeed = 99999999u;

    MenuModel();

    [[nodiscard]] usize row_count() const { return row_count_; }
    [[nodiscard]] const MenuRow& row(usize index) const;
    [[nodiscard]] usize selected() const { return selected_; }
    void select(usize index);

    /// Applies a key press; returns the action to run when the press confirmed an action row.
    [[nodiscard]] MenuAction handle(MenuKey key);

    [[nodiscard]] Mode mode() const { return mode_; }
    void set_mode(Mode mode);
    [[nodiscard]] u32 seed() const { return seed_; }
    void set_seed(u32 seed);
    /// Next seed in a fixed pseudo random sequence (deterministic, so a test can assert it).
    void randomise_seed();
    [[nodiscard]] bool seed_visible() const { return mode_ == Mode::Endless; }

    /// The session the focused row would start.
    [[nodiscard]] SessionConfig session_config() const { return session_config_for(selected_); }
    [[nodiscard]] SessionConfig session_config_for(usize index) const;

private:
    void rebuild_rows();

    Mode mode_ = Mode::Story;
    u32 seed_ = 1;
    usize selected_ = 0;
    usize row_count_ = 0;
    MenuRow rows_[kMaxRows]{};
};

} // namespace mine
