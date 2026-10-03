// Protocol, byte stream and shared memory channel tests: the three layers that every message of the
// framework passes through, exercised directly instead of through a running game.
#include <t2d/core/bitstream.h>
#include <t2d/net/protocol.h>
#include <t2d/net/shm_link.h>
#include <t2d/sim/tilemap.h>

#include <support/test_support.h>

#include <cstring>
#include <string>
#include <vector>

using namespace t2d;

namespace {

[[nodiscard]] std::vector<u8> bytes_of(std::string_view text) {
    return std::vector<u8>(text.begin(), text.end());
}

[[nodiscard]] std::vector<u8> receive_one(net::SharedLink& link) {
    std::vector<u8> buffer(net::kMaxMessageSize);
    const usize size = link.receive(Span<u8>(buffer.data(), buffer.size()));
    buffer.resize(size);
    return buffer;
}

} // namespace

T2D_TEST(varints_round_trip_at_their_boundaries) {
    const u32 values[] = {0u, 1u, 127u, 128u, 300u, 16383u, 16384u, 0x001FFFFFu, 0x7FFFFFFFu, 0xFFFFFFFFu};
    for (const u32 value : values) {
        std::vector<u8> buffer;
        ByteWriter writer(buffer);
        writer.write_varint(value);
        T2D_CHECK_GT(buffer.size(), 0u);
        T2D_CHECK_LT(buffer.size(), 6u);
        ByteReader reader(ConstSpan<const u8>(buffer.data(), buffer.size()));
        T2D_CHECK_EQ(reader.read_varint(), value);
        T2D_CHECK(reader.ok());
    }
}

T2D_TEST(zigzag_round_trips_signed_values) {
    for (const i32 value : {0, 1, -1, 2, -2, 1000000, -1000000, 2147483647, -2147483647 - 1}) {
        T2D_CHECK_EQ(zigzag_decode(zigzag_encode(value)), value);
    }
    // The encoding itself must be compact for small magnitudes and never negative.
    T2D_CHECK_EQ(zigzag_encode(0), 0u);
    T2D_CHECK_EQ(zigzag_encode(-1), 1u);
    T2D_CHECK_EQ(zigzag_encode(1), 2u);
}

T2D_TEST(fixed_point_positions_quantise_to_a_sixteenth_of_a_pixel) {
    std::vector<u8> buffer;
    ByteWriter writer(buffer);
    writer.write_fixed_16(0.0f);
    writer.write_fixed_16(1.5f);
    writer.write_fixed_16(-2.25f);
    writer.write_fixed_16(100.0625f); // exactly 1601 sixteenths
    ByteReader reader(ConstSpan<const u8>(buffer.data(), buffer.size()));
    T2D_CHECK_EQ(reader.read_fixed_16(), 0.0f);
    T2D_CHECK_EQ(reader.read_fixed_16(), 1.5f);
    T2D_CHECK_EQ(reader.read_fixed_16(), -2.25f);
    T2D_CHECK_EQ(reader.read_fixed_16(), 100.0625f);
    T2D_CHECK(reader.ok());

    // Finer detail cannot survive the wire format: values round to the nearest sixteenth, with ties
    // away from zero.
    const auto round_trip = [](f32 value) {
        std::vector<u8> quantised;
        ByteWriter quantising_writer(quantised);
        quantising_writer.write_fixed_16(value);
        ByteReader quantising_reader(ConstSpan<const u8>(quantised.data(), quantised.size()));
        return quantising_reader.read_fixed_16();
    };
    T2D_CHECK_EQ(round_trip(3.0f + 1.0f / 64.0f), 3.0f);       // 48.25 -> 48
    T2D_CHECK_EQ(round_trip(3.0f + 1.0f / 32.0f), 3.0625f);    // 48.5  -> 49, ties away from zero
    T2D_CHECK_EQ(round_trip(-3.0f - 1.0f / 32.0f), -3.0625f);  // -48.5 -> -49
    T2D_CHECK_EQ(round_trip(-3.0f - 1.0f / 64.0f), -3.0f);
}

