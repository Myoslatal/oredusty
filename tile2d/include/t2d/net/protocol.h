// Tile2D - the wire protocol shared by every link.
//
// Framing:  [u8 type][u16 size][payload ...]   (little endian, size excludes the 3 byte header)
// Payloads are built with the ByteWriter/ByteReader helpers, so adding a field is a two line change
// and unknown/truncated data is always rejected instead of misread.
#pragma once

#include <t2d/core/bitstream.h>
#include <t2d/core/types.h>

#include <string>
#include <string_view>
#include <vector>

namespace t2d::net {

inline constexpr u16 kProtocolVersion = 1;

/// Identity of a player inside one session: the server assigns it during the handshake and never
/// reuses it while the session lives. kInvalidId (core/types.h) means "no player".
using PlayerId = u32;
inline constexpr usize kMessageHeaderSize = 3;
/// Map transfers are chunked so a big level never blocks the snapshot stream.
inline constexpr usize kMapChunkSize = 8 * 1024;

/// The vocabulary every link shares. It is deliberately about the *session*, not about a game: how a
/// player moves, what a snapshot contains and what a command means belong to the game that rides the
/// link, which is why there is no input or state message here.
enum class MessageType : u8 {
    None = 0,
    Hello = 1,        ///< client -> server: protocol version + player name
    Welcome = 2,      ///< server -> client: assigned id, tick rate and map geometry
    Reject = 3,       ///< server -> client: handshake refused
    PlayerJoined = 4, ///< server -> all
    PlayerLeft = 5,   ///< server -> all
    Ping = 6,
    Pong = 7,
    Disconnect = 8,
    MapData = 9,      ///< server -> client: one chunk of the serialised tile map
    ServerStats = 10, ///< server -> client: tick/time/load for a HUD
};

[[nodiscard]] const char* message_type_name(MessageType type);

/// Wraps a payload into one framed message. Returns an empty vector when the payload exceeds
/// kMaxMessageSize, which the 16 bit frame header could not describe.
[[nodiscard]] std::vector<u8> encode_message(MessageType type, ConstSpan<const u8> payload);
/// Splits a framed message. Returns false for truncated/invalid frames.
[[nodiscard]] bool decode_message(ConstSpan<const u8> message, MessageType& type, ConstSpan<const u8>& payload);

// --- payloads --------------------------------------------------------------

struct HelloMessage {
    u16 protocol_version = kProtocolVersion;
    std::string name;
    u64 client_token = 0; ///< echoed by Welcome so a client can match the answer to its request
};

struct WelcomeMessage {
    PlayerId player_id = kInvalidId;
    u32 tick = 0;
    u16 tick_rate = 60;
    /// Map geometry, so the client can read the MapData chunks that follow. Where a player starts is
    /// the game's business, not the handshake's.
    i32 map_width = 0;
    i32 map_height = 0;
    f32 tile_size = 16.0f;
    u64 client_token = 0;
};

enum class RejectReason : u8 {
    ProtocolMismatch = 1,
    ServerFull = 2,
    Banned = 3,
    ShuttingDown = 4,
};

struct RejectMessage {
    RejectReason reason = RejectReason::ServerFull;
    std::string text;
};

struct PlayerJoinedMessage {
    PlayerId player_id = kInvalidId;
    std::string name;
};

struct PlayerLeftMessage {
    PlayerId player_id = kInvalidId;
    u8 reason = 0;
};

struct PingMessage {
    u32 sequence = 0;
    u64 sender_time_ms = 0;
};

struct PongMessage {
    u32 sequence = 0;
    u64 sender_time_ms = 0;
    u64 responder_time_ms = 0;
};

struct DisconnectMessage {
    u8 reason = 0;
};

struct MapDataMessage {
    u16 chunk_index = 0;
    u16 chunk_count = 0;
    std::vector<u8> payload;
};

struct ServerStatsMessage {
    u32 tick = 0;
    u16 players = 0;
    f32 tick_ms = 0.0f;
    u32 bytes_in_per_second = 0;
    u32 bytes_out_per_second = 0;
};

// --- encode/decode ---------------------------------------------------------

[[nodiscard]] std::vector<u8> encode_hello(const HelloMessage& message);
[[nodiscard]] bool decode_hello(ConstSpan<const u8> payload, HelloMessage& out);

[[nodiscard]] std::vector<u8> encode_welcome(const WelcomeMessage& message);
[[nodiscard]] bool decode_welcome(ConstSpan<const u8> payload, WelcomeMessage& out);

[[nodiscard]] std::vector<u8> encode_reject(const RejectMessage& message);
[[nodiscard]] bool decode_reject(ConstSpan<const u8> payload, RejectMessage& out);

[[nodiscard]] std::vector<u8> encode_player_joined(const PlayerJoinedMessage& message);
[[nodiscard]] bool decode_player_joined(ConstSpan<const u8> payload, PlayerJoinedMessage& out);

[[nodiscard]] std::vector<u8> encode_player_left(const PlayerLeftMessage& message);
[[nodiscard]] bool decode_player_left(ConstSpan<const u8> payload, PlayerLeftMessage& out);

[[nodiscard]] std::vector<u8> encode_ping(const PingMessage& message);
[[nodiscard]] bool decode_ping(ConstSpan<const u8> payload, PingMessage& out);

[[nodiscard]] std::vector<u8> encode_pong(const PongMessage& message);
[[nodiscard]] bool decode_pong(ConstSpan<const u8> payload, PongMessage& out);

[[nodiscard]] std::vector<u8> encode_disconnect(const DisconnectMessage& message);
[[nodiscard]] bool decode_disconnect(ConstSpan<const u8> payload, DisconnectMessage& out);

[[nodiscard]] std::vector<u8> encode_map_data(const MapDataMessage& message);
[[nodiscard]] bool decode_map_data(ConstSpan<const u8> payload, MapDataMessage& out);

[[nodiscard]] std::vector<u8> encode_server_stats(const ServerStatsMessage& message);
[[nodiscard]] bool decode_server_stats(ConstSpan<const u8> payload, ServerStatsMessage& out);

} // namespace t2d::net
