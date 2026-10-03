// Tile2D - state snapshots: what the server sends to clients, and what clients use to correct their
// prediction. Full snapshots describe the whole world; deltas reference an earlier baseline.
#pragma once

#include <t2d/core/types.h>
#include <t2d/sim/world.h>

#include <optional>
#include <vector>

namespace t2d {

inline constexpr u32 kSnapshotMagic = 0x53324454u; ///< "T2DS"
inline constexpr u8 kSnapshotVersion = 1;

enum class SnapshotKind : u8 { Full = 0, Delta = 1 };

/// Field masks used by delta encoding; a bit is set when the field differs from the baseline.
enum SnapshotField : u8 {
    kFieldPosition = 1u << 0,
    kFieldVelocity = 1u << 1,
    kFieldFlags = 1u << 2,
    kFieldCoins = 1u << 3,
    kFieldCommandTick = 1u << 4,
    kFieldCounters = 1u << 5,
    kFieldSpawn = 1u << 6,
    kFieldPickupState = 1u << 0, ///< pickups reuse bit 0 for "alive/kind"
    kFieldAll = 0x7Fu,
};

struct Snapshot {
    SnapshotKind kind = SnapshotKind::Full;
    u32 tick = 0;
    /// For deltas: the tick of the state this delta must be applied to (kInvalidId = none).
    u32 baseline_tick = kInvalidId;
    /// Last command tick the server applied for the receiving player. The client replays every
    /// command newer than this one on top of the snapshot.
    u32 acked_command_tick = 0;
    u64 server_time_ms = 0;
    /// World::checksum() at capture time, so a client can detect a desync immediately.
    u64 checksum = 0;
    std::vector<PlayerState> players;
    /// Delta only: which fields of the parallel entry in \p players actually changed. Full
    /// snapshots leave this empty (every field is present).
    std::vector<u8> player_masks;
    std::vector<Pickup> pickups;
    std::vector<u8> pickup_masks;
    std::vector<PlayerId> removed_players;
    std::vector<PickupId> removed_pickups;

    [[nodiscard]] const PlayerState* find_player(PlayerId id) const;
    [[nodiscard]] const Pickup* find_pickup(PickupId id) const;
};

/// Captures the complete world state.
[[nodiscard]] Snapshot capture_snapshot(const World& world, u32 acked_command_tick, u64 server_time_ms);

/// Builds a delta against \p baseline, or a full snapshot when the baseline does not match
/// (different tick, or the caller passed an empty one).
[[nodiscard]] Snapshot make_delta(const Snapshot& current, const Snapshot& baseline);

/// Reconstructs a full snapshot by applying \p delta on top of \p baseline. Returns false when the
/// delta does not reference the baseline (the caller should then ask for a full snapshot).
[[nodiscard]] bool apply_delta(const Snapshot& baseline, const Snapshot& delta, Snapshot& out);

/// Overwrites the world with a full snapshot: players missing from the snapshot are removed.
void apply_snapshot(World& world, const Snapshot& snapshot);

// --- wire format -----------------------------------------------------------
[[nodiscard]] std::vector<u8> encode_snapshot(const Snapshot& snapshot);
[[nodiscard]] bool decode_snapshot(ConstSpan<const u8> data, Snapshot& out);
/// Number of bytes a snapshot occupies (convenience for stats/logging).
[[nodiscard]] usize snapshot_size(const Snapshot& snapshot);

/// Human readable summary used in logs and desync reports.
[[nodiscard]] std::string describe_snapshot(const Snapshot& snapshot);

} // namespace t2d