T2D_TEST(the_external_writer_reports_overflow_instead_of_writing_past_the_buffer) {
    u8 storage[8];
    std::memset(storage, 0xAB, sizeof(storage));
    ByteWriter writer(storage, 4);
    writer.write_u32(0x11223344u);
    T2D_CHECK(writer.valid());
    T2D_CHECK_EQ(writer.size(), 4u);

    writer.write_u32(0x55667788u); // does not fit
    T2D_CHECK_FALSE(writer.valid());
    T2D_CHECK_EQ(writer.size(), 4u);
    for (usize i = 4; i < sizeof(storage); ++i) {
        T2D_CHECK_MSG(storage[i] == 0xABu, "byte {} past the capacity was overwritten", i);
    }
    // A writer that overflowed must not keep writing either.
    writer.write_u8(1u);
    for (usize i = 4; i < sizeof(storage); ++i) {
        T2D_CHECK(storage[i] == 0xABu);
    }
}

T2D_TEST(a_truncated_read_is_rejected) {
    const std::vector<u8> truncated = bytes_of("\x01\x02");
    ByteReader reader(ConstSpan<const u8>(truncated.data(), truncated.size()));
    T2D_CHECK_EQ(reader.read_u32(), 0u);
    T2D_CHECK_FALSE(reader.ok());
    T2D_CHECK_EQ(reader.remaining(), 0u);
    // Everything after the failure stays safe to call.
    T2D_CHECK_EQ(reader.read_varint(), 0u);
    T2D_CHECK(reader.read_string().empty());

    // A varint that never terminates is malformed, not a huge number.
    std::vector<u8> endless(8, 0x80u);
    ByteReader endless_reader(ConstSpan<const u8>(endless.data(), endless.size()));
    T2D_CHECK_EQ(endless_reader.read_varint(), 0u);
    T2D_CHECK_FALSE(endless_reader.ok());
}

T2D_TEST(every_message_payload_round_trips) {
    {
        net::HelloMessage message;
        message.protocol_version = net::kProtocolVersion;
        message.name = "player one";
        message.client_token = 0x1122334455667788ull;
        net::HelloMessage out;
        T2D_REQUIRE(net::decode_hello(net::encode_hello(message), out));
        T2D_CHECK_EQ(out.protocol_version, message.protocol_version);
        T2D_CHECK_EQ(out.name, message.name);
        T2D_CHECK_EQ(out.client_token, message.client_token);
    }
    {
        net::WelcomeMessage message;
        message.player_id = 7;
        message.tick = 12345;
        message.tick_rate = 240;
        message.spawn = Vec2{12.5f, -3.25f};
        message.map_width = 64;
        message.map_height = 32;
        message.tile_size = 16.0f;
        message.client_token = 99;
        net::WelcomeMessage out;
        T2D_REQUIRE(net::decode_welcome(net::encode_welcome(message), out));
        T2D_CHECK_EQ(out.player_id, message.player_id);
        T2D_CHECK_EQ(out.tick, message.tick);
        T2D_CHECK_EQ(out.tick_rate, message.tick_rate);
        T2D_CHECK_EQ(out.spawn.x, message.spawn.x);
        T2D_CHECK_EQ(out.spawn.y, message.spawn.y);
        T2D_CHECK_EQ(out.map_width, message.map_width);
        T2D_CHECK_EQ(out.tile_size, message.tile_size);
        T2D_CHECK_EQ(out.client_token, message.client_token);
    }
    {
        net::RejectMessage message{net::RejectReason::ProtocolMismatch, "protocol 2 expected 1"};
        net::RejectMessage out;
        T2D_REQUIRE(net::decode_reject(net::encode_reject(message), out));
        T2D_CHECK_EQ(static_cast<u32>(out.reason), static_cast<u32>(message.reason));
        T2D_CHECK_EQ(out.text, message.text);
    }
    {
        net::PlayerJoinedMessage message{3, "bob"};
        net::PlayerJoinedMessage out;
        T2D_REQUIRE(net::decode_player_joined(net::encode_player_joined(message), out));
        T2D_CHECK_EQ(out.player_id, message.player_id);
        T2D_CHECK_EQ(out.name, message.name);
    }
    {
        net::PlayerLeftMessage message{4, 2};
        net::PlayerLeftMessage out;
        T2D_REQUIRE(net::decode_player_left(net::encode_player_left(message), out));
        T2D_CHECK_EQ(out.player_id, message.player_id);
        T2D_CHECK_EQ(out.reason, message.reason);
    }
    {
        net::CommandMessage message;
        message.command.tick = 4242;
        message.command.buttons = kButtonRight | kButtonJumpPressed;
        message.last_snapshot_tick = 4240;
        net::CommandMessage out;
        T2D_REQUIRE(net::decode_command(net::encode_command(message), out));
        T2D_CHECK_EQ(out.command.tick, message.command.tick);
        T2D_CHECK_EQ(static_cast<u32>(out.command.buttons), static_cast<u32>(message.command.buttons));
        T2D_CHECK_EQ(out.last_snapshot_tick, message.last_snapshot_tick);
    }
    {
        net::PingMessage message{5, 123456};
        net::PingMessage out;
        T2D_REQUIRE(net::decode_ping(net::encode_ping(message), out));
        T2D_CHECK_EQ(out.sequence, message.sequence);
        T2D_CHECK_EQ(out.sender_time_ms, message.sender_time_ms);
    }
    {
        net::PongMessage message{6, 111, 222};
        net::PongMessage out;
        T2D_REQUIRE(net::decode_pong(net::encode_pong(message), out));
        T2D_CHECK_EQ(out.sequence, message.sequence);
        T2D_CHECK_EQ(out.sender_time_ms, message.sender_time_ms);
        T2D_CHECK_EQ(out.responder_time_ms, message.responder_time_ms);
    }
    {
        net::DisconnectMessage message{3};
        net::DisconnectMessage out;
        T2D_REQUIRE(net::decode_disconnect(net::encode_disconnect(message), out));
        T2D_CHECK_EQ(out.reason, message.reason);
    }
    {
        net::MapDataMessage message;
        message.chunk_index = 3;
        message.chunk_count = 9;
        message.payload.assign(4096, 0x5Au);
        net::MapDataMessage out;
        T2D_REQUIRE(net::decode_map_data(net::encode_map_data(message), out));
        T2D_CHECK_EQ(out.chunk_index, message.chunk_index);
        T2D_CHECK_EQ(out.chunk_count, message.chunk_count);
        T2D_CHECK_EQ(out.payload.size(), message.payload.size());
        T2D_CHECK(out.payload == message.payload);
    }
    {
        net::ServerStatsMessage message;
        message.tick = 900;
        message.players = 4;
        message.tick_ms = 1.25f;
        message.bytes_in_per_second = 2048;
        message.bytes_out_per_second = 4096;
        net::ServerStatsMessage out;
        T2D_REQUIRE(net::decode_server_stats(net::encode_server_stats(message), out));
        T2D_CHECK_EQ(out.tick, message.tick);
        T2D_CHECK_EQ(out.players, message.players);
        T2D_CHECK_EQ(out.tick_ms, message.tick_ms);
        T2D_CHECK_EQ(out.bytes_out_per_second, message.bytes_out_per_second);
    }
}

