// Snapshot stream tests: the wire format and the delta merge, without a single thread or sleep.
//
// These are the tests that would have caught the desync that the in-process integration test was
// reporting: the server's full snapshots were fine, but decoding a delta discarded the per-entity
// field masks, so the client's merge overwrote every field the sender had left out with the
// decoder's default value (flags always ended up as kPlayerAlive, i.e. "not on ground").
#include <t2d/sim/snapshot.h>

#include <support/test_support.h>

#include <algorithm>
#include <vector>

using namespace t2d;

namespace {

[[nodiscard]] TileMap make_flat_level() {
    TileMap map(48, 20, 16.0f);
    for (i32 x = 0; x < 48; ++x) {
        map.set(x, 16, 3);
        for (i32 y = 17; y < 20; ++y) map.set(x, y, 2);
    }
    for (i32 x = 20; x < 28; ++x) map.set(x, 12, 4);
    map.set(24, 10, 6); // a coin on the raised platform
    return map;
}

[[nodiscard]] WorldConfig make_config() {
    WorldConfig config;
    config.map = make_flat_level();
    return config;
}

/// The same input pattern the integration test plays, as a pure function of the tick.
[[nodiscard]] PlayerCommand scripted_command(u32 tick, u32 seed) {
    PlayerCommand command;
    const u32 phase = (tick + seed) % 120u;
    if (phase < 60u) command.buttons |= kButtonRight;
    else command.buttons |= kButtonLeft;
    if (phase % 45u == 0u) command.buttons |= kButtonJumpPressed | kButtonJumpHeld;
    if (phase % 45u < 5u) command.buttons |= kButtonJumpHeld;
    command.tick = tick;
    return command;
}

/// Runs a world for \p ticks so it reaches an interesting state (moving, on the ground, counters set).
void warm_up(World& world, PlayerId player, u32 ticks, u32 seed = 7) {
    for (u32 i = 0; i < ticks; ++i) {
        world.submit_command(player, scripted_command(world.tick() + 1, seed));
        world.step();
    }
}

} // namespace

T2D_TEST(a_full_snapshot_round_trips_through_the_wire_format) {
    World server(make_config());
    const PlayerId player = server.spawn_player();
    warm_up(server, player, 40);

    const Snapshot captured = capture_snapshot(server, 3, 1234);
    const std::vector<u8> bytes = encode_snapshot(captured);
    T2D_CHECK_GT(bytes.size(), 0u);

    Snapshot decoded;
    T2D_REQUIRE(decode_snapshot(bytes, decoded));
    T2D_CHECK_EQ(static_cast<int>(decoded.kind), static_cast<int>(SnapshotKind::Full));
    T2D_CHECK_EQ(decoded.tick, captured.tick);
    T2D_CHECK_EQ(decoded.acked_command_tick, 3u);
    T2D_CHECK_EQ(decoded.server_time_ms, 1234ull);
    T2D_CHECK_EQ(decoded.checksum, captured.checksum);
    T2D_REQUIRE(decoded.players.size() == captured.players.size());
    T2D_REQUIRE(decoded.pickups.size() == captured.pickups.size());
    T2D_CHECK_EQ(static_cast<u32>(captured.players[0].flags), static_cast<u32>(decoded.players[0].flags));
    // A full snapshot describes every field, so it carries no masks.
    T2D_CHECK(decoded.player_masks.empty());
    T2D_CHECK(decoded.pickup_masks.empty());

    // Applying it to a fresh world must reproduce the server's checksum exactly.
    World client(make_config());
    apply_snapshot(client, decoded);
    T2D_CHECK_EQ(capture_snapshot(client, 0, 0).checksum, captured.checksum);
}

