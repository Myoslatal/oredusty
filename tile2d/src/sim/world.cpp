#include <t2d/sim/world.h>

#include <t2d/core/log.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace t2d {
namespace {

constexpr u64 kFnvOffset = 0xCBF29CE484222325ull;
constexpr u64 kFnvPrime = 0x100000001B3ull;

[[nodiscard]] u64 hash_bytes(u64 hash, const void* data, usize size) {
    const auto* bytes = static_cast<const u8*>(data);
    for (usize i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
    return hash;
}

template <class T>
[[nodiscard]] u64 hash_value(u64 hash, const T& value) {
    return hash_bytes(hash, &value, sizeof(T));
}

/// Snapshots and checksums use 1/16 pixel fixed point: identical on every platform, and the client
/// compares its prediction against exactly the numbers the server sent.
[[nodiscard]] i32 quantize(f32 value) {
    return static_cast<i32>(value * 16.0f + (value >= 0.0f ? 0.5f : -0.5f));
}

[[nodiscard]] Aabb2 pickup_bounds(const Pickup& pickup) {
    return Aabb2::from_top_left(pickup.position, kPickupSize);
}

} // namespace

World::World(const WorldConfig& config) : config_(config) { spawn_pickups(); }

void World::spawn_pickups() {
    pickups_.clear();
    pickups_.reserve(64);
    // Coin tiles in the level act as spawn markers: the simulation turns each of them into a pickup
    // and clears the cell, so level authoring stays plain ASCII art and the collision map never
    // contains the markers.
    constexpr TileId kCoinMarker = 6;
    const f32 tile = config_.map.tile_size();
    const Vec2 inset{(tile - kPickupSize.x) * 0.5f, (tile - kPickupSize.y) * 0.5f};
    for (i32 y = 0; y < config_.map.height(); ++y) {
        for (i32 x = 0; x < config_.map.width(); ++x) {
            if (config_.map.at(x, y) != kCoinMarker) continue;
            Pickup pickup;
            pickup.id = next_pickup_id_++;
            pickup.position = config_.map.tile_to_world(x, y) + inset;
            pickup.kind = 0;
            pickup.alive = true;
            pickups_.push_back(pickup);
            config_.map.set(x, y, kEmptyTile);
        }
    }
    if (!pickups_.empty()) T2D_DEBUG("world: {} pickup(s) spawned from the level", pickups_.size());
}

PlayerId World::spawn_player() {
    PlayerId id = next_player_id_++;
    if (!free_ids_.empty()) {
        const auto smallest = std::min_element(free_ids_.begin(), free_ids_.end());
        id = *smallest;
        free_ids_.erase(smallest);
    }

    PlayerState player;
    player.id = id;
    player.spawn_position = spawn_point(static_cast<u32>(players_.size()));
    player.position = player.spawn_position;
    player.flags = kPlayerAlive | kPlayerFacingRight;

    // Keep the vector sorted by id so that every peer iterates in the same order.
    const auto position = std::lower_bound(players_.begin(), players_.end(), id,
                                           [](const PlayerState& state, PlayerId value) { return state.id < value; });
    const usize index = static_cast<usize>(position - players_.begin());
    players_.insert(position, player);
    commands_.insert(commands_.begin() + static_cast<isize>(index), PlayerCommand{});
    held_commands_.insert(held_commands_.begin() + static_cast<isize>(index), PlayerCommand{});
    return id;
}

void World::despawn_player(PlayerId id) {
    for (usize i = 0; i < players_.size(); ++i) {
        if (players_[i].id != id) continue;
        players_.erase(players_.begin() + static_cast<isize>(i));
        commands_.erase(commands_.begin() + static_cast<isize>(i));
        held_commands_.erase(held_commands_.begin() + static_cast<isize>(i));
        free_ids_.push_back(id);
        return;
    }
}

PlayerState& World::ensure_player(PlayerId id) {
    if (PlayerState* existing = find_player(id); existing != nullptr) return *existing;
    PlayerState player;
    player.id = id;
    player.spawn_position = spawn_point(static_cast<u32>(players_.size()));
    player.position = player.spawn_position;
    const auto position = std::lower_bound(players_.begin(), players_.end(), id,
                                           [](const PlayerState& state, PlayerId value) { return state.id < value; });
    const usize index = static_cast<usize>(position - players_.begin());
    players_.insert(position, player);
    commands_.insert(commands_.begin() + static_cast<isize>(index), PlayerCommand{});
    held_commands_.insert(held_commands_.begin() + static_cast<isize>(index), PlayerCommand{});
    return players_[index];
}

void World::submit_command(PlayerId id, const PlayerCommand& command) {
    for (usize i = 0; i < players_.size(); ++i) {
        if (players_[i].id != id) continue;
        // Late commands (older than the one already queued) are dropped: the simulation never
        // rewinds, it only ever moves forward.
        if (commands_[i].tick <= command.tick) commands_[i] = command;
        return;
    }
}

const PlayerCommand& World::last_command(PlayerId id) const {
    static const PlayerCommand empty{};
    for (usize i = 0; i < players_.size(); ++i) {
        if (players_[i].id == id) return held_commands_[i];
    }
    return empty;
}

const PlayerState* World::find_player(PlayerId id) const {
    for (const PlayerState& player : players_) {
        if (player.id == id) return &player;
    }
    return nullptr;
}

PlayerState* World::find_player(PlayerId id) {
    for (PlayerState& player : players_) {
        if (player.id == id) return &player;
    }
    return nullptr;
}

void World::apply_player_state(PlayerId id, const PlayerState& state) {
    PlayerState& target = ensure_player(id);
    const Vec2 spawn = target.spawn_position;
    target = state;
    if (state.spawn_position == Vec2{}) target.spawn_position = spawn;
}

const Pickup* World::find_pickup(PickupId id) const {
    for (const Pickup& pickup : pickups_) {
        if (pickup.id == id) return &pickup;
    }
    return nullptr;
}

void World::apply_pickup_state(PickupId id, const Pickup& pickup) {
    for (Pickup& existing : pickups_) {
        if (existing.id == id) {
            existing = pickup;
            return;
        }
    }
    pickups_.push_back(pickup);
}

Vec2 World::spawn_point(u32 index) const {
    const f32 tile = config_.map.tile_size();
    const i32 columns = std::max(config_.map.width(), 1);
    // Spread spawns across the map, then drop them onto the first free spot above solid ground.
    const i32 column = (static_cast<i32>(index) * 5 + 3) % columns;
    i32 ground_row = config_.map.height() - 1;
    for (i32 y = 0; y < config_.map.height(); ++y) {
        if (config_.map.is_solid(column, y, config_.tileset)) {
            ground_row = y;
            break;
        }
    }
    const f32 x = static_cast<f32>(column) * tile + (tile - config_.tuning.size.x) * 0.5f;
    const f32 y = static_cast<f32>(std::max(ground_row - 2, 0)) * tile;
    return Vec2{x, y};
}

PlayerStepResult World::step_player(PlayerState& player, const PlayerCommand& command) const {
    const PlayerTuning& tuning = config_.tuning;
    const TileMap& map = config_.map;
    const Tileset& tileset = config_.tileset;
    PlayerStepResult result;

    player.last_command_tick = command.tick;

    if (!player.alive()) {
        if (player.respawn_ticks > 0 && --player.respawn_ticks == 0) {
            player.position = player.spawn_position;
            player.velocity = {};
            player.flags = static_cast<u8>(kPlayerAlive | (player.flags & kPlayerFacingRight));
            player.coyote_ticks = 0;
            player.jump_buffer_ticks = 0;
            result.respawned = true;
        }
        return result;
    }

    const f32 delta = kTickSeconds;
    const f32 direction = (command.right() ? 1.0f : 0.0f) - (command.left() ? 1.0f : 0.0f);
    const f32 speed = tuning.move_speed * (command.run() ? tuning.run_multiplier : 1.0f);
    const bool was_on_ground = player.on_ground();
    const f32 accel = (was_on_ground ? tuning.ground_accel : tuning.air_accel) * delta;

    if (direction != 0.0f) {
        player.velocity.x = approach(player.velocity.x, direction * speed, accel);
        if (direction > 0.0f) player.flags |= kPlayerFacingRight;
        else player.flags &= static_cast<u8>(~kPlayerFacingRight);
        if (command.run()) player.flags |= kPlayerRunning;
        else player.flags &= static_cast<u8>(~kPlayerRunning);
    } else {
        const f32 deceleration = (was_on_ground ? tuning.ground_friction : tuning.air_drag) * delta;
        player.velocity.x = approach(player.velocity.x, 0.0f, deceleration);
        player.flags &= static_cast<u8>(~kPlayerRunning);
    }

    // Jump buffering and coyote time: both are tiny counters, replicated so that the client's
    // prediction continues exactly where the server left off.
    const u32 coyote_max = seconds_to_ticks(tuning.coyote_seconds);
    const u32 buffer_max = seconds_to_ticks(tuning.jump_buffer_seconds);
    if (command.jump_pressed()) player.jump_buffer_ticks = static_cast<u8>(buffer_max);
    if (was_on_ground) player.coyote_ticks = static_cast<u8>(coyote_max);
    else if (player.coyote_ticks > 0) --player.coyote_ticks;

    if (player.jump_buffer_ticks > 0 && player.coyote_ticks > 0) {
        player.velocity.y = -tuning.jump_velocity;
        player.jump_buffer_ticks = 0;
        player.coyote_ticks = 0;
        player.flags &= static_cast<u8>(~kPlayerOnGround);
        result.jumped = true;
    } else if (player.jump_buffer_ticks > 0) {
        --player.jump_buffer_ticks;
    }

    // Releasing the jump button early cuts the rise. Clamping (instead of multiplying) keeps the
    // operation idempotent, so applying it on every held-released tick cannot stack up.
    if (!command.jump_held() && player.velocity.y < 0.0f) {
        const f32 cut = -tuning.jump_velocity * tuning.jump_cut_multiplier;
        player.velocity.y = std::max(player.velocity.y, cut);
    }

    player.velocity.y = std::min(player.velocity.y + tuning.gravity * delta, tuning.max_fall_speed);

    const TileMap::MoveResult move =
        map.move_aabb(player.bounds(tuning), player.velocity, delta, tileset, command.down());
    player.position = move.position;
    player.velocity = move.velocity;

    if (move.on_ground) player.flags |= kPlayerOnGround;
    else player.flags &= static_cast<u8>(~kPlayerOnGround);
    result.landed = move.on_ground && !was_on_ground;
    result.hit_ceiling = move.hit_ceiling;

    if (map.overlaps_hazard(player.bounds(tuning), tileset)) {
        player.flags &= static_cast<u8>(~kPlayerAlive);
        player.velocity = {};
        player.respawn_ticks = static_cast<u8>(seconds_to_ticks(tuning.hazard_respawn_seconds));
        result.died = true;
    }
    return result;
}

PlayerStepResult World::predict_player(PlayerId id, const PlayerCommand& command) {
    PlayerState* player = find_player(id);
    if (player == nullptr) return {};
    const PlayerStepResult result = step_player(*player, command);

    // Pickup collection is part of the prediction so the local player's coin counter reacts
    // immediately; the server's snapshot stays authoritative and corrects it if it disagrees.
    if (player->alive()) {
        const Aabb2 bounds = player->bounds(config_.tuning);
        for (Pickup& pickup : pickups_) {
            if (!pickup.alive) continue;
            if (!bounds.overlaps(pickup_bounds(pickup))) continue;
            pickup.alive = false;
            ++player->coins;
        }
    }
    return result;
}

void World::step() {
    events_.clear();

    for (usize i = 0; i < players_.size(); ++i) {
        PlayerState& player = players_[i];

        PlayerCommand command = commands_[i];
        const bool repeated = command.tick == 0;
        if (repeated) {
            // No command arrived for this tick: keep the held buttons but never repeat the jump edge.
            command = held_commands_[i];
            command.buttons &= static_cast<u8>(~kButtonJumpPressed);
        } else {
            held_commands_[i] = command;
        }

        const PlayerStepResult result = step_player(player, command);

        if (result.jumped) events_.jumps.push_back(Events::PlayerJumped{player.id, player.position});
        if (result.died) events_.deaths.push_back(Events::PlayerDied{player.id, player.position});

        if (player.alive()) {
            const Aabb2 bounds = player.bounds(config_.tuning);
            for (Pickup& pickup : pickups_) {
                if (!pickup.alive) continue;
                if (!bounds.overlaps(pickup_bounds(pickup))) continue;
                pickup.alive = false;
                ++player.coins;
                events_.coins.push_back(Events::CoinCollected{player.id, pickup.id, pickup.position});
            }
        }

        commands_[i] = PlayerCommand{};
    }

    ++tick_;
}

u64 World::checksum() const {
    u64 hash = kFnvOffset;
    hash = hash_value(hash, tick_);
    for (const PlayerState& player : players_) {
        const i32 x = quantize(player.position.x);
        const i32 y = quantize(player.position.y);
        const i32 vx = quantize(player.velocity.x);
        const i32 vy = quantize(player.velocity.y);
        hash = hash_value(hash, player.id);
        hash = hash_value(hash, x);
        hash = hash_value(hash, y);
        hash = hash_value(hash, vx);
        hash = hash_value(hash, vy);
        hash = hash_value(hash, player.coins);
        hash = hash_value(hash, player.flags);
        hash = hash_value(hash, player.coyote_ticks);
        hash = hash_value(hash, player.jump_buffer_ticks);
        hash = hash_value(hash, player.respawn_ticks);
    }
    for (const Pickup& pickup : pickups_) {
        hash = hash_value(hash, pickup.id);
        hash = hash_value(hash, static_cast<u8>(pickup.alive ? 1u : 0u));
        hash = hash_value(hash, quantize(pickup.position.x));
        hash = hash_value(hash, quantize(pickup.position.y));
    }
    return hash;
}

std::string World::describe(PlayerId id) const {
    const PlayerState* player = find_player(id);
    if (player == nullptr) return std::format("player {}: <missing>", id);
    return std::format("player {}: pos=({:.3f},{:.3f}) vel=({:.3f},{:.3f}) flags=0x{:02x} coins={} tick={}", id,
                       static_cast<f64>(player->position.x), static_cast<f64>(player->position.y),
                       static_cast<f64>(player->velocity.x), static_cast<f64>(player->velocity.y), player->flags,
                       player->coins, player->last_command_tick);
}

} // namespace t2d
