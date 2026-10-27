// The save archive: the versioned container a game fills with its own chunks - encoding, decoding, the
// checksum, the file helpers, and two games with entirely different mechanics using one module
// (docs/SAVE.md).
#include <t2d/save/archive.h>

#include <support/test_support.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace t2d;
namespace save = t2d::save;

namespace {

/// The bytes of a save in the documented layout, written straight into a Writer: what the encoder would
/// produce, which is how a test builds the files the encoder itself refuses to produce (a bad id, a
/// duplicate, a lying header).
[[nodiscard]] std::vector<u8> raw_save(std::string_view game, u32 game_version,
                                       const std::vector<save::Chunk>& chunks,
                                       u32 magic = save::kMagic, u16 format = save::kFormatVersion,
                                       bool corrupt_crc = false) {
    save::Writer writer;
    writer.write_u32(magic);
    writer.write_u16(format);
    writer.write_u16(static_cast<u16>(game.size()));
    writer.write_bytes(ConstSpan<const u8>(reinterpret_cast<const u8*>(game.data()), game.size()));
    writer.write_u32(game_version);
    writer.write_u32(static_cast<u32>(chunks.size()));
    for (const save::Chunk& chunk : chunks) {
        writer.write_u32(chunk.id);
        writer.write_u32(chunk.version);
        writer.write_u32(static_cast<u32>(chunk.payload.size()));
        writer.write_u32(save::crc32(chunk.payload) ^ (corrupt_crc ? 1u : 0u));
        writer.write_bytes(chunk.payload);
    }
    return writer.finish();
}

/// One chunk carrying a single u32: enough to look at a header without building a whole game.
[[nodiscard]] save::Chunk u32_chunk(u32 id, u32 value, u32 version = 1) {
    save::Writer writer;
    writer.write_u32(value);
    return save::Chunk{id, version, writer.finish()};
}

/// A path under the system temporary directory that no other run of this test uses.
[[nodiscard]] std::filesystem::path scratch_path(std::string_view name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string directory = std::string("t2d_save_") + std::to_string(stamp);
    return std::filesystem::temp_directory_path() / directory / std::string(name);
}

} // namespace

T2D_TEST(the_magic_and_the_fourcc_are_the_format_s_own) {
    // What a hex dump of a save starts with, and how a game names its chunks.
    T2D_CHECK_EQ(save::fourcc('T', '2', 'D', 'S'), 0x53443254u);
    T2D_CHECK_EQ(save::kMagic, save::fourcc('T', '2', 'D', 'S'));
    T2D_CHECK_EQ(save::fourcc('A', 'B', 'C', 'D'), 0x44434241u);
    T2D_CHECK_EQ(save::kExtension, std::string_view(".t2dsave"));
    T2D_CHECK_EQ(save::kFormatVersion, static_cast<u16>(1));
}

