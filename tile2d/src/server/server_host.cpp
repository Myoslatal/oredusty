#include <t2d/server/server_host.h>

#include <t2d/core/log.h>
#include <t2d/core/time.h>

#include <algorithm>
#include <format>

namespace t2d {
namespace {

constexpr u32 kMaxCatchUpTicks = 4;

/// How far ahead of the newest acknowledged tick a client is allowed to be before we resend a full
/// snapshot (a client that fell behind must not be fed deltas it cannot resolve).
constexpr u32 kAckTooOldTicks = 90;
constexpr u64 kPingIntervalMs = 1000;

} // namespace

Scope<ServerHost> ServerHost::create(const ServerConfig& config) {
    Scope<ServerHost> server(new ServerHost());
    server->config_ = config;
    server->world_ = make_scope<World>(config.world);
    server->encoded_map_ = server->world_->map().serialize();
    server->receive_buffer_.resize(net::kMaxMessageSize);
    server->history_.reserve(config.baseline_history);

    if (config.local_client) {
        net::SharedLinkPair pair = net::create_shared_link_pair();
        if (pair.server == nullptr || pair.client == nullptr) {
            T2D_ERROR("server: failed to create the shared memory link for the local client");
            return nullptr;
        }
        server->local_link_ = std::move(pair.server);
        server->local_client_link_ = std::move(pair.client);
    }

    if (config.port != 0) {
        net::UdpTransport::Config transport_config;
        transport_config.port = config.port;
        transport_config.server_mode = true;
        transport_config.max_links = config.max_players;
        transport_config.link.kcp.no_delay = true;
        transport_config.link.kcp.interval_ms = 10;
        transport_config.link.kcp.fast_resend_threshold = 2;
        server->transport_ = net::UdpTransport::create(transport_config);
        if (server->transport_ == nullptr) {
            T2D_ERROR("server: cannot listen on udp port {}", config.port);
            return nullptr;
        }
    }

    T2D_INFO("server: world {}x{} tiles, {} pickup(s), {} tick/s, snapshots every {} tick(s)", config.world.map.width(),
             config.world.map.height(), server->world_->pickups().size(), kTickRate, config.snapshot_interval_ticks);
    return server;
}

ServerHost::~ServerHost() { stop(); }

u16 ServerHost::port() const { return transport_ != nullptr ? transport_->port() : 0; }

net::SharedLink* ServerHost::take_local_client_link() {
    if (local_client_link_ == nullptr) return nullptr;
    return local_client_link_.release();
}

void ServerHost::start() {
    if (running_.load(std::memory_order_acquire)) return;
    stop_requested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] {
        set_thread_name(config_.local_client ? "server" : "dedicated");
        run();
    });
}

void ServerHost::stop() {
    if (!running_.load(std::memory_order_acquire) && !thread_.joinable()) {
        // Never started: still clean up the sessions so destructors see a consistent state.
        return;
    }
    stop_requested_.store(true, std::memory_order_release);
    if (local_link_ != nullptr) {
        // Wake the server thread out of wait_for_data() immediately.
        local_link_->close();
    }
    if (thread_.joinable()) thread_.join();
    running_.store(false, std::memory_order_release);
    if (transport_ != nullptr) transport_->update(now_ms());
    T2D_INFO("server: stopped after {} tick(s)", stats_.ticks);
}

void ServerHost::run() {
    const f32 tick_seconds = 1.0f / static_cast<f32>(kTickRate);
    const u64 tick_ms = static_cast<u64>(tick_seconds * 1000.0f + 0.5f);
    u64 next_tick = now_ms();

    while (!stop_requested_.load(std::memory_order_acquire)) {
        const u64 now = now_ms();
        if (now < next_tick) {
            // Sleep until the next tick, but stay responsive: the local link wakes us up as soon as
            // it has a message (client input, handshake) and the deadline caps the sleep.
            const u32 wait = static_cast<u32>(std::min<u64>(next_tick - now, 4));
            if (local_link_ != nullptr && local_link_->queued_incoming() == 0) {
                (void)local_link_->wait_for_data(wait);
            } else {
                sleep_ms(wait);
            }
            continue;
        }

        tick_once(now);

        next_tick += tick_ms;
        if (now > next_tick + tick_ms * kMaxCatchUpTicks) {
            // The thread was stalled (debugger, scheduler, a very slow tick): resynchronise instead
            // of spinning through a backlog of ticks.
            ++stats_.catch_up_ticks;
            next_tick = now + tick_ms;
        }
    }
}

