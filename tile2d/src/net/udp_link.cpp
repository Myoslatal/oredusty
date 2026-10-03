#include <t2d/net/udp_link.h>

#include <t2d/core/log.h>
#include <t2d/core/time.h>

#include <algorithm>
#include <cstring>

namespace t2d::net {
namespace {

/// A KCP segment header is 24 bytes: anything shorter cannot carry a conv and is ignored.
constexpr usize kMinimumKcpDatagram = kKcpHeaderSize;

[[nodiscard]] u32 peek_conv(ConstSpan<const u8> packet) {
    u32 conv = 0;
    std::memcpy(&conv, packet.data(), sizeof(conv));
    return conv;
}

} // namespace

UdpLink::~UdpLink() = default;

LinkState UdpLink::state() const { return state_; }

u32 UdpLink::conv() const { return kcp_ != nullptr ? kcp_->conv() : 0; }

bool UdpLink::dead() const {
    if (kcp_ != nullptr && kcp_->dead_link()) return true;
    return timed_out_;
}

bool UdpLink::send(ConstSpan<const u8> message) {
    if (state_ == LinkState::Closed || kcp_ == nullptr) {
        ++stats_.messages_dropped;
        return false;
    }
    if (message.size() > kcp_->max_message_size()) {
        T2D_ERROR("udp link: message of {} bytes exceeds the KCP maximum of {}", message.size(),
                  kcp_->max_message_size());
        ++stats_.messages_dropped;
        return false;
    }
    if (!kcp_->send(message)) {
        ++stats_.messages_dropped;
        return false;
    }
    ++stats_.messages_sent;
    stats_.bytes_sent += message.size();
    return true;
}

usize UdpLink::receive(Span<u8> destination) {
    if (kcp_ == nullptr) return 0;
    const usize size = kcp_->receive(destination);
    if (size > 0) {
        ++stats_.messages_received;
        stats_.bytes_received += size;
    }
    return size;
}

void UdpLink::update(u64 now_ms) {
    if (kcp_ == nullptr || state_ == LinkState::Closed) return;
    kcp_->update(now_ms);

    if (last_packet_ms_ != 0 && now_ms > last_packet_ms_ + config_.timeout_ms) {
        if (!timed_out_) T2D_DEBUG("udp link {}: timed out after {} ms", peer_.to_string(), config_.timeout_ms);
        timed_out_ = true;
    }
    if (timed_out_ && !kcp_->dead_link()) {
        // A late packet revives the link (the peer may just have been stalled).
        timed_out_ = false;
    }
    if (kcp_->dead_link()) state_ = LinkState::Closed;
}

const LinkStats& UdpLink::stats() const {
    const KcpStats& kcp_stats = kcp_->stats();
    stats_.packets_sent = kcp_stats.output_packets;
    stats_.packets_received = kcp_stats.input_packets;
    stats_.packets_dropped = kcp_stats.retransmissions;
    stats_.rtt_ms = kcp_stats.rtt_ms;
    stats_.send_queue_messages = kcp_stats.wait_send_segments;
    stats_.receive_queue_messages = static_cast<u32>(kcp_->peek_size() > 0 ? 1u : 0u);
    stats_.pending_bytes = static_cast<u32>(kcp_->queued_receive_bytes());
    const u64 segments = kcp_stats.segments_sent != 0 ? kcp_stats.segments_sent : 1;
    stats_.packet_loss = static_cast<f32>(static_cast<f64>(kcp_stats.retransmissions) / static_cast<f64>(segments));
    return stats_;
}

void UdpLink::close() {
    state_ = LinkState::Closed;
    if (kcp_ != nullptr) kcp_->reset();
}

bool UdpLink::wait_for_data(u32 timeout_ms) {
    if (kcp_ != nullptr && kcp_->peek_size() > 0) return true;
    if (transport_ == nullptr) return false;
    if (!transport_->server_mode()) {
        // Client: a single peer, so waiting on the socket is exactly right.
        return transport_->wait_readable(timeout_ms);
    }
    // Server: the socket is shared by every player; the server thread paces itself on its tick timer.
    sleep_ms(std::min<u32>(timeout_ms, 1));
    return kcp_ != nullptr && kcp_->peek_size() > 0;
}

// ---------------------------------------------------------------- transport --

Scope<UdpTransport> UdpTransport::create(const Config& config) {
    Scope<UdpTransport> transport(new UdpTransport());
    transport->config_ = config;
    transport->socket_ = UdpSocket::create();
    if (transport->socket_ == nullptr) return nullptr;
    if (!transport->socket_->bind(config.port)) return nullptr;
    transport->receive_buffer_.resize(kKcpHeaderSize + 2048);
    T2D_INFO("udp transport: listening on port {} ({} mode, up to {} links)", transport->socket_->local_port(),
             config.server_mode ? "server" : "client", config.max_links);
    return transport;
}

UdpTransport::~UdpTransport() = default;

u16 UdpTransport::port() const { return socket_ != nullptr ? socket_->local_port() : 0; }

bool UdpTransport::wait_readable(u32 timeout_ms) {
    return socket_ != nullptr && socket_->wait_readable(timeout_ms);
}

UdpLink* UdpTransport::create_link(const Endpoint& peer, u32 conv) {
    if (links_.size() >= config_.max_links) {
        ++stats_.packets_dropped;
        return nullptr;
    }
    Scope<UdpLink> link(new UdpLink());
    link->transport_ = this;
    link->peer_ = peer;
    link->config_ = config_.link;
    link->created_ms_ = now_ms();
    link->state_ = LinkState::Connecting;

    KcpConfig kcp_config = config_.link.kcp;
    kcp_config.conv = conv;
    link->kcp_ = make_scope<Kcp>(conv, kcp_config);
    link->kcp_->set_output([this, peer](ConstSpan<const u8> packet) { send_raw(peer, packet); });

    UdpLink* raw = link.get();
    links_.push_back(std::move(link));
    T2D_DEBUG("udp transport: new link to {} (conv 0x{:08x})", peer.to_string(), conv);
    return raw;
}

UdpLink* UdpTransport::connect(const Endpoint& peer, u32 conv) {
    if (conv == 0) conv = next_conv_++;
    return create_link(peer, conv);
}

UdpLink* UdpTransport::find(const Endpoint& peer) const {
    for (const Scope<UdpLink>& link : links_) {
        if (link->peer_ == peer) return link.get();
    }
    return nullptr;
}

void UdpTransport::remove(UdpLink* link) {
    links_.erase(std::remove_if(links_.begin(), links_.end(),
                                [link](const Scope<UdpLink>& candidate) { return candidate.get() == link; }),
                 links_.end());
}

std::vector<Endpoint> UdpTransport::collect_dead_links() {
    std::vector<Endpoint> dead;
    for (const Scope<UdpLink>& link : links_) {
        if (!link->dead()) continue;
        dead.push_back(link->peer_);
    }
    for (const Endpoint& peer : dead) remove(find(peer));
    return dead;
}

void UdpTransport::send_raw(const Endpoint& peer, ConstSpan<const u8> data) {
    if (socket_ == nullptr) return;
    if (socket_->send_to(peer, data)) {
        ++stats_.packets_sent;
        stats_.bytes_sent += data.size();
    } else {
        ++stats_.packets_dropped;
    }
}

void UdpTransport::update(u64 now_ms) {
    if (socket_ == nullptr) return;

    for (u32 processed = 0; processed < config_.max_packets_per_update; ++processed) {
        Endpoint source;
        const usize size = socket_->receive_from(source, Span<u8>(receive_buffer_.data(), receive_buffer_.size()));
        if (size == 0) break;
        ++stats_.packets_received;
        stats_.bytes_received += size;

        if (size < kMinimumKcpDatagram) {
            ++stats_.packets_dropped;
            continue;
        }
        const u32 conv = peek_conv(ConstSpan<const u8>(receive_buffer_.data(), size));

        UdpLink* link = find(source);
        if (link == nullptr) {
            if (!config_.server_mode) {
                ++stats_.unknown_peers;
                continue;
            }
            link = create_link(source, conv);
            if (link == nullptr) continue;
        }
        if (link->conv() != conv) {
            // Someone is guessing conversations; drop it.
            ++stats_.packets_dropped;
            continue;
        }
        link->kcp_->input(ConstSpan<const u8>(receive_buffer_.data(), size));
        link->last_packet_ms_ = now_ms;
        link->timed_out_ = false;
        if (link->state_ == LinkState::Connecting && link->kcp_->stats().input_packets > 0) {
            link->state_ = LinkState::Connected;
        }
    }

    for (const Scope<UdpLink>& link : links_) link->update(now_ms);
}

ConstSpan<UdpLink*> UdpTransport::links() const {
    // The vector holds unique_ptrs; hand out a stable view by storing raw pointers in a scratch list.
    static thread_local std::vector<UdpLink*> scratch;
    scratch.clear();
    scratch.reserve(links_.size());
    for (const Scope<UdpLink>& link : links_) scratch.push_back(link.get());
    return ConstSpan<UdpLink*>(scratch.data(), scratch.size());
}

} // namespace t2d::net
