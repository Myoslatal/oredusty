// Tile2D - KCP over UDP: the link used for every player that is not on this machine.
//
// One UdpTransport owns the socket (the server multiplexes all players over a single port, the
// client owns one socket for its server) and dispatches datagrams to one UdpLink per peer. A link
// is a KCP conversation: reliable, in order, message oriented, with the latency of UDP.
#pragma once

#include <t2d/core/types.h>
#include <t2d/net/kcp.h>
#include <t2d/net/link.h>
#include <t2d/net/socket.h>

#include <vector>

namespace t2d::net {

struct UdpLinkConfig {
    KcpConfig kcp{};                 ///< conv is assigned by the transport
    u32 timeout_ms = 8000;           ///< no valid datagram for this long closes the link
    u32 handshake_timeout_ms = 5000; ///< a link that never becomes usable is dropped
    bool send_keepalive_pings = true;
};

class UdpTransport;

class UdpLink final : public ILink {
public:
    ~UdpLink() override;

    [[nodiscard]] LinkState state() const override;
    [[nodiscard]] bool send(ConstSpan<const u8> message) override;
    [[nodiscard]] usize receive(Span<u8> destination) override;
    void update(u64 now_ms) override;
    [[nodiscard]] const LinkStats& stats() const override;
    void close() override;
    [[nodiscard]] bool wait_for_data(u32 timeout_ms) override;
    [[nodiscard]] const char* kind() const override { return "kcp"; }

    [[nodiscard]] const Endpoint& peer() const { return peer_; }
    [[nodiscard]] u32 conv() const;
    /// True when no valid datagram arrived within the configured timeout.
    [[nodiscard]] bool timed_out() const { return timed_out_; }
    [[nodiscard]] bool dead() const;
    [[nodiscard]] u64 last_packet_ms() const { return last_packet_ms_; }
    /// Transport level liveness: the peer sent us something that KCP accepted.
    [[nodiscard]] bool saw_traffic() const { return last_packet_ms_ != 0; }
    [[nodiscard]] Kcp& kcp() { return *kcp_; }
    void mark_state(LinkState state) { state_ = state; }

private:
    friend class UdpTransport;
    UdpLink() = default;

    UdpTransport* transport_ = nullptr;
    Endpoint peer_{};
    Scope<Kcp> kcp_;
    UdpLinkConfig config_{};
    LinkState state_ = LinkState::Connecting;
    /// Refreshed from the KCP statistics on every stats() call, hence mutable.
    mutable LinkStats stats_{};
    u64 created_ms_ = 0;
    u64 last_packet_ms_ = 0;
    bool timed_out_ = false;
};

class UdpTransport {
public:
    struct Config {
        u16 port = 0;                ///< 0 = ephemeral port (client)
        bool server_mode = false;    ///< accept links from unknown peers
        u32 max_links = 32;
        UdpLinkConfig link{};
        /// Datagrams processed per update() before returning to the caller (backpressure).
        u32 max_packets_per_update = 512;
    };

    [[nodiscard]] static Scope<UdpTransport> create(const Config& config);
    ~UdpTransport();
    T2D_NON_MOVABLE(UdpTransport);

    /// Receives everything that arrived, dispatches by peer, then pumps every link's KCP timers.
    /// Re-entrant calls (a link's update pumping its own transport) return immediately.
    void update(u64 now_ms);
    /// Creates a link to \p peer (client side). \p conv of 0 picks the next free one.
    UdpLink* connect(const Endpoint& peer, u32 conv = 0);
    [[nodiscard]] ConstSpan<UdpLink*> links() const;
    [[nodiscard]] usize link_count() const { return links_.size(); }
    [[nodiscard]] UdpLink* find(const Endpoint& peer) const;
    /// Destroys links that timed out or hit the KCP dead link threshold; returns their endpoints.
    [[nodiscard]] std::vector<Endpoint> collect_dead_links();
    void remove(UdpLink* link);

    [[nodiscard]] u16 port() const;
    [[nodiscard]] bool server_mode() const { return config_.server_mode; }
    [[nodiscard]] bool wait_readable(u32 timeout_ms);
    [[nodiscard]] UdpSocket& socket() { return *socket_; }

    struct Stats {
        u64 packets_received = 0;
        u64 packets_sent = 0;
        u64 packets_dropped = 0;   ///< send failures or datagrams rejected before reaching a link
        u64 unknown_peers = 0;     ///< datagrams from peers we have no link for (client mode)
        u64 bytes_received = 0;
        u64 bytes_sent = 0;
    };
    [[nodiscard]] const Stats& stats() const { return stats_; }

private:
    friend class UdpLink;
    void send_raw(const Endpoint& peer, ConstSpan<const u8> data);
    UdpLink* create_link(const Endpoint& peer, u32 conv);

    UdpTransport() = default;
    Scope<UdpSocket> socket_;
    Config config_{};
    std::vector<Scope<UdpLink>> links_;
    Stats stats_{};
    u32 next_conv_ = 0x1000;
    /// Guards update(): links pump their transport, and the transport updates its links.
    bool pumping_ = false;
    std::vector<u8> receive_buffer_;
};

} // namespace t2d::net