void ServerHost::tick_once(u64 now) {
    const u64 start = now_ms();

    poll_local(now);
    poll_remotes(now);
    world_->step();

    const u32 tick = world_->tick();
    if (tick % config_.snapshot_interval_ticks == 0) {
        Snapshot full = capture_snapshot(*world_, 0, now);
        history_.push_back(full);
        while (history_.size() > config_.baseline_history) history_.erase(history_.begin());

        for (const Scope<Session>& session : sessions_) {
            if (!session->ready) continue;
            send_snapshot(*session, now);
        }
    }

    // Keep every link's timers (KCP retransmission, timeouts) up to date.
    if (transport_ != nullptr) transport_->update(now);
    if (local_link_ != nullptr) local_link_->update(now);

    const f32 tick_ms = static_cast<f32>(now_ms() - start);
    average_tick_ms_ += (tick_ms - average_tick_ms_) * 0.05f;
    {
        std::scoped_lock lock(stats_mutex_);
        stats_.tick = world_->tick();
        stats_.ticks += 1;
        stats_.last_tick_ms = tick_ms;
        stats_.average_tick_ms = average_tick_ms_;
        stats_.players = static_cast<u32>(world_->players().size());
    }
    update_stats(now);
}

void ServerHost::poll_local(u64 now) {
    if (local_link_ == nullptr) return;
    for (;;) {
        const usize size = local_link_->receive(Span<u8>(receive_buffer_.data(), receive_buffer_.size()));
        if (size == 0) break;
        net::MessageType type = net::MessageType::None;
        ConstSpan<const u8> payload;
        if (!net::decode_message(ConstSpan<const u8>(receive_buffer_.data(), size), type, payload)) {
            ++stats_.messages_dropped;
            continue;
        }
        Session* session = find_session(local_link_.get());
        if (session == nullptr) {
            // First message from the local client: create its session.
            auto created = make_scope<Session>();
            created->link = local_link_.get();
            created->local = true;
            sessions_.push_back(std::move(created));
            session = sessions_.back().get();
        }
        handle_message(*session, type, payload, now);
    }
    if (local_link_->state() == net::LinkState::Closed) {
        for (const Scope<Session>& session : sessions_) {
            if (session->local && session->player_id != kInvalidId) drop_session(*session, 1);
        }
    }
}

void ServerHost::poll_remotes(u64 now) {
    if (transport_ == nullptr) return;

    for (net::UdpLink* link : transport_->links()) {
        Session* session = find_session(link);
        if (session == nullptr) {
            auto created = make_scope<Session>();
            created->link = link;
            created->local = false;
            sessions_.push_back(std::move(created));
            session = sessions_.back().get();
            T2D_DEBUG("server: new remote peer {}", link->peer().to_string());
        }
        for (;;) {
            const usize size = link->receive(Span<u8>(receive_buffer_.data(), receive_buffer_.size()));
            if (size == 0) break;
            net::MessageType type = net::MessageType::None;
            ConstSpan<const u8> payload;
            if (!net::decode_message(ConstSpan<const u8>(receive_buffer_.data(), size), type, payload)) {
                ++stats_.messages_dropped;
                continue;
            }
            handle_message(*session, type, payload, now);
        }
        session->rtt_ms = link->stats().rtt_ms;
    }

    // Deal with peers that vanished.
    for (const net::Endpoint& peer : transport_->collect_dead_links()) {
        for (usize i = 0; i < sessions_.size(); ++i) {
            Session& session = *sessions_[i];
            if (session.local || session.link == nullptr) continue;
            if (static_cast<net::UdpLink*>(session.link)->peer() != peer) continue;
            T2D_INFO("server: {} timed out", peer.to_string());
            drop_session(session, 1);
            break;
        }
    }
}

void ServerHost::handle_message(Session& session, net::MessageType type, ConstSpan<const u8> payload, u64 now) {
    switch (type) {
        case net::MessageType::Hello: {
            handle_hello(session, payload, now);
            break;
        }
        case net::MessageType::Command: {
            net::CommandMessage message;
            if (!net::decode_command(payload, message) || session.player_id == kInvalidId) {
                ++stats_.messages_dropped;
                break;
            }
            world_->submit_command(session.player_id, message.command);
            session.acked_snapshot_tick = message.last_snapshot_tick;
            ++session.commands_received;
            ++stats_.commands_received;
            break;
        }
        case net::MessageType::Ping: {
            net::PingMessage ping;
            if (!net::decode_ping(payload, ping)) break;
            const net::PongMessage pong{ping.sequence, ping.sender_time_ms, now};
            deliver(session, net::MessageType::Pong, net::encode_pong(pong));
            break;
        }
        case net::MessageType::NeedFullSnapshot: {
            session.wants_full_snapshot = true;
            break;
        }
        case net::MessageType::Disconnect: {
            T2D_INFO("server: {} disconnected", session.name.empty() ? "<unnamed>" : session.name);
            drop_session(session, 1);
            break;
        }
        default:
            ++stats_.messages_dropped;
            break;
    }
}

