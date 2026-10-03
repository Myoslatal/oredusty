#include <t2d/client/local_client.h>

#include <t2d/core/log.h>
#include <t2d/core/time.h>

#include <algorithm>
#include <format>

namespace t2d {
namespace {

constexpr u64 kClientToken = 0x7E57C0DEull;

/// Compares the world against the snapshot it was just built from, field by field. Returns an empty
/// string when they agree; otherwise it names the first divergence, which turns a "checksum
/// mismatch" into a concrete bug report.
[[nodiscard]] std::string diagnose_mismatch(const World& world, const Snapshot& snapshot) {
    if (world.tick() != snapshot.tick) {
        return std::format("tick: world {} vs snapshot {}", world.tick(), snapshot.tick);
    }
    if (world.players().size() != snapshot.players.size()) {
        return std::format("player count: world {} vs snapshot {}", world.players().size(),
                           snapshot.players.size());
    }
    for (usize i = 0; i < world.players().size(); ++i) {
        const PlayerState& a = world.players()[i];
        const PlayerState& b = snapshot.players[i];
        const auto check = [&](const char* field, auto lhs, auto rhs) -> std::string {
            if (lhs == rhs) return {};
            return std::format("player {} field {}: world {} vs snapshot {}", a.id, field, lhs, rhs);
        };
        for (const std::string& message :
             {check("id", a.id, b.id), check("x", a.position.x, b.position.x), check("y", a.position.y, b.position.y),
              check("vx", a.velocity.x, b.velocity.x), check("vy", a.velocity.y, b.velocity.y),
              check("coins", a.coins, b.coins), check("flags", static_cast<u32>(a.flags), static_cast<u32>(b.flags)),
              check("coyote", static_cast<u32>(a.coyote_ticks), static_cast<u32>(b.coyote_ticks)),
              check("jump_buffer", static_cast<u32>(a.jump_buffer_ticks), static_cast<u32>(b.jump_buffer_ticks)),
              check("respawn", static_cast<u32>(a.respawn_ticks), static_cast<u32>(b.respawn_ticks))}) {
            if (!message.empty()) return message;
        }
    }
    if (world.pickups().size() != snapshot.pickups.size()) {
        return std::format("pickup count: world {} vs snapshot {}", world.pickups().size(),
                           snapshot.pickups.size());
    }
    for (usize i = 0; i < world.pickups().size(); ++i) {
        const Pickup& a = world.pickups()[i];
        const Pickup& b = snapshot.pickups[i];
        if (a.id != b.id) return std::format("pickup {} id: world {} vs snapshot {}", i, a.id, b.id);
        if (a.alive != b.alive) return std::format("pickup {} alive: world {} vs snapshot {}", a.id, a.alive, b.alive);
        if (a.position.x != b.position.x || a.position.y != b.position.y) {
            return std::format("pickup {} position: world ({},{}) vs snapshot ({},{})", a.id, a.position.x,
                               a.position.y, b.position.x, b.position.y);
        }
    }
    return {};
}

} // namespace

Scope<LocalClient> LocalClient::create(const ClientConfig& config, net::ILink* link) {
    if (link == nullptr) {
        T2D_ERROR("local client: a link is required");
        return nullptr;
    }
    Scope<LocalClient> client(new LocalClient());
    client->config_ = config;
    client->link_ = link;
    client->player_name_ = config.player_name;
    client->snapshot_buffer_ = SnapshotBuffer(config.snapshot_history);
    client->baseline_history_.reserve(config.baseline_history);
    client->receive_buffer_.resize(net::kMaxMessageSize);
    client->link_kind_storage_ = link->kind();
    client->stats_.link_kind = client->link_kind_storage_;
    client->stats_.interpolation_delay_ms = config.interpolation_delay_ms;
    return client;
}

LocalClient::~LocalClient() = default;

bool LocalClient::connected() const {
    return link_ != nullptr && link_->state() == net::LinkState::Connected && !rejected_;
}

void LocalClient::start(u64 now_ms) {
    handshake_sent_ = false;
    load_start_ms_ = now_ms;
    status_ = "connecting";
    if (link_ == nullptr) return;

    net::HelloMessage hello;
    hello.protocol_version = net::kProtocolVersion;
    hello.name = player_name_;
    hello.client_token = kClientToken;
    const std::vector<u8> framed = net::encode_message(net::MessageType::Hello, net::encode_hello(hello));
    if (link_->send(framed)) {
        handshake_sent_ = true;
        ++stats_.messages_sent;
        stats_.bytes_sent += framed.size();
    }
}

void LocalClient::disconnect() {
    if (link_ != nullptr) {
        const net::DisconnectMessage message{0};
        const std::vector<u8> framed = net::encode_message(net::MessageType::Disconnect,
                                                          net::encode_disconnect(message));
        link_->send(framed);
        link_->close();
    }
    status_ = "disconnected";
}

void LocalClient::pump_link(u64 now_ms) {
    if (link_ == nullptr) return;
    link_->update(now_ms);
    for (;;) {
        const usize size = link_->receive(Span<u8>(receive_buffer_.data(), receive_buffer_.size()));
        if (size == 0) break;
        ++stats_.messages_received;
        stats_.bytes_received += size;
        net::MessageType type = net::MessageType::None;
        ConstSpan<const u8> payload;
        if (!net::decode_message(ConstSpan<const u8>(receive_buffer_.data(), size), type, payload)) {
            ++stats_.snapshots_dropped;
            continue;
        }
        handle_message(type, payload, now_ms);
    }
}

void LocalClient::handle_message(net::MessageType type, ConstSpan<const u8> payload, u64 now_ms) {
    switch (type) {
        case net::MessageType::Welcome:
            handle_welcome(payload);
            break;
        case net::MessageType::MapData:
            handle_map_data(payload, now_ms);
            break;
        case net::MessageType::Snapshot:
            handle_snapshot(payload, now_ms);
            break;
        case net::MessageType::Pong: {
            net::PongMessage pong;
            if (!net::decode_pong(payload, pong)) break;
            const u64 rtt = now_ms > pong.sender_time_ms ? now_ms - pong.sender_time_ms : 0;
            const u32 sample = static_cast<u32>(std::min<u64>(rtt, 5000));
            stats_.jitter_ms = stats_.rtt_ms > sample ? stats_.rtt_ms - sample : sample - stats_.rtt_ms;
            stats_.rtt_ms = stats_.rtt_ms == 0 ? sample : (stats_.rtt_ms * 7 + sample) / 8;
            break;
        }
        case net::MessageType::PlayerJoined: {
            net::PlayerJoinedMessage joined;
            if (!net::decode_player_joined(payload, joined)) break;
            player_names_[joined.player_id] = joined.name;
            T2D_INFO("client: {} joined", joined.name);
            break;
        }
        case net::MessageType::PlayerLeft: {
            net::PlayerLeftMessage left;
            if (!net::decode_player_left(payload, left)) break;
            player_names_.erase(left.player_id);
            T2D_INFO("client: player {} left", left.player_id);
            break;
        }
        case net::MessageType::Ping: {
            net::PingMessage ping;
            if (!net::decode_ping(payload, ping)) break;
            const net::PongMessage pong{ping.sequence, ping.sender_time_ms, now_ms};
            const std::vector<u8> framed = net::encode_message(net::MessageType::Pong, net::encode_pong(pong));
            if (link_->send(framed)) {
                ++stats_.messages_sent;
                stats_.bytes_sent += framed.size();
            }
            break;
        }
        case net::MessageType::Reject: {
            net::RejectMessage reject;
            if (!net::decode_reject(payload, reject)) break;
            rejected_ = true;
            status_ = std::format("rejected: {}", reject.text.empty() ? "server refused" : reject.text);
            T2D_ERROR("client: {}", status_);
            break;
        }
        case net::MessageType::Disconnect: {
            net::DisconnectMessage message;
            if (!net::decode_disconnect(payload, message)) break;
            status_ = std::format("server closed the connection (reason {})", message.reason);
            T2D_WARN("client: {}", status_);
            break;
        }
        case net::MessageType::ServerStats: {
            net::ServerStatsMessage server_stats;
            if (!net::decode_server_stats(payload, server_stats)) break;
            server_name_ = std::format("{} player(s), {:.2f} ms/tick", server_stats.players,
                                       static_cast<f64>(server_stats.tick_ms));
            break;
        }
        default:
            break;
    }
}

void LocalClient::handle_welcome(ConstSpan<const u8> payload) {
    net::WelcomeMessage welcome;
    if (!net::decode_welcome(payload, welcome)) return;
    local_player_id_ = welcome.player_id;
    if (welcome.tick_rate > 0) tick_rate_ = welcome.tick_rate;
    next_command_tick_ = welcome.tick + 1;
    map_chunks_expected_ = 0;
    map_chunks_received_ = 0;
    map_chunks_.clear();
    load_start_ms_ = now_ms();
    status_ = "loading level";
    player_names_[welcome.player_id] = player_name_;

    // The level arrives separately; the tuning is shared code, which is what makes prediction exact.
    T2D_INFO("client: welcomed as player {} (server tick {}, level {}x{} at {} px/tile)", welcome.player_id,
             welcome.tick, welcome.map_width, welcome.map_height, static_cast<f64>(welcome.tile_size));
}

void LocalClient::handle_map_data(ConstSpan<const u8> payload, u64 now_ms) {
    net::MapDataMessage chunk;
    if (!net::decode_map_data(payload, chunk)) return;
    if (chunk.chunk_count == 0 || chunk.chunk_index >= chunk.chunk_count) return;
    if (map_chunks_.size() != chunk.chunk_count) {
        map_chunks_.assign(chunk.chunk_count, {});
        map_chunks_expected_ = chunk.chunk_count;
    }
    map_chunks_[chunk.chunk_index] = std::move(chunk.payload);
    ++map_chunks_received_;
    stats_.map_chunks_expected = map_chunks_expected_;
    stats_.map_chunks_received = map_chunks_received_;

    if (map_chunks_received_ >= map_chunks_expected_) try_finish_loading(now_ms);
}

void LocalClient::try_finish_loading(u64 now_ms) {
    std::vector<u8> bytes;
    usize total = 0;
    for (const std::vector<u8>& chunk : map_chunks_) total += chunk.size();
    bytes.reserve(total);
    for (const std::vector<u8>& chunk : map_chunks_) bytes.insert(bytes.end(), chunk.begin(), chunk.end());

    const auto map = TileMap::deserialize(bytes);
    if (!map.has_value()) {
        status_ = "level data is corrupt";
        T2D_ERROR("client: the level sent by the server could not be decoded ({} bytes)", bytes.size());
        return;
    }
    WorldConfig world_config;
    world_config.map = *map;
    world_ = make_scope<World>(world_config);
    // The World constructor turns coin marker tiles into pickups; the server already did that, so
    // the snapshot that follows carries the authoritative pickup list and replaces ours.
    ready_ = true;
    status_ = "ready";
    stats_.level_load_ms = static_cast<f64>(now_ms - load_start_ms_);
    T2D_INFO("client: level loaded in {:.1f} ms ({}x{} tiles, {} pickup marker(s))", stats_.level_load_ms,
             map->width(), map->height(), world_->pickups().size());
}

void LocalClient::handle_snapshot(ConstSpan<const u8> payload, u64 now_ms) {
    Snapshot incoming;
    if (!decode_snapshot(payload, incoming)) {
        ++stats_.snapshots_dropped;
        return;
    }
    ++stats_.snapshots_received;

    Snapshot resolved;
    if (incoming.kind == SnapshotKind::Full) {
        resolved = incoming;
        ++stats_.full_snapshots_received;
    } else {
        ++stats_.delta_snapshots_received;
        // The server builds the delta against the newest snapshot this client acknowledged, which is
        // at least one round trip older than the newest one it has sent. Resolving against that exact
        // tick (instead of only the newest state) is what keeps deltas applicable.
        const Snapshot* base = find_baseline(incoming.baseline_tick);
        if (base == nullptr || !apply_delta(*base, incoming, resolved)) {
            ++stats_.snapshots_dropped;
            request_full_snapshot();
            return;
        }
    }
    remember_baseline(resolved);

    if (!ready_ || world_ == nullptr) {
        baseline_ = resolved;
        return; // the level has not arrived yet
    }

    apply_snapshot(*world_, resolved);

    // The state we just installed must hash to exactly what the server hashed: any difference means
    // the decode or the apply path is broken.
    const u64 local_checksum = capture_snapshot(*world_, resolved.acked_command_tick, now_ms).checksum;
    if (local_checksum != resolved.checksum) {
        ++stats_.desyncs;
        const std::string difference = diagnose_mismatch(*world_, resolved);
        T2D_WARN("client: state mismatch at tick {} (local 0x{:016x}, server 0x{:016x}) - {}",
                 resolved.tick, local_checksum, resolved.checksum,
                 difference.empty() ? "checksums differ but every compared field agrees" : difference);
        // Recovery: drop the (now suspect) baselines and ask for a full snapshot, so a single bad
        // delta cannot keep the client drifting forever.
        baseline_ = Snapshot{};
        baseline_history_.clear();
        request_full_snapshot();
    }

    snapshot_buffer_.push(resolved);
    baseline_ = resolved;
    last_snapshot_tick_ = resolved.tick;
    last_snapshot_ms_ = now_ms;
    stats_.server_tick = resolved.tick;
    stats_.players = static_cast<u32>(resolved.players.size());

    const PlayerState* authoritative = resolved.find_player(local_player_id_);
    if (authoritative != nullptr && config_.enable_prediction) {
        f32 error = 0.0f;
        predictor_.reconcile(*world_, local_player_id_, *authoritative, resolved.acked_command_tick, error);
        predictor_.acknowledge(resolved.acked_command_tick);
        stats_.prediction_error = error;
        stats_.max_prediction_error = std::max(stats_.max_prediction_error, error);
        if (error > 0.05f) ++stats_.mispredictions;
    } else if (authoritative != nullptr) {
        world_->apply_player_state(local_player_id_, *authoritative);
    }

    // Stay a couple of ticks ahead of the server so input arrives in time.
    const u32 target = resolved.tick + config_.command_lead_ticks;
    if (next_command_tick_ < target) next_command_tick_ = target;
    stats_.pending_commands = static_cast<u32>(predictor_.pending_count());
}

const Snapshot* LocalClient::find_baseline(u32 tick) const {
    for (const Snapshot& candidate : baseline_history_) {
        if (candidate.tick == tick) return &candidate;
    }
    return nullptr;
}

void LocalClient::remember_baseline(const Snapshot& snapshot) {
    const usize limit = std::max<usize>(config_.baseline_history, 2);
    baseline_history_.push_back(snapshot);
    while (baseline_history_.size() > limit) baseline_history_.erase(baseline_history_.begin());
}

void LocalClient::request_full_snapshot() {
    if (link_ == nullptr) return;
    const std::vector<u8> framed = net::encode_message(net::MessageType::NeedFullSnapshot, {});
    link_->send(framed);
}

void LocalClient::send_command(u32 tick, const PlayerCommand& command) {
    net::CommandMessage message;
    message.command = command;
    message.command.tick = tick;
    message.last_snapshot_tick = last_snapshot_tick_;
    const std::vector<u8> framed = net::encode_message(net::MessageType::Command, net::encode_command(message));
    if (link_->send(framed)) {
        ++stats_.messages_sent;
        stats_.bytes_sent += framed.size();
    }
}

void LocalClient::send_ping(u64 now_ms) {
    if (now_ms < last_ping_ms_ + config_.ping_interval_ms) return;
    last_ping_ms_ = now_ms;
    net::PingMessage ping;
    ping.sequence = ++ping_sequence_;
    ping.sender_time_ms = now_ms;
    const std::vector<u8> framed = net::encode_message(net::MessageType::Ping, net::encode_ping(ping));
    if (link_->send(framed)) {
        ++stats_.messages_sent;
        stats_.bytes_sent += framed.size();
    }
}

void LocalClient::update(u64 now_ms, const PlayerCommand& command) {
    if (last_update_ms_ == 0) last_update_ms_ = now_ms;
    f32 delta_seconds = static_cast<f32>(now_ms - last_update_ms_) / 1000.0f;
    last_update_ms_ = now_ms;
    if (delta_seconds > 0.25f) delta_seconds = 0.25f; // a stall must not teleport the player

    pump_link(now_ms);

    if (link_ == nullptr || link_->state() == net::LinkState::Closed) {
        if (status_ != "disconnected") status_ = "disconnected";
        update_render_players(now_ms);
        return;
    }

    if (ready_ && world_ != nullptr) {
        // One command per simulation tick, independent of the frame rate.
        accumulated_seconds_ += delta_seconds;
        const f32 tick_seconds = 1.0f / static_cast<f32>(tick_rate_);
        u32 ticks = 0;
        while (accumulated_seconds_ >= tick_seconds && ticks < 8) {
            accumulated_seconds_ -= tick_seconds;
            ++ticks;

            const u32 tick = next_command_tick_++;
            PlayerCommand predicted = command;
            predicted.tick = tick;
            predictor_.record(predicted);
            send_command(tick, predicted);
            if (config_.enable_prediction) {
                (void)world_->predict_player(local_player_id_, predicted);
            }
            stats_.predicted_tick = tick;
        }
        stats_.pending_commands = static_cast<u32>(predictor_.pending_count());
    }

    send_ping(now_ms);
    update_render_players(now_ms);
}

void LocalClient::update_render_players(u64 now_ms) {
    render_players_.clear();

    // Remote players: interpolated a fixed delay behind the newest snapshot.
    const Snapshot* newest = snapshot_buffer_.newest();
    if (newest != nullptr) {
        const f32 rate = static_cast<f32>(tick_rate_);
        const f32 since_snapshot = static_cast<f32>(now_ms - last_snapshot_ms_) / 1000.0f * rate;
        const f32 target_tick = static_cast<f32>(newest->tick) + std::min(since_snapshot, 4.0f);
        render_tick_ = target_tick - config_.interpolation_delay_ms / 1000.0f * rate;

        std::vector<PlayerState> interpolated;
        snapshot_buffer_.sample_all(render_tick_, interpolated);
        for (const PlayerState& player : interpolated) {
            if (player.id == local_player_id_) continue;
            RenderPlayer render;
            render.id = player.id;
            render.position = player.position;
            render.velocity = player.velocity;
            render.flags = player.flags;
            render.coins = player.coins;
            render.local = false;
            if (const auto it = player_names_.find(player.id); it != player_names_.end()) render.name = it->second;
            render_players_.push_back(std::move(render));
        }
    }

    // The local player is drawn from the prediction, i.e. with no added latency at all.
    if (world_ != nullptr) {
        if (const PlayerState* local = world_->find_player(local_player_id_)) {
            RenderPlayer render;
            render.id = local->id;
            render.position = local->position;
            render.velocity = local->velocity;
            render.flags = local->flags;
            render.coins = local->coins;
            render.local = true;
            render.name = player_name_;
            (void)0;
            render_players_.push_back(std::move(render));
        }
    }
    stats_.predicted_tick = world_ != nullptr ? world_->tick() : 0;
}

} // namespace t2d
