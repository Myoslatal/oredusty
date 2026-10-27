// Tile2D - the save archive: one versioned container a **game** fills with its own chunks.
//
// This is a module of its own on purpose (docs/SAVE.md): the file format and the bytes in it are the
// framework's business, what a chunk means is the game's. A game picks chunk ids (fourccs), versions
// them itself, and writes whatever payload its rules need - the archive never looks inside one. That is
// what makes it reusable by games with entirely different mechanics, and by a game and its tooling.
//
// The file is **header-only**: a game compiles it into its own program or code table, so the engine's
// published surface (engine.api) does not grow for a format that only games use. Costs: every game gets
// its own copy of these inline functions (a few KiB of code), and there is no shared save helper in the
// launcher. That trade is deliberate - see docs/SAVE.md.
//
// The layout (all little endian, no padding, no trailing bytes):
//
//     'T' '2' 'D' 'S'                 magic
//     u16                             format version (1)
//     u16 + bytes                     game name, UTF-8, 1..64 bytes
//     u32                             game version (the game's own number)
//     u32                             chunk count, 0..256
//     per chunk:
//         u32                         chunk id (fourcc, never 0)
//         u32                         chunk version (the game's to interpret)
//         u32                         payload size, up to 64 MiB
//         u32                         CRC-32 (IEEE 802.3) of the payload
//         bytes                       the payload
//
// The reader trusts nothing: a wrong magic or format version, a truncated stream, an out-of-range
// length, a duplicate chunk id, a bad checksum or a trailing byte is refused with a readable reason.
// Nothing here throws, allocates past what the bytes say, or depends on a game's content.
#pragma once

#include <t2d/core/types.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace t2d::save {

/// The extension a save file is expected to carry, dot included: a game's save is its name plus this.
inline constexpr std::string_view kExtension = ".t2dsave";

/// "Four characters" as the little-endian u32 the file stores: fourcc('T','2','D','S') is 0x53443254.
[[nodiscard]] constexpr u32 fourcc(char a, char b, char c, char d) {
    return static_cast<u32>(static_cast<u8>(a)) | (static_cast<u32>(static_cast<u8>(b)) << 8u) |
           (static_cast<u32>(static_cast<u8>(c)) << 16u) | (static_cast<u32>(static_cast<u8>(d)) << 24u);
}

// --- what the file is made of ---------------------------------------------------------------------

/// The magic every .t2dsave starts with.
inline constexpr u32 kMagic = fourcc('T', '2', 'D', 'S');
/// Bumped when the **container** changes; a file written by another version is refused, never guessed.
inline constexpr u16 kFormatVersion = 1;
/// Longest game name the format accepts, in bytes.
inline constexpr usize kMaxGameLength = 64;
/// Most chunks one archive may hold.
inline constexpr usize kMaxChunks = 256;
/// Largest single chunk payload; a save that needs more is not one file's job.
inline constexpr u32 kMaxPayloadSize = 64u << 20;
/// Most bytes a payload string field may occupy (its length is a u16).
inline constexpr usize kMaxStringLength = 0xFFFF;

/// One piece of a save: an id the game chose, a version the game interprets, and its bytes.
struct Chunk {
    /// A fourcc, so a hex dump of a save says which part of it is which. Never 0.
    u32 id = 0;
    /// The game's own version of this chunk's payload: the archive records it and never reads it.
    u32 version = 1;
    std::vector<u8> payload;
};

/// One save file: which game wrote it, and the chunks it consists of.
struct File {
    /// The game's name (1..64 bytes of UTF-8): what a save manager shows, and what tells two games apart.
    std::string game;
    /// The game's own build/version number; the archive only carries it.
    u32 game_version = 1;
    /// The game's chunks, in the order the game wrote them.
    std::vector<Chunk> chunks;

