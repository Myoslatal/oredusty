// Mine - what a session is, without saying what is inside one.
//
// The engine knows *how* a mine is produced, never what it contains: story worlds come from the
// designer's data, endless worlds are generated from a seed. Layer content is data (see
// docs/GAME_DESIGN.md), so nothing here enumerates resources, structures, machines or recipes.
#pragma once

#include <t2d/core/types.h>

#include <string>

namespace mine {

using t2d::f32;
using t2d::u16;
using t2d::u32;
using t2d::u8;
using t2d::usize;

/// Where a mine's layers come from.
enum class Mode : u8 { Story, Endless };

/// How this process takes part in a session. The transports behind these roles already exist in the
/// framework (shared memory for a local client, KCP for remote players, no client thread at all for
/// a dedicated server); whether the game ships multiplayer is the designer's decision.
enum class Role : u8 { Single, Host, Join };

[[nodiscard]] constexpr const char* mode_name(Mode mode) {
    return mode == Mode::Endless ? "ENDLESS" : "STORY";
}

[[nodiscard]] constexpr const char* role_name(Role role) {
    switch (role) {
        case Role::Single: return "SINGLE";
        case Role::Host: return "HOST";
        case Role::Join: return "JOIN";
    }
    return "?";
}

struct SessionConfig {
    Mode mode = Mode::Story;
    Role role = Role::Single;
    u32 seed = 1;
    std::string connect_address = "127.0.0.1:7777";
    u16 port = 7777;
};

} // namespace mine
