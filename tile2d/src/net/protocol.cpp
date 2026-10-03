#include <t2d/net/protocol.h>

#include <t2d/core/log.h>
#include <t2d/net/link.h>

namespace t2d::net {
namespace {

/// Every decode function starts here: the reader is bounds checked, so a malformed payload can only
/// ever produce a "false" answer, never a wild read.
[[nodiscard]] bool payload_ok(const ByteReader& reader) { return reader.ok(); }

} // namespace

const char* link_state_name(LinkState state) {
    switch (state) {
        case LinkState::Disconnected: return "disconnected";
        case LinkState::Connecting: return "connecting";
        case LinkState::Connected: return "connected";
        case LinkState::Closed: return "closed";
    }
    return "?";
}

const char* message_type_name(MessageType type) {
    switch (type) {
        case MessageType::None: return "none";
        case MessageType::Hello: return "hello";
        case MessageType::Welcome: return "welcome";
        case MessageType::Reject: return "reject";
        case MessageType::PlayerJoined: return "player_joined";
        case MessageType::PlayerLeft: return "player_left";
        case MessageType::Ping: return "ping";
        case MessageType::Pong: return "pong";
        case MessageType::Disconnect: return "disconnect";
        case MessageType::MapData: return "map_data";
        case MessageType::ServerStats: return "server_stats";
    }
    return "unknown";
}

std::vector<u8> encode_message(MessageType type, ConstSpan<const u8> payload) {
    if (payload.size() > kMaxMessageSize) {
        // The frame header carries a 16 bit size, so a larger payload would be truncated on the wire
        // and decoded as garbage. Refusing it is the only honest answer.
        T2D_ERROR("protocol: a {} byte payload does not fit in a message ({} byte limit)", payload.size(),
                  kMaxMessageSize);
        return {};
    }
    std::vector<u8> message;
    message.reserve(kMessageHeaderSize + payload.size());
    message.push_back(static_cast<u8>(type));
    const u16 size = static_cast<u16>(payload.size());
    message.push_back(static_cast<u8>(size & 0xFFu));
    message.push_back(static_cast<u8>((size >> 8) & 0xFFu));
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

bool decode_message(ConstSpan<const u8> message, MessageType& type, ConstSpan<const u8>& payload) {
    if (message.size() < kMessageHeaderSize) return false;
    type = static_cast<MessageType>(message[0]);
    const u16 size = static_cast<u16>(message[1] | (static_cast<u16>(message[2]) << 8));
    if (message.size() < kMessageHeaderSize + size) return false;
    payload = message.subspan(kMessageHeaderSize, size);
    return true;
}

// ------------------------------------------------------------------- hello ---

std::vector<u8> encode_hello(const HelloMessage& message) {
    std::vector<u8> payload;
    ByteWriter writer(payload);
    writer.write_u16(message.protocol_version);
    writer.write_u64(message.client_token);
    writer.write_string(message.name);
    return payload;
}

bool decode_hello(ConstSpan<const u8> payload, HelloMessage& out) {
    ByteReader reader(payload);
    out.protocol_version = reader.read_u16();
    out.client_token = reader.read_u64();
    out.name = reader.read_string();
    return payload_ok(reader) && out.name.size() <= 32;
}

// ----------------------------------------------------------------- welcome ---

std::vector<u8> encode_welcome(const WelcomeMessage& message) {
    std::vector<u8> payload;
    ByteWriter writer(payload);
    writer.write_varint(message.player_id);
    writer.write_varint(message.tick);
    writer.write_u16(message.tick_rate);
    writer.write_i32(message.map_width);
    writer.write_i32(message.map_height);
    writer.write_f32(message.tile_size);
    writer.write_u64(message.client_token);
    return payload;
}

bool decode_welcome(ConstSpan<const u8> payload, WelcomeMessage& out) {
    ByteReader reader(payload);
    out.player_id = reader.read_varint();
    out.tick = reader.read_varint();
    out.tick_rate = reader.read_u16();
    out.map_width = reader.read_i32();
    out.map_height = reader.read_i32();
    out.tile_size = reader.read_f32();
    out.client_token = reader.read_u64();
    return payload_ok(reader);
}

// ------------------------------------------------------------------ reject ---

std::vector<u8> encode_reject(const RejectMessage& message) {
    std::vector<u8> payload;
    ByteWriter writer(payload);
    writer.write_u8(static_cast<u8>(message.reason));
    writer.write_string(message.text);
    return payload;
}

bool decode_reject(ConstSpan<const u8> payload, RejectMessage& out) {
    ByteReader reader(payload);
    out.reason = static_cast<RejectReason>(reader.read_u8());
    out.text = reader.read_string();
    return payload_ok(reader);
}

// ----------------------------------------------------------- player events ---

std::vector<u8> encode_player_joined(const PlayerJoinedMessage& message) {
    std::vector<u8> payload;
    ByteWriter writer(payload);
    writer.write_varint(message.player_id);
    writer.write_string(message.name);
    return payload;
}

bool decode_player_joined(ConstSpan<const u8> payload, PlayerJoinedMessage& out) {
    ByteReader reader(payload);
    out.player_id = reader.read_varint();
    out.name = reader.read_string();
    return payload_ok(reader) && out.name.size() <= 32;
}

std::vector<u8> encode_player_left(const PlayerLeftMessage& message) {
    std::vector<u8> payload;
    ByteWriter writer(payload);
    writer.write_varint(message.player_id);
    writer.write_u8(message.reason);
    return payload;
}

bool decode_player_left(ConstSpan<const u8> payload, PlayerLeftMessage& out) {
    ByteReader reader(payload);
    out.player_id = reader.read_varint();
    out.reason = reader.read_u8();
    return payload_ok(reader);
}

// -------------------------------------------------------------- ping / pong --

std::vector<u8> encode_ping(const PingMessage& message) {
    std::vector<u8> payload;
    ByteWriter writer(payload);
    writer.write_varint(message.sequence);
    writer.write_u64(message.sender_time_ms);
    return payload;
}

bool decode_ping(ConstSpan<const u8> payload, PingMessage& out) {
    ByteReader reader(payload);
    out.sequence = reader.read_varint();
    out.sender_time_ms = reader.read_u64();
    return payload_ok(reader);
}

std::vector<u8> encode_pong(const PongMessage& message) {
    std::vector<u8> payload;
    ByteWriter writer(payload);
    writer.write_varint(message.sequence);
    writer.write_u64(message.sender_time_ms);
    writer.write_u64(message.responder_time_ms);
    return payload;
}

bool decode_pong(ConstSpan<const u8> payload, PongMessage& out) {
    ByteReader reader(payload);
    out.sequence = reader.read_varint();
    out.sender_time_ms = reader.read_u64();
    out.responder_time_ms = reader.read_u64();
    return payload_ok(reader);
}

// -------------------------------------------------------------- disconnect ---

std::vector<u8> encode_disconnect(const DisconnectMessage& message) {
    std::vector<u8> payload;
    ByteWriter writer(payload);
    writer.write_u8(message.reason);
    return payload;
}

bool decode_disconnect(ConstSpan<const u8> payload, DisconnectMessage& out) {
    ByteReader reader(payload);
    out.reason = reader.read_u8();
    return payload_ok(reader);
}

// ----------------------------------------------------------------- map data ---

std::vector<u8> encode_map_data(const MapDataMessage& message) {
    std::vector<u8> payload;
    payload.reserve(message.payload.size() + 8);
    ByteWriter writer(payload);
    writer.write_u16(message.chunk_index);
    writer.write_u16(message.chunk_count);
    writer.write_varint(static_cast<u32>(message.payload.size()));
    writer.write_bytes(message.payload);
    return payload;
}

bool decode_map_data(ConstSpan<const u8> payload, MapDataMessage& out) {
    ByteReader reader(payload);
    out.chunk_index = reader.read_u16();
    out.chunk_count = reader.read_u16();
    const u32 size = reader.read_varint();
    if (!reader.ok() || size > kMaxMessageSize) return false;
    const ConstSpan<const u8> bytes = reader.read_span(size);
    if (!reader.ok()) return false;
    out.payload.assign(bytes.begin(), bytes.end());
    return true;
}

// ------------------------------------------------------------- server stats ---

std::vector<u8> encode_server_stats(const ServerStatsMessage& message) {
    std::vector<u8> payload;
    ByteWriter writer(payload);
    writer.write_varint(message.tick);
    writer.write_u16(message.players);
    writer.write_f32(message.tick_ms);
    writer.write_varint(message.bytes_in_per_second);
    writer.write_varint(message.bytes_out_per_second);
    return payload;
}

bool decode_server_stats(ConstSpan<const u8> payload, ServerStatsMessage& out) {
    ByteReader reader(payload);
    out.tick = reader.read_varint();
    out.players = reader.read_u16();
    out.tick_ms = reader.read_f32();
    out.bytes_in_per_second = reader.read_varint();
    out.bytes_out_per_second = reader.read_varint();
    return payload_ok(reader);
}

} // namespace t2d::net