void ServerHost::handle_hello(Session& session, ConstSpan<const u8> payload, u64 now) {
    net::HelloMessage hello;
    if (!net::decode_hello(payload, hello)) {
        ++stats_.messages_dropped;
        return;
    }
    if (hello.protocol_version != net::kProtocolVersion) {
        const net::RejectMessage reject{net::RejectReason::ProtocolMismatch,
                                        std::format("protocol {} expected {}", hello.protocol_version,
                                                    net::kProtocolVersion)};
        deliver(session, net::MessageType::Reject, net::encode_reject(reject));
        return;
    }
    if (session.player_id != kInvalidId) return; // already accepted (a duplicated Hello)
    if (sessions_.size() > config_.max_players) {
        const net::RejectMessage reject{net::RejectReason::ServerFull, "server is full"};
        deliver(session, net::MessageType::Reject, net::encode_reject(reject));
        return;
    }
    accept_player(session, hello.name, now);
}

void ServerHost::accept_player(Session& session, std::string_view name, u64 now) {
    session.player_id = world_->spawn_player();
    session.name = std::string(name.empty() ? "player" : name);
    session.ready = true;
    session.wants_full_snapshot = true;

    const PlayerState* player = world_->find_player(session.player_id);
    net::WelcomeMessage welcome;
    welcome.player_id = session.player_id;
    welcome.tick = world_->tick();
    welcome.tick_rate = kTickRate;
    welcome.spawn = player != nullptr ? player->spawn_position : Vec2{};
    welcome.map_width = world_->map().width();
    welcome.map_height = world_->map().height();
    welcome.tile_size = world_->map().tile_size();
    welcome.client_token = 0;
    deliver(session, net::MessageType::Welcome, net::encode_welcome(welcome));
    send_map(session);

    const net::PlayerJoinedMessage joined{session.player_id, session.name};
    broadcast(net::MessageType::PlayerJoined, net::encode_player_joined(joined), session.link);

    T2D_INFO("server: {} joined as player {} ({} player(s) now)", session.name, session.player_id,
             sessions_.size());
    send_snapshot(session, now);
}

void ServerHost::send_map(Session& session) {
    const usize total = encoded_map_.size();
    const u16 chunk_count = static_cast<u16>((total + net::kMapChunkSize - 1) / net::kMapChunkSize);
    for (u16 index = 0; index < chunk_count; ++index) {
        const usize offset = index * net::kMapChunkSize;
        const usize size = std::min(net::kMapChunkSize, total - offset);
        net::MapDataMessage chunk;
        chunk.chunk_index = index;
        chunk.chunk_count = chunk_count;
        chunk.payload.assign(encoded_map_.begin() + static_cast<isize>(offset),
                             encoded_map_.begin() + static_cast<isize>(offset + size));
        deliver(session, net::MessageType::MapData, net::encode_map_data(chunk));
    }
    T2D_DEBUG("server: sent the level to {} in {} chunk(s) ({} bytes)", session.name, chunk_count, total);
}

const Snapshot* ServerHost::find_baseline(u32 tick) const {
    for (const Snapshot& snapshot : history_) {
        if (snapshot.tick == tick) return &snapshot;
    }
    return nullptr;
}

void ServerHost::send_snapshot(Session& session, u64 now) {
    if (history_.empty()) return;
    const Snapshot& current = history_.back();

    const bool force_full = session.wants_full_snapshot ||
                            (current.tick % config_.full_snapshot_interval_ticks) == 0 ||
                            session.acked_snapshot_tick == 0 ||
                            current.tick < session.acked_snapshot_tick ||
                            current.tick - session.acked_snapshot_tick > kAckTooOldTicks;

    Snapshot outgoing = current;
    // The ack field is per recipient: it tells *this* client which of its commands the server has
    // consumed, which is exactly what it needs to replay the rest.
    const PlayerState* player = world_->find_player(session.player_id);
    outgoing.acked_command_tick = player != nullptr ? player->last_command_tick : 0;

    if (!force_full) {
        const Snapshot* baseline = find_baseline(session.acked_snapshot_tick);
        if (baseline != nullptr) outgoing = make_delta(outgoing, *baseline);
    }
    if (outgoing.kind == SnapshotKind::Full) session.wants_full_snapshot = false;

    const std::vector<u8> encoded = encode_snapshot(outgoing);
    deliver(session, net::MessageType::Snapshot, encoded);

    session.last_sent_snapshot_tick = outgoing.tick;
    if (outgoing.kind == SnapshotKind::Full) {
        ++stats_.full_snapshots_sent;
    } else {
        ++stats_.delta_snapshots_sent;
    }
    ++stats_.snapshots_sent;
    {
        std::scoped_lock lock(stats_mutex_);
        stats_.last_snapshot_bytes = static_cast<u32>(encoded.size());
    }
    (void)now;

    if (session.link != nullptr && now > session.last_ping_ms + kPingIntervalMs && !session.local) {
        net::PingMessage ping;
        ping.sequence = ++session.ping_sequence;
        ping.sender_time_ms = now;
        deliver(session, net::MessageType::Ping, net::encode_ping(ping));
        session.last_ping_ms = now;
    }
}