    [[nodiscard]] const Chunk* find(u32 id) const {
        for (const Chunk& chunk : chunks) {
            if (chunk.id == id) return &chunk;
        }
        return nullptr;
    }
    [[nodiscard]] Chunk* find(u32 id) {
        for (Chunk& chunk : chunks) {
            if (chunk.id == id) return &chunk;
        }
        return nullptr;
    }
    [[nodiscard]] bool has(u32 id) const { return find(id) != nullptr; }
};

// --- the checksum ---------------------------------------------------------------------------------

/// CRC-32, the IEEE 802.3 polynomial (the one zip and PNG use), reflected, initial and final xor 0xFFFF.
[[nodiscard]] inline u32 crc32(ConstSpan<const u8> data) {
    u32 crc = 0xFFFF'FFFFu;
    for (const u8 byte : data) {
        crc ^= static_cast<u32>(byte);
        for (u32 bit = 0; bit < 8u; ++bit) {
            // Branch free: if the low bit is set, xor the reversed polynomial after the shift.
            const u32 mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB8'8320u & mask);
        }
    }
    return ~crc;
}

// --- payload helpers ------------------------------------------------------------------------------

namespace detail {

inline void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

} // namespace detail

/// Builds a chunk payload. Writes are little endian and never throw; after the first failure the writer
/// keeps its error and ignores what comes after it, so a caller can write a whole record and check once.
class Writer {
public:
    void write_u8(u8 value) { push(&value, 1); }
    void write_u16(u16 value) { put(value, 2); }
    void write_u32(u32 value) { put(value, 4); }
    void write_u64(u64 value) { put(value, 8); }
    /// Signed values are zigzag encoded, like the engine's other byte streams (core/bitstream.h).
    void write_i32(i32 value) { write_u32((static_cast<u32>(value) << 1u) ^ static_cast<u32>(value >> 31)); }
    /// The bit pattern, so a float round-trips exactly (NaN payloads included).
    void write_f32(f32 value) { write_u32(std::bit_cast<u32>(value)); }
    void write_bool(bool value) { write_u8(value ? 1u : 0u); }
    /// u16 length + the bytes: what a field a reader has to be able to skip over uses.
    void write_string(std::string_view text);
    void write_bytes(ConstSpan<const u8> bytes);

    [[nodiscard]] bool ok() const { return error_.empty(); }
    [[nodiscard]] const std::string& error() const { return error_; }
    /// The payload written so far. This is the end of the writer: what comes after is undefined.
    [[nodiscard]] std::vector<u8> finish() { return std::move(bytes_); }

private:
    void fail(std::string message);
    void put(u64 value, usize count);
    void push(const void* data, usize count);

    std::vector<u8> bytes_;
    std::string error_;
};

inline void Writer::write_string(std::string_view text) {
    if (!ok()) return;
    if (text.size() > kMaxStringLength) {
        fail("a string longer than 65535 bytes cannot be written to a save payload");
        return;
    }
    put(text.size(), 2);
    push(text.data(), text.size());
}

inline void Writer::write_bytes(ConstSpan<const u8> bytes) {
    if (!ok() || bytes.empty()) return;
    push(bytes.data(), bytes.size());
}

inline void Writer::fail(std::string message) {
    if (error_.empty()) error_ = std::move(message);
}

inline void Writer::put(u64 value, usize count) {
    if (!ok()) return;
    std::array<u8, 8> bytes{};
    for (usize index = 0; index < count; ++index) {
        bytes[index] = static_cast<u8>((value >> (index * 8u)) & 0xFFu);
    }
    push(bytes.data(), count);
}

inline void Writer::push(const void* data, usize count) {
    if (!ok() || count == 0) return;
    const auto* first = static_cast<const u8*>(data);
    bytes_.insert(bytes_.end(), first, first + count);
}

/// Reads a chunk payload. Every read is bounds checked and returns false on failure - a truncated or
/// corrupt payload leaves the reader with an error, never an exception and never a partial answer.
class Reader {
public:
    explicit Reader(ConstSpan<const u8> data) : data_(data) {}

