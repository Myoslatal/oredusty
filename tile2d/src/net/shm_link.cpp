#include <t2d/net/shm_link.h>

#include <t2d/core/log.h>
#include <t2d/core/time.h>

#include <atomic>
#include <chrono>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace t2d::net {
namespace {

constexpr u32 kSharedMagic = 0x324C3254u; ///< "T2L2"
constexpr u32 kSharedVersion = 1;
constexpr usize kCacheLine = 64;

struct ChannelHeader {
    std::atomic<u64> write_seq{0};  ///< published by the producer, consumed by the reader
    std::atomic<u64> read_seq{0};   ///< published by the consumer
    u64 messages = 0;               ///< producer counters (only the producer writes them)
    u64 bytes = 0;
    u64 dropped = 0;
    u64 high_water = 0;
};

struct PeerStats {
    std::atomic<u64> messages{0};
    std::atomic<u64> bytes{0};
    std::atomic<u32> queue{0};
};

struct SharedHeader {
    u32 magic = kSharedMagic;
    u32 version = kSharedVersion;
    u32 slot_count = 0;
    u32 slot_size = 0;
    u64 stride = 0;
    /// [0] = server -> client, [1] = client -> server.
    ChannelHeader channels[2];
    /// Filled in by each side so the peer can show queue depths without messaging.
    PeerStats published[2];
};

struct SharedRegion {
    void* base = nullptr;
    usize size = 0;
    bool unlink_file = false;
    std::string path;

    ~SharedRegion() {
        if (base != nullptr && base != MAP_FAILED) munmap(base, size);
        if (unlink_file && !path.empty()) ::unlink(path.c_str());
    }
};

[[nodiscard]] constexpr usize channel_offset(const SharedHeader& header, u32 channel) {
    return align_up(sizeof(SharedHeader), kCacheLine) + channel * header.slot_count * header.stride;
}

[[nodiscard]] u8* slot_pointer(SharedHeader* header, u32 channel, u64 sequence) {
    const u64 index = sequence % header->slot_count;
    return reinterpret_cast<u8*>(header) + channel_offset(*header, channel) + index * header->stride;
}

} // namespace

struct SharedLink::Impl {
    Ref<SharedRegion> region;
    SharedHeader* header = nullptr;
    bool server_endpoint = false;
    u32 outbound_channel = 0; ///< channel this endpoint writes to
    u32 inbound_channel = 1;
    LinkState state = LinkState::Connected;
    LinkStats stats{};
    bool futex_wait = true;

    [[nodiscard]] ChannelHeader& outbound() { return header->channels[outbound_channel]; }
    [[nodiscard]] ChannelHeader& inbound() { return header->channels[inbound_channel]; }
    [[nodiscard]] PeerStats& published_out() { return header->published[outbound_channel]; }
    [[nodiscard]] PeerStats& published_in() { return header->published[inbound_channel]; }

    [[nodiscard]] u64 queued(u32 channel) const {
        const ChannelHeader& header_channel = header->channels[channel];
        const u64 write = header_channel.write_seq.load(std::memory_order_acquire);
        const u64 read = header_channel.read_seq.load(std::memory_order_acquire);
        return write - read;
    }
};

SharedLink::~SharedLink() = default;

LinkState SharedLink::state() const { return impl_->state; }