T2D_TEST(a_delta_leaves_the_fields_it_does_not_describe_alone) {
    World server(make_config());
    const PlayerId player = server.spawn_player();
    warm_up(server, player, 20);

    const Snapshot baseline = capture_snapshot(server, 0, 0);
    T2D_REQUIRE(baseline.players.size() == 1);
    // The regression only exists when the baseline has state a "default" PlayerState would not have.
    T2D_REQUIRE((baseline.players[0].flags & kPlayerOnGround) != 0u);
    T2D_REQUIRE(baseline.players[0].spawn_position != Vec2{});

    // A hand built delta that moves the player and says nothing else: exactly what the encoder
    // produces for "only the position changed".
    Snapshot delta;
    delta.kind = SnapshotKind::Delta;
    delta.tick = baseline.tick + 1;
    delta.baseline_tick = baseline.tick;
    delta.checksum = baseline.checksum;
    PlayerState moved = baseline.players[0];
    moved.position = Vec2{moved.position.x + 4.0f, moved.position.y};
    // Positions travel as 1/16 px fixed point, so the received value is the quantised one.
    const auto on_the_wire = [](f32 value) {
        return static_cast<f32>(static_cast<i32>(value * 16.0f + (value >= 0.0f ? 0.5f : -0.5f))) * (1.0f / 16.0f);
    };
    delta.players.push_back(moved);
    delta.player_masks.push_back(kFieldPosition);

    const std::vector<u8> bytes = encode_snapshot(delta);
    Snapshot over_the_wire;
    T2D_REQUIRE(decode_snapshot(bytes, over_the_wire));
    T2D_REQUIRE(over_the_wire.player_masks.size() == over_the_wire.players.size());
    T2D_CHECK_EQ(static_cast<u32>(over_the_wire.player_masks[0]), static_cast<u32>(kFieldPosition));

    Snapshot applied;
    T2D_REQUIRE(apply_delta(baseline, over_the_wire, applied));
    T2D_REQUIRE(applied.players.size() == 1);
    const PlayerState& result = applied.players[0];
    T2D_CHECK_EQ(result.position.x, on_the_wire(moved.position.x));
    T2D_CHECK_EQ(result.position.y, on_the_wire(moved.position.y));
    // Untouched fields must survive: this is what used to be zeroed by a blind merge.
    T2D_CHECK_EQ(static_cast<u32>(result.flags), static_cast<u32>(baseline.players[0].flags));
    T2D_CHECK_EQ(result.coins, baseline.players[0].coins);
    T2D_CHECK_EQ(static_cast<u32>(result.coyote_ticks), static_cast<u32>(baseline.players[0].coyote_ticks));
    T2D_CHECK_EQ(static_cast<u32>(result.jump_buffer_ticks), static_cast<u32>(baseline.players[0].jump_buffer_ticks));
    T2D_CHECK_EQ(static_cast<u32>(result.respawn_ticks), static_cast<u32>(baseline.players[0].respawn_ticks));
    T2D_CHECK_EQ(result.spawn_position.x, baseline.players[0].spawn_position.x);
    T2D_CHECK_EQ(result.spawn_position.y, baseline.players[0].spawn_position.y);
    T2D_CHECK_EQ(applied.tick, delta.tick);
}

T2D_TEST(a_delta_without_parallel_masks_is_refused) {
    World server(make_config());
    const PlayerId player = server.spawn_player();
    warm_up(server, player, 20);

    const Snapshot baseline = capture_snapshot(server, 0, 0);
    Snapshot delta;
    delta.kind = SnapshotKind::Delta;
    delta.tick = baseline.tick + 1;
    delta.baseline_tick = baseline.tick;
    delta.players.push_back(baseline.players[0]); // no matching mask

    Snapshot applied;
    T2D_CHECK_FALSE(apply_delta(baseline, delta, applied));

    // A delta against the wrong baseline is refused too, so the client knows to ask for a full one.
    Snapshot orphan = delta;
    orphan.player_masks.push_back(kFieldPosition);
    orphan.baseline_tick = baseline.tick + 77;
    T2D_CHECK_FALSE(apply_delta(baseline, orphan, applied));
}