    [[nodiscard]] bool read_u8(u8& out) {
        if (!take(1)) return false;
        out = data_[cursor_ - 1];
        return true;
    }
    [[nodiscard]] bool read_u16(u16& out);
    [[nodiscard]] bool read_u32(u32& out);
    [[nodiscard]] bool read_u64(u64& out);
    [[nodiscard]] bool read_i32(i32& out);
    [[nodiscard]] bool read_f32(f32& out);
    [[nodiscard]] bool read_bool(bool& out);
    /// A u16 length then that many bytes; \p max_length is the most this field may hold.
    [[nodiscard]] bool read_string(std::string& out, usize max_length = kMaxStringLength);
    [[nodiscard]] bool read_bytes(usize count, std::vector<u8>& out);
    /// The next \p count bytes **in place**, without copying them out (a payload's view of its own file).
    [[nodiscard]] bool read_span(usize count, ConstSpan<const u8>& out);

    [[nodiscard]] bool ok() const { return error_.empty(); }
    [[nodiscard]] const std::string& error() const { return error_; }
    /// True when every byte has been consumed.
    [[nodiscard]] bool empty() const { return cursor_ >= data_.size(); }
    [[nodiscard]] usize remaining() const { return data_.size() - cursor_; }

private:
    [[nodiscard]] bool take(usize count);
    void fail(std::string message);

    ConstSpan<const u8> data_;
    usize cursor_ = 0;
    std::string error_;
};

inline bool Reader::read_u16(u16& out) {
    if (!take(2)) return false;
    const u32 value =
        static_cast<u32>(data_[cursor_ - 2]) | (static_cast<u32>(data_[cursor_ - 1]) << 8u);
    out = static_cast<u16>(value);
    return true;
}

inline bool Reader::read_u32(u32& out) {
    if (!take(4)) return false;
    u64 value = 0;
    for (usize index = 0; index < 4; ++index) {
        value |= static_cast<u64>(data_[cursor_ - 4 + index]) << (index * 8u);
    }
    out = static_cast<u32>(value);
    return true;
}

inline bool Reader::read_u64(u64& out) {
    if (!take(8)) return false;
    u64 value = 0;
    for (usize index = 0; index < 8; ++index) {
        value |= static_cast<u64>(data_[cursor_ - 8 + index]) << (index * 8u);
    }
    out = value;
    return true;
}

inline bool Reader::read_i32(i32& out) {
    u32 value = 0;
    if (!read_u32(value)) return false;
    out = static_cast<i32>(value >> 1u) ^ -static_cast<i32>(value & 1u);
    return true;
}

inline bool Reader::read_f32(f32& out) {
    u32 value = 0;
    if (!read_u32(value)) return false;
    out = std::bit_cast<f32>(value);
    return true;
}

inline bool Reader::read_bool(bool& out) {
    u8 value = 0;
    if (!read_u8(value)) return false;
    if (value > 1u) {
        fail("a bool in a save payload is neither 0 nor 1");
        return false;
    }
    out = value == 1u;
    return true;
}

inline bool Reader::read_string(std::string& out, usize max_length) {
    u16 length = 0;
    if (!read_u16(length)) return false;
    if (length > max_length) {
        fail("a string in a save payload is longer than its field allows");
        return false;
    }
    if (!take(length)) return false;
    out.assign(reinterpret_cast<const char*>(data_.data() + cursor_ - length), length);
    return true;
}

inline bool Reader::read_bytes(usize count, std::vector<u8>& out) {
    if (!take(count)) return false;
    out.assign(data_.data() + cursor_ - count, data_.data() + cursor_);
    return true;
}

inline bool Reader::read_span(usize count, ConstSpan<const u8>& out) {
    if (!take(count)) return false;
    out = data_.subspan(cursor_ - count, count);
    return true;
}

inline bool Reader::take(usize count) {
    if (!ok()) return false;
    if (count > remaining()) {
        fail("the save ends in the middle of a field");
        return false;
    }
    cursor_ += count;
    return true;
}

inline void Reader::fail(std::string message) {
    if (error_.empty()) error_ = std::move(message);
}

