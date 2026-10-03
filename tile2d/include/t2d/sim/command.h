// Tile2D - player input commands. The only thing a client is allowed to send to the server.
#pragma once

#include <t2d/core/types.h>

namespace t2d {

/// Button bits. The low bits are "held" state, the high bit is the edge triggered jump so a command
/// is self contained (the server never has to look at a previous command to know a jump started).
enum ButtonBits : u8 {
    kButtonLeft = 1u << 0,
    kButtonRight = 1u << 1,
    kButtonDown = 1u << 2,      ///< hold to drop through one-way platforms
    kButtonJumpHeld = 1u << 3,
    kButtonJumpPressed = 1u << 4, ///< edge: jump went down this tick
    kButtonRun = 1u << 5,         ///< hold to sprint (used by the demo level)
};

[[nodiscard]] constexpr bool button_held(u8 buttons, u8 mask) { return (buttons & mask) != 0u; }

struct PlayerCommand {
    u32 tick = 0;       ///< simulation tick the client wants this applied on
    u8 buttons = 0;

    [[nodiscard]] bool left() const { return button_held(buttons, kButtonLeft); }
    [[nodiscard]] bool right() const { return button_held(buttons, kButtonRight); }
    [[nodiscard]] bool down() const { return button_held(buttons, kButtonDown); }
    [[nodiscard]] bool jump_held() const { return button_held(buttons, kButtonJumpHeld); }
    [[nodiscard]] bool jump_pressed() const { return button_held(buttons, kButtonJumpPressed); }
    [[nodiscard]] bool run() const { return button_held(buttons, kButtonRun); }

    friend constexpr bool operator==(const PlayerCommand&, const PlayerCommand&) = default;
};

} // namespace t2d