T2D_TEST(framing_rejects_truncated_and_mismatched_frames) {
    const std::vector<u8> payload = bytes_of("payload");
    const std::vector<u8> frame = net::encode_message(net::MessageType::Snapshot, payload);
    T2D_CHECK_EQ(frame.size(), net::kMessageHeaderSize + payload.size());

    net::MessageType type = net::MessageType::None;
    ConstSpan<const u8> decoded;
    T2D_REQUIRE(net::decode_message(ConstSpan<const u8>(frame.data(), frame.size()), type, decoded));
    T2D_CHECK_EQ(static_cast<u32>(type), static_cast<u32>(net::MessageType::Snapshot));
    T2D_CHECK_EQ(decoded.size(), payload.size());
    T2D_CHECK(std::memcmp(decoded.data(), payload.data(), payload.size()) == 0);

    // Shorter than a header.
    T2D_CHECK_FALSE(net::decode_message(ConstSpan<const u8>(frame.data(), 2), type, decoded));
    // The size field promises more than the buffer holds.
    T2D_CHECK_FALSE(net::decode_message(ConstSpan<const u8>(frame.data(), frame.size() - 1), type, decoded));
    // A frame that is longer than it claims is fine: the extra bytes are simply not part of it.
    std::vector<u8> padded = frame;
    padded.push_back(0xFFu);
    T2D_REQUIRE(net::decode_message(ConstSpan<const u8>(padded.data(), padded.size()), type, decoded));
    T2D_CHECK_EQ(decoded.size(), payload.size());
}