T2D_TEST(two_games_with_different_mechanics_share_one_archive) {
    // Game A: a turn based puzzle. Its chunks hold whose turn it is and the board as bytes.
    constexpr u32 kTurn = save::fourcc('T', 'U', 'R', 'N');
    constexpr u32 kBoard = save::fourcc('B', 'O', 'A', 'R');
    save::File puzzle;
    puzzle.game = "puzzle";
    puzzle.game_version = 3;
    {
        save::Writer turn;
        turn.write_u32(7);
        turn.write_string("blue");
        puzzle.chunks.push_back(save::Chunk{kTurn, 1, turn.finish()});
    }
    const std::vector<u8> board_cells{1, 0, 2, 0, 0, 2, 0, 1, 3, 0, 0, 2};
    {
        save::Writer board;
        board.write_u8(4);
        board.write_u8(3);
        board.write_bytes(board_cells);
        puzzle.chunks.push_back(save::Chunk{kBoard, 2, board.finish()});
    }

    // Game B: a physics sandbox. Different chunk ids, a different version, floats and a clock - and it
    // shares nothing with game A but the container.
    constexpr u32 kBodies = save::fourcc('B', 'D', 'Y', '1');
    constexpr u32 kClock = save::fourcc('C', 'L', 'K', '1');
    save::File sandbox;
    sandbox.game = "sandbox";
    sandbox.game_version = 12;
    {
        save::Writer bodies;
        bodies.write_u8(2);
        for (const std::array<f32, 3>& body : {std::array<f32, 3>{1.5f, -2.25f, 0.125f},
                                              std::array<f32, 3>{-0.5f, 3.0f, 1.875f}}) {
            bodies.write_u32(1);
            for (const f32 value : body) bodies.write_f32(value);
            bodies.write_bool(false);
        }
        sandbox.chunks.push_back(save::Chunk{kBodies, 4, bodies.finish()});
    }
    {
        save::Writer clock;
        clock.write_u64(123456789ull);
        clock.write_bool(true);
        sandbox.chunks.push_back(save::Chunk{kClock, 2, clock.finish()});
    }

    // The same encoder and decoder move both: the module knows nothing about either game.
    const std::vector<u8> puzzle_bytes = save::encode(puzzle);
    const std::vector<u8> sandbox_bytes = save::encode(sandbox);
    T2D_CHECK(!puzzle_bytes.empty());
    T2D_CHECK(!sandbox_bytes.empty());
    const std::optional<save::File> puzzle_back = save::decode(puzzle_bytes);
    const std::optional<save::File> sandbox_back = save::decode(sandbox_bytes);
    T2D_REQUIRE(puzzle_back.has_value());
    T2D_REQUIRE(sandbox_back.has_value());
    T2D_CHECK_EQ(puzzle_back->game, std::string("puzzle"));
    T2D_CHECK_EQ(puzzle_back->game_version, 3u);
    T2D_CHECK_EQ(sandbox_back->game, std::string("sandbox"));
    T2D_CHECK_EQ(sandbox_back->game_version, 12u);
    // Each game reads its own chunks: the other game's ids simply are not there.
    T2D_CHECK_FALSE(puzzle_back->has(kBodies));
    T2D_CHECK_FALSE(sandbox_back->has(kTurn));
    const save::Chunk* turn_chunk = puzzle_back->find(kTurn);
    const save::Chunk* board_chunk = puzzle_back->find(kBoard);
    T2D_REQUIRE(turn_chunk != nullptr);
    T2D_REQUIRE(board_chunk != nullptr);
    T2D_CHECK_EQ(turn_chunk->version, 1u);
    T2D_CHECK_EQ(board_chunk->version, 2u);
    {
        save::Reader turn(turn_chunk->payload);
        u32 number = 0;
        std::string player;
        T2D_CHECK(turn.read_u32(number));
        T2D_CHECK(turn.read_string(player));
        T2D_CHECK_EQ(number, 7u);
        T2D_CHECK_EQ(player, std::string("blue"));
        T2D_CHECK(turn.empty());
    }
    {
        save::Reader board(board_chunk->payload);
        u8 width = 0;
        u8 height = 0;
        std::vector<u8> cells;
        T2D_CHECK(board.read_u8(width));
        T2D_CHECK(board.read_u8(height));
        T2D_CHECK(board.read_bytes(width * static_cast<usize>(height), cells));
        T2D_CHECK_EQ(width, 4);
        T2D_CHECK_EQ(height, 3);
        T2D_CHECK_EQ(cells, board_cells);
    }
    {
        const save::Chunk* bodies_chunk = sandbox_back->find(kBodies);
        T2D_REQUIRE(bodies_chunk != nullptr);
        save::Reader bodies(bodies_chunk->payload);
        u8 count = 0;
        T2D_CHECK(bodies.read_u8(count));
        T2D_CHECK_EQ(count, 2);
        for (const std::array<f32, 3>& expected : {std::array<f32, 3>{1.5f, -2.25f, 0.125f},
                                                   std::array<f32, 3>{-0.5f, 3.0f, 1.875f}}) {
            u32 id = 0;
            T2D_CHECK(bodies.read_u32(id));
            T2D_CHECK_EQ(id, 1u);
            for (const f32 value : expected) {
                f32 read = 0.0f;
                T2D_CHECK(bodies.read_f32(read));
                T2D_CHECK_EQ(read, value);   // the bit pattern, so a float never drifts
            }
            bool asleep = true;
            T2D_CHECK(bodies.read_bool(asleep));
            T2D_CHECK_FALSE(asleep);
        }
        T2D_CHECK(bodies.empty());
    }
    {
        const save::Chunk* clock_chunk = sandbox_back->find(kClock);
        T2D_REQUIRE(clock_chunk != nullptr);
        save::Reader clock(clock_chunk->payload);
        u64 ticks = 0;
        bool paused = false;
        T2D_CHECK(clock.read_u64(ticks));
        T2D_CHECK(clock.read_bool(paused));
        T2D_CHECK_EQ(ticks, 123456789ull);
        T2D_CHECK(paused);
    }
}