bool SharedLink::send(ConstSpan<const u8> message) {
    if (impl_->state == LinkState::Closed) {
        ++impl_->stats.messages_dropped;
        return false;
    }
    if (message.size() > impl_->header->slot_size) {
        T2D_ERROR("shared link: message of {} bytes exceeds the slot size {}", message.size(),
                  impl_->header->slot_size);
        ++impl_->stats.messages_dropped;
        return false;
    }

    ChannelHeader& channel = impl_->outbound();
    const u64 write = channel.write_seq.load(std::memory_order_relaxed);
    const u64 read = channel.read_seq.load(std::memory_order_acquire);
    const u64 capacity = impl_->header->slot_count;
    if (write - read >= capacity) {
        ++channel.dropped;
        ++impl_->stats.messages_dropped;
        return false; // caller decides whether to retry; snapshots are superseded anyway
    }

    u8* slot = slot_pointer(impl_->header, impl_->outbound_channel, write);
    const u32 length = static_cast<u32>(message.size());
    std::memcpy(slot, &length, sizeof(length));
    if (length > 0) std::memcpy(slot + sizeof(length), message.data(), length);

    channel.write_seq.store(write + 1, std::memory_order_release);
    ++channel.messages;
    channel.bytes += message.size();
    const u64 depth = write + 1 - read;
    if (depth > channel.high_water) channel.high_water = depth;

    impl_->published_out().messages.store(channel.messages, std::memory_order_relaxed);
    impl_->published_out().bytes.store(channel.bytes, std::memory_order_relaxed);
    impl_->published_out().queue.store(static_cast<u32>(depth), std::memory_order_relaxed);

    if (impl_->futex_wait) channel.write_seq.notify_one();

    impl_->stats.messages_sent += 1;
    impl_->stats.bytes_sent += message.size();
    impl_->stats.send_queue_messages = static_cast<u32>(depth);
    return true;
}

usize SharedLink::receive(Span<u8> destination) {
    ChannelHeader& channel = impl_->inbound();
    const u64 read = channel.read_seq.load(std::memory_order_relaxed);
    const u64 write = channel.write_seq.load(std::memory_order_acquire);
    if (read >= write) return 0;

    const u8* slot = slot_pointer(impl_->header, impl_->inbound_channel, read);
    u32 length = 0;
    std::memcpy(&length, slot, sizeof(length));
    if (length > destination.size()) {
        T2D_ERROR("shared link: incoming message of {} bytes does not fit in the {} byte buffer", length,
                  destination.size());
        channel.read_seq.store(read + 1, std::memory_order_release);
        ++impl_->stats.messages_dropped;
        return 0;
    }
    if (length > 0) std::memcpy(destination.data(), slot + sizeof(length), length);
    channel.read_seq.store(read + 1, std::memory_order_release);

    impl_->stats.messages_received += 1;
    impl_->stats.bytes_received += length;
    impl_->stats.receive_queue_messages = static_cast<u32>(write - (read + 1));
    return length;
}

void SharedLink::update(u64 now_ms) {
    (void)now_ms; // nothing to pump: the channel has no timers of its own
    if (impl_->state == LinkState::Closed) return;
    // Publish a fresh view of the inbound queue so the peer sees backpressure.
    impl_->published_in().queue.store(static_cast<u32>(impl_->queued(impl_->inbound_channel)),
                                      std::memory_order_relaxed);
}

const LinkStats& SharedLink::stats() const {
    impl_->stats.send_queue_messages = static_cast<u32>(impl_->queued(impl_->outbound_channel));
    impl_->stats.receive_queue_messages = static_cast<u32>(impl_->queued(impl_->inbound_channel));
    impl_->stats.pending_bytes = static_cast<u32>(impl_->queued(impl_->outbound_channel) * 64u);
    return impl_->stats;
}

void SharedLink::close() {
    impl_->state = LinkState::Closed;
    if (impl_->futex_wait) {
        impl_->outbound().write_seq.notify_all();
    }
}

bool SharedLink::wait_for_data(u32 timeout_ms) {
    ChannelHeader& channel = impl_->inbound();
    const u64 deadline = now_ms() + timeout_ms;
    for (;;) {
        const u64 write = channel.write_seq.load(std::memory_order_acquire);
        if (channel.read_seq.load(std::memory_order_acquire) < write) return true;
        if (impl_->state == LinkState::Closed) return false;
        const u64 current = now_ms();
        if (current >= deadline) return false;
        const u64 remaining = deadline - current;
        // The producer publishes and notifies after storing the message, but a short sleep is the
        // portable option here: it also works when the channel is backed by a file (no shared futex
        // word), and 1 ms of polling on a channel that is only used between two threads of one
        // process costs nothing measurable.
        (void)write;
        sleep_ms(static_cast<u32>(std::min<u64>(remaining, 1)));
    }
}