T2D_TEST(oversized_payloads_are_refused_instead_of_being_truncated) {
    std::vector<u8> oversized(net::kMaxMessageSize + 1u, 0x11u);
    const std::vector<u8> frame = net::encode_message(net::MessageType::Snapshot,
                                                      ConstSpan<const u8>(oversized.data(), oversized.size()));
    T2D_CHECK(frame.empty());

    std::vector<u8> accepted(net::kMaxMessageSize, 0x22u);
    const std::vector<u8> ok = net::encode_message(net::MessageType::Snapshot,
                                                   ConstSpan<const u8>(accepted.data(), accepted.size()));
    T2D_CHECK_EQ(ok.size(), net::kMessageHeaderSize + net::kMaxMessageSize);
}

T2D_TEST(a_level_survives_chunking_and_reassembly) {
    TileMap map(48, 24, 16.0f);
    for (i32 x = 0; x < 48; ++x) {
        map.set(x, 20, 3);
        map.set(x, 21, 2);
        if (x % 7 == 0) map.set(x, 12, 4);
    }
    const std::vector<u8> serialised = map.serialize();
    T2D_REQUIRE(!serialised.empty());

    const u16 chunk_count = static_cast<u16>((serialised.size() + net::kMapChunkSize - 1) / net::kMapChunkSize);
    T2D_CHECK_GE(chunk_count, 1u);
    std::vector<std::vector<u8>> received(chunk_count);
    for (u16 index = 0; index < chunk_count; ++index) {
        const usize offset = index * net::kMapChunkSize;
        const usize size = std::min(net::kMapChunkSize, serialised.size() - offset);
        net::MapDataMessage message;
        message.chunk_index = index;
        message.chunk_count = chunk_count;
        message.payload.assign(serialised.begin() + static_cast<isize>(offset),
                               serialised.begin() + static_cast<isize>(offset + size));
        const std::vector<u8> payload = net::encode_map_data(message);
        const std::vector<u8> frame = net::encode_message(net::MessageType::MapData, payload);
        net::MessageType type = net::MessageType::None;
        ConstSpan<const u8> decoded;
        T2D_REQUIRE(net::decode_message(ConstSpan<const u8>(frame.data(), frame.size()), type, decoded));
        T2D_CHECK_EQ(static_cast<u32>(type), static_cast<u32>(net::MessageType::MapData));
        net::MapDataMessage out;
        T2D_REQUIRE(net::decode_map_data(decoded, out));
        T2D_CHECK_EQ(out.chunk_count, chunk_count);
        received[out.chunk_index] = out.payload;
    }

    std::vector<u8> reassembled;
    for (const std::vector<u8>& chunk : received) reassembled.insert(reassembled.end(), chunk.begin(), chunk.end());
    T2D_CHECK(reassembled == serialised);
    const auto restored = TileMap::deserialize(reassembled);
    T2D_REQUIRE(restored.has_value());
    T2D_CHECK_EQ(restored->width(), map.width());
    T2D_CHECK_EQ(restored->height(), map.height());
    T2D_CHECK_EQ(restored->at(0, 20), map.at(0, 20));
    T2D_CHECK_EQ(restored->at(21, 12), map.at(21, 12));
}

T2D_TEST(truncated_payloads_are_rejected_by_every_decoder) {
    net::HelloMessage hello;
    hello.name = "abcdefgh";
    const std::vector<u8> hello_payload = net::encode_hello(hello);
    for (usize size = 0; size < hello_payload.size(); ++size) {
        net::HelloMessage out;
        T2D_CHECK_MSG(!net::decode_hello(ConstSpan<const u8>(hello_payload.data(), size), out),
                      "a {} byte hello payload was accepted", size);
    }

    net::MapDataMessage map_data;
    map_data.chunk_index = 1;
    map_data.chunk_count = 2;
    map_data.payload.assign(64, 0x7Fu);
    const std::vector<u8> map_payload = net::encode_map_data(map_data);
    for (usize size = 0; size < map_payload.size(); ++size) {
        net::MapDataMessage out;
        T2D_CHECK(!net::decode_map_data(ConstSpan<const u8>(map_payload.data(), size), out));
    }

    // A payload that claims a longer string than it carries is rejected, not truncated.
    std::vector<u8> lying;
    ByteWriter writer(lying);
    writer.write_u16(net::kProtocolVersion);
    writer.write_u64(0);
    writer.write_varint(200); // says 200 bytes follow
    writer.write_u8('a');
    net::HelloMessage out;
    T2D_CHECK_FALSE(net::decode_hello(ConstSpan<const u8>(lying.data(), lying.size()), out));
}