// --- encode and decode ----------------------------------------------------------------------------

/// The archive as the bytes of a .t2dsave. Returns an empty vector - and a reason in \p error - when the
/// file cannot be written as one: no game name, a name longer than kMaxGameLength, more than kMaxChunks
/// chunks, a chunk id of 0 or the same id twice, or a payload over kMaxPayloadSize. Two calls with the
/// same File produce the same bytes.
[[nodiscard]] inline std::vector<u8> encode(const File& file, std::string* error = nullptr) {
    if (file.game.empty()) {
        detail::set_error(error, "the archive has no game name");
        return {};
    }
    if (file.game.size() > kMaxGameLength) {
        detail::set_error(error, "the game name is longer than 64 bytes");
        return {};
    }
    if (file.chunks.size() > kMaxChunks) {
        detail::set_error(error, "the archive holds more than 256 chunks");
        return {};
    }
    for (usize index = 0; index < file.chunks.size(); ++index) {
        const Chunk& chunk = file.chunks[index];
        if (chunk.id == 0u) {
            detail::set_error(error, "a chunk id of 0 is not a fourcc");
            return {};
        }
        if (chunk.payload.size() > kMaxPayloadSize) {
            detail::set_error(error, "a chunk payload is larger than 64 MiB");
            return {};
        }
        for (usize earlier = 0; earlier < index; ++earlier) {
            if (file.chunks[earlier].id == chunk.id) {
                detail::set_error(error, "the same chunk id is written twice");
                return {};
            }
        }
    }
    Writer writer;
    writer.write_u32(kMagic);
    writer.write_u16(kFormatVersion);
    writer.write_u16(static_cast<u16>(file.game.size()));
    writer.write_bytes(ConstSpan<const u8>(reinterpret_cast<const u8*>(file.game.data()), file.game.size()));
    writer.write_u32(file.game_version);
    writer.write_u32(static_cast<u32>(file.chunks.size()));
    for (const Chunk& chunk : file.chunks) {
        writer.write_u32(chunk.id);
        writer.write_u32(chunk.version);
        writer.write_u32(static_cast<u32>(chunk.payload.size()));
        writer.write_u32(crc32(ConstSpan<const u8>(chunk.payload)));
        writer.write_bytes(ConstSpan<const u8>(chunk.payload));
    }
    if (!writer.ok()) {
        detail::set_error(error, writer.error());
        return {};
    }
    return writer.finish();
}

/// Reads a .t2dsave. Anything that cannot be trusted - magic, format version, a cut stream, a name of the
/// wrong length, too many chunks, a chunk id of 0, a payload over the limit, the same id twice, a bad
/// checksum, trailing bytes - returns nothing and a reason in \p error. A chunk's own version is **not**
/// checked here: it is the game's to interpret, and a chunk the game does not know is kept as it was.
[[nodiscard]] inline std::optional<File> decode(ConstSpan<const u8> bytes, std::string* error = nullptr) {
    const auto fail = [error](std::string message) -> std::optional<File> {
        detail::set_error(error, std::move(message));
        return std::nullopt;
    };
    Reader reader(bytes);
    u32 magic = 0;
    if (!reader.read_u32(magic)) return fail(reader.error());
    if (magic != kMagic) return fail("not a .t2dsave: the magic does not match");
    u16 format = 0;
    if (!reader.read_u16(format)) return fail(reader.error());
    if (format != kFormatVersion) return fail("the .t2dsave format version is not one this build knows");
    u16 game_length = 0;
    if (!reader.read_u16(game_length)) return fail(reader.error());
    if (game_length == 0u) return fail("the archive has no game name");
    if (game_length > kMaxGameLength) return fail("the game name is longer than 64 bytes");
    std::vector<u8> game_bytes;
    if (!reader.read_bytes(game_length, game_bytes)) return fail(reader.error());
    File file;
    file.game.assign(reinterpret_cast<const char*>(game_bytes.data()), game_bytes.size());
    if (!reader.read_u32(file.game_version)) return fail(reader.error());
    u32 chunk_count = 0;
    if (!reader.read_u32(chunk_count)) return fail(reader.error());
    if (chunk_count > kMaxChunks) return fail("the archive holds more than 256 chunks");
    file.chunks.reserve(chunk_count);
    for (u32 index = 0; index < chunk_count; ++index) {
        Chunk chunk;
        u32 payload_size = 0;
        u32 expected_crc = 0;
        if (!reader.read_u32(chunk.id) || !reader.read_u32(chunk.version) || !reader.read_u32(payload_size) ||
            !reader.read_u32(expected_crc)) {
            return fail(reader.error());
        }
        if (chunk.id == 0u) return fail("a chunk id of 0 is not a fourcc");
        if (payload_size > kMaxPayloadSize) return fail("a chunk payload is larger than 64 MiB");
        if (file.has(chunk.id)) return fail("the same chunk id appears twice");
        ConstSpan<const u8> payload;
        if (!reader.read_span(payload_size, payload)) return fail(reader.error());
        if (crc32(payload) != expected_crc) return fail("a chunk payload does not match its checksum");
        chunk.payload.assign(payload.begin(), payload.end());
        file.chunks.push_back(std::move(chunk));
    }
    if (!reader.empty()) return fail("the save has bytes after its last chunk");
    return file;
}

