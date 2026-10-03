// Tile2D - the shared memory link between the server thread and the local client thread.
//
// The channel is a real shared mapping (mmap(MAP_SHARED|MAP_ANONYMOUS), or a file under /dev/shm
// when a backing file is given) carved into two single-producer/single-consumer rings, one per
// direction, plus a small statistics block both sides may read. No locks, no syscalls per message,
// no serialisation: the bytes written by the producer are handed to the consumer as-is, which is
// exactly what the protocol layer above expects from any link.
#pragma once

#include <t2d/core/types.h>
#include <t2d/net/link.h>

#include <memory>
#include <string>

namespace t2d::net {

struct SharedLinkOptions {
    u32 slot_count = 128;              ///< messages that fit in one direction
    u32 slot_size = kMaxMessageSize;   ///< bytes per message
    /// Empty: anonymous shared mapping (both endpoints must live in this process).
    /// Non empty: a file under /dev/shm, which makes the very same channel usable across processes.
    std::string backing_file;
    /// Use C++20 atomic wait/notify for wait_for_data() instead of polling.
    bool use_futex_wait = true;
};

class SharedLink;

struct SharedLinkPair {
    Scope<SharedLink> server;  ///< handed to the server thread
    Scope<SharedLink> client;  ///< handed to the client thread
};

/// One endpoint of the channel. Each endpoint is single threaded (one producer, one consumer),
/// which is what makes the lock free rings correct without any further synchronisation.
class SharedLink final : public ILink {
public:
    ~SharedLink() override;

    [[nodiscard]] LinkState state() const override;
    [[nodiscard]] bool send(ConstSpan<const u8> message) override;
    [[nodiscard]] usize receive(Span<u8> destination) override;
    void update(u64 now_ms) override;
    [[nodiscard]] const LinkStats& stats() const override;
    void close() override;
    [[nodiscard]] bool wait_for_data(u32 timeout_ms) override;
    [[nodiscard]] const char* kind() const override { return "shared-memory"; }

    [[nodiscard]] bool is_server_endpoint() const;
    [[nodiscard]] usize shared_bytes() const;
    /// Messages currently queued in each direction.
    [[nodiscard]] u32 queued_outgoing() const;
    [[nodiscard]] u32 queued_incoming() const;
    /// Highest number of queued outgoing messages seen so far (tests use it to prove the ring wraps).
    [[nodiscard]] u32 outgoing_high_water() const;
    /// Counters published by the other side, readable without sending anything.
    [[nodiscard]] u64 peer_messages_sent() const;
    [[nodiscard]] u32 peer_queue_depth() const;
    /// Drops every queued message (used when a local session ends and the channel is reused).
    void reset_queues();

private:
    friend SharedLinkPair create_shared_link_pair(const SharedLinkOptions& options);
    SharedLink() = default;
    struct Impl;
    Scope<Impl> impl_;
};

/// Creates both endpoints of one channel. Throws nothing: returns empty scopes when the mapping
/// could not be created (the caller reports the error).
[[nodiscard]] SharedLinkPair create_shared_link_pair(const SharedLinkOptions& options = {});

} // namespace t2d::net
