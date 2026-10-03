#include <t2d/client/predictor.h>

#include <t2d/core/log.h>

#include <algorithm>

namespace t2d {

Predictor::Predictor(usize capacity) : capacity_(capacity < 8 ? 8 : capacity) {}

void Predictor::record(const PlayerCommand& command) {
    if (!pending_.empty() && command.tick <= pending_.back().tick) return; // stale or duplicate
    if (pending_.size() >= capacity_) {
        pending_.erase(pending_.begin());
        ++dropped_;
    }
    pending_.push_back(command);
}

void Predictor::acknowledge(u32 tick) {
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [tick](const PlayerCommand& command) { return command.tick <= tick; }),
                   pending_.end());
}

void Predictor::clear() { pending_.clear(); }

u32 Predictor::oldest_pending_tick() const { return pending_.empty() ? 0u : pending_.front().tick; }
u32 Predictor::newest_pending_tick() const { return pending_.empty() ? 0u : pending_.back().tick; }

u32 Predictor::reconcile(World& world, PlayerId player_id, const PlayerState& authoritative, u32 acked_tick,
                         f32& out_error) {
    PlayerState* local = world.find_player(player_id);
    out_error = 0.0f;
    if (local == nullptr) {
        world.apply_player_state(player_id, authoritative);
        return 0;
    }

    // How far was our prediction from the server for the acknowledged tick? (0 means perfect.)
    if (local->last_command_tick >= acked_tick) {
        out_error = length(local->position - authoritative.position);
    }

    // Rewind to the authoritative state, then replay everything the server has not consumed yet.
    const Vec2 spawn = local->spawn_position;
    *local = authoritative;
    if (authoritative.spawn_position == Vec2{}) local->spawn_position = spawn;

    u32 replayed = 0;
    for (const PlayerCommand& command : pending_) {
        if (command.tick <= acked_tick) continue;
        (void)world.predict_player(player_id, command);
        ++replayed;
    }

    ++stats_.reconciliations;
    stats_.last_replayed = replayed;
    stats_.last_error = out_error;
    stats_.max_error = std::max(stats_.max_error, out_error);
    const f32 weight = 1.0f / static_cast<f32>(stats_.reconciliations);
    stats_.average_error += (out_error - stats_.average_error) * weight;
    if (out_error > 0.0001f) ++stats_.corrections;
    return replayed;
}

} // namespace t2d