bool SharedLink::is_server_endpoint() const { return impl_->server_endpoint; }

usize SharedLink::shared_bytes() const { return impl_->region->size; }

u32 SharedLink::queued_outgoing() const {
    return static_cast<u32>(impl_->queued(impl_->outbound_channel));
}

u32 SharedLink::queued_incoming() const { return static_cast<u32>(impl_->queued(impl_->inbound_channel)); }

u32 SharedLink::outgoing_high_water() const {
    return static_cast<u32>(impl_->outbound().high_water);
}

u64 SharedLink::peer_messages_sent() const {
    return impl_->published_in().messages.load(std::memory_order_relaxed);
}

u32 SharedLink::peer_queue_depth() const {
    return impl_->published_in().queue.load(std::memory_order_relaxed);
}

void SharedLink::reset_queues() {
    impl_->outbound().write_seq.store(0, std::memory_order_release);
    impl_->outbound().read_seq.store(0, std::memory_order_release);
    impl_->inbound().write_seq.store(0, std::memory_order_release);
    impl_->inbound().read_seq.store(0, std::memory_order_release);
}

SharedLinkPair create_shared_link_pair(const SharedLinkOptions& options) {
    SharedLinkPair pair;
    if (options.slot_count == 0 || options.slot_size < 64) {
        T2D_ERROR("shared link: invalid options (slot_count={}, slot_size={})", options.slot_count, options.slot_size);
        return pair;
    }

    const u64 stride = align_up(sizeof(u32) + options.slot_size, kCacheLine);
    const usize header_bytes = align_up(sizeof(SharedHeader), kCacheLine);
    const usize channel_bytes = static_cast<usize>(stride) * options.slot_count;
    const usize total = header_bytes + channel_bytes * 2;

    auto region = make_ref<SharedRegion>();
    region->size = total;

    if (options.backing_file.empty()) {
        void* base = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (base == MAP_FAILED) {
            T2D_ERROR("shared link: mmap of {} bytes failed", total);
            return pair;
        }
        region->base = base;
    } else {
        const int fd = ::open(options.backing_file.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0600);
        if (fd < 0) {
            T2D_ERROR("shared link: cannot create backing file '{}'", options.backing_file);
            return pair;
        }
        if (::ftruncate(fd, static_cast<off_t>(total)) != 0) {
            T2D_ERROR("shared link: ftruncate('{}') failed", options.backing_file);
            ::close(fd);
            return pair;
        }
        void* base = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd); // the mapping keeps the file alive
        if (base == MAP_FAILED) {
            T2D_ERROR("shared link: mmap('{}') failed", options.backing_file);
            return pair;
        }
        region->base = base;
        region->path = options.backing_file;
        region->unlink_file = true;
    }

    auto* header = static_cast<SharedHeader*>(region->base);
    header->magic = kSharedMagic;
    header->version = kSharedVersion;
    header->slot_count = options.slot_count;
    header->slot_size = options.slot_size;
    header->stride = stride;

    const auto make_endpoint = [&](bool server_endpoint) {
        // Inside a friend function the closure has the same access rights, so this is allowed.
        Scope<SharedLink> link(new SharedLink());
        link->impl_ = make_scope<SharedLink::Impl>();
        link->impl_->region = region;
        link->impl_->header = header;
        link->impl_->server_endpoint = server_endpoint;
        link->impl_->outbound_channel = server_endpoint ? 0u : 1u;
        link->impl_->inbound_channel = server_endpoint ? 1u : 0u;
        link->impl_->futex_wait = options.use_futex_wait;
        link->impl_->state = LinkState::Connected;
        return link;
    };

    pair.server = make_endpoint(true);
    pair.client = make_endpoint(false);
    T2D_DEBUG("shared link: {} KiB mapped ({} slots x {} bytes per direction)",
              static_cast<f64>(total) / 1024.0, options.slot_count, options.slot_size);
    return pair;
}

} // namespace t2d::net
