// Tile2D - snapshot history used to interpolate remote players.
//
// The client renders slightly in the past (interpolation delay): between two snapshots it lerps the
// positions of every player that is not locally controlled. This hides jitter and makes 20 Hz
// snapshot traffic look smooth at 60 fps.
#pragma once

#include <t2d/core/types.h>
#include <t2d/sim/snapshot.h>

#include <vector>

namespace t2d {

class SnapshotBuffer {
public:
    explicit SnapshotBuffer(usize capacity = 32);

    /// Inserts a snapshot (full ones only - deltas must be resolved first). Snapshots older than the
    /// newest one are dropped, duplicates are ignored.
    void push(Snapshot snapshot);
    void clear();

    [[nodiscard]] usize size() const { return snapshots_.size(); }
    [[nodiscard]] usize capacity() const { return capacity_; }
    [[nodiscard]] const Snapshot* newest() const;
    [[nodiscard]] const Snapshot* oldest() const;
    [[nodiscard]] const Snapshot* find(u32 tick) const;
    [[nodiscard]] u32 newest_tick() const;
    [[nodiscard]] u32 oldest_tick() const;

    /// Interpolates one player at fractional tick \p tick. Returns false when the player is not in
    /// the buffer; with a single snapshot the newest state is returned unchanged.
    [[nodiscard]] bool sample(f32 tick, PlayerId id, PlayerState& out) const;
    /// Interpolates every player known to the newest snapshot.
    void sample_all(f32 tick, std::vector<PlayerState>& out) const;

private:
    [[nodiscard]] const Snapshot* newest_not_after(f32 tick) const;

    std::vector<Snapshot> snapshots_; ///< sorted by tick ascending, oldest first
    usize capacity_ = 32;
};

} // namespace t2d