T2D_TEST(the_shared_link_carries_messages_in_order_in_both_directions) {
    net::SharedLinkPair pair = net::create_shared_link_pair();
    T2D_REQUIRE(pair.server != nullptr);
    T2D_REQUIRE(pair.client != nullptr);
    T2D_CHECK_GT(pair.server->shared_bytes(), 0u);
    T2D_CHECK(pair.server->is_server_endpoint());
    T2D_CHECK_FALSE(pair.client->is_server_endpoint());
    T2D_CHECK_EQ(std::string(pair.server->kind()), std::string("shared-memory"));

    // The ring holds 128 messages, so the stream is pipelined: send a batch, drain it, repeat. That
    // also makes the ring wrap many times over.
    constexpr u32 kMessages = 500;
    constexpr u32 kBatch = 64;
    for (u32 i = 0; i < kMessages; ++i) {
        const std::string text = std::format("client->server {}", i);
        T2D_REQUIRE(pair.client->send(ConstSpan<const u8>(reinterpret_cast<const u8*>(text.data()), text.size())));
        if ((i + 1) % kBatch != 0) continue;
        T2D_CHECK_EQ(pair.client->queued_outgoing(), kBatch);
        T2D_CHECK_EQ(pair.server->queued_incoming(), kBatch);
        for (u32 k = 0; k < kBatch; ++k) {
            const u32 index = i + 1 - kBatch + k;
            const std::vector<u8> message = receive_one(*pair.server);
            const std::string text_in(message.begin(), message.end());
            T2D_CHECK_EQ(text_in, std::format("client->server {}", index));
        }
    }
    // Drain whatever the last partial batch left behind.
    while (pair.client->queued_outgoing() > 0) {
        T2D_CHECK_GT(receive_one(*pair.server).size(), 0u);
    }
    u8 scratch[1] = {0};
    T2D_CHECK_EQ(pair.server->receive(Span<u8>(scratch, 1)), 0u); // the queue is empty
    T2D_CHECK_EQ(pair.client->queued_outgoing(), 0u);

    // And back the other way, with sizes that vary so the ring has to wrap.
    for (u32 i = 0; i < kMessages; ++i) {
        const std::string text(1 + (i % 64), static_cast<char>('a' + (i % 26)));
        T2D_REQUIRE(pair.server->send(ConstSpan<const u8>(reinterpret_cast<const u8*>(text.data()), text.size())));
        if ((i + 1) % kBatch != 0) continue;
        for (u32 k = 0; k < kBatch; ++k) {
            const u32 index = i + 1 - kBatch + k;
            const std::vector<u8> message = receive_one(*pair.client);
            T2D_CHECK_EQ(message.size(), 1u + (index % 64));
        }
    }
    while (pair.server->queued_outgoing() > 0) {
        T2D_CHECK_GT(receive_one(*pair.client).size(), 0u);
    }
    T2D_CHECK_GT(pair.server->outgoing_high_water(), 0u);
    pair.server->update(0);
    pair.client->update(0);
    T2D_CHECK_GT(pair.client->peer_messages_sent(), 0ull);
    T2D_CHECK_EQ(pair.server->stats().messages_received, static_cast<u64>(kMessages));
    T2D_CHECK_EQ(pair.client->stats().messages_sent, static_cast<u64>(kMessages));
}