T2D_TEST(payload_values_round_trip) {
    // Everything a chunk's payload is made of, at the edges of its width.
    save::Writer writer;
    writer.write_u8(0xFFu);
    writer.write_u16(0xFFFFu);
    writer.write_u32(0xFFFF'FFFFu);
    writer.write_u64(0xFFFF'FFFF'FFFF'FFFFull);
    writer.write_i32(-2147483647 - 1);
    writer.write_i32(2147483647);
    writer.write_i32(0);
    writer.write_f32(-0.0f);
    writer.write_f32(1.0f / 3.0f);
    writer.write_bool(true);
    writer.write_bool(false);
    writer.write_string("");
    writer.write_string("a UTF-8 name: 石头仓库");
    const std::vector<u8> bytes{0, 1, 2, 253, 254, 255};
    writer.write_bytes(bytes);
    T2D_CHECK(writer.ok());
    const std::vector<u8> payload = writer.finish();
    T2D_CHECK_FALSE(payload.empty());

    save::Reader reader(payload);
    u8 small = 0;
    u16 medium = 0;
    u32 large = 0;
    u64 huge = 0;
    i32 signed_number = 0;
    f32 real = 0.0f;
    bool flag = false;
    std::string text;
    std::vector<u8> read_bytes;
    T2D_CHECK(reader.read_u8(small));
    T2D_CHECK_EQ(small, 0xFFu);
    T2D_CHECK(reader.read_u16(medium));
    T2D_CHECK_EQ(medium, 0xFFFFu);
    T2D_CHECK(reader.read_u32(large));
    T2D_CHECK_EQ(large, 0xFFFF'FFFFu);
    T2D_CHECK(reader.read_u64(huge));
    T2D_CHECK_EQ(huge, 0xFFFF'FFFF'FFFF'FFFFull);
    T2D_CHECK(reader.read_i32(signed_number));
    T2D_CHECK_EQ(signed_number, -2147483647 - 1);
    T2D_CHECK(reader.read_i32(signed_number));
    T2D_CHECK_EQ(signed_number, 2147483647);
    T2D_CHECK(reader.read_i32(signed_number));
    T2D_CHECK_EQ(signed_number, 0);
    T2D_CHECK(reader.read_f32(real));
    T2D_CHECK_EQ(real, 0.0f);   // negative zero reads back as zero
    T2D_CHECK(reader.read_f32(real));
    T2D_CHECK_EQ(real, 1.0f / 3.0f);
    T2D_CHECK(reader.read_bool(flag));
    T2D_CHECK(flag);
    T2D_CHECK(reader.read_bool(flag));
    T2D_CHECK_FALSE(flag);
    T2D_CHECK(reader.read_string(text));
    T2D_CHECK(text.empty());
    T2D_CHECK(reader.read_string(text));
    T2D_CHECK_EQ(text, std::string("a UTF-8 name: 石头仓库"));
    T2D_CHECK(reader.read_bytes(bytes.size(), read_bytes));
    T2D_CHECK_EQ(read_bytes, bytes);
    T2D_CHECK(reader.empty());
    T2D_CHECK(reader.ok());
}

T2D_TEST(chunks_keep_their_order_and_unknown_ones_are_kept) {
    // The archive preserves what the game wrote: order, ids, versions and payload bytes. A chunk a game
    // does not know is still a chunk - the game ignores it, the archive keeps it.
    constexpr u32 kFirst = save::fourcc('O', 'N', 'E', '1');
    constexpr u32 kSecond = save::fourcc('T', 'W', 'O', '2');
    constexpr u32 kFuture = save::fourcc('F', 'U', 'T', 'R');
    save::File file;
    file.game = "puzzle";
    file.chunks.push_back(u32_chunk(kSecond, 2));
    file.chunks.push_back(u32_chunk(kFirst, 1));
    file.chunks.push_back(save::Chunk{kFuture, 99, {0xAAu, 0xBBu}});
    const std::vector<u8> bytes = save::encode(file);
    const std::optional<save::File> back = save::decode(bytes);
    T2D_REQUIRE(back.has_value());
    T2D_REQUIRE(back->chunks.size() == 3u);
    T2D_CHECK_EQ(back->chunks[0].id, kSecond);   // the order the game wrote, not a sort
    T2D_CHECK_EQ(back->chunks[1].id, kFirst);
    T2D_CHECK_EQ(back->chunks[2].id, kFuture);
    T2D_CHECK_EQ(back->chunks[2].version, 99u);   // a version the archive does not judge
    T2D_CHECK_EQ(back->chunks[2].payload, (std::vector<u8>{0xAAu, 0xBBu}));
    T2D_CHECK_FALSE(back->has(save::fourcc('N', 'O', 'N', 'E')));
    // Writing it again gives the same bytes, unknown chunk included.
    T2D_CHECK_EQ(save::encode(*back), bytes);
}

T2D_TEST(an_empty_archive_round_trips) {
    save::File file;
    file.game = "empty";
    file.game_version = 1;
    const std::vector<u8> bytes = save::encode(file);
    T2D_CHECK_FALSE(bytes.empty());
    const std::optional<save::File> back = save::decode(bytes);
    T2D_REQUIRE(back.has_value());
    T2D_CHECK_EQ(back->game, std::string("empty"));
    T2D_CHECK_EQ(back->game_version, 1u);
    T2D_CHECK(back->chunks.empty());
}

T2D_TEST(encoding_the_same_file_twice_gives_the_same_bytes) {
    save::File file;
    file.game = "puzzle";
    file.game_version = 7;
    file.chunks.push_back(u32_chunk(save::fourcc('T', 'U', 'R', 'N'), 42));
    file.chunks.push_back(save::Chunk{save::fourcc('R', 'A', 'W', '1'), 3, {1u, 2u, 3u, 4u}});
    T2D_CHECK_EQ(save::encode(file), save::encode(file));
    const std::vector<u8> bytes = save::encode(file);
    const std::optional<save::File> back = save::decode(bytes);
    T2D_REQUIRE(back.has_value());
    T2D_CHECK_EQ(save::encode(*back), bytes);   // a decoded save is written back unchanged
}

T2D_TEST(a_payload_that_is_cut_short_is_refused_by_the_reader) {
    // The payload helpers refuse to read past their bytes and say why, rather than throwing or guessing.
    save::Reader empty_reader(ConstSpan<const u8>{});
    u8 byte = 0;
    T2D_CHECK_FALSE(empty_reader.read_u8(byte));
    T2D_CHECK_FALSE(empty_reader.ok());
    T2D_CHECK_FALSE(empty_reader.error().empty());

    const std::vector<u8> two_bytes{1u, 2u};
    save::Reader short_reader(two_bytes);
    u32 value = 0;
    T2D_CHECK_FALSE(short_reader.read_u32(value));
    T2D_CHECK_FALSE(short_reader.error().empty());

    const std::vector<u8> bad_bool{2u};
    save::Reader bool_reader(bad_bool);
    bool flag = false;
    T2D_CHECK_FALSE(bool_reader.read_bool(flag));

    // A string whose length runs past the end, and one longer than the field a caller allows.
    save::Writer writer;
    writer.write_u16(9u);   // claims nine bytes, writes none
    const std::vector<u8> short_string = writer.finish();
    save::Reader string_reader(short_string);
    std::string text;
    T2D_CHECK_FALSE(string_reader.read_string(text));

    save::Writer long_writer;
    long_writer.write_string("abcdef");
    const std::vector<u8> long_string = long_writer.finish();
    save::Reader capped_reader(long_string);
    T2D_CHECK_FALSE(capped_reader.read_string(text, 3u));

    // A writer refuses what a payload field cannot hold, and keeps the reason.
    save::Writer too_long;
    too_long.write_string(std::string(70000, 'x'));
    T2D_CHECK_FALSE(too_long.ok());
    T2D_CHECK_FALSE(too_long.error().empty());
}

T2D_TEST(a_broken_archive_is_refused_with_a_reason) {
    constexpr u32 kTurn = save::fourcc('T', 'U', 'R', 'N');
    const std::vector<u8> good = raw_save("puzzle", 3, {u32_chunk(kTurn, 7)});
    T2D_CHECK(save::decode(good).has_value());

    std::vector<std::vector<u8>> broken;
    broken.push_back(raw_save("puzzle", 3, {u32_chunk(kTurn, 7)}, 0x12345678u));   // wrong magic
    broken.push_back(raw_save("puzzle", 3, {u32_chunk(kTurn, 7)}, save::kMagic, 2u));   // format 2
    broken.push_back(raw_save("", 1, {}));                                      // no game name
    broken.push_back(raw_save(std::string(65, 'g'), 1, {}));                    // name too long
    broken.push_back(raw_save("puzzle", 1, {save::Chunk{0u, 1, {}}}));          // chunk id 0
    broken.push_back(raw_save("puzzle", 1, {u32_chunk(kTurn, 1), u32_chunk(kTurn, 2)}));   // twice
    broken.push_back(raw_save("puzzle", 3, {u32_chunk(kTurn, 7)}, save::kMagic, save::kFormatVersion,
                              true));                                   // payload and checksum disagree
    {
        std::vector<u8> cut(good.begin(), good.end() - 1);
        broken.push_back(std::move(cut));                                   // cut payload
    }
    {
        std::vector<u8> trailing = good;
        trailing.push_back(0u);
        broken.push_back(std::move(trailing));                              // bytes after the last chunk
    }
    // A chunk header that claims more payload than the format allows, with no bytes behind it.
    const std::string game = "puzzle";
    const auto header = [&] {
        save::Writer writer;
        writer.write_u32(save::kMagic);
        writer.write_u16(save::kFormatVersion);
        writer.write_u16(static_cast<u16>(game.size()));
        writer.write_bytes(ConstSpan<const u8>(reinterpret_cast<const u8*>(game.data()), game.size()));
        writer.write_u32(1);
        return writer;
    };
    {
        save::Writer writer = header();
        writer.write_u32(1);                            // one chunk
        writer.write_u32(kTurn);
        writer.write_u32(1);
        writer.write_u32(save::kMaxPayloadSize + 1);    // too big
        writer.write_u32(0);
        broken.push_back(writer.finish());
    }
    {
        save::Writer writer = header();
        writer.write_u32(save::kMaxChunks + 1);         // more chunks than the format allows
        broken.push_back(writer.finish());
    }
    std::vector<u8> empty;
    broken.push_back(std::move(empty));                 // not even a magic

    for (const std::vector<u8>& bytes : broken) {
        std::string error;
        T2D_CHECK_FALSE(save::decode(bytes, &error).has_value());
        T2D_CHECK(!error.empty());
    }
}

T2D_TEST(encoding_refuses_what_the_format_cannot_hold) {
    const auto refuses = [](const save::File& file) {
        std::string error;
        return save::encode(file, &error).empty() && !error.empty();
    };
    save::File no_game;
    no_game.chunks.push_back(u32_chunk(save::fourcc('T', 'U', 'R', 'N'), 1));
    T2D_CHECK(refuses(no_game));

    save::File long_name;
    long_name.game = std::string(65, 'g');
    T2D_CHECK(refuses(long_name));

    save::File zero_id;
    zero_id.game = "puzzle";
    zero_id.chunks.push_back(save::Chunk{0u, 1, {}});
    T2D_CHECK(refuses(zero_id));

    save::File twice;
    twice.game = "puzzle";
    twice.chunks.push_back(u32_chunk(save::fourcc('T', 'U', 'R', 'N'), 1));
    twice.chunks.push_back(u32_chunk(save::fourcc('T', 'U', 'R', 'N'), 2));
    T2D_CHECK(refuses(twice));

    save::File many;
    many.game = "puzzle";
    for (u32 index = 0; index < save::kMaxChunks + 1u; ++index) {
        many.chunks.push_back(u32_chunk(index + 1u, index));
    }
    T2D_CHECK(refuses(many));

    save::File huge;
    huge.game = "puzzle";
    huge.chunks.push_back(save::Chunk{save::fourcc('B', 'I', 'G', '1'), 1,
                                      std::vector<u8>(save::kMaxPayloadSize + 1u)});
    T2D_CHECK(refuses(huge));
    T2D_CHECK_EQ(save::kMaxPayloadSize, 64u << 20);
}

T2D_TEST(a_save_round_trips_through_a_file) {
    const std::filesystem::path path = scratch_path("puzzle.t2dsave");
    std::error_code code;
    std::filesystem::create_directories(path.parent_path(), code);
    save::File file;
    file.game = "puzzle";
    file.game_version = 3;
    file.chunks.push_back(u32_chunk(save::fourcc('T', 'U', 'R', 'N'), 7));
    T2D_CHECK(save::write_file(path.string(), file));
    std::string error;
    const std::optional<save::File> back = save::read_file(path.string(), &error);
    T2D_REQUIRE(back.has_value());
    T2D_CHECK(error.empty());
    T2D_CHECK_EQ(back->game, std::string("puzzle"));
    T2D_REQUIRE(back->chunks.size() == 1u);
    T2D_CHECK_EQ(back->chunks[0].id, save::fourcc('T', 'U', 'R', 'N'));
    std::filesystem::remove_all(path.parent_path(), code);

    // What is not there says so, with the path in the reason.
    error.clear();
    T2D_CHECK_FALSE(save::read_file(path.string(), &error).has_value());
    T2D_CHECK(error.find("puzzle.t2dsave") != std::string::npos);

    // A file that is not a save is refused by the decoder rather than read as one.
    const std::filesystem::path notes = scratch_path("notes.t2dsave");
    std::filesystem::create_directories(notes.parent_path(), code);
    {
        std::FILE* out = std::fopen(notes.string().c_str(), "wb");
        T2D_REQUIRE(out != nullptr);
        std::fputs("this is not a save", out);
        std::fclose(out);
    }
    error.clear();
    T2D_CHECK_FALSE(save::read_file(notes.string(), &error).has_value());
    T2D_CHECK(error.find("magic") != std::string::npos);
    std::filesystem::remove_all(notes.parent_path(), code);
}

T2D_TEST(names_get_the_save_extension) {
    T2D_CHECK(save::ends_with_extension("campaign.t2dsave"));
    T2D_CHECK(save::ends_with_extension("Campaign.T2DSAVE"));   // a name is not case sensitive
    T2D_CHECK(save::ends_with_extension("a.b.t2dsave"));
    T2D_CHECK_FALSE(save::ends_with_extension("campaign.save"));
    T2D_CHECK_FALSE(save::ends_with_extension("t2dsave"));
    T2D_CHECK_FALSE(save::ends_with_extension(""));
    T2D_CHECK_EQ(save::with_extension("campaign"), std::string("campaign.t2dsave"));
    T2D_CHECK_EQ(save::with_extension("saves/campaign"), std::string("saves/campaign.t2dsave"));
    T2D_CHECK_EQ(save::with_extension("campaign.t2dsave"), std::string("campaign.t2dsave"));
    T2D_CHECK_EQ(save::with_extension("campaign.T2DSAVE"), std::string("campaign.T2DSAVE"));
}

T2D_TEST_MAIN
