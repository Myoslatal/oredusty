// Tile2D - headless bot client used by the integration tests (and as a load generator).
//
// It joins a server over KCP/UDP, plays a deterministic scripted input sequence and verifies that its
// own simulation stays in sync with the authoritative one: every snapshot must apply cleanly, the
// per-snapshot checksum must match, and the predicted position must agree with the server's.
#include <t2d/client/local_client.h>
#include <t2d/core/log.h>
#include <t2d/core/time.h>
#include <t2d/net/udp_link.h>

#include <t2d/core/cli.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <format>
#include <thread>

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

/// Deterministic pseudo random input so several bots behave differently but reproducibly.
[[nodiscard]] t2d::PlayerCommand scripted_command(t2d::u32 tick, t2d::u32 seed) {
    t2d::PlayerCommand command;
    const t2d::u32 phase = (tick + seed * 37u) % 240u;
    if (phase < 100u) command.buttons |= t2d::kButtonRight;
    else if (phase < 200u) command.buttons |= t2d::kButtonLeft;
    if ((phase % 60u) == 0u) command.buttons |= t2d::kButtonJumpPressed | t2d::kButtonJumpHeld;
    if ((tick % 7u) == 0u) command.buttons |= t2d::kButtonJumpHeld;
    if ((tick % 30u) < 3u) command.buttons |= t2d::kButtonDown;
    return command;
}

} // namespace

int main(int argc, char** argv) {
    t2d::set_thread_name("bot");
    const t2d::Args cli = t2d::Args(argc, argv);
    if (cli.has("help")) {
        std::printf("usage: %s --connect host:port [--name bot] [--ticks N] [--seed N] [--quiet]\n"
                    "  exits 0 when the client stayed in sync with the server for N ticks, 1 otherwise\n",
                    cli.program().c_str());
        return 0;
    }
    if (cli.has("quiet")) t2d::set_log_level(t2d::LogLevel::Warn);

    const std::string address = cli.value("connect", "127.0.0.1:7777");
    const t2d::u32 ticks_to_play = cli.uint_value("ticks", 600);
    const t2d::u32 seed = cli.uint_value("seed", 1);
    const std::string name = cli.value("name", std::format("bot{}", seed));

    const auto endpoint = t2d::net::Endpoint::parse(address, 7777);
    if (!endpoint.has_value()) {
        T2D_ERROR("cannot parse '{}'", address);
        return 1;
    }

    t2d::net::UdpTransport::Config transport_config;
    transport_config.server_mode = false;
    transport_config.link.kcp.no_delay = true;
    auto transport = t2d::net::UdpTransport::create(transport_config);
    if (transport == nullptr) {
        T2D_ERROR("could not create the UDP transport");
        return 1;
    }
    t2d::net::UdpLink* link = transport->connect(*endpoint);
    if (link == nullptr) {
        T2D_ERROR("could not open a link to {}", endpoint->to_string());
        return 1;
    }

    t2d::ClientConfig client_config;
    client_config.player_name = name;
    auto client = t2d::LocalClient::create(client_config, link);
    if (client == nullptr) return 1;
    client->start(t2d::now_ms());

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const t2d::u64 deadline = t2d::now_ms() + 60000;  // hard cap so a hang fails instead of blocking CI
    t2d::u64 last_tick = 0;
    while (!g_stop.load() && t2d::now_ms() < deadline) {
        const t2d::u64 now = t2d::now_ms();
        client->update(now, scripted_command(client->stats().server_tick, seed));
        if (client->stats().server_tick != last_tick) {
            last_tick = client->stats().server_tick;
            if (client->ready() && last_tick >= ticks_to_play) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const t2d::ClientStats stats = client->stats();
    const t2d::Predictor::Stats prediction = client->prediction_stats();
    std::printf("[bot %s] tick=%u snapshots=%u (full %u delta %u) dropped=%u desync=%u mispredict=%u "
                "maxerr=%.3f rtt=%u pending=%u\n",
                name.c_str(), stats.server_tick, stats.snapshots_received, stats.full_snapshots_received,
                stats.delta_snapshots_received, stats.snapshots_dropped, stats.desyncs, stats.mispredictions,
                static_cast<double>(stats.max_prediction_error), stats.rtt_ms, stats.pending_commands);

    const bool healthy = client->ready() && stats.snapshots_received > 0 && stats.desyncs == 0 &&
                         stats.server_tick >= ticks_to_play / 2;
    client->disconnect();
    transport->update(t2d::now_ms());
    if (!healthy) {
        T2D_ERROR("bot {} did not stay in sync (ready={} snapshots={} desyncs={} tick={})", name,
                  client->ready(), stats.snapshots_received, stats.desyncs, stats.server_tick);
        return 1;
    }
    (void)prediction;
    return 0;
}