// --- the file -------------------------------------------------------------------------------------

/// Reads \p path and decodes it. The reason in \p error says whether the file could not be read or its
/// bytes could not be trusted.
[[nodiscard]] inline std::optional<File> read_file(const std::string& path, std::string* error = nullptr) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        detail::set_error(error, "cannot open '" + path + "' for reading");
        return std::nullopt;
    }
    std::vector<u8> bytes;
    std::array<u8, 4096> buffer{};
    bool read_failed = false;
    while (true) {
        const usize got = std::fread(buffer.data(), 1, buffer.size(), file);
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(got));
        if (got < buffer.size()) {
            read_failed = std::ferror(file) != 0;
            break;
        }
    }
    std::fclose(file);
    if (read_failed) {
        detail::set_error(error, "cannot read '" + path + "'");
        return std::nullopt;
    }
    return decode(ConstSpan<const u8>(bytes), error);
}

/// Encodes \p file and writes it to \p path. Returns false and a reason when either step fails.
inline bool write_file(const std::string& path, const File& file, std::string* error = nullptr) {
    const std::vector<u8> bytes = encode(file, error);
    if (bytes.empty()) return false;   // encode failed: the reason is already in *error
    std::FILE* out = std::fopen(path.c_str(), "wb");
    if (out == nullptr) {
        detail::set_error(error, "cannot open '" + path + "' for writing");
        return false;
    }
    const usize written = std::fwrite(bytes.data(), 1, bytes.size(), out);
    const bool closed = std::fclose(out) == 0;
    if (written != bytes.size() || !closed) {
        detail::set_error(error, "cannot write '" + path + "'");
        return false;
    }
    return true;
}

// --- names ----------------------------------------------------------------------------------------

namespace detail {

/// The ASCII lower case of one character, for the extension comparison (a path is not a locale).
[[nodiscard]] inline char ascii_lower(char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

} // namespace detail

/// True when \p path ends in kExtension, ignoring case (a save named "Campaign.T2DSAVE" counts).
[[nodiscard]] inline bool ends_with_extension(std::string_view path) {
    if (path.size() < kExtension.size()) return false;
    const std::string_view tail = path.substr(path.size() - kExtension.size());
    for (usize index = 0; index < kExtension.size(); ++index) {
        if (detail::ascii_lower(tail[index]) != kExtension[index]) return false;
    }
    return true;
}

/// \p path with kExtension appended unless it already ends in one: what a "save as" box calls.
[[nodiscard]] inline std::string with_extension(std::string_view path) {
    if (ends_with_extension(path)) return std::string(path);
    std::string named(path);
    named += kExtension;
    return named;
}

} // namespace t2d::save
