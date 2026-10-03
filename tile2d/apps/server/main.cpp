// Tile2D - dedicated server: no window, no renderer, no local client thread.
#include <t2d/core/cli.h>
#include <t2d/core/log.h>
#include <t2d/core/time.h>
#include <t2d/server/server_host.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

[[nodiscard]] std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (char character : text) {
        if (character == '\n') {
            if (!current.empty() && current.back() == '\r') current.pop_back();
            if (!current.empty()) lines.push_back(current);
            current.clear();
        } else {
            current.push_back(character);
        }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

[[nodiscard]] t2d::TileMap load_level(const std::string& path) {
    if (!path.empty()) {
        if (const auto text = t2d::read_text(path); text.has_value()) {
            if (const auto map = t2d::TileMap::from_ascii(split_lines(*text), t2d::TileMap::default_legend(), 16.0f);
                map.has_value()) {
                return *map;
            }
            T2D_WARN("could not parse level '{}'", path);
        } else {
            T2D_WARN("could not read level '{}'", path);
        }
    }
    t2d::TileMap map(48, 24, 16.0f);
    for (t2d::i32 x = 0; x < 48; ++x) {
        map.set(x, 20, 3);
        for (t2d::i32 y = 21; y < 24; ++y) map.set(x, y, 2);
    }
    for (t2d::i32 x = 8; x < 12; ++x) map.set(x, 17, 4);
    map.set(20, 19, 6);
    return map;
}

} // namespace

int main(int argc, char** argv) {
    t2d::set_thread_name("main");
    const t2d::Args args(argc, argv);
    if (args.has("help")) {
        std::printf("usage: %s [--port N] [--level path] [--max-players N] [--ticks N] [--quiet]\n"
                    "  --port N           UDP port to listen on (default 7777)\n"
                    "  --level path       ASCII level file (default: built-in arena)\n"
                    "  --max-players N    player limit (default 8)\n"
                    "  --ticks N          stop after N simulation ticks (used by the tests)\n"
                    "  --snapshot-rate N  send a snapshot every N ticks (default 2)\n"
                    "  --tick-rate N      simulation ticks per second (default 60)\n",
                    args.program().c_str());
        return 0;
    }
    if (args.has("quiet")) t2d::set_log_level(t2d::LogLevel::Warn);

    t2d::ServerConfig config;
    config.port = static_cast<t2d::u16>(args.uint_value("port", 7777));
    config.max_players = args.uint_value("max-players", 8);
    config.snapshot_interval_ticks = args.uint_value("snapshot-rate", 2);
    const t2d::u32 tick_rate = args.uint_value("tick-rate", t2d::kTickRate);
    if (tick_rate < 1 || tick_rate > 1000) {
        T2D_ERROR("--tick-rate must be between 1 and 1000");
        return 1;
    }
    config.tick_rate = tick_rate;
    config.local_client = false;
    config.world.map = load_level(args.value("level"));
    config.name = "tile2d dedicated";

    auto server = t2d::ServerHost::create(config);
    if (server == nullptr) {
        T2D_ERROR("the server could not start (is the port already in use?)");
        return 1;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    server->start();

    const t2d::u32 max_ticks = args.uint_value("ticks", 0);
    t2d::u64 last_report = t2d::now_ms();
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const t2d::ServerStats stats = server->stats();
        const t2d::u64 now = t2d::now_ms();
        if (now - last_report >= 5000) {
            last_report = now;
            T2D_INFO("tick {} | {} player(s) ({} local, {} remote) | {:.2f} ms/tick | {:.1f} KiB/s out",
                     stats.tick, stats.players, stats.local_players, stats.remote_players,
                     static_cast<double>(stats.average_tick_ms), stats.bytes_sent_per_second / 1024.0);
        }
        if (max_ticks != 0 && stats.tick >= max_ticks) break;
    }

    server->stop();
    const t2d::ServerStats stats = server->stats();
    std::printf("[server] ticks=%llu snapshots=%llu (full %llu delta %llu) commands=%llu players=%u\n",
                static_cast<unsigned long long>(stats.ticks), static_cast<unsigned long long>(stats.snapshots_sent),
                static_cast<unsigned long long>(stats.full_snapshots_sent),
                static_cast<unsigned long long>(stats.delta_snapshots_sent),
                static_cast<unsigned long long>(stats.commands_received), stats.players);
    return 0;
}
