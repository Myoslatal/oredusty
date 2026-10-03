// Tile2D - the transport abstraction every client/server path goes through.
//
// Two implementations exist and they are interchangeable at runtime:
//   * SharedLink  - two endpoints of an mmap(MAP_SHARED) region filled with lock free SPSC rings.
//                   Used between the server thread and the local client thread in the same process
//                   (single player, and the local player in a hosted game). No serialisation
//                   syscalls, no copies beyond the ring, no packets on the wire.
//   * UdpLink     - KCP over a UDP socket, used for everybody who is not on this machine.
//
// Both carry the exact same framed messages, so the protocol layer above never knows which one it
// is talking to - that is what keeps the "semi separated" design honest: the local player is not a
// special case in the game logic, only in the transport.
#pragma once

#include <t2d/core/types.h>

namespace t2d::net {

/// Largest message the framework can carry in one piece (snapshots, map chunks, ...).
inline constexpr usize kMaxMessageSize = 16 * 1024;

enum class LinkState : u8 {
    Disconnected = 0, ///< never connected, or closed
    Connecting,       ///< handshake in flight
    Connected,        ///< usable
    Closed,           ///< closed by either side
};

[[nodiscard]] const char* link_state_name(LinkState state);

struct LinkStats {
    u64 messages_sent = 0;
    u64 messages_received = 0;
    u64 bytes_sent = 0;
    u64 bytes_received = 0;
    u64 packets_sent = 0;
    u64 packets_received = 0;
    u64 packets_dropped = 0;   ///< send failures + packets the peer never acked (transport dependent)
    u64 messages_dropped = 0;  ///< messages the local side had to drop (queue full / closed link)
    u32 rtt_ms = 0;
    u32 send_queue_messages = 0;
    u32 receive_queue_messages = 0;
    u32 pending_bytes = 0;     ///< bytes queued for transmission
    f32 packet_loss = 0.0f;    ///< 0..1 estimate, only meaningful for KCP links
};

class ILink {
public:
    virtual ~ILink() = default;

    [[nodiscard]] virtual LinkState state() const = 0;
    /// Queues one whole message. False when the link is closed or its queue is full.
    [[nodiscard]] virtual bool send(ConstSpan<const u8> message) = 0;
    /// Pops one whole message. Returns 0 when nothing is ready. \p destination must be at least
    /// kMaxMessageSize bytes.
    [[nodiscard]] virtual usize receive(Span<u8> destination) = 0;
    /// Pumps timers and I/O. Call once per frame/tick.
    virtual void update(u64 now_ms) = 0;
    [[nodiscard]] virtual const LinkStats& stats() const = 0;
    virtual void close() = 0;
    /// Blocks until a message arrives or \p timeout_ms expires. Returns true when data is ready.
    /// Used by the client thread so it does not spin while waiting for the server.
    virtual bool wait_for_data(u32 timeout_ms) = 0;
    /// "shared-memory" or "kcp" - for logs and the debug HUD.
    [[nodiscard]] virtual const char* kind() const = 0;
};

} // namespace t2d::net
