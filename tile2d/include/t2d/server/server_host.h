// Tile2D - the authoritative server, running in its own thread.
//
// It owns the simulation and talks to two kinds of links through the same ILink interface:
//   * the local client thread, over the shared memory channel (single player, and the host's own
//     player in an online game),
//   * remote players, over KCP/UDP.
// The dedicated server mode simply does not create the local link and never starts a client thread,
// which is why the server binary links no graphics code at all.
#pragma once

#include <t2d/core/types.h>
#include <t2d/net/protocol.h>
#include <t2d/net/shm_link.h>
#include <t2d/net/udp_link.h>
#include <t2d/sim/world.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace t2d {

struct ServerConfig {
    WorldConfig world{};
    /// UDP port to listen on. 0 disables networking entirely (pure single player).
    u16 port = 0;
    u32 max_players = 8;
    /// Snapshot every N ticks (2 => 30 Hz with a 60 Hz simulation).
    u32 snapshot_interval_ticks = 2;
    /// A full snapshot is sent this often even when deltas would do (recovery from packet loss).
    u32 full_snapshot_interval_ticks = 120;
    /// How many past snapshots are kept so deltas can reference a client's acknowledged tick.
    u32 baseline_history = 64;
    /// Create the shared memory link for a local client thread.
    bool local_client = true;
    std::string name = "Tile2D server";
};

struct ServerPlayerInfo {
    PlayerId id = kInvalidId;
    std::string name;
    bool local = false;
    bool remote = false;
    bool ready = false; ///< handshake finished and the level was sent
    u32 rtt_ms = 0;
    u32 acked_snapshot_tick = 0;
};

struct ServerStats {
    u32 tick = 0;
    f32 last_tick_ms = 0.0f;
    f32 average_tick_ms = 0.0f;
    f32 ticks_per_second = 0.0f;
    u32 players = 0;
    u32 local_players = 0;
    u32 remote_players = 0;
    u64 ticks = 0;
    u64 catch_up_ticks = 0;
    u64 snapshots_sent = 0;
    u64 delta_snapshots_sent = 0;
    u64 full_snapshots_sent = 0;
    u64 commands_received = 0;
    u64 messages_dropped = 0;
    u32 last_snapshot_bytes = 0;
    f64 bytes_sent_per_second = 0.0;
    f64 bytes_received_per_second = 0.0;
};

class ServerHost {
public:
    [[nodiscard]] static Scope<ServerHost> create(const ServerConfig& config);
    ~ServerHost();
    T2D_NON_MOVABLE(ServerHost);

    /// Spawns the simulation thread.
    void start();
    /// Signals the thread and joins it (safe to call twice).
    void stop();
    [[nodiscard]] bool running() const { return running_.load(std::memory_order_acquire); }

    /// The channel endpoint the server thread uses (nullptr in dedicated mode).
    [[nodiscard]] net::SharedLink* local_link() const { return local_link_.get(); }
    /// Transfers ownership of the *client* end of the shared channel to the caller (once). The app
    /// passes it to its LocalClient; the server keeps its own end. Returns nullptr when there is no
    /// local client or when it was already taken.
    [[nodiscard]] net::SharedLink* take_local_client_link();
    [[nodiscard]] u16 port() const;
    [[nodiscard]] const ServerConfig& config() const { return config_; }

    [[nodiscard]] ServerStats stats() const;
    [[nodiscard]] std::vector<ServerPlayerInfo> players() const;

    /// Runs exactly one tick on the calling thread. Used by tests that drive the server by hand;
    /// must not be called while the thread is running.
    void tick_once(u64 now_ms);

    /// Direct access for tests (never touch this while the server thread runs).
    [[nodiscard]] World& world() { return *world_; }
    [[nodiscard]] const World& world() const { return *world_; }

private:
    ServerHost() = default;

    struct Session {
        net::ILink* link = nullptr;
        bool local = false;
        PlayerId player_id = kInvalidId;
        std::string name;
        bool ready = false;
        u32 acked_snapshot_tick = 0;
        u32 last_sent_snapshot_tick = 0;
        u32 rtt_ms = 0;
        u32 ping_sequence = 0;
        u64 last_ping_ms = 0;
        u64 bytes_sent = 0;
        u64 commands_received = 0;
        bool wants_full_snapshot = true;
    };

    void run();
    void poll_local(u64 now_ms);
    void poll_remotes(u64 now_ms);
    void handle_message(Session& session, net::MessageType type, ConstSpan<const u8> payload, u64 now_ms);
    void handle_hello(Session& session, ConstSpan<const u8> payload, u64 now_ms);
    void accept_player(Session& session, std::string_view name, u64 now_ms);
    void drop_session(Session& session, u8 reason);
    void send_map(Session& session);
    void send_snapshot(Session& session, u64 now_ms);
    void broadcast(net::MessageType type, ConstSpan<const u8> payload, net::ILink* except = nullptr);
    void deliver(Session& session, net::MessageType type, ConstSpan<const u8> payload);
    void update_stats(u64 now_ms);
    [[nodiscard]] Session* find_session(net::ILink* link);
    [[nodiscard]] const Snapshot* find_baseline(u32 tick) const;
    [[nodiscard]] Session* find_session_for(PlayerId id);

    ServerConfig config_{};
    Scope<World> world_;
    Scope<net::SharedLink> local_link_;        ///< server end of the shared channel
    Scope<net::SharedLink> local_client_link_; ///< client end, handed over by take_local_client_link()
    Scope<net::UdpTransport> transport_;
    std::vector<Scope<Session>> sessions_;
    std::vector<Snapshot> history_;   ///< recent full snapshots, used as delta baselines
    std::vector<u8> receive_buffer_;
    std::vector<u8> encoded_map_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    mutable std::mutex stats_mutex_;
    ServerStats stats_{};
    f64 bytes_sent_window_ = 0.0;
    f64 bytes_received_window_ = 0.0;
    u64 window_start_ms_ = 0;
    f32 average_tick_ms_ = 0.0f;
};

} // namespace t2d
