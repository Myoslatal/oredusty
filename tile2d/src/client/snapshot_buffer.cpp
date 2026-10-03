#include <t2d/client/snapshot_buffer.h>

#include <t2d/core/log.h>

#include <algorithm>

namespace t2d {

SnapshotBuffer::SnapshotBuffer(usize capacity) : capacity_(capacity < 2 ? 2 : capacity) {}

void SnapshotBuffer::clear() { snapshots_.clear(); }

void SnapshotBuffer::push(Snapshot snapshot) {
    if (snapshot.kind != SnapshotKind::Full) {
        T2D_WARN("snapshot buffer: refusing a delta snapshot (resolve deltas before buffering)");
        return;
    }
    if (!snapshots_.empty() && snapshot.tick <= snapshots_.back().tick) {
        // Duplicate or out of order: the newest state stays, the interpolation window is short
        // enough that dropping a late snapshot is harmless.
        return;
    }
    snapshots_.push_back(std::move(snapshot));
    while (snapshots_.size() > capacity_) snapshots_.erase(snapshots_.begin());
}

const Snapshot* SnapshotBuffer::newest() const { return snapshots_.empty() ? nullptr : &snapshots_.back(); }

const Snapshot* SnapshotBuffer::oldest() const { return snapshots_.empty() ? nullptr : &snapshots_.front(); }

const Snapshot* SnapshotBuffer::find(u32 tick) const {
    for (const Snapshot& snapshot : snapshots_) {
        if (snapshot.tick == tick) return &snapshot;
    }
    return nullptr;
}

u32 SnapshotBuffer::newest_tick() const { return snapshots_.empty() ? 0u : snapshots_.back().tick; }

u32 SnapshotBuffer::oldest_tick() const { return snapshots_.empty() ? 0u : snapshots_.front().tick; }

const Snapshot* SnapshotBuffer::newest_not_after(f32 tick) const {
    const Snapshot* candidate = nullptr;
    for (const Snapshot& snapshot : snapshots_) {
        if (static_cast<f32>(snapshot.tick) <= tick) candidate = &snapshot;
        else break;
    }
    return candidate;
}

bool SnapshotBuffer::sample(f32 tick, PlayerId id, PlayerState& out) const {
    if (snapshots_.empty()) return false;

    const Snapshot* before = newest_not_after(tick);
    if (before == nullptr) {
        // Older than everything buffered: clamp to the oldest state instead of extrapolating.
        const PlayerState* player = oldest()->find_player(id);
        if (player == nullptr) return false;
        out = *player;
        return true;
    }

    const Snapshot* after = nullptr;
    for (const Snapshot& snapshot : snapshots_) {
        if (snapshot.tick > before->tick) {
            after = &snapshot;
            break;
        }
    }

    const PlayerState* state_before = before->find_player(id);
    if (state_before == nullptr) return false;
    if (after == nullptr || after->tick == before->tick) {
        out = *state_before;
        return true;
    }
    const PlayerState* state_after = after->find_player(id);
    if (state_after == nullptr) {
        out = *state_before;
        return true;
    }

    const f32 span = static_cast<f32>(after->tick - before->tick);
    const f32 alpha = clamp((tick - static_cast<f32>(before->tick)) / span, 0.0f, 1.0f);
    out = *state_after;
    out.position = lerp(state_before->position, state_after->position, alpha);
    out.velocity = lerp(state_before->velocity, state_after->velocity, alpha);
    // Discrete fields snap to the newer snapshot: interpolating a flag or a counter is meaningless.
    out.flags = state_after->flags;
    out.coins = state_after->coins;
    out.id = state_after->id;
    return true;
}

void SnapshotBuffer::sample_all(f32 tick, std::vector<PlayerState>& out) const {
    out.clear();
    if (snapshots_.empty()) return;
    const Snapshot* snapshot = newest();
    out.reserve(snapshot->players.size());
    for (const PlayerState& player : snapshot->players) {
        PlayerState interpolated;
        if (sample(tick, player.id, interpolated)) out.push_back(interpolated);
    }
}

} // namespace t2d
