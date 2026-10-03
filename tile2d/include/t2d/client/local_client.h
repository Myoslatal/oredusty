// Tile2D - the local client: input sampling, prediction, reconciliation and interpolation.
//
// It does not know (or care) whether its link is the shared memory channel to the server thread in
// this process or a KCP connection to somebody else's server: both satisfy ILink, so the code path is
// identical. That is how single player and online play stay the same program.
#pragma once

#include <t2d/client/predictor.h>
#include <t2d/client/snapshot_buffer.h>
#include <t2d/core/types.h>
#include <t2d/net/link.h>
#include <t2d/net/protocol.h>
#include <t2d/sim/world.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace t2d {

struct ClientConfig {
    std::string player_name = "player";
    /// How far behind the server remote entities are rendered (hides jitter, costs latency).
    f32 interpolation_delay_ms = 100.0f;
    usize snapshot_history = 32;
    /// How many reconstructed states the client keeps so a delta can be applied against the tick the
    /// server referenced. The server may acknowledge an older snapshot than the newest one it sent
    /// (an ack is a round trip behind), so keeping only the newest state drops those deltas.
    usize baseline_history = 64;
    bool enable_prediction = true;
    u32 ping_interval_ms = 500;
    /// How many ticks ahead of the server's newest snapshot the client aims to be.
    u32 command_lead_ticks = 2;
};

/// One entity as it should be drawn.
struct RenderPlayer {
    PlayerId id = kInvalidId;
    Vec2 position{};
    Vec2 velocity{};
    u8 flags = 0;
    u16 coins = 0;
    bool local = false;
    std::string name;
};

struct ClientStats {
    u32 server_tick = 0;
    u32 predicted_tick = 0;
    u32 rtt_ms = 0;
    u32 jitter_ms = 0;
    u32 snapshots_received = 0;
    u32 full_snapshots_received = 0;
    u32 delta_snapshots_received = 0;
    u32 snapshots_dropped = 0;
    u32 desyncs = 0;
    u32 mispredictions = 0;
    f32 prediction_error = 0.0f;
    f32 max_prediction_error = 0.0f;
    u32 pending_commands = 0;
    u64 messages_sent = 0;
    u64 messages_received = 0;
    u64 bytes_sent = 0;
    u64 bytes_received = 0;
    u32 map_chunks_received = 0;
    u32 map_chunks_expected = 0;
    f32 interpolation_delay_ms = 0.0f;
    u32 players = 0;
    std::string_view link_kind = "none";
    f64 level_load_ms = 0.0;
};

class LocalClient {
public:
    /// The link is owned by the caller (the process owns the shared memory endpoint or the UDP
    /// transport); the client only uses it.
    [[nodiscard]] static Scope<LocalClient> create(const ClientConfig& config, net::ILink* link);
    ~LocalClient();
    T2D_NON_MOVABLE(LocalClient);

    /// Sends the handshake. Called once before the first update().
    void start(u64 now_ms);

    /// Pumps the link, sends one command per simulation tick and advances prediction/interpolation.
    /// \p command carries the input sampled for this frame (the tick field is filled in here).
    void update(u64 now_ms, const PlayerCommand& command);

    [[nodiscard]] bool link_ready() const { return link_ != nullptr; }
    [[nodiscard]] bool ready() const { return ready_; }           ///< level loaded, snapshots flowing
    [[nodiscard]] bool connected() const;
    [[nodiscard]] PlayerId local_player_id() const { return local_player_id_; }

    /// Predicted world (the local player is ahead of the server, everything else is authoritative).
    [[nodiscard]] World* world() { return world_.get(); }
    [[nodiscard]] const World* world() const { return world_.get(); }
    [[nodiscard]] const TileMap* map() const { return world_ != nullptr ? &world_->map() : nullptr; }
    [[nodiscard]] const Tileset* tileset() const { return world_ != nullptr ? &world_->tileset() : nullptr; }

    /// Entities to draw: remote players interpolated, the local player predicted.
    [[nodiscard]] const std::vector<RenderPlayer>& render_players() const { return render_players_; }
    /// Tick the remote players are being interpolated at (for debugging).
    [[nodiscard]] f32 render_tick() const { return render_tick_; }

    [[nodiscard]] const ClientStats& stats() const { return stats_; }
    [[nodiscard]] const Predictor::Stats& prediction_stats() const { return predictor_.stats(); }
    [[nodiscard]] const std::string& status() const { return status_; }
    [[nodiscard]] const std::string& server_name() const { return server_name_; }
    /// Names of the players known to this client, keyed by player id.
    [[nodiscard]] const std::unordered_map<PlayerId, std::string>& player_names() const { return player_names_; }

    /// Sends a Disconnect message and closes the link.
    void disconnect();

private:
    LocalClient() = default;

    void pump_link(u64 now_ms);
    void handle_message(net::MessageType type, ConstSpan<const u8> payload, u64 now_ms);
    void handle_welcome(ConstSpan<const u8> payload);
    void handle_map_data(ConstSpan<const u8> payload, u64 now_ms);
    void handle_snapshot(ConstSpan<const u8> payload, u64 now_ms);
    void try_finish_loading(u64 now_ms);
    void send_command(u32 tick, const PlayerCommand& command);
    void send_ping(u64 now_ms);
    void update_render_players(u64 now_ms);
    /// The reconstructed state with this tick, or nullptr when it has already been forgotten.
    [[nodiscard]] const Snapshot* find_baseline(u32 tick) const;
    /// Keeps the reconstructed states a delta may reference, oldest first.
    void remember_baseline(const Snapshot& snapshot);
    void request_full_snapshot();

    ClientConfig config_{};
    net::ILink* link_ = nullptr;
    Scope<World> world_;
    Predictor predictor_;
    SnapshotBuffer snapshot_buffer_;
    Snapshot baseline_;                       ///< newest fully reconstructed server state
    std::vector<Snapshot> baseline_history_;  ///< recent states, keyed by tick, for delta resolution
    std::vector<u8> receive_buffer_;
    std::vector<std::vector<u8>> map_chunks_;
    u32 map_chunks_expected_ = 0;
    u32 map_chunks_received_ = 0;
    u64 load_start_ms_ = 0;
    u64 last_snapshot_ms_ = 0;
    u64 last_ping_ms_ = 0;
    u32 ping_sequence_ = 0;
    u32 next_command_tick_ = 1;
    u32 last_snapshot_tick_ = 0;
    /// Ticks per second, announced by the server's welcome message (kTickRate until it arrives).
    u32 tick_rate_ = kTickRate;
    PlayerId local_player_id_ = kInvalidId;
    bool ready_ = false;
    bool handshake_sent_ = false;
    bool rejected_ = false;
    std::string status_ = "idle";
    std::string server_name_ = "server";
    std::string player_name_;
    std::string link_kind_storage_ = "none";
    std::unordered_map<PlayerId, std::string> player_names_;
    std::vector<RenderPlayer> render_players_;
    f32 render_tick_ = 0.0f;
    ClientStats stats_{};
    f32 accumulated_seconds_ = 0.0f;
    u64 last_update_ms_ = 0;
    f32 tick_remainder_ = 0.0f;
};

} // namespace t2d
