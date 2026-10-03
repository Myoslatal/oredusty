// Tile2D - client side prediction: run the local player ahead of the server and replay the
// unacknowledged commands when a snapshot arrives.
//
// The rule that makes this exact (and testable): the simulation is deterministic, so replaying
// commands [acked + 1 .. newest] on top of the authoritative state must reproduce the state the
// server ends up with. Any difference is a genuine bug or a desync, and the tests assert there is
// none.
#pragma once

#include <t2d/core/types.h>
#include <t2d/sim/command.h>
#include <t2d/sim/world.h>

#include <vector>

namespace t2d {

class Predictor {
public:
    /// Number of commands kept for replay (one second at 60 Hz is plenty, and the ring is bounded so
    /// a stalled connection cannot grow memory without limit).
    explicit Predictor(usize capacity = 256);

    /// Remembers a command that was applied locally (in prediction) and sent to the server.
    void record(const PlayerCommand& command);

    /// Drops every recorded command at or before \p tick.
    void acknowledge(u32 tick);
    void clear();

    [[nodiscard]] usize pending_count() const { return pending_.size(); }
    [[nodiscard]] u32 oldest_pending_tick() const;
    [[nodiscard]] u32 newest_pending_tick() const;
    [[nodiscard]] u64 dropped() const { return dropped_; }
    [[nodiscard]] ConstSpan<PlayerCommand> pending() const { return pending_; }

    /// Rewinds the local player of \p world to \p authoritative and replays every command newer
    /// than \p acked_tick. Returns the number of replayed commands; \p out_error receives the
    /// distance between the prediction and the server state before the replay.
    u32 reconcile(World& world, PlayerId player_id, const PlayerState& authoritative, u32 acked_tick,
                  f32& out_error);

    struct Stats {
        u64 reconciliations = 0;
        u64 corrections = 0;
        f32 last_error = 0.0f;
        f32 max_error = 0.0f;
        f32 average_error = 0.0f;
        u32 last_replayed = 0;
    };
    [[nodiscard]] const Stats& stats() const { return stats_; }

private:
    std::vector<PlayerCommand> pending_;
    usize capacity_ = 256;
    u64 dropped_ = 0;
    Stats stats_{};
};

} // namespace t2d
