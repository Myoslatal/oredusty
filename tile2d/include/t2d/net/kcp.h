// Tile2D - KCP: an ARQ (automatic repeat request) protocol over UDP, wire compatible with the
// original ikcp implementation (same 24 byte segment header, same commands and semantics).
//
// The framework uses it for the link between remote players and the server: messages are delivered
// reliably, in order and without duplicates, while keeping the latency of a datagram protocol.
// Local clients do not use it at all - they talk to the server thread over shared memory.
//
// Usage:
//     Kcp kcp(conv, config);
//     kcp.set_output([&](ConstSpan<const u8> packet) { socket.send_to(peer, packet); });
//     kcp.send(payload);                        // queue a message
//     kcp.input(received_packet);               // feed everything that arrives
//     kcp.update(now_ms());                     // every frame (or use check() to sleep precisely)
//     while (kcp.receive(buffer) > 0) { ... }   // drain complete messages
#pragma once

#include <t2d/core/types.h>

#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace t2d::net {

/// Segment commands, identical to the ikcp wire format.
enum class KcpCommand : u8 { Push = 81, Ack = 82, Wask = 83, Wins = 84 };

inline constexpr usize kKcpHeaderSize = 24;

struct KcpConfig {
    u32 conv = 0;                     ///< connection id, must match on both ends
    u32 mtu = 1400;                   ///< max packet size including the 24 byte header
    u16 send_window = 256;            ///< segments in flight (send window)
    u16 receive_window = 256;         ///< receive window advertised to the peer
    u32 min_rto_ms = 30;              ///< lower bound for the retransmission timeout
    u32 interval_ms = 10;             ///< internal flush/update interval
    bool no_delay = false;            ///< disable the delayed-ACK/Nagle-ish behaviour
    u32 fast_resend_threshold = 2;    ///< resend a segment after N later ACKs (0 = off)
    bool no_congestion_control = false; ///< keep the congestion window at the send window
    bool stream_mode = false;         ///< false: preserve message boundaries (game friendly)
    u32 dead_link_threshold = 20;     ///< retransmissions before dead_link() turns true
};

struct KcpStats {
    u64 segments_sent = 0;
    u64 segments_received = 0;
    u64 retransmissions = 0;
    u64 fast_retransmissions = 0;
    u64 duplicate_acks = 0;
    u64 input_packets = 0;
    u64 output_packets = 0;
    u64 output_bytes = 0;
    u64 dropped_malformed = 0;
    u32 rtt_ms = 0;
    u32 rto_ms = 0;
    u32 cwnd = 0;
    u32 send_window = 0;
    u32 receive_window = 0;
    u32 remote_window = 0;
    u32 wait_send_segments = 0;
    u32 queued_receive_bytes = 0;
    u32 send_queue_segments = 0;
    u32 receive_queue_segments = 0;
    u32 xmit = 0;                     ///< retransmission counter of the oldest unacked segment
};

class Kcp {
public:
    /// Called for every packet that must go on the wire.
    using OutputFn = std::function<void(ConstSpan<const u8> packet)>;

    explicit Kcp(u32 conv, const KcpConfig& config = {});
    Kcp(const KcpConfig& config, u32 conv);
    ~Kcp();
    T2D_NON_COPYABLE(Kcp);
    Kcp(Kcp&&) noexcept;
    Kcp& operator=(Kcp&&) noexcept;

    void set_output(OutputFn output);
    [[nodiscard]] u32 conv() const;

    /// Queues a message (message mode) or stream bytes. Returns false when the data is larger than
    /// the configured maximum message size (255 fragments) or when the queue is full.
    bool send(ConstSpan<const u8> data);

    /// Consumes one datagram received from the network. Malformed input is rejected (returns false)
    /// and counted in stats().dropped_malformed; valid packets also produce the ACKs to send.
    bool input(ConstSpan<const u8> packet);

    /// Drives retransmission and ACK timers. \p now_ms is a monotonic millisecond clock and must
    /// never go backwards (KCP uses it for RTT estimation).
    void update(u64 now_ms);

    /// Next timestamp at which update() should be called; useful to sleep instead of spinning.
    [[nodiscard]] u64 check(u64 now_ms) const;

    /// Pops one complete message into \p destination. Returns the number of bytes written, or 0
    /// when nothing is ready (or the destination is too small - peek_size() to size it).
    usize receive(Span<u8> destination);
    /// Size of the next complete message, or 0 when nothing is ready.
    [[nodiscard]] usize peek_size() const;

    /// Total bytes waiting in the receive queue.
    [[nodiscard]] usize queued_receive_bytes() const;
    /// Segments queued for sending (including unacked ones).
    [[nodiscard]] usize wait_send_segments() const;
    /// True once a segment has been retransmitted more than dead_link_threshold times.
    [[nodiscard]] bool dead_link() const;

    void set_mtu(u32 mtu);
    void set_window_size(u16 send, u16 receive);
    void set_nodelay(bool no_delay, u32 interval_ms, u32 fast_resend_threshold, bool no_congestion_control);
    [[nodiscard]] const KcpConfig& config() const;
    [[nodiscard]] const KcpStats& stats() const;

    /// Flushes pending segments immediately (normally driven by update()).
    void flush();
    /// Drops every queued segment and message (used when a connection is torn down).
    void reset();

    /// Maximum payload of one message: (mtu - header) * 255 fragments.
    [[nodiscard]] usize max_message_size() const;

private:
    struct Impl;
    Scope<Impl> impl_;
};

} // namespace t2d::net