T2D_TEST(the_shared_link_drops_messages_that_do_not_fit_its_ring) {
    net::SharedLinkOptions options;
    options.slot_count = 4;
    net::SharedLinkPair pair = net::create_shared_link_pair(options);
    T2D_REQUIRE(pair.server != nullptr);
    T2D_REQUIRE(pair.client != nullptr);

    for (u32 i = 0; i < 4; ++i) {
        const std::string text = std::format("message {}", i);
        T2D_REQUIRE(pair.client->send(ConstSpan<const u8>(reinterpret_cast<const u8*>(text.data()), text.size())));
    }
    T2D_CHECK_EQ(pair.client->queued_outgoing(), 4u);
    // The ring is full: the producer must be told, not silently overwritten.
    const std::string extra = "one too many";
    T2D_CHECK_FALSE(pair.client->send(ConstSpan<const u8>(reinterpret_cast<const u8*>(extra.data()), extra.size())));
    T2D_CHECK_EQ(pair.client->queued_outgoing(), 4u);
    T2D_CHECK_GT(pair.client->stats().messages_dropped, 0ull);

    // Draining one slot makes room for exactly one more message.
    T2D_CHECK_EQ(receive_one(*pair.server).size(), std::string("message 0").size());
    T2D_REQUIRE(pair.client->send(ConstSpan<const u8>(reinterpret_cast<const u8*>(extra.data()), extra.size())));
    T2D_CHECK_EQ(pair.client->queued_outgoing(), 4u);
    for (u32 i = 1; i < 4; ++i) {
        const std::vector<u8> message = receive_one(*pair.server);
        T2D_CHECK_EQ(std::string(message.begin(), message.end()), std::format("message {}", i));
    }
    const std::vector<u8> last = receive_one(*pair.server);
    T2D_CHECK_EQ(std::string(last.begin(), last.end()), extra);
    T2D_CHECK_EQ(pair.client->queued_outgoing(), 0u);
}

T2D_TEST(the_shared_link_refuses_messages_larger_than_a_slot) {
    net::SharedLinkOptions options;
    options.slot_count = 8;
    options.slot_size = 256;
    net::SharedLinkPair pair = net::create_shared_link_pair(options);
    T2D_REQUIRE(pair.client != nullptr);

    std::vector<u8> too_big(257, 0x33u);
    T2D_CHECK_FALSE(pair.client->send(ConstSpan<const u8>(too_big.data(), too_big.size())));
    T2D_CHECK_GT(pair.client->stats().messages_dropped, 0ull);
    T2D_CHECK_EQ(pair.client->queued_outgoing(), 0u);

    std::vector<u8> fits(256, 0x44u);
    T2D_REQUIRE(pair.client->send(ConstSpan<const u8>(fits.data(), fits.size())));
    const std::vector<u8> received = receive_one(*pair.server);
    T2D_CHECK_EQ(received.size(), fits.size());
    T2D_CHECK(received == fits);
}

T2D_TEST(closing_a_shared_link_stops_sends_and_wakes_waiters) {
    net::SharedLinkPair pair = net::create_shared_link_pair();
    T2D_REQUIRE(pair.client != nullptr);
    T2D_REQUIRE(pair.server != nullptr);

    T2D_CHECK_EQ(static_cast<u32>(pair.client->state()), static_cast<u32>(net::LinkState::Connected));
    // Nothing queued: a waiter times out instead of blocking forever.
    T2D_CHECK_FALSE(pair.client->wait_for_data(2));

    pair.client->close();
    T2D_CHECK_EQ(static_cast<u32>(pair.client->state()), static_cast<u32>(net::LinkState::Closed));
    T2D_CHECK_FALSE(pair.client->wait_for_data(2));
    const std::string text = "after close";
    T2D_CHECK_FALSE(pair.client->send(ConstSpan<const u8>(reinterpret_cast<const u8*>(text.data()), text.size())));
    pair.client->update(1234); // must be harmless after close

    // The other endpoint is unaffected.
    T2D_REQUIRE(pair.server->send(ConstSpan<const u8>(reinterpret_cast<const u8*>(text.data()), text.size())));
    T2D_CHECK_EQ(static_cast<u32>(pair.server->state()), static_cast<u32>(net::LinkState::Connected));
}

T2D_TEST(resetting_the_shared_link_drops_queued_messages) {
    net::SharedLinkPair pair = net::create_shared_link_pair();
    T2D_REQUIRE(pair.client != nullptr);
    T2D_REQUIRE(pair.server != nullptr);

    const std::string text = "stale";
    for (u32 i = 0; i < 5; ++i) {
        T2D_REQUIRE(pair.client->send(ConstSpan<const u8>(reinterpret_cast<const u8*>(text.data()), text.size())));
    }
    T2D_CHECK_EQ(pair.server->queued_incoming(), 5u);
    pair.server->reset_queues();
    T2D_CHECK_EQ(pair.server->queued_incoming(), 0u);
    T2D_CHECK_EQ(receive_one(*pair.server).size(), 0u);

    // The channel still works afterwards.
    T2D_REQUIRE(pair.client->send(ConstSpan<const u8>(reinterpret_cast<const u8*>(text.data()), text.size())));
    T2D_CHECK_EQ(receive_one(*pair.server).size(), text.size());
}

T2D_TEST_MAIN
