// Single player, end to end, in one process: the server thread and the local client thread talk over
// the shared memory channel. This is the test that proves the core claim of the design - single
// player is not a special code path, it is the same client and the same protocol with another link.
#include <t2d/client/local_client.h>
#include <t2d/core/time.h>
#include <t2d/server/server_host.h>

#include <support/test_support.h>

#include <thread>

using namespace t2d;

namespace {

struct Rig {
    Scope<ServerHost> server;
    Scope<net::SharedLink> link;
    Scope<LocalClient> client;
    [[nodiscard]] bool valid() const { return server != nullptr && link != nullptr && client != nullptr; }
};

[[nodiscard]] TileMap make_flat_level() {
    TileMap map(48, 20, 16.0f);
    for (i32 x = 0; x < 48; ++x) {
        map.set(x, 16, 3);
        for (i32 y = 17; y < 20; ++y) map.set(x, y, 2);
    }
    for (i32 x = 20; x < 28; ++x) map.set(x, 12, 4);
    map.set(24, 10, 6);   // a coin on the raised platform
    return map;
}

[[nodiscard]] Rig make_rig(u32 snapshot_interval_ticks = 2) {
    Rig rig;
    ServerConfig config;
    config.port = 0;                 // no sockets at all: shared memory only
    config.local_client = true;
    config.snapshot_interval_ticks = snapshot_interval_ticks;
    // Pacing only: the simulation advances the same way per tick, so twenty seconds of gameplay fit
    // into a few hundred milliseconds. A dedicated server would use the default 60.
    config.tick_rate = 240;
    config.world.map = make_flat_level();

    rig.server = ServerHost::create(config);
    if (rig.server == nullptr) return rig;
    rig.link = Scope<net::SharedLink>(rig.server->take_local_client_link());
    if (rig.link == nullptr) return rig;
    rig.server->start();

    ClientConfig client_config;
    client_config.player_name = "tester";
    rig.client = LocalClient::create(client_config, rig.link.get());
    if (rig.client == nullptr) return rig;
    rig.client->start(now_ms());
    return rig;
}

/// Plays until the server reached \p ticks, with a deterministic input pattern.
void play(LocalClient& client, u32 ticks, u32 seed = 7) {
    const u64 deadline = now_ms() + 30000;
    while (client.stats().server_tick < ticks && now_ms() < deadline) {
        PlayerCommand command;
        const u32 phase = (client.stats().server_tick + seed) % 120u;
        if (phase < 60u) command.buttons |= kButtonRight;
        else command.buttons |= kButtonLeft;
        if (phase % 45u == 0u) command.buttons |= kButtonJumpPressed | kButtonJumpHeld;
        if (phase % 45u < 5u) command.buttons |= kButtonJumpHeld;
        client.update(now_ms(), command);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

} // namespace

T2D_TEST(shared_memory_link_carries_the_handshake_and_the_level) {
    Rig rig = make_rig();
    if (!rig.valid()) T2D_SKIP("the shared memory channel could not be created");

    T2D_CHECK(rig.link->shared_bytes() > 0u);
    T2D_CHECK_FALSE(rig.link->is_server_endpoint());
    T2D_CHECK(rig.server->local_link() != nullptr);
    T2D_CHECK(rig.server->local_link()->is_server_endpoint());
    T2D_CHECK_EQ(std::string(rig.link->kind()), std::string("shared-memory"));

    play(*rig.client, 40);

    T2D_CHECK_MSG(rig.client->ready(), "the client never finished loading the level");
    T2D_CHECK(rig.client->connected());
    T2D_REQUIRE(rig.client->map() != nullptr);
    T2D_CHECK_EQ(rig.client->map()->width(), 48);
    T2D_CHECK_EQ(rig.client->map()->height(), 20);
    T2D_CHECK(rig.client->world() != nullptr);
    T2D_CHECK_EQ(rig.client->world()->pickups().size(), rig.server->world().pickups().size());
    T2D_CHECK_GT(rig.client->stats().snapshots_received, 5u);
    T2D_CHECK_GT(rig.client->stats().map_chunks_received, 0u);
    T2D_CHECK_EQ(rig.client->stats().desyncs, 0u);
}

T2D_TEST(single_player_client_predicts_and_never_desyncs) {
    Rig rig = make_rig();
    if (!rig.valid()) T2D_SKIP("the shared memory channel could not be created");

    play(*rig.client, 180);
    T2D_REQUIRE(rig.client->ready());

    const ClientStats& stats = rig.client->stats();
    const ServerStats server_stats = rig.server->stats();
    T2D_CHECK_MSG(stats.server_tick >= 150u, "the server tick only reached {}", stats.server_tick);
    T2D_CHECK_GT(server_stats.ticks, 150ull);
    T2D_CHECK_EQ(stats.desyncs, 0u);
    T2D_CHECK_GT(stats.full_snapshots_received, 0u);
    T2D_CHECK_GT(stats.delta_snapshots_received, 0u);
    T2D_CHECK_GT(server_stats.delta_snapshots_sent, 0ull);
    T2D_CHECK_GT(server_stats.commands_received, 100ull);

    const World* world = rig.client->world();
    T2D_REQUIRE(world != nullptr);
    const PlayerState* local = world->find_player(rig.client->local_player_id());
    T2D_REQUIRE(local != nullptr);
    T2D_CHECK_MSG(local->position.x > 20.0f, "the player barely moved (x={:.2f})",
                  static_cast<f64>(local->position.x));
    T2D_CHECK_FALSE(world->map().overlaps_solid(local->bounds(world->tuning()), world->tileset()));

    T2D_CHECK_MSG(stats.max_prediction_error < 1.5f, "prediction error reached {:.3f}",
                  static_cast<f64>(stats.max_prediction_error));
    T2D_CHECK_MSG(stats.mispredictions <= 2u, "{} mispredictions over 180 ticks", stats.mispredictions);
    T2D_CHECK_GT(rig.client->prediction_stats().reconciliations, 0ull);
}

T2D_TEST(server_keeps_running_when_the_client_stops_sending) {
    Rig rig = make_rig();
    if (!rig.valid()) T2D_SKIP("the shared memory channel could not be created");
    play(*rig.client, 60);
    T2D_CHECK_EQ(rig.server->world().players().size(), 1u);

    for (u32 i = 0; i < 30; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const ServerStats stats = rig.server->stats();
    T2D_CHECK_GT(stats.tick, 60u);
    T2D_CHECK_EQ(rig.server->world().players().size(), 1u);
}

T2D_TEST_MAIN
