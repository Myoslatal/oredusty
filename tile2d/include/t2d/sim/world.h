// Tile2D - the authoritative simulation.
//
// The same code runs on the server (authoritative) and on the client (prediction of the local player
// and rendering of everything else). It is deterministic: given the same map, tuning and command
// sequence it produces bit identical state, which the tests assert by comparing checksums.
#pragma once

#include <t2d/core/math2d.h>
#include <t2d/core/types.h>
#include <t2d/sim/command.h>
#include <t2d/sim/tilemap.h>
#include <t2d/sim/tileset.h>
#include <t2d/sim/tuning.h>

#include <string>
#include <vector>

namespace t2d {

using PlayerId = u32;
using PickupId = u32;

enum PlayerFlags : u8 {
    kPlayerOnGround = 1u << 0,
    kPlayerFacingRight = 1u << 1,
    kPlayerAlive = 1u << 2,
    kPlayerRunning = 1u << 3,
};

struct PlayerState {
    PlayerId id = kInvalidId;
    Vec2 position{};        ///< top-left corner of the AABB
    Vec2 velocity{};
    /// Where the player respawns after dying. Replicated so that the client's prediction of a
    /// death/respawn cycle is identical to the server's.
    Vec2 spawn_position{};
    u32 last_command_tick = 0;
    u16 coins = 0;
    u8 flags = kPlayerAlive;
    u8 coyote_ticks = 0;
    u8 jump_buffer_ticks = 0;
    u8 respawn_ticks = 0;

    [[nodiscard]] bool on_ground() const { return (flags & kPlayerOnGround) != 0u; }
    [[nodiscard]] bool alive() const { return (flags & kPlayerAlive) != 0u; }
    [[nodiscard]] bool facing_right() const { return (flags & kPlayerFacingRight) != 0u; }
    [[nodiscard]] Aabb2 bounds(const PlayerTuning& tuning) const {
        return Aabb2::from_top_left(position, tuning.size);
    }
    friend bool operator==(const PlayerState&, const PlayerState&) = default;
};

/// Pickups are 8x8 world units, centred inside the tile they were authored on.
inline constexpr Vec2 kPickupSize{8.0f, 8.0f};

struct Pickup {
    PickupId id = kInvalidId;
    Vec2 position{};      ///< top-left corner
    u8 kind = 0;          ///< 0 = coin, 1 = gem (worth more; the demo only uses the sprite)
    bool alive = true;

    friend bool operator==(const Pickup&, const Pickup&) = default;
};

struct WorldConfig {
    TileMap map{};
    Tileset tileset = Tileset::default_platformer();
    PlayerTuning tuning{};
    u64 seed = 0x5EED1234ull;
    i32 pickups_per_row = 3;
};

/// Result of a single player step: what happened this tick, so the server can turn it into events
/// (sound, particles) and the client into animation.
struct PlayerStepResult {
    bool jumped = false;
    bool landed = false;
    bool hit_ceiling = false;
    bool died = false;
    bool respawned = false;
};

class World {
public:
    struct Events {
        struct CoinCollected {
            PlayerId player = kInvalidId;
            PickupId pickup = kInvalidId;
            Vec2 position{};
        };
        struct PlayerJumped {
            PlayerId player = kInvalidId;
            Vec2 position{};
        };
        struct PlayerDied {
            PlayerId player = kInvalidId;
            Vec2 position{};
        };
        std::vector<CoinCollected> coins;
        std::vector<PlayerJumped> jumps;
        std::vector<PlayerDied> deaths;

        void clear() {
            coins.clear();
            jumps.clear();
            deaths.clear();
        }
    };

    explicit World(const WorldConfig& config);

    [[nodiscard]] const WorldConfig& config() const { return config_; }
    [[nodiscard]] const TileMap& map() const { return config_.map; }
    [[nodiscard]] const Tileset& tileset() const { return config_.tileset; }
    [[nodiscard]] const PlayerTuning& tuning() const { return config_.tuning; }

    [[nodiscard]] u32 tick() const { return tick_; }
    void set_tick(u32 tick) { tick_ = tick; }

    // --- players ------------------------------------------------------------
    /// Allocates the next free player id and drops the player at a deterministic spawn point.
    [[nodiscard]] PlayerId spawn_player();
    void despawn_player(PlayerId id);
    /// Queues the command that will be applied on the *next* step for that player.
    void submit_command(PlayerId id, const PlayerCommand& command);
    [[nodiscard]] const PlayerCommand& last_command(PlayerId id) const;

    [[nodiscard]] ConstSpan<PlayerState> players() const { return players_; }
    [[nodiscard]] const PlayerState* find_player(PlayerId id) const;
    [[nodiscard]] PlayerState* find_player(PlayerId id);
    /// Client side: creates a placeholder for a player that only exists on the server yet.
    PlayerState& ensure_player(PlayerId id);
    void apply_player_state(PlayerId id, const PlayerState& state);

    // --- pickups ------------------------------------------------------------
    [[nodiscard]] ConstSpan<Pickup> pickups() const { return pickups_; }
    [[nodiscard]] const Pickup* find_pickup(PickupId id) const;
    void apply_pickup_state(PickupId id, const Pickup& pickup);
    void set_pickups(std::vector<Pickup> pickups) { pickups_ = std::move(pickups); }

    // --- simulation ---------------------------------------------------------
    /// Advances exactly one tick: applies queued commands, integrates movement, resolves pickups
    /// and hazards. Fills events() for this tick.
    void step();
    /// Deterministic single player step, shared with client side prediction.
    [[nodiscard]] PlayerStepResult step_player(PlayerState& player, const PlayerCommand& command) const;

    /// Client side prediction entry point: steps exactly one player (movement plus their own pickup
    /// collection) so the local player can run ahead of the server without touching anybody else.
    [[nodiscard]] PlayerStepResult predict_player(PlayerId id, const PlayerCommand& command);

    /// Deterministic spawn point for the n-th player (spread along the map's top area).
    [[nodiscard]] Vec2 spawn_point(u32 index) const;

    /// FNV-1a over the quantised state (1/16 px) - the same value on server and client when in sync.
    [[nodiscard]] u64 checksum() const;
    /// Full description of one player's quantised state, for desync reports.
    [[nodiscard]] std::string describe(PlayerId id) const;

    [[nodiscard]] const Events& events() const { return events_; }

private:
    void spawn_pickups();

    WorldConfig config_{};
    std::vector<PlayerState> players_;
    std::vector<Pickup> pickups_;
    std::vector<PlayerCommand> commands_;      ///< parallel to players_
    std::vector<PlayerCommand> held_commands_; ///< last command seen per player (kept when none arrives)
    std::vector<PlayerId> free_ids_;
    PlayerId next_player_id_ = 1;
    PickupId next_pickup_id_ = 1;
    u32 tick_ = 0;
    Events events_{};
};

} // namespace t2d