void ServerHost::deliver(Session& session, net::MessageType type, ConstSpan<const u8> payload) {
    if (session.link == nullptr) return;
    const std::vector<u8> framed = net::encode_message(type, payload);
    if (!session.link->send(framed)) {
        ++stats_.messages_dropped;
        return;
    }
    session.bytes_sent += framed.size();
    bytes_sent_window_ += static_cast<f64>(framed.size());
}

void ServerHost::broadcast(net::MessageType type, ConstSpan<const u8> payload, net::ILink* except) {
    const std::vector<u8> framed = net::encode_message(type, payload);
    for (const Scope<Session>& session : sessions_) {
        if (session->link == nullptr || session->link == except || session->player_id == kInvalidId) continue;
        if (!session->link->send(framed)) {
            ++stats_.messages_dropped;
            continue;
        }
        session->bytes_sent += framed.size();
        bytes_sent_window_ += static_cast<f64>(framed.size());
    }
}

void ServerHost::drop_session(Session& session, u8 reason) {
    if (session.player_id != kInvalidId) {
        const net::PlayerLeftMessage left{session.player_id, reason};
        const std::vector<u8> payload = net::encode_player_left(left);
        broadcast(net::MessageType::PlayerLeft, payload, session.link);
        world_->despawn_player(session.player_id);
        T2D_INFO("server: player {} ({}) removed", session.player_id, session.name);
    }
    if (session.link != nullptr && !session.local) {
        if (auto* udp = static_cast<net::UdpLink*>(session.link); udp != nullptr) {
            udp->close();
            if (transport_ != nullptr) transport_->remove(udp);
        }
    }
    session.link = nullptr;
    session.player_id = kInvalidId;
    session.ready = false;

    sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                                   [&session](const Scope<Session>& candidate) {
                                       return candidate.get() == &session;
                                   }),
                    sessions_.end());
}

ServerHost::Session* ServerHost::find_session(net::ILink* link) {
    for (const Scope<Session>& session : sessions_) {
        if (session->link == link) return session.get();
    }
    return nullptr;
}

ServerHost::Session* ServerHost::find_session_for(PlayerId id) {
    for (const Scope<Session>& session : sessions_) {
        if (session->player_id == id) return session.get();
    }
    return nullptr;
}

void ServerHost::update_stats(u64 now) {
    if (window_start_ms_ == 0) window_start_ms_ = now;
    if (now >= window_start_ms_ + 1000) {
        const f64 seconds = static_cast<f64>(now - window_start_ms_) / 1000.0;
        std::scoped_lock lock(stats_mutex_);
        stats_.bytes_sent_per_second = bytes_sent_window_ / seconds;
        stats_.bytes_received_per_second = bytes_received_window_ / seconds;
        stats_.ticks_per_second = static_cast<f32>(static_cast<f64>(stats_.ticks) / seconds);
        bytes_sent_window_ = 0.0;
        bytes_received_window_ = 0.0;
        window_start_ms_ = now;
    }
}

ServerStats ServerHost::stats() const {
    std::scoped_lock lock(stats_mutex_);
    ServerStats copy = stats_;
    copy.local_players = 0;
    copy.remote_players = 0;
    for (const Scope<Session>& session : sessions_) {
        if (!session->ready) continue;
        if (session->local) ++copy.local_players;
        else ++copy.remote_players;
    }
    return copy;
}

std::vector<ServerPlayerInfo> ServerHost::players() const {
    std::vector<ServerPlayerInfo> out;
    out.reserve(sessions_.size());
    for (const Scope<Session>& session : sessions_) {
        if (session->player_id == kInvalidId) continue;
        ServerPlayerInfo info;
        info.id = session->player_id;
        info.name = session->name;
        info.local = session->local;
        info.remote = !session->local;
        info.ready = session->ready;
        info.rtt_ms = session->rtt_ms;
        info.acked_snapshot_tick = session->acked_snapshot_tick;
        out.push_back(std::move(info));
    }
    return out;
}

} // namespace t2d