T2D_TEST(a_long_delta_stream_keeps_the_client_bit_identical_to_the_server) {
    World server(make_config());
    const PlayerId player = server.spawn_player();

    // The client's world starts from the level as the server sends it (after the coin markers were
    // turned into pickups), which is why it has no pickups of its own.
    const auto client_map = TileMap::deserialize(server.map().serialize());
    T2D_REQUIRE(client_map.has_value());
    WorldConfig client_config = make_config();
    client_config.map = *client_map;
    client_config.map.set(24, 10, kEmptyTile); // the marker was consumed by the server's constructor
    World client(client_config);
    T2D_CHECK_EQ(client.pickups().size(), 0u);

    constexpr u32 kSnapshotInterval = 2;
    constexpr u32 kFullInterval = 120;
    constexpr u32 kHistoryLimit = 64;
    constexpr u32 kAckTooOld = 90;
    constexpr u32 kTicks = 360;

    std::vector<Snapshot> history;
    Snapshot client_baseline;
    u32 acked = 0;
    bool wants_full = true;
    u32 fulls = 0;
    u32 deltas = 0;
    u32 mismatches = 0;
    u32 applied_snapshots = 0;

    for (u32 step = 0; step < kTicks; ++step) {
        server.submit_command(player, scripted_command(server.tick() + 1, 7));
        server.step();
        if (server.tick() % kSnapshotInterval != 0) continue;

        const Snapshot current = capture_snapshot(server, 0, 0);
        history.push_back(current);
        while (history.size() > kHistoryLimit) history.erase(history.begin());

        Snapshot outgoing = current;
        const PlayerState* state = server.find_player(player);
        outgoing.acked_command_tick = state != nullptr ? state->last_command_tick : 0;
        const bool force_full = wants_full || (current.tick % kFullInterval) == 0 || acked == 0 ||
                                current.tick < acked || current.tick - acked > kAckTooOld;
        if (!force_full) {
            const Snapshot* baseline = nullptr;
            for (const Snapshot& candidate : history) {
                if (candidate.tick == acked) baseline = &candidate;
            }
            if (baseline != nullptr) {
                Snapshot delta = make_delta(outgoing, *baseline);
                // make_delta must never lose the mask/entry pairing: apply_delta refuses such a delta.
                T2D_CHECK_EQ(delta.player_masks.size(), delta.players.size());
                T2D_CHECK_EQ(delta.pickup_masks.size(), delta.pickups.size());
                outgoing = std::move(delta);
            }
        }
        if (outgoing.kind == SnapshotKind::Full) wants_full = false;

        const std::vector<u8> bytes = encode_snapshot(outgoing);
        Snapshot received;
        T2D_REQUIRE(decode_snapshot(bytes, received));

        Snapshot resolved;
        if (received.kind == SnapshotKind::Full) {
            ++fulls;
            resolved = received;
        } else {
            ++deltas;
            T2D_REQUIRE(apply_delta(client_baseline, received, resolved));
        }

        apply_snapshot(client, resolved);
        ++applied_snapshots;
        const u64 client_checksum = capture_snapshot(client, resolved.acked_command_tick, 0).checksum;
        if (client_checksum != resolved.checksum) ++mismatches;

        client_baseline = resolved;
        acked = resolved.tick;
    }

    T2D_CHECK_GT(fulls, 2u);
    T2D_CHECK_GT(deltas, 100u);
    T2D_CHECK_EQ(mismatches, 0u);
    T2D_CHECK_EQ(applied_snapshots, kTicks / kSnapshotInterval);
    T2D_CHECK_EQ(client.tick(), server.tick());
    const PlayerState* server_player = server.find_player(player);
    const PlayerState* client_player = client.find_player(player);
    T2D_REQUIRE(server_player != nullptr);
    T2D_REQUIRE(client_player != nullptr);
    T2D_CHECK_EQ(static_cast<u32>(client_player->flags), static_cast<u32>(server_player->flags));
    T2D_CHECK_EQ(client_player->coins, server_player->coins);
    T2D_CHECK_EQ(client.pickups().size(), server.pickups().size());
    T2D_CHECK_EQ(capture_snapshot(client, 0, 0).checksum, capture_snapshot(server, 0, 0).checksum);
}

T2D_TEST(delta_encoding_is_small_next_to_a_full_snapshot) {
    World server(make_config());
    const PlayerId player = server.spawn_player();
    warm_up(server, player, 200);

    const Snapshot before = capture_snapshot(server, 0, 0);
    warm_up(server, player, 1);
    const Snapshot after = capture_snapshot(server, 0, 0);
    const Snapshot delta = make_delta(after, before);
    T2D_CHECK_EQ(static_cast<int>(delta.kind), static_cast<int>(SnapshotKind::Delta));
    T2D_CHECK_LT(encode_snapshot(delta).size(), encode_snapshot(after).size());
}

T2D_TEST_MAIN
