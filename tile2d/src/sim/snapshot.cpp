#include <t2d/sim/snapshot.h>

#include <t2d/core/bitstream.h>
#include <t2d/core/log.h>

#include <algorithm>
#include <format>

namespace t2d {
namespace {

/// Two values are "equal" for delta purposes only when they encode to the same fixed point number:
/// finer differences cannot survive the wire format, so sending them would just waste bytes.
[[nodiscard]] i32 quantise(f32 value) { return static_cast<i32>(value * 16.0f + (value >= 0.0f ? 0.5f : -0.5f)); }
[[nodiscard]] bool same_fixed(f32 a, f32 b) { return quantise(a) == quantise(b); }
[[nodiscard]] bool same_vec(Vec2 a, Vec2 b) { return same_fixed(a.x, b.x) && same_fixed(a.y, b.y); }

[[nodiscard]] u8 player_diff_mask(const PlayerState& current, const PlayerState& baseline) {
    u8 mask = 0;
    if (!same_vec(current.position, baseline.position)) mask |= kFieldPosition;
    if (!same_vec(current.velocity, baseline.velocity)) mask |= kFieldVelocity;
    if (current.flags != baseline.flags) mask |= kFieldFlags;
    if (current.coins != baseline.coins) mask |= kFieldCoins;
    if (current.last_command_tick != baseline.last_command_tick) mask |= kFieldCommandTick;
    if (current.coyote_ticks != baseline.coyote_ticks || current.jump_buffer_ticks != baseline.jump_buffer_ticks ||
        current.respawn_ticks != baseline.respawn_ticks) {
        mask |= kFieldCounters;
    }
    if (!same_vec(current.spawn_position, baseline.spawn_position)) mask |= kFieldSpawn;
    return mask;
}

[[nodiscard]] u8 pickup_diff_mask(const Pickup& current, const Pickup& baseline) {
    u8 mask = 0;
    if (!same_vec(current.position, baseline.position)) mask |= kFieldPosition;
    if (current.alive != baseline.alive || current.kind != baseline.kind) mask |= kFieldPickupState;
    return mask;
}

void write_player(ByteWriter& writer, const PlayerState& player, u8 mask) {
    writer.write_varint(player.id);
    writer.write_u8(mask);
    if ((mask & kFieldPosition) != 0u) {
        writer.write_fixed_16(player.position.x);
        writer.write_fixed_16(player.position.y);
    }
    if ((mask & kFieldVelocity) != 0u) {
        writer.write_fixed_16(player.velocity.x);
        writer.write_fixed_16(player.velocity.y);
    }
    if ((mask & kFieldFlags) != 0u) writer.write_u8(player.flags);
    if ((mask & kFieldCoins) != 0u) writer.write_varint(player.coins);
    if ((mask & kFieldCommandTick) != 0u) writer.write_varint(player.last_command_tick);
    if ((mask & kFieldCounters) != 0u) {
        writer.write_u8(player.coyote_ticks);
        writer.write_u8(player.jump_buffer_ticks);
        writer.write_u8(player.respawn_ticks);
    }
    if ((mask & kFieldSpawn) != 0u) {
        writer.write_fixed_16(player.spawn_position.x);
        writer.write_fixed_16(player.spawn_position.y);
    }
}

[[nodiscard]] bool read_player(ByteReader& reader, PlayerState& player) {
    player.id = reader.read_varint();
    const u8 mask = reader.read_u8();
    if (!reader.ok()) return false;
    if ((mask & kFieldPosition) != 0u) {
        player.position.x = reader.read_fixed_16();
        player.position.y = reader.read_fixed_16();
    }
    if ((mask & kFieldVelocity) != 0u) {
        player.velocity.x = reader.read_fixed_16();
        player.velocity.y = reader.read_fixed_16();
    }
    if ((mask & kFieldFlags) != 0u) player.flags = reader.read_u8();
    if ((mask & kFieldCoins) != 0u) player.coins = static_cast<u16>(reader.read_varint());
    if ((mask & kFieldCommandTick) != 0u) player.last_command_tick = reader.read_varint();
    if ((mask & kFieldCounters) != 0u) {
        player.coyote_ticks = reader.read_u8();
        player.jump_buffer_ticks = reader.read_u8();
        player.respawn_ticks = reader.read_u8();
    }
    if ((mask & kFieldSpawn) != 0u) {
        player.spawn_position.x = reader.read_fixed_16();
        player.spawn_position.y = reader.read_fixed_16();
    }
    return reader.ok();
}

void write_pickup(ByteWriter& writer, const Pickup& pickup, u8 mask) {
    writer.write_varint(pickup.id);
    writer.write_u8(mask);
    if ((mask & kFieldPosition) != 0u) {
        writer.write_fixed_16(pickup.position.x);
        writer.write_fixed_16(pickup.position.y);
    }
    if ((mask & kFieldPickupState) != 0u) {
        writer.write_u8(pickup.kind);
        writer.write_bool(pickup.alive);
    }
}

[[nodiscard]] bool read_pickup(ByteReader& reader, Pickup& pickup) {
    pickup.id = reader.read_varint();
    const u8 mask = reader.read_u8();
    if (!reader.ok()) return false;
    if ((mask & kFieldPosition) != 0u) {
        pickup.position.x = reader.read_fixed_16();
        pickup.position.y = reader.read_fixed_16();
    }
    if ((mask & kFieldPickupState) != 0u) {
        pickup.kind = reader.read_u8();
        pickup.alive = reader.read_bool();
    }
    return reader.ok();
}

/// Copies only the fields selected by \p mask from \p delta into \p target.
void merge_player(PlayerState& target, const PlayerState& delta, u8 mask) {
    if ((mask & kFieldPosition) != 0u) target.position = delta.position;
    if ((mask & kFieldVelocity) != 0u) target.velocity = delta.velocity;
    if ((mask & kFieldFlags) != 0u) target.flags = delta.flags;
    if ((mask & kFieldCoins) != 0u) target.coins = delta.coins;
    if ((mask & kFieldCommandTick) != 0u) target.last_command_tick = delta.last_command_tick;
    if ((mask & kFieldCounters) != 0u) {
        target.coyote_ticks = delta.coyote_ticks;
        target.jump_buffer_ticks = delta.jump_buffer_ticks;
        target.respawn_ticks = delta.respawn_ticks;
    }
    if ((mask & kFieldSpawn) != 0u) target.spawn_position = delta.spawn_position;
}

void merge_pickup(Pickup& target, const Pickup& delta, u8 mask) {
    if ((mask & kFieldPosition) != 0u) target.position = delta.position;
    if ((mask & kFieldPickupState) != 0u) {
        target.kind = delta.kind;
        target.alive = delta.alive;
    }
}

void write_header(ByteWriter& writer, const Snapshot& snapshot) {
    writer.write_u32(kSnapshotMagic);
    writer.write_u8(kSnapshotVersion);
    writer.write_u8(static_cast<u8>(snapshot.kind));
    writer.write_varint(snapshot.tick);
    writer.write_varint(snapshot.baseline_tick);
    writer.write_varint(snapshot.acked_command_tick);
    writer.write_u64(snapshot.server_time_ms);
    writer.write_u64(snapshot.checksum);
}

} // namespace

const PlayerState* Snapshot::find_player(PlayerId id) const {
    for (const PlayerState& player : players) {
        if (player.id == id) return &player;
    }
    return nullptr;
}

const Pickup* Snapshot::find_pickup(PickupId id) const {
    for (const Pickup& pickup : pickups) {
        if (pickup.id == id) return &pickup;
    }
    return nullptr;
}

Snapshot capture_snapshot(const World& world, u32 acked_command_tick, u64 server_time_ms) {
    Snapshot snapshot;
    snapshot.kind = SnapshotKind::Full;
    snapshot.tick = world.tick();
    snapshot.baseline_tick = kInvalidId;
    snapshot.acked_command_tick = acked_command_tick;
    snapshot.server_time_ms = server_time_ms;
    snapshot.checksum = world.checksum();
    snapshot.players.assign(world.players().begin(), world.players().end());
    snapshot.pickups.assign(world.pickups().begin(), world.pickups().end());
    return snapshot;
}

Snapshot make_delta(const Snapshot& current, const Snapshot& baseline) {
    if (baseline.kind != SnapshotKind::Full || baseline.tick == kInvalidId || current.tick <= baseline.tick) {
        return current; // no usable baseline: send everything
    }

    Snapshot delta;
    delta.kind = SnapshotKind::Delta;
    delta.tick = current.tick;
    delta.baseline_tick = baseline.tick;
    delta.acked_command_tick = current.acked_command_tick;
    delta.server_time_ms = current.server_time_ms;
    delta.checksum = current.checksum;

    for (const PlayerState& player : current.players) {
        const PlayerState* previous = baseline.find_player(player.id);
        const u8 mask = previous != nullptr ? player_diff_mask(player, *previous) : static_cast<u8>(kFieldAll);
        if (mask == 0) continue; // unchanged: the receiver keeps its baseline entry
        delta.players.push_back(player);
        delta.player_masks.push_back(mask);
    }
    for (const PlayerState& player : baseline.players) {
        if (current.find_player(player.id) == nullptr) delta.removed_players.push_back(player.id);
    }

    for (const Pickup& pickup : current.pickups) {
        const Pickup* previous = baseline.find_pickup(pickup.id);
        const u8 mask = previous != nullptr ? pickup_diff_mask(pickup, *previous)
                                            : (kFieldPosition | kFieldPickupState);
        if (mask == 0) continue;
        delta.pickups.push_back(pickup);
        delta.pickup_masks.push_back(mask);
    }
    for (const Pickup& pickup : baseline.pickups) {
        if (current.find_pickup(pickup.id) == nullptr) delta.removed_pickups.push_back(pickup.id);
    }
    return delta;
}

bool apply_delta(const Snapshot& baseline, const Snapshot& delta, Snapshot& out) {
    if (delta.kind == SnapshotKind::Full) {
        out = delta;
        return true;
    }
    if (baseline.kind != SnapshotKind::Full || baseline.tick != delta.baseline_tick) return false;

    out = baseline;
    out.kind = SnapshotKind::Full;
    out.tick = delta.tick;
    out.baseline_tick = kInvalidId;
    out.acked_command_tick = delta.acked_command_tick;
    out.server_time_ms = delta.server_time_ms;
    out.checksum = delta.checksum;
    out.player_masks.clear();
    out.pickup_masks.clear();

    for (usize i = 0; i < delta.players.size(); ++i) {
        const PlayerState& incoming = delta.players[i];
        const u8 mask = i < delta.player_masks.size() ? delta.player_masks[i] : static_cast<u8>(kFieldAll);
        bool merged = false;
        for (PlayerState& existing : out.players) {
            if (existing.id != incoming.id) continue;
            merge_player(existing, incoming, mask);
            merged = true;
            break;
        }
        if (!merged) out.players.push_back(incoming); // a brand new player always arrives complete
    }
    for (PlayerId id : delta.removed_players) {
        out.players.erase(std::remove_if(out.players.begin(), out.players.end(),
                                         [id](const PlayerState& player) { return player.id == id; }),
                          out.players.end());
    }

    for (usize i = 0; i < delta.pickups.size(); ++i) {
        const Pickup& incoming = delta.pickups[i];
        const u8 mask = i < delta.pickup_masks.size() ? delta.pickup_masks[i]
                                                      : (kFieldPosition | kFieldPickupState);
        bool merged = false;
        for (Pickup& existing : out.pickups) {
            if (existing.id != incoming.id) continue;
            merge_pickup(existing, incoming, mask);
            merged = true;
            break;
        }
        if (!merged) out.pickups.push_back(incoming);
    }
    for (PickupId id : delta.removed_pickups) {
        out.pickups.erase(std::remove_if(out.pickups.begin(), out.pickups.end(),
                                         [id](const Pickup& pickup) { return pickup.id == id; }),
                          out.pickups.end());
    }

    std::sort(out.players.begin(), out.players.end(),
              [](const PlayerState& a, const PlayerState& b) { return a.id < b.id; });
    return true;
}

void apply_snapshot(World& world, const Snapshot& snapshot) {
    world.set_tick(snapshot.tick);

    std::vector<PlayerId> to_remove;
    for (const PlayerState& player : world.players()) {
        if (snapshot.find_player(player.id) == nullptr) to_remove.push_back(player.id);
    }
    for (PlayerId id : to_remove) world.despawn_player(id);
    for (const PlayerState& player : snapshot.players) world.apply_player_state(player.id, player);

    std::vector<Pickup> pickups(snapshot.pickups.begin(), snapshot.pickups.end());
    world.set_pickups(std::move(pickups));
}

std::vector<u8> encode_snapshot(const Snapshot& snapshot) {
    std::vector<u8> bytes;
    bytes.reserve(256);
    ByteWriter writer(bytes);
    write_header(writer, snapshot);

    const bool full = snapshot.kind == SnapshotKind::Full;
    writer.write_varint(static_cast<u32>(snapshot.players.size()));
    for (usize i = 0; i < snapshot.players.size(); ++i) {
        const u8 mask = full
                            ? static_cast<u8>(kFieldAll)
                            : (i < snapshot.player_masks.size() ? snapshot.player_masks[i]
                                                                : static_cast<u8>(kFieldAll));
        write_player(writer, snapshot.players[i], mask);
    }
    if (!full) {
        writer.write_varint(static_cast<u32>(snapshot.removed_players.size()));
        for (PlayerId id : snapshot.removed_players) writer.write_varint(id);
    }

    writer.write_varint(static_cast<u32>(snapshot.pickups.size()));
    for (usize i = 0; i < snapshot.pickups.size(); ++i) {
        const u8 mask = full ? (kFieldPosition | kFieldPickupState)
                             : (i < snapshot.pickup_masks.size()
                                    ? snapshot.pickup_masks[i]
                                    : static_cast<u8>(kFieldPosition | kFieldPickupState));
        write_pickup(writer, snapshot.pickups[i], mask);
    }
    if (!full) {
        writer.write_varint(static_cast<u32>(snapshot.removed_pickups.size()));
        for (PickupId id : snapshot.removed_pickups) writer.write_varint(id);
    }
    return bytes;
}

bool decode_snapshot(ConstSpan<const u8> data, Snapshot& out) {
    ByteReader reader(data);
    if (reader.read_u32() != kSnapshotMagic) return false;
    if (reader.read_u8() != kSnapshotVersion) return false;
    out.kind = static_cast<SnapshotKind>(reader.read_u8());
    out.tick = reader.read_varint();
    out.baseline_tick = reader.read_varint();
    out.acked_command_tick = reader.read_varint();
    out.server_time_ms = reader.read_u64();
    out.checksum = reader.read_u64();
    if (!reader.ok()) return false;
    if (out.kind != SnapshotKind::Full && out.kind != SnapshotKind::Delta) return false;

    out.players.clear();
    out.player_masks.clear();
    out.pickups.clear();
    out.pickup_masks.clear();
    out.removed_players.clear();
    out.removed_pickups.clear();

    constexpr u32 kMaxEntities = 65536;
    const u32 player_count = reader.read_varint();
    if (!reader.ok() || player_count > kMaxEntities) return false;
    out.players.reserve(player_count);
    for (u32 i = 0; i < player_count; ++i) {
        PlayerState player;
        if (!read_player(reader, player)) return false;
        out.players.push_back(player);
    }

    if (out.kind == SnapshotKind::Delta) {
        const u32 removed = reader.read_varint();
        if (!reader.ok() || removed > kMaxEntities) return false;
        for (u32 i = 0; i < removed; ++i) out.removed_players.push_back(reader.read_varint());
    }

    const u32 pickup_count = reader.read_varint();
    if (!reader.ok() || pickup_count > kMaxEntities) return false;
    out.pickups.reserve(pickup_count);
    for (u32 i = 0; i < pickup_count; ++i) {
        Pickup pickup;
        if (!read_pickup(reader, pickup)) return false;
        out.pickups.push_back(pickup);
    }

    if (out.kind == SnapshotKind::Delta) {
        const u32 removed = reader.read_varint();
        if (!reader.ok() || removed > kMaxEntities) return false;
        for (u32 i = 0; i < removed; ++i) out.removed_pickups.push_back(reader.read_varint());
    }
    return reader.ok();
}

usize snapshot_size(const Snapshot& snapshot) { return encode_snapshot(snapshot).size(); }

std::string describe_snapshot(const Snapshot& snapshot) {
    return std::format("snapshot kind={} tick={} baseline={} acked_cmd={} players={} pickups={}",
                       snapshot.kind == SnapshotKind::Full ? "full" : "delta", snapshot.tick,
                       snapshot.baseline_tick == kInvalidId ? -1 : static_cast<i64>(snapshot.baseline_tick),
                       snapshot.acked_command_tick, snapshot.players.size(), snapshot.pickups.size());
}

} // namespace t2d
