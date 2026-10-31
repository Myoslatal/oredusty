// The file-backed chunk store: the in-file hash index, append-only CRC records, torn-write fallback,
// tail recovery, growth and the single rewrite path that compacts and rebuilds a damaged index. The
// tests use real files in the system temporary directory and never sleep.
#include <t2d/sim/chunk_store.h>

#include <t2d/save/archive.h>   // save::crc32, for the hand-built crash-tail fixtures

#include <support/test_support.h>

#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace t2d;

namespace {

/// A path under the system temporary directory that no other run of this test uses.
[[nodiscard]] std::filesystem::path scratch_path(std::string_view name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string directory = std::string("t2d_chunk_store_") + std::to_string(stamp);
    return std::filesystem::temp_directory_path() / directory / std::string(name);
}

/// Owns the store path and removes the whole scratch directory when the test leaves scope.
struct Scratch {
    std::filesystem::path file;

    explicit Scratch(std::string_view name) : file(scratch_path(name)) {
        std::error_code ignored;
        std::filesystem::create_directories(file.parent_path(), ignored);
    }
    ~Scratch() {
        std::error_code ignored;
        std::filesystem::remove_all(file.parent_path(), ignored);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    [[nodiscard]] std::string string() const { return file.string(); }
    [[nodiscard]] u64 disk_size() const {
        std::error_code ignored;
        const auto size = std::filesystem::file_size(file, ignored);
        return ignored ? 0u : static_cast<u64>(size);
    }
};

[[nodiscard]] ChunkStore open_required(const Scratch& scratch,
                                     const ChunkStore::Options& options = ChunkStore::Options{}) {
    std::string error;
    std::optional<ChunkStore> store = ChunkStore::open(scratch.string(), options, &error);
    T2D_REQUIRE(store.has_value());
    T2D_CHECK_MSG(error.empty(), "a successful open reported '{}'", error);
    return std::move(*store);
}

/// Deterministic payload bytes, different per seed and size.
[[nodiscard]] std::vector<u8> make_payload(u32 seed, usize size) {
    std::vector<u8> payload(size);
    u32 state = seed * 1664525u + 1013904223u;
    for (u8& byte : payload) {
        state = state * 1664525u + 1013904223u;
        byte = static_cast<u8>((state >> 16u) & 0xFFu);
    }
    return payload;
}

[[nodiscard]] bool same_bytes(const std::vector<u8>& a, const std::vector<u8>& b) { return a == b; }

/// The deterministic key order the store promises for its rewrites, so a model map can use it too.
struct KeyLessForTest {
    [[nodiscard]] bool operator()(const ChunkKey& a, const ChunkKey& b) const {
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    }
};

// --- raw file pokes, so a test can build the crash states the writer itself refuses to produce ------

void put_u32_at(u8* out, usize offset, u32 value) {
    for (u32 index = 0; index < 4u; ++index) out[offset + index] = static_cast<u8>((value >> (index * 8u)) & 0xFFu);
}
void put_u64_at(u8* out, usize offset, u64 value) {
    for (u32 index = 0; index < 8u; ++index) out[offset + index] = static_cast<u8>((value >> (index * 8u)) & 0xFFu);
}

/// The same 40-byte record header the store writes, built by hand so a test can append a valid record
/// after the committed data_end (a crash after append but before the header commit).
[[nodiscard]] std::vector<u8> raw_record(const ChunkKey& key, const std::vector<u8>& payload, u32 state = 1u) {
    std::array<u8, ChunkStore::kRecordHeaderSize> header{};
    put_u32_at(header.data(), 0, ChunkStore::kRecordMagic);
    put_u32_at(header.data(), 4, state);
    put_u32_at(header.data(), 8, static_cast<u32>(payload.size()));
    put_u64_at(header.data(), 12, std::bit_cast<u64>(key.x));
    put_u64_at(header.data(), 20, std::bit_cast<u64>(key.y));
    put_u32_at(header.data(), 28, save::crc32(ConstSpan<const u8>(payload)));
    put_u32_at(header.data(), 32, save::crc32(ConstSpan<const u8>(header.data(), 32)));
    put_u32_at(header.data(), 36, 0u);
    std::vector<u8> record(header.begin(), header.end());
    record.insert(record.end(), payload.begin(), payload.end());
    return record;
}

void append_bytes(const std::filesystem::path& path, ConstSpan<const u8> bytes) {
    std::FILE* file = std::fopen(path.string().c_str(), "ab");
    T2D_REQUIRE(file != nullptr);
    T2D_REQUIRE(bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
    T2D_REQUIRE(std::fclose(file) == 0);
}

void flip_byte(const std::filesystem::path& path, u64 offset) {
    std::FILE* file = std::fopen(path.string().c_str(), "r+b");
    T2D_REQUIRE(file != nullptr);
    T2D_REQUIRE(std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0);
    u8 byte = 0;
    T2D_REQUIRE(std::fread(&byte, 1, 1, file) == 1);
    byte ^= 0xFFu;
    T2D_REQUIRE(std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0);
    T2D_REQUIRE(std::fwrite(&byte, 1, 1, file) == 1);
    T2D_REQUIRE(std::fclose(file) == 0);
}

void zero_range(const std::filesystem::path& path, u64 offset, usize count) {
    std::FILE* file = std::fopen(path.string().c_str(), "r+b");
    T2D_REQUIRE(file != nullptr);
    T2D_REQUIRE(std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0);
    const std::vector<u8> zeros(count, 0u);
    T2D_REQUIRE(std::fwrite(zeros.data(), 1, zeros.size(), file) == zeros.size());
    T2D_REQUIRE(std::fclose(file) == 0);
}

[[nodiscard]] u64 first_record_offset(const ChunkStore& store) {
    return ChunkStore::kIndexOffset + static_cast<u64>(store.stats().slots) * ChunkStore::kSlotSize;
}

[[nodiscard]] const ChunkStore::Options default_options() {
    ChunkStore::Options options;
    options.initial_slots = 16;
    options.max_load_factor = 0.7f;
    return options;
}

// --- raw v2 file helpers, so a test can damage exactly one slot/record and check the rest -----------

[[nodiscard]] u32 raw_u32(const u8* at) {
    u32 value = 0;
    for (u32 index = 0; index < 4u; ++index) value |= static_cast<u32>(at[index]) << (index * 8u);
    return value;
}
[[nodiscard]] u64 raw_u64(const u8* at) {
    u64 value = 0;
    for (u32 index = 0; index < 8u; ++index) value |= static_cast<u64>(at[index]) << (index * 8u);
    return value;
}
[[nodiscard]] i64 raw_i64(const u8* at) { return std::bit_cast<i64>(raw_u64(at)); }

[[nodiscard]] bool raw_all_zero(const u8* at, usize count) {
    for (usize index = 0; index < count; ++index) {
        if (at[index] != 0u) return false;
    }
    return true;
}

[[nodiscard]] std::vector<u8> read_whole_file(const std::filesystem::path& path) {
    std::FILE* file = std::fopen(path.string().c_str(), "rb");
    T2D_REQUIRE(file != nullptr);
    T2D_REQUIRE(std::fseek(file, 0, SEEK_END) == 0);
    const long size = std::ftell(file);
    T2D_REQUIRE(size >= 0);
    T2D_REQUIRE(std::fseek(file, 0, SEEK_SET) == 0);
    std::vector<u8> bytes(static_cast<usize>(size));
    if (!bytes.empty()) T2D_REQUIRE(std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size());
    T2D_REQUIRE(std::fclose(file) == 0);
    return bytes;
}

void write_whole_file(const std::filesystem::path& path, ConstSpan<const u8> bytes) {
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    T2D_REQUIRE(file != nullptr);
    T2D_REQUIRE(bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
    T2D_REQUIRE(std::fclose(file) == 0);
}

/// The slot/half that currently holds a live, CRC-valid entry for the key.
[[nodiscard]] bool find_live_slot(const std::vector<u8>& file, u32 slots, const ChunkKey& key,
                                  u32& slot_index, int& half_index) {
    for (u32 slot = 0; slot < slots; ++slot) {
        for (int half = 0; half < 2; ++half) {
            const u8* at = file.data() + ChunkStore::kIndexOffset +
                           static_cast<u64>(slot) * ChunkStore::kSlotSize +
                           static_cast<u64>(half) * ChunkStore::kSlotHalfSize;
            if (raw_all_zero(at, ChunkStore::kSlotHalfSize)) continue;
            if (raw_u32(at + 32) != save::crc32(ConstSpan<const u8>(at, 32))) continue;
            if ((raw_u32(at) & 3u) != 1u) continue;   // live
            if (raw_i64(at + 4) != key.x || raw_i64(at + 12) != key.y) continue;
            slot_index = slot;
            half_index = half;
            return true;
        }
    }
    return false;
}

/// The file offset of the live record for key, scanning the log (independently of the index).
[[nodiscard]] bool find_record_for_key(const std::vector<u8>& file, u32 slots, const ChunkKey& key,
                                       u64& record_offset) {
    u64 position = ChunkStore::kIndexOffset + static_cast<u64>(slots) * ChunkStore::kSlotSize;
    while (position + ChunkStore::kRecordHeaderSize <= file.size()) {
        const u8* at = file.data() + position;
        if (raw_u32(at) != ChunkStore::kRecordMagic ||
            raw_u32(at + 32) != save::crc32(ConstSpan<const u8>(at, 32))) {
            ++position;
            continue;
        }
        const u32 state = raw_u32(at + 4);
        const u32 size = raw_u32(at + 8);
        if (position + ChunkStore::kRecordHeaderSize + size > file.size()) break;
        if (state == 1u && raw_i64(at + 12) == key.x && raw_i64(at + 20) == key.y) {
            record_offset = position;
            return true;
        }
        position += ChunkStore::kRecordHeaderSize + size;
    }
    return false;
}

/// Rewrites both header copies to another format version, keeping their CRCs valid.
void rewrite_header_version(const std::filesystem::path& path, u32 version) {
    std::vector<u8> file = read_whole_file(path);
    T2D_REQUIRE(file.size() >= 2u * ChunkStore::kHeaderCopySize);
    for (u32 copy = 0; copy < 2u; ++copy) {
        u8* page = file.data() + static_cast<u64>(copy) * ChunkStore::kHeaderCopySize;
        put_u32_at(page, 4, version);
        put_u32_at(page, 56, save::crc32(ConstSpan<const u8>(page, 56)));
    }
    write_whole_file(path, file);
}

/// Flip a byte of the half that holds key's live entry (its CRC becomes invalid -> poisoned).
void poison_live_slot(const std::filesystem::path& path, u32 slots, const ChunkKey& key) {
    const std::vector<u8> file = read_whole_file(path);
    u32 slot = 0;
    int half = 0;
    T2D_REQUIRE(find_live_slot(file, slots, key, slot, half));
    const u64 offset = ChunkStore::kIndexOffset + static_cast<u64>(slot) * ChunkStore::kSlotSize +
                       static_cast<u64>(half) * ChunkStore::kSlotHalfSize + 8u;   // inside key_y
    flip_byte(path, offset);
}

/// Poison every all-zero (valid empty) slot, so any probe that would have stopped early must continue.
void poison_empty_slots(const std::filesystem::path& path, u32 slots) {
    const std::vector<u8> file = read_whole_file(path);
    for (u32 slot = 0; slot < slots; ++slot) {
        const u64 base = ChunkStore::kIndexOffset + static_cast<u64>(slot) * ChunkStore::kSlotSize;
        if (!raw_all_zero(file.data() + base, ChunkStore::kSlotSize)) continue;
        flip_byte(path, base + 20u);   // inside record_offset; the all-zero half becomes poisoned
    }
}

/// Corrupt the payload byte of key's live record, leaving its header intact.
void corrupt_record_payload(const std::filesystem::path& path, u32 slots, const ChunkKey& key) {
    const std::vector<u8> file = read_whole_file(path);
    u64 offset = 0;
    T2D_REQUIRE(find_record_for_key(file, slots, key, offset));
    flip_byte(path, offset + ChunkStore::kRecordHeaderSize);
}

/// Corrupt a record header (magic) at a known offset.
void corrupt_record_header(const std::filesystem::path& path, u64 record_offset) {
    flip_byte(path, record_offset);
}

[[nodiscard]] bool report_has_kind(const ScanReport& report, CorruptChunk::Kind kind) {
    for (const CorruptChunk& chunk : report.corruptions) {
        if (chunk.kind == kind) return true;
    }
    return false;
}

[[nodiscard]] bool report_has_key(const ScanReport& report, const ChunkKey& key) {
    for (const CorruptChunk& chunk : report.corruptions) {
        if (chunk.has_key && chunk.key == key) return true;
    }
    return false;
}

[[nodiscard]] bool report_has_offset(const ScanReport& report, CorruptChunk::Kind kind, u64 offset) {
    for (const CorruptChunk& chunk : report.corruptions) {
        if (chunk.kind == kind && chunk.record_offset == offset) return true;
    }
    return false;
}

} // namespace

T2D_TEST(a_store_is_created_as_a_hash_file_and_reopens_empty) {
    Scratch scratch("empty.t2dk");
    {
        ChunkStore store = open_required(scratch, default_options());
        T2D_CHECK(store.is_open());
        T2D_CHECK_EQ(store.path(), scratch.string());
        T2D_CHECK_EQ(store.chunk_count(), 0u);
        T2D_CHECK_EQ(store.stats().slots, 16u);
        T2D_CHECK_EQ(store.file_size(), ChunkStore::kIndexOffset + 16ull * ChunkStore::kSlotSize);
        T2D_CHECK_EQ(scratch.disk_size(), store.file_size());
        T2D_CHECK_EQ(ChunkStore::kHeaderCopySize, 4096u);
        T2D_CHECK_EQ(ChunkStore::kIndexOffset, 8192u);
        T2D_CHECK_EQ(ChunkStore::kSlotHalfSize, 36u);
        T2D_CHECK_EQ(ChunkStore::kSlotSize, 72u);
        T2D_CHECK_EQ(ChunkStore::kFormatVersion, 2u);

        std::string error;
        T2D_CHECK_FALSE(store.contains(ChunkKey{0, 0}));
        T2D_CHECK_FALSE(store.read(ChunkKey{0, 0}, &error).has_value());
        T2D_CHECK(error.empty());   // a missing key is not an error
        T2D_CHECK_EQ(store.stats().lookups, 2u);   // contains() is a lookup too
        T2D_CHECK_EQ(store.stats().misses, 2u);
    }
    {
        ChunkStore reopened = open_required(scratch, default_options());
        T2D_CHECK_EQ(reopened.chunk_count(), 0u);
        T2D_CHECK_EQ(reopened.file_size(), ChunkStore::kIndexOffset + 16ull * ChunkStore::kSlotSize);
        T2D_CHECK_FALSE(reopened.contains(ChunkKey{1, -1}));
    }
}

T2D_TEST(write_and_read_round_trip_exact_bytes_including_empty_payloads) {
    Scratch scratch("roundtrip.t2dk");
    ChunkStore store = open_required(scratch, default_options());
    const std::vector<u8> first = make_payload(1u, 37u);
    const std::vector<u8> empty;
    const std::vector<u8> third = make_payload(3u, 1u);

    T2D_CHECK(store.write(ChunkKey{2, 3}, first));
    T2D_CHECK(store.write(ChunkKey{-7, 9}, empty));
    T2D_CHECK(store.write(ChunkKey{100, -200}, third));
    T2D_CHECK_EQ(store.chunk_count(), 3u);
    T2D_CHECK(store.contains(ChunkKey{2, 3}));
    T2D_CHECK_EQ(store.stats().writes, 3u);

    store.reset_stats();
    std::string error;
    const std::optional<std::vector<u8>> back = store.read(ChunkKey{2, 3}, &error);
    T2D_REQUIRE(back.has_value());
    T2D_CHECK(same_bytes(*back, first));
    T2D_CHECK(error.empty());
    const std::optional<std::vector<u8>> empty_back = store.read(ChunkKey{-7, 9});
    T2D_REQUIRE(empty_back.has_value());
    T2D_CHECK_EQ(empty_back->size(), 0u);
    const std::optional<std::vector<u8>> third_back = store.read(ChunkKey{100, -200});
    T2D_REQUIRE(third_back.has_value());
    T2D_CHECK(same_bytes(*third_back, third));
    T2D_CHECK_GT(store.file_size(), ChunkStore::kIndexOffset + 16ull * ChunkStore::kSlotSize);
    T2D_CHECK_GT(store.stats().bytes_read, 0u);
    T2D_CHECK_EQ(store.stats().hits, 3u);
}

T2D_TEST(overwrites_append_a_new_record_and_read_new_bytes) {
    Scratch scratch("overwrite.t2dk");
    ChunkStore store = open_required(scratch, default_options());
    const ChunkKey key{4, -4};
    const std::vector<u8> first = make_payload(11u, 64u);
    const std::vector<u8> second = make_payload(12u, 8u);
    T2D_CHECK(store.write(key, first));
    const u64 after_first = store.file_size();
    T2D_CHECK(store.write(key, second));
    T2D_CHECK_EQ(store.chunk_count(), 1u);
    T2D_CHECK_EQ(store.stats().overwrites, 1u);
    T2D_CHECK_EQ(store.stats().writes, 2u);
    T2D_CHECK_GT(store.file_size(), after_first);   // append-only until compact()
    const std::optional<std::vector<u8>> back = store.read(key);
    T2D_REQUIRE(back.has_value());
    T2D_CHECK(same_bytes(*back, second));
    T2D_CHECK(store.flush(true));

    store = open_required(scratch, default_options());
    const std::optional<std::vector<u8>> reopened = store.read(key);
    T2D_REQUIRE(reopened.has_value());
    T2D_CHECK(same_bytes(*reopened, second));
    T2D_CHECK_EQ(store.chunk_count(), 1u);
}

T2D_TEST(negative_and_i64_extreme_keys_work) {
    Scratch scratch("keys.t2dk");
    ChunkStore store = open_required(scratch, default_options());
    const ChunkKey keys[] = {
        ChunkKey{-1, -1},
        ChunkKey{std::numeric_limits<i64>::min(), std::numeric_limits<i64>::max()},
        ChunkKey{std::numeric_limits<i64>::max(), std::numeric_limits<i64>::min()},
        ChunkKey{0, 0},
        ChunkKey{-9223372036854775807ll, 1234567890123ll},
    };
    for (usize index = 0; index < std::size(keys); ++index) {
        const std::vector<u8> payload = make_payload(static_cast<u32>(index) + 20u, 16u + index);
        T2D_CHECK(store.write(keys[index], payload));
        T2D_CHECK(store.contains(keys[index]));
    }
    T2D_CHECK_EQ(store.chunk_count(), std::size(keys));
    for (usize index = 0; index < std::size(keys); ++index) {
        const std::optional<std::vector<u8>> back = store.read(keys[index]);
        T2D_REQUIRE(back.has_value());
        T2D_CHECK(same_bytes(*back, make_payload(static_cast<u32>(index) + 20u, 16u + index)));
    }
    store = open_required(scratch, default_options());
    for (const ChunkKey& key : keys) T2D_CHECK(store.contains(key));
}

T2D_TEST(erase_is_idempotent_and_a_write_after_it_works) {
    Scratch scratch("erase.t2dk");
    ChunkStore store = open_required(scratch, default_options());
    const ChunkKey key{-3, 8};
    T2D_CHECK(store.write(key, make_payload(1u, 20u)));
    T2D_CHECK_EQ(store.chunk_count(), 1u);
    T2D_CHECK(store.erase(key));
    T2D_CHECK_EQ(store.chunk_count(), 0u);
    T2D_CHECK_EQ(store.stats().erases, 1u);
    T2D_CHECK_FALSE(store.contains(key));
    T2D_CHECK_FALSE(store.read(key).has_value());
    T2D_CHECK(store.erase(key));                       // idempotent: missing is still success
    T2D_CHECK_EQ(store.stats().erases, 1u);
    T2D_CHECK_EQ(store.chunk_count(), 0u);

    const std::vector<u8> again = make_payload(2u, 30u);
    T2D_CHECK(store.write(key, again));                // a tombstone is reusable
    const std::optional<std::vector<u8>> back = store.read(key);
    T2D_REQUIRE(back.has_value());
    T2D_CHECK(same_bytes(*back, again));
    T2D_CHECK_EQ(store.chunk_count(), 1u);
    T2D_CHECK(store.flush(true));

    store = open_required(scratch, default_options());
    const std::optional<std::vector<u8>> reopened = store.read(key);
    T2D_REQUIRE(reopened.has_value());
    T2D_CHECK(same_bytes(*reopened, again));
    T2D_CHECK_EQ(store.chunk_count(), 1u);
    T2D_CHECK_FALSE(store.contains(ChunkKey{99, 99}));
}

T2D_TEST(many_chunks_survive_growth_and_every_one_reads_after_reopen) {
    Scratch scratch("many.t2dk");
    ChunkStore::Options options = default_options();
    options.initial_slots = 16;
    ChunkStore store = open_required(scratch, options);
    constexpr u32 kChunks = 2048;
    for (u32 index = 0; index < kChunks; ++index) {
        const ChunkKey key{static_cast<i64>(index % 64u) - 32, static_cast<i64>(index / 64u) - 16};
        T2D_CHECK(store.write(key, make_payload(index, 24u)));
    }
    T2D_CHECK_EQ(store.chunk_count(), kChunks);
    T2D_CHECK_GT(store.stats().slots, options.initial_slots);
    T2D_CHECK_GE(store.stats().slots, 4096u);   // 2048 / 0.7 needs 4096 slots
    T2D_CHECK_GT(store.stats().rewrites, 0u);
    T2D_CHECK(store.flush(true));

    store = open_required(scratch, options);
    T2D_CHECK_EQ(store.chunk_count(), kChunks);
    store.reset_stats();
    for (u32 index = 0; index < kChunks; ++index) {
        const ChunkKey key{static_cast<i64>(index % 64u) - 32, static_cast<i64>(index / 64u) - 16};
        const std::optional<std::vector<u8>> back = store.read(key);
        T2D_REQUIRE(back.has_value());
        T2D_CHECK(same_bytes(*back, make_payload(index, 24u)));
    }
    T2D_CHECK_EQ(store.stats().lookups, kChunks);
    T2D_CHECK_EQ(store.stats().hits, kChunks);
    T2D_CHECK_EQ(store.stats().misses, 0u);
    // A read is: probes (one 72-byte slot read each) plus exactly one record read on a hit.
    T2D_CHECK_EQ(store.stats().reads, store.stats().probes + store.stats().hits);
    T2D_CHECK_LT(store.stats().probes, 4ull * store.stats().lookups);   // no per-lookup scan
    T2D_CHECK_LT(store.stats().bytes_read, 2ull * store.file_size());
    for (u32 missing = 0; missing < 100u; ++missing) {
        T2D_CHECK_FALSE(store.read(ChunkKey{static_cast<i64>(missing) + 1000, 0}).has_value());
    }
    T2D_CHECK_EQ(store.stats().misses, 100u);
}

T2D_TEST(compaction_reclaims_overwritten_and_erased_space_and_is_idempotent) {
    Scratch scratch("compact.t2dk");
    ChunkStore::Options options = default_options();
    options.initial_slots = 64;
    ChunkStore store = open_required(scratch, options);
    for (u32 index = 0; index < 200u; ++index) {
        T2D_CHECK(store.write(ChunkKey{static_cast<i64>(index), 0}, make_payload(index, 1024u)));
    }
    for (u32 index = 0; index < 80u; ++index) {
        T2D_CHECK(store.write(ChunkKey{static_cast<i64>(index), 0}, make_payload(index + 500u, 100u)));
    }
    for (u32 index = 80u; index < 120u; ++index) T2D_CHECK(store.erase(ChunkKey{static_cast<i64>(index), 0}));
    T2D_CHECK_EQ(store.chunk_count(), 160u);
    T2D_CHECK(store.flush(true));
    const u64 before = store.file_size();
    store.reset_stats();
    T2D_CHECK(store.compact());
    const u64 after = store.file_size();
    T2D_CHECK_LT(after, before);
    T2D_CHECK_EQ(store.chunk_count(), 160u);
    T2D_CHECK_EQ(store.stats().rewrites, 1u);
    for (u32 index = 0; index < 80u; ++index) {
        const std::optional<std::vector<u8>> back = store.read(ChunkKey{static_cast<i64>(index), 0});
        T2D_REQUIRE(back.has_value());
        T2D_CHECK(same_bytes(*back, make_payload(index + 500u, 100u)));
    }
    for (u32 index = 120u; index < 200u; ++index) {
        const std::optional<std::vector<u8>> back = store.read(ChunkKey{static_cast<i64>(index), 0});
        T2D_REQUIRE(back.has_value());
        T2D_CHECK(same_bytes(*back, make_payload(index, 1024u)));
    }
    for (u32 index = 80u; index < 120u; ++index) {
        T2D_CHECK_FALSE(store.read(ChunkKey{static_cast<i64>(index), 0}).has_value());
    }

    T2D_CHECK(store.compact());   // idempotent: no garbage left, same deterministic layout
    T2D_CHECK_EQ(store.file_size(), after);
    T2D_CHECK_EQ(store.chunk_count(), 160u);
    store = open_required(scratch, options);
    T2D_CHECK_EQ(store.chunk_count(), 160u);
    for (u32 index = 120u; index < 200u; ++index) {
        T2D_CHECK(store.contains(ChunkKey{static_cast<i64>(index), 0}));
    }
}

T2D_TEST(a_corrupt_payload_is_an_error_never_the_wrong_bytes) {
    Scratch scratch("corrupt-payload.t2dk");
    const ChunkKey key{7, 7};
    {
        ChunkStore store = open_required(scratch, default_options());
        T2D_CHECK(store.write(key, make_payload(7u, 48u)));
        T2D_CHECK(store.flush(true));
        const u64 record = first_record_offset(store);
        T2D_CHECK_EQ(record, ChunkStore::kIndexOffset + 16ull * ChunkStore::kSlotSize);
        T2D_CHECK_EQ(store.file_size(), record + ChunkStore::kRecordHeaderSize + 48u);
    }
    flip_byte(scratch.file, ChunkStore::kIndexOffset + 16ull * ChunkStore::kSlotSize + ChunkStore::kRecordHeaderSize);

    ChunkStore store = open_required(scratch, default_options());
    std::string error;
    const std::optional<std::vector<u8>> back = store.read(key, &error);
    T2D_CHECK_FALSE(back.has_value());
    T2D_CHECK_FALSE(error.empty());
    T2D_CHECK_GE(store.stats().corrupt_records, 1u);
    T2D_CHECK_FALSE(store.contains(key));   // corrupt reads as absent here, with the failure counted

    // Overwriting the key appends a fresh record and the new value reads cleanly again.
    const std::vector<u8> fixed = make_payload(8u, 12u);
    T2D_CHECK(store.write(key, fixed));
    const std::optional<std::vector<u8>> good = store.read(key);
    T2D_REQUIRE(good.has_value());
    T2D_CHECK(same_bytes(*good, fixed));
}

T2D_TEST(a_corrupt_record_magic_is_reported_and_the_store_still_opens) {
    Scratch scratch("corrupt-record.t2dk");
    const ChunkKey key{-9, 2};
    {
        ChunkStore store = open_required(scratch, default_options());
        T2D_CHECK(store.write(key, make_payload(9u, 24u)));
        T2D_CHECK(store.flush(true));
    }
    flip_byte(scratch.file, ChunkStore::kIndexOffset + 16ull * ChunkStore::kSlotSize);   // record magic

    std::string error;
    std::optional<ChunkStore> opened = ChunkStore::open(scratch.string(), default_options(), &error);
    T2D_REQUIRE(opened.has_value());   // a corrupt record is not a corrupt store
    T2D_CHECK(error.empty());
    T2D_CHECK_EQ(opened->chunk_count(), 1u);
    error.clear();
    T2D_CHECK_FALSE(opened->read(key, &error).has_value());
    T2D_CHECK_FALSE(error.empty());
    T2D_CHECK_GE(opened->stats().corrupt_records, 1u);
}

T2D_TEST(unreadable_headers_short_files_and_directories_fail_open_with_a_reason) {
    Scratch scratch("bad-open.t2dk");
    {
        ChunkStore store = open_required(scratch, default_options());
        T2D_CHECK(store.write(ChunkKey{1, 1}, make_payload(1u, 8u)));
        T2D_CHECK(store.flush(true));
    }
    flip_byte(scratch.file, 0);                              // copy A magic
    flip_byte(scratch.file, ChunkStore::kHeaderCopySize);    // copy B magic
    std::string error;
    T2D_CHECK_FALSE(ChunkStore::open(scratch.string(), default_options(), &error).has_value());
    T2D_CHECK(error.find("header") != std::string::npos);
    T2D_CHECK(error.find(scratch.file.filename().string()) != std::string::npos);

    // A short file is not a store, and neither is a directory.
    const std::filesystem::path short_path = scratch.file.parent_path() / "short.t2dk";
    {
        std::FILE* file = std::fopen(short_path.string().c_str(), "wb");
        T2D_REQUIRE(file != nullptr);
        const std::array<u8, 100> junk{};
        T2D_REQUIRE(std::fwrite(junk.data(), 1, junk.size(), file) == junk.size());
        T2D_REQUIRE(std::fclose(file) == 0);
    }
    error.clear();
    T2D_CHECK_FALSE(ChunkStore::open(short_path.string(), default_options(), &error).has_value());
    T2D_CHECK_FALSE(error.empty());
    T2D_CHECK(error.find("header") != std::string::npos);

    error.clear();
    T2D_CHECK_FALSE(
        ChunkStore::open(scratch.file.parent_path().string(), default_options(), &error).has_value());
    T2D_CHECK_FALSE(error.empty());
    error.clear();
    T2D_CHECK_FALSE(
        ChunkStore::open((scratch.file.parent_path() / "missing" / "x.t2dk").string(), default_options(), &error)
            .has_value());
    T2D_CHECK_FALSE(error.empty());
}

T2D_TEST(one_corrupt_header_copy_falls_back_to_the_other_and_recovers_the_tail) {
    const ChunkKey key{3, 9};
    const std::vector<u8> payload = make_payload(31u, 40u);

    // Newest header copy is B (create writes A and B, the first write commits B). Corrupt it: the
    // older copy A is still valid, and the record appended after A's data_end is recovered as a tail.
    {
        Scratch scratch("header-b.t2dk");
        {
            ChunkStore store = open_required(scratch, default_options());
            T2D_CHECK(store.write(key, payload));
            T2D_CHECK(store.flush(true));
        }
        flip_byte(scratch.file, ChunkStore::kHeaderCopySize + 8ull);   // B's sequence field/CRC region
        ChunkStore store = open_required(scratch, default_options());
        const std::optional<std::vector<u8>> back = store.read(key);
        T2D_REQUIRE(back.has_value());
        T2D_CHECK(same_bytes(*back, payload));
        T2D_CHECK_EQ(store.stats().tail_recovered, 1u);
        T2D_CHECK_EQ(store.chunk_count(), 1u);
    }
    // Corrupt the older copy A instead: the newest copy B names the record, no recovery is needed.
    {
        Scratch scratch("header-a.t2dk");
        {
            ChunkStore store = open_required(scratch, default_options());
            T2D_CHECK(store.write(key, payload));
            T2D_CHECK(store.flush(true));
        }
        flip_byte(scratch.file, 0);
        ChunkStore store = open_required(scratch, default_options());
        const std::optional<std::vector<u8>> back = store.read(key);
        T2D_REQUIRE(back.has_value());
        T2D_CHECK(same_bytes(*back, payload));
        T2D_CHECK_EQ(store.stats().tail_recovered, 0u);
    }
}

T2D_TEST(tail_records_are_recovered_and_partial_tails_are_ignored) {
    const ChunkKey first{1, 0};
    const ChunkKey tail{2, 0};
    const std::vector<u8> first_payload = make_payload(41u, 20u);
    const std::vector<u8> tail_payload = make_payload(42u, 12u);

    // A complete valid record after the committed data_end: recover it.
    {
        Scratch scratch("tail.t2dk");
        {
            ChunkStore store = open_required(scratch, default_options());
            T2D_CHECK(store.write(first, first_payload));
            T2D_CHECK(store.flush(true));
        }
        const auto record = raw_record(tail, tail_payload);
        append_bytes(scratch.file, ConstSpan<const u8>(record));
        ChunkStore store = open_required(scratch, default_options());
        T2D_CHECK_EQ(store.stats().tail_recovered, 1u);
        const std::optional<std::vector<u8>> back = store.read(tail);
        T2D_REQUIRE(back.has_value());
        T2D_CHECK(same_bytes(*back, tail_payload));
        const std::optional<std::vector<u8>> first_back = store.read(first);
        T2D_REQUIRE(first_back.has_value());
        T2D_CHECK(same_bytes(*first_back, first_payload));
        T2D_CHECK(store.flush(true));
        store = open_required(scratch, default_options());
        T2D_CHECK_EQ(store.stats().tail_recovered, 0u);   // the recovery was committed
        T2D_CHECK(store.contains(tail));
    }
    // A partial tail (a crash mid-append) stops the scan and never breaks the committed chunks.
    {
        Scratch scratch("partial-tail.t2dk");
        {
            ChunkStore store = open_required(scratch, default_options());
            T2D_CHECK(store.write(first, first_payload));
            T2D_CHECK(store.flush(true));
        }
        const auto record = raw_record(tail, tail_payload);
        append_bytes(scratch.file, ConstSpan<const u8>(record.data(), 20u));
        ChunkStore store = open_required(scratch, default_options());
        T2D_CHECK_EQ(store.stats().tail_recovered, 0u);
        const std::optional<std::vector<u8>> first_back = store.read(first);
        T2D_REQUIRE(first_back.has_value());
        T2D_CHECK(same_bytes(*first_back, first_payload));
        T2D_CHECK_EQ(store.chunk_count(), 1u);
        T2D_CHECK(store.write(tail, tail_payload));   // the partial tail is overwritten by the next append
        T2D_CHECK(store.flush(true));
        store = open_required(scratch, default_options());
        T2D_CHECK(store.contains(first));
        T2D_CHECK(store.contains(tail));
    }
}

T2D_TEST(a_moved_store_transfers_the_handle) {
    Scratch scratch("move.t2dk");
    const ChunkKey key{5, 6};
    const std::vector<u8> payload = make_payload(51u, 18u);
    ChunkStore first = open_required(scratch, default_options());
    T2D_CHECK(first.write(key, payload));
    ChunkStore second = std::move(first);
    T2D_CHECK_FALSE(first.is_open());
    T2D_CHECK(second.is_open());
    const std::optional<std::vector<u8>> back = second.read(key);
    T2D_REQUIRE(back.has_value());
    T2D_CHECK(same_bytes(*back, payload));

    ChunkStore third = open_required(scratch, default_options());
    third = std::move(second);
    T2D_CHECK_FALSE(second.is_open());
    T2D_CHECK_EQ(third.chunk_count(), 1u);
    T2D_CHECK(third.contains(key));
}

T2D_TEST(options_are_clamped_and_drive_growth) {
    {
        Scratch scratch("options-small.t2dk");
        ChunkStore store = open_required(scratch, ChunkStore::Options{3u, 0.0f});
        T2D_CHECK_EQ(store.stats().slots, 16u);   // min slots, not the requested 3
        T2D_CHECK_EQ(store.file_size(), ChunkStore::kIndexOffset + 16ull * ChunkStore::kSlotSize);
    }
    {
        Scratch scratch("options-round.t2dk");
        ChunkStore store = open_required(scratch, ChunkStore::Options{100u, 2.0f});
        T2D_CHECK_EQ(store.stats().slots, 128u);   // rounded up to a power of two
    }
    {
        // max_load_factor is clamped: 0.5 means growth after 8 of 16 slots, not 0.1 or 2.0.
        Scratch scratch("options-load.t2dk");
        ChunkStore store = open_required(scratch, ChunkStore::Options{16u, 0.5f});
        for (u32 index = 0; index < 8u; ++index) T2D_CHECK(store.write(ChunkKey{index, 0}, make_payload(index, 8u)));
        T2D_CHECK_EQ(store.stats().slots, 16u);
        T2D_CHECK(store.write(ChunkKey{8, 0}, make_payload(9u, 8u)));
        T2D_CHECK_EQ(store.stats().slots, 32u);
        T2D_CHECK_EQ(store.chunk_count(), 9u);
    }
    {
        // A NaN load factor falls back to the documented 0.7 default.
        Scratch scratch("options-nan.t2dk");
        ChunkStore store = open_required(scratch, ChunkStore::Options{16u, std::numeric_limits<f32>::quiet_NaN()});
        for (u32 index = 0; index < 11u; ++index) T2D_CHECK(store.write(ChunkKey{index, 1}, make_payload(index, 8u)));
        T2D_CHECK_EQ(store.stats().slots, 16u);   // 11 <= 16 * 0.7 = 11.2
        T2D_CHECK(store.write(ChunkKey{11, 1}, make_payload(99u, 8u)));
        T2D_CHECK_EQ(store.stats().slots, 32u);
    }
}

T2D_TEST(stats_are_plausible_and_reset_keeps_the_state_counters) {
    Scratch scratch("stats.t2dk");
    ChunkStore store = open_required(scratch, default_options());
    store.reset_stats();
    T2D_CHECK_EQ(store.stats().slots, 16u);
    T2D_CHECK_EQ(store.stats().chunk_count, 0u);
    T2D_CHECK_EQ(store.stats().lookups, 0u);

    const std::vector<u8> payload = make_payload(61u, 10u);
    T2D_CHECK(store.write(ChunkKey{1, 2}, payload));
    T2D_CHECK(store.write(ChunkKey{3, 4}, payload));
    T2D_CHECK_EQ(store.stats().writes, 2u);
    T2D_CHECK_EQ(store.stats().bytes_written, 2u * (ChunkStore::kRecordHeaderSize + payload.size()));
    T2D_CHECK_EQ(store.stats().chunk_count, 2u);
    store.reset_stats();
    T2D_CHECK_EQ(store.stats().writes, 0u);
    T2D_CHECK_EQ(store.stats().bytes_written, 0u);
    T2D_CHECK_EQ(store.stats().slots, 16u);
    T2D_CHECK_EQ(store.stats().chunk_count, 2u);

    T2D_CHECK(store.read(ChunkKey{1, 2}).has_value());
    T2D_CHECK_FALSE(store.read(ChunkKey{9, 9}).has_value());
    T2D_CHECK_EQ(store.stats().lookups, 2u);
    T2D_CHECK_EQ(store.stats().hits, 1u);
    T2D_CHECK_EQ(store.stats().misses, 1u);
    T2D_CHECK_EQ(store.stats().reads, store.stats().probes + store.stats().hits);
    T2D_CHECK_GT(store.stats().probes, 0u);
    T2D_CHECK_GT(store.stats().bytes_read, 0u);
}

T2D_TEST(a_read_touches_the_index_and_one_record_not_the_whole_file) {
    Scratch scratch("no-scan.t2dk");
    ChunkStore store = open_required(scratch, default_options());
    constexpr u32 kChunks = 512;
    for (u32 index = 0; index < kChunks; ++index) {
        const ChunkKey key{static_cast<i64>(index), -static_cast<i64>(index)};
        T2D_CHECK(store.write(key, make_payload(index, 128u)));
    }
    T2D_CHECK(store.flush(true));
    const u64 size = store.file_size();
    T2D_CHECK_GT(size, 512ull * ChunkStore::kRecordHeaderSize);

    store.reset_stats();
    const std::optional<std::vector<u8>> back = store.read(ChunkKey{123, -123});
    T2D_REQUIRE(back.has_value());
    T2D_CHECK(same_bytes(*back, make_payload(123, 128u)));
    T2D_CHECK_EQ(store.stats().lookups, 1u);
    T2D_CHECK_EQ(store.stats().hits, 1u);
    T2D_CHECK_LT(store.stats().probes, 8u);
    T2D_CHECK_LT(store.stats().bytes_read, 4096ull);
    T2D_CHECK_LT(store.stats().bytes_read, size / 8ull);
}

T2D_TEST(compact_rebuilds_a_damaged_index_from_the_record_log) {
    Scratch scratch("index-recovery.t2dk");
    ChunkStore::Options options = default_options();
    options.initial_slots = 32;
    {
        ChunkStore store = open_required(scratch, options);
        for (u32 index = 0; index < 50u; ++index) {
            T2D_CHECK(store.write(ChunkKey{static_cast<i64>(index), 0}, make_payload(index, 40u)));
        }
        T2D_CHECK_EQ(store.chunk_count(), 50u);
        T2D_CHECK(store.flush(true));
    }
    // Clobber the first part of the index: the header is fine, so open() succeeds, but the slots no
    // longer lead to the records. compact() must rebuild the index from the intact record log.
    zero_range(scratch.file, ChunkStore::kIndexOffset, 8u * ChunkStore::kSlotSize);
    ChunkStore store = open_required(scratch, options);
    T2D_CHECK_EQ(store.chunk_count(), 50u);
    T2D_CHECK(store.compact());
    T2D_CHECK_EQ(store.chunk_count(), 50u);
    T2D_CHECK_GE(store.stats().rewrites, 1u);
    for (u32 index = 0; index < 50u; ++index) {
        const std::optional<std::vector<u8>> back = store.read(ChunkKey{static_cast<i64>(index), 0});
        T2D_REQUIRE(back.has_value());
        T2D_CHECK(same_bytes(*back, make_payload(index, 40u)));
    }
    store = open_required(scratch, options);
    T2D_CHECK_EQ(store.chunk_count(), 50u);
    for (u32 index = 0; index < 50u; ++index) T2D_CHECK(store.contains(ChunkKey{static_cast<i64>(index), 0}));
}

T2D_TEST(a_real_open_world_round_trips_edits_through_the_store) {
    Scratch scratch("open-world.t2dk");
    OpenWorldConfig config;
    config.tile_size = 16.0f;
    config.layers = 1;
    config.seed = 0x51u;
    config.load_margin_chunks = 1;
    config.unload_margin_chunks = 2;
    config.max_loaded_chunks = 0;
    const Aabb2 view = Aabb2::from_top_left(Vec2{0.0f, 0.0f}, Vec2{512.0f, 512.0f});
    const auto generator = [](const ChunkKey&, WorldChunk& chunk) {
        for (TileId& cell : chunk.tiles(0)) cell = 1;
    };

    {
        ChunkStore store = open_required(scratch, default_options());
        OpenWorld world(config);
        world.set_generator(generator);
        world.set_store(store.loader(), store.saver());
        T2D_CHECK_EQ(world.update(view), 9u);
        T2D_CHECK_EQ(store.chunk_count(), 0u);   // generated chunks are clean and never stored
        T2D_CHECK(world.set(5, 5, 9));
        T2D_CHECK(world.flush());
        T2D_CHECK_EQ(store.chunk_count(), 1u);   // one edited chunk
        T2D_CHECK(store.contains(ChunkKey{0, 0}));
        T2D_CHECK(store.flush(true));
    }
    {
        ChunkStore store = open_required(scratch, default_options());
        T2D_CHECK_EQ(store.chunk_count(), 1u);
        OpenWorld world(config);
        world.set_generator(generator);
        world.set_store(store.loader(), store.saver());
        T2D_CHECK_EQ(world.update(view), 9u);
        T2D_CHECK_EQ(world.at(5, 5), 9);          // the edit was restored
        T2D_CHECK_EQ(world.at(1, 1), 1);          // its neighbour was regenerated, not loaded
        T2D_CHECK_EQ(world.dirty_chunk_count(), 0u);
        T2D_CHECK_EQ(store.stats().writes, 0u);

        // Rebasing changes the relative coordinates but not the absolute store key.
        const OpenWorld::OriginShift shift = world.set_origin(ChunkKey{1000, -1000});
        T2D_CHECK_EQ(shift.x, 1000 * WorldChunk::kSize);
        const i32 relative_x = 5 - 1000 * WorldChunk::kSize;
        const i32 relative_y = 5 + 1000 * WorldChunk::kSize;
        T2D_CHECK_EQ(world.at(relative_x, relative_y), 9);
        T2D_CHECK(store.contains(ChunkKey{0, 0}));
        T2D_CHECK_EQ(store.chunk_count(), 1u);
    }
}

T2D_TEST(a_mixed_write_erase_workload_matches_after_reopen) {
    Scratch scratch("mixed.t2dk");
    ChunkStore::Options options = default_options();
    options.initial_slots = 32;
    ChunkStore store = open_required(scratch, options);
    std::map<ChunkKey, std::vector<u8>, KeyLessForTest> model;

    for (u32 index = 0; index < 80u; ++index) {
        const ChunkKey key{static_cast<i64>(index) - 40, static_cast<i64>(index % 7u)};
        const std::vector<u8> payload = make_payload(index, 12u + index % 17u);
        T2D_CHECK(store.write(key, payload));
        model[key] = payload;
    }
    for (u32 index = 0; index < 80u; index += 3u) {
        const ChunkKey key{static_cast<i64>(index) - 40, static_cast<i64>(index % 7u)};
        T2D_CHECK(store.erase(key));
        model.erase(key);
    }
    for (u32 index = 0; index < 80u; index += 2u) {
        if (index % 3u == 0u) continue;   // erased above
        const ChunkKey key{static_cast<i64>(index) - 40, static_cast<i64>(index % 7u)};
        const std::vector<u8> replacement = make_payload(index + 900u, 9u);
        T2D_CHECK(store.write(key, replacement));
        model[key] = replacement;
    }
    T2D_CHECK_EQ(store.chunk_count(), model.size());
    T2D_CHECK(store.compact());
    T2D_CHECK(store.flush(true));

    store = open_required(scratch, options);
    T2D_CHECK_EQ(store.chunk_count(), model.size());
    for (const auto& entry : model) {
        const std::optional<std::vector<u8>> back = store.read(entry.first);
        T2D_REQUIRE(back.has_value());
        T2D_CHECK(same_bytes(*back, entry.second));
    }
    for (u32 index = 0; index < 80u; index += 3u) {
        const ChunkKey key{static_cast<i64>(index) - 40, static_cast<i64>(index % 7u)};
        if (model.find(key) == model.end()) T2D_CHECK_FALSE(store.contains(key));
    }
}

// --- v2: corruption isolation and game-chosen policy ---------------------------------------------

T2D_TEST(a_damaged_slot_half_is_isolated_and_never_impersonates) {
    Scratch scratch("slot-crc.t2dk");
    const ChunkKey a{1, 1};
    const ChunkKey b{2, 2};
    const ChunkKey c{3, 3};
    const std::vector<u8> payload_a = make_payload(1u, 20u);
    const std::vector<u8> payload_b = make_payload(2u, 20u);
    const std::vector<u8> payload_c = make_payload(3u, 20u);
    {
        ChunkStore store = open_required(scratch, default_options());
        T2D_CHECK(store.write(a, payload_a));
        T2D_CHECK(store.write(b, payload_b));
        T2D_CHECK(store.write(c, payload_c));
        T2D_CHECK(store.flush(true));
    }
    poison_live_slot(scratch.file, 16u, b);

    ChunkStore store = open_required(scratch, default_options());
    const ReadResult damaged = store.fetch(b);
    T2D_CHECK_MSG(damaged.status == ReadStatus::Corrupt || damaged.status == ReadStatus::Missing,
                  "a poisoned slot must not return bytes; status was {}", static_cast<int>(damaged.status));
    T2D_CHECK_FALSE(damaged.ok());
    T2D_CHECK(damaged.bytes.empty());
    if (damaged.status == ReadStatus::Corrupt) T2D_CHECK_FALSE(damaged.error.empty());

    // The other entries are untouched, in the same probe region or not.
    const ReadResult back_a = store.fetch(a);
    T2D_CHECK(back_a.ok());
    T2D_CHECK(same_bytes(back_a.bytes, payload_a));
    const ReadResult back_c = store.fetch(c);
    T2D_CHECK(back_c.ok());
    T2D_CHECK(same_bytes(back_c.bytes, payload_c));
    T2D_CHECK_GE(store.stats().corrupt_slots, 1u);

    // Writing the key again inserts a fresh valid entry into the poisoned half (repair).
    const std::vector<u8> fixed = make_payload(9u, 7u);
    T2D_CHECK(store.write(b, fixed));
    const ReadResult repaired = store.fetch(b);
    T2D_CHECK(repaired.ok());
    T2D_CHECK(same_bytes(repaired.bytes, fixed));
    T2D_CHECK(store.fetch(a).ok());
    T2D_CHECK(store.fetch(c).ok());
}

T2D_TEST(erase_drops_a_damaged_chunk_instead_of_letting_compact_resurrect_it) {
    Scratch scratch("erase-damaged.t2dk");
    const ChunkKey a{1, 1};
    const ChunkKey b{2, 2};
    const ChunkKey c{3, 3};
    const std::vector<u8> payload_a = make_payload(120u, 20u);
    const std::vector<u8> payload_b = make_payload(121u, 20u);
    const std::vector<u8> payload_c = make_payload(122u, 20u);
    {
        ChunkStore store = open_required(scratch, default_options());
        T2D_CHECK(store.write(a, payload_a));
        T2D_CHECK(store.write(b, payload_b));
        T2D_CHECK(store.write(c, payload_c));
        T2D_CHECK(store.flush(true));
    }
    poison_live_slot(scratch.file, 16u, a);

    {
        ChunkStore store = open_required(scratch, default_options());
        const ReadResult damaged = store.fetch(a);
        T2D_CHECK(damaged.status == ReadStatus::Corrupt || damaged.status == ReadStatus::Missing);
        // The game decides to drop the damaged chunk even though its last index entry was lost. The
        // deletion record must still be appended, or the record-log compaction below would resurrect it.
        T2D_CHECK(store.erase(a));
        T2D_CHECK(store.compact());
        T2D_CHECK(store.fetch(a).status == ReadStatus::Missing);
        T2D_CHECK_EQ(store.chunk_count(), 2u);
        const ReadResult back_b = store.fetch(b);
        T2D_CHECK(back_b.ok());
        T2D_CHECK(same_bytes(back_b.bytes, payload_b));
        const ReadResult back_c = store.fetch(c);
        T2D_CHECK(back_c.ok());
        T2D_CHECK(same_bytes(back_c.bytes, payload_c));
    }
    {
        ChunkStore store = open_required(scratch, default_options());
        T2D_CHECK_EQ(store.chunk_count(), 2u);
        T2D_CHECK(store.fetch(a).status == ReadStatus::Missing);
        T2D_CHECK(store.fetch(b).ok());
        T2D_CHECK(store.fetch(c).ok());
    }
}

T2D_TEST(a_poisoned_empty_slot_does_not_terminate_a_probe_or_hide_keys) {
    Scratch scratch("poison-empty.t2dk");
    const ChunkKey keys[] = {ChunkKey{0, 0}, ChunkKey{1, -1}, ChunkKey{-2, 3},
                             ChunkKey{4, 5}, ChunkKey{6, -7}, ChunkKey{-8, 9}};
    std::vector<std::vector<u8>> payloads;
    {
        ChunkStore store = open_required(scratch, default_options());
        for (usize index = 0; index < std::size(keys); ++index) {
            payloads.push_back(make_payload(static_cast<u32>(index) + 70u, 18u));
            T2D_CHECK(store.write(keys[index], payloads.back()));
        }
        T2D_CHECK(store.flush(true));
    }
    // Every all-zero slot becomes a poisoned (non-terminating) slot. A probe that stops at an empty
    // slot would now stop at a poisoned one instead; it must keep going and still find the keys.
    poison_empty_slots(scratch.file, 16u);
    {
        ChunkStore store = open_required(scratch, default_options());
        for (usize index = 0; index < std::size(keys); ++index) {
            const ReadResult result = store.fetch(keys[index]);
            T2D_CHECK(result.ok());
            T2D_CHECK(same_bytes(result.bytes, payloads[index]));
        }
        const ReadResult missing = store.fetch(ChunkKey{40, 40});
        T2D_CHECK(missing.status == ReadStatus::Missing);
        T2D_CHECK(missing.error.empty());
    }

    // Poisoning a live slot for key 0 does not make the other keys unreadable either.
    poison_live_slot(scratch.file, 16u, keys[0]);
    {
        ChunkStore store = open_required(scratch, default_options());
        for (usize index = 1; index < std::size(keys); ++index) {
            const ReadResult result = store.fetch(keys[index]);
            T2D_CHECK(result.ok());
            T2D_CHECK(same_bytes(result.bytes, payloads[index]));
        }
        const ReadResult damaged = store.fetch(keys[0]);
        T2D_CHECK(damaged.status == ReadStatus::Corrupt || damaged.status == ReadStatus::Missing);
    }
}

T2D_TEST(fetch_distinguishes_ok_missing_corrupt_and_not_open) {
    Scratch scratch("status.t2dk");
    const ChunkKey key{5, 5};
    const std::vector<u8> payload = make_payload(55u, 33u);
    {
        ChunkStore store = open_required(scratch, default_options());
        T2D_CHECK(store.write(key, payload));
        T2D_CHECK(store.flush(true));

        const ReadResult ok = store.fetch(key);
        T2D_CHECK(ok.status == ReadStatus::Ok);
        T2D_CHECK(ok.ok());
        T2D_CHECK(ok.error.empty());
        T2D_CHECK(same_bytes(ok.bytes, payload));

        const ReadResult missing = store.fetch(ChunkKey{8, 8});
        T2D_CHECK(missing.status == ReadStatus::Missing);
        T2D_CHECK(missing.error.empty());
        T2D_CHECK(missing.bytes.empty());
        std::string missing_error;
        T2D_CHECK_FALSE(store.read(ChunkKey{8, 8}, &missing_error).has_value());
        T2D_CHECK(missing_error.empty());

        ChunkStore moved = std::move(store);
        const ReadResult not_open = store.fetch(key);
        T2D_CHECK(not_open.status == ReadStatus::NotOpen);
        T2D_CHECK_FALSE(not_open.error.empty());
        std::string not_open_error;
        T2D_CHECK_FALSE(store.read(key, &not_open_error).has_value());
        T2D_CHECK_FALSE(not_open_error.empty());
        T2D_CHECK(moved.fetch(key).ok());
    }
    corrupt_record_payload(scratch.file, 16u, key);
    {
        ChunkStore store = open_required(scratch, default_options());
        const ReadResult corrupt = store.fetch(key);
        T2D_CHECK(corrupt.status == ReadStatus::Corrupt);
        T2D_CHECK(corrupt.bytes.empty());
        T2D_CHECK_FALSE(corrupt.error.empty());
        T2D_CHECK_FALSE(store.contains(key));
        std::string error;
        T2D_CHECK_FALSE(store.read(key, &error).has_value());
        T2D_CHECK_FALSE(error.empty());
    }
}

T2D_TEST(scan_reports_healthy_and_damaged_entries_and_does_not_modify_the_file) {
    Scratch scratch("scan.t2dk");
    const ChunkKey a{0, 0};
    const ChunkKey b{1, 0};
    const ChunkKey c{0, 1};
    const std::vector<u8> payload_a = make_payload(81u, 20u);
    const std::vector<u8> payload_b = make_payload(82u, 20u);
    const std::vector<u8> payload_c = make_payload(83u, 20u);
    {
        ChunkStore store = open_required(scratch, default_options());
        T2D_CHECK(store.write(a, payload_a));
        T2D_CHECK(store.write(b, payload_b));
        T2D_CHECK(store.write(c, payload_c));
        T2D_CHECK(store.flush(true));
    }
    const std::vector<u8> before = read_whole_file(scratch.file);
    {
        ChunkStore store = open_required(scratch, default_options());
        const ScanReport healthy = store.scan();
        T2D_CHECK(healthy.ok());
        T2D_CHECK(healthy.corruptions.empty());
        T2D_CHECK_EQ(healthy.live_chunks, 3u);
        T2D_CHECK_EQ(healthy.checked_slots, 16u);
        T2D_CHECK_EQ(healthy.checked_records, 3u);
    }
    T2D_CHECK(read_whole_file(scratch.file) == before);   // a scan never writes

    corrupt_record_payload(scratch.file, 16u, b);
    const std::vector<u8> after_payload = read_whole_file(scratch.file);
    {
        ChunkStore store = open_required(scratch, default_options());
        const ScanReport report = store.scan();
        T2D_CHECK(report.ok());
        T2D_CHECK(report_has_key(report, b));
        T2D_CHECK(report_has_kind(report, CorruptChunk::Kind::RecordPayload));
        T2D_CHECK(store.fetch(a).ok());
        T2D_CHECK(store.fetch(c).ok());
        T2D_CHECK(store.fetch(b).status == ReadStatus::Corrupt);
        const ScanReport second = store.scan();
        T2D_CHECK_EQ(second.corruptions.size(), report.corruptions.size());
    }
    T2D_CHECK(read_whole_file(scratch.file) == after_payload);

    poison_live_slot(scratch.file, 16u, c);
    {
        ChunkStore store = open_required(scratch, default_options());
        const ScanReport report = store.scan();
        T2D_CHECK(report.ok());
        T2D_CHECK(report_has_kind(report, CorruptChunk::Kind::IndexSlot));
        T2D_CHECK(store.fetch(a).ok());
        T2D_CHECK(store.fetch(b).status == ReadStatus::Corrupt);
    }
}

T2D_TEST(scan_resynchronises_past_a_corrupt_record_header) {
    Scratch scratch("scan-resync.t2dk");
    const ChunkKey a{0, 0};
    const ChunkKey b{1, 0};
    const ChunkKey c{2, 0};
    const std::vector<u8> payload_a = make_payload(91u, 24u);
    const std::vector<u8> payload_b = make_payload(92u, 24u);
    const std::vector<u8> payload_c = make_payload(93u, 24u);
    {
        ChunkStore store = open_required(scratch, default_options());
        T2D_CHECK(store.write(a, payload_a));
        T2D_CHECK(store.write(b, payload_b));
        T2D_CHECK(store.write(c, payload_c));
        T2D_CHECK(store.flush(true));
    }
    const u64 first_record = ChunkStore::kIndexOffset + 16ull * ChunkStore::kSlotSize;
    corrupt_record_header(scratch.file, first_record);
    {
        ChunkStore store = open_required(scratch, default_options());
        T2D_CHECK(store.fetch(b).ok());
        T2D_CHECK(store.fetch(c).ok());
        T2D_CHECK(store.fetch(a).status == ReadStatus::Corrupt);
        const ScanReport report = store.scan();
        T2D_CHECK(report.ok());
        T2D_CHECK(report_has_offset(report, CorruptChunk::Kind::RecordHeader, first_record));
        T2D_CHECK_GE(report.checked_records, 2u);   // the later records were seen after the resync
        T2D_CHECK_GE(store.stats().resyncs, 1u);
    }
}

T2D_TEST(compact_default_aborts_but_the_game_policy_can_drop) {
    Scratch scratch("policy.t2dk");
    const ChunkKey a{1, 1};
    const ChunkKey b{2, 2};
    const ChunkKey c{3, 3};
    const std::vector<u8> payload_a = make_payload(101u, 20u);
    const std::vector<u8> payload_b = make_payload(102u, 20u);
    const std::vector<u8> payload_c = make_payload(103u, 20u);
    {
        ChunkStore store = open_required(scratch, default_options());
        T2D_CHECK(store.write(a, payload_a));
        T2D_CHECK(store.write(b, payload_b));
        T2D_CHECK(store.write(c, payload_c));
        T2D_CHECK(store.flush(true));
    }
    corrupt_record_payload(scratch.file, 16u, a);
    const std::vector<u8> damaged_file = read_whole_file(scratch.file);
    {
        ChunkStore store = open_required(scratch, default_options());
        std::string error;
        T2D_CHECK_FALSE(store.compact(&error));
        T2D_CHECK_FALSE(error.empty());
        T2D_CHECK(error.find("scan") != std::string::npos || error.find("policy") != std::string::npos);
        // The default abort left the original untouched and every other chunk readable.
        T2D_CHECK(store.fetch(b).ok());
        T2D_CHECK(store.fetch(c).ok());
        T2D_CHECK(store.fetch(a).status == ReadStatus::Corrupt);
        T2D_CHECK(read_whole_file(scratch.file) == damaged_file);
    }
    T2D_CHECK(read_whole_file(scratch.file) == damaged_file);

    {
        ChunkStore store = open_required(scratch, default_options());
        // The game may also choose Abort explicitly: nothing is written and the report says why.
        CompactReport abort_report;
        const CorruptPolicy abort_policy = [](const CorruptChunk&) { return CorruptAction::Abort; };
        T2D_CHECK_FALSE(store.compact(abort_policy, &abort_report));
        T2D_CHECK(abort_report.aborted);
        T2D_CHECK_FALSE(abort_report.corruptions.empty());
        T2D_CHECK(read_whole_file(scratch.file) == damaged_file);

        CompactReport report;
        int calls = 0;
        const CorruptPolicy drop = [&calls](const CorruptChunk&) {
            ++calls;
            return CorruptAction::Drop;
        };
        T2D_CHECK(store.compact(drop, &report));
        T2D_CHECK_EQ(report.kept, 2u);
        T2D_CHECK_EQ(report.dropped.size(), 1u);
        T2D_CHECK(report.dropped.size() == 1u && report.dropped[0] == a);
        T2D_CHECK_FALSE(report.aborted);
        T2D_CHECK_FALSE(report.corruptions.empty());
        T2D_CHECK_EQ(calls, 1);
        T2D_CHECK(store.fetch(a).status == ReadStatus::Missing);
        const ReadResult back_b = store.fetch(b);
        T2D_CHECK(back_b.ok());
        T2D_CHECK(same_bytes(back_b.bytes, payload_b));
        const ReadResult back_c = store.fetch(c);
        T2D_CHECK(back_c.ok());
        T2D_CHECK(same_bytes(back_c.bytes, payload_c));
        T2D_CHECK_EQ(store.chunk_count(), 2u);
    }
    {
        ChunkStore store = open_required(scratch, default_options());
        T2D_CHECK_EQ(store.chunk_count(), 2u);
        T2D_CHECK(store.fetch(b).ok());
        T2D_CHECK(store.fetch(c).ok());
        T2D_CHECK(store.fetch(a).status == ReadStatus::Missing);
        // The game may now write a replacement for the dropped key.
        const std::vector<u8> replacement = make_payload(104u, 11u);
        T2D_CHECK(store.write(a, replacement));
        const ReadResult replaced = store.fetch(a);
        T2D_CHECK(replaced.ok());
        T2D_CHECK(same_bytes(replaced.bytes, replacement));
    }
}

T2D_TEST(a_corrupt_early_header_resynchronises_and_drop_rebuilds_later_chunks) {
    Scratch scratch("resync-compact.t2dk");
    const ChunkKey a{0, 0};
    const ChunkKey b{1, 0};
    const ChunkKey c{2, 0};
    const std::vector<u8> payload_a = make_payload(111u, 24u);
    const std::vector<u8> payload_b = make_payload(112u, 24u);
    const std::vector<u8> payload_c = make_payload(113u, 24u);
    {
        ChunkStore store = open_required(scratch, default_options());
        T2D_CHECK(store.write(a, payload_a));
        T2D_CHECK(store.write(b, payload_b));
        T2D_CHECK(store.write(c, payload_c));
        T2D_CHECK(store.flush(true));
    }
    const u64 first_record = ChunkStore::kIndexOffset + 16ull * ChunkStore::kSlotSize;
    corrupt_record_header(scratch.file, first_record);
    {
        ChunkStore store = open_required(scratch, default_options());
        const ScanReport report = store.scan();
        T2D_CHECK(report_has_offset(report, CorruptChunk::Kind::RecordHeader, first_record));
        T2D_CHECK_GE(report.checked_records, 2u);
        T2D_CHECK_GE(store.stats().resyncs, 1u);
        std::string error;
        T2D_CHECK_FALSE(store.compact(&error));   // safe default: abort, original untouched
        T2D_CHECK(store.fetch(b).ok());
        T2D_CHECK(store.fetch(c).ok());
        T2D_CHECK(store.fetch(a).status == ReadStatus::Corrupt);

        CompactReport compact_report;
        const CorruptPolicy drop = [](const CorruptChunk&) { return CorruptAction::Drop; };
        T2D_CHECK(store.compact(drop, &compact_report));
        T2D_CHECK_EQ(compact_report.kept, 2u);
        T2D_CHECK_FALSE(compact_report.corruptions.empty());
        T2D_CHECK_FALSE(compact_report.aborted);
        T2D_CHECK(store.fetch(b).ok());
        T2D_CHECK(store.fetch(c).ok());
        T2D_CHECK(store.fetch(a).status == ReadStatus::Missing);
        T2D_CHECK_EQ(store.chunk_count(), 2u);
    }
    {
        ChunkStore store = open_required(scratch, default_options());
        T2D_CHECK_EQ(store.chunk_count(), 2u);
        T2D_CHECK(store.fetch(b).ok());
        T2D_CHECK(store.fetch(c).ok());
        T2D_CHECK(store.fetch(a).status == ReadStatus::Missing);
        const ScanReport clean = store.scan();
        T2D_CHECK(clean.ok());
        T2D_CHECK(clean.corruptions.empty());
    }
}

T2D_TEST(open_world_regenerates_only_the_damaged_chunk_and_the_game_drops_it) {
    Scratch scratch("world-damage.t2dk");
    OpenWorldConfig config;
    config.tile_size = 16.0f;
    config.layers = 1;
    config.seed = 0x71u;
    config.load_margin_chunks = 1;
    config.unload_margin_chunks = 2;
    config.max_loaded_chunks = 0;
    const Aabb2 view = Aabb2::from_top_left(Vec2{0.0f, 0.0f}, Vec2{1024.0f, 1024.0f});
    const auto generator = [](const ChunkKey&, WorldChunk& chunk) {
        for (TileId& cell : chunk.tiles(0)) cell = 1;
    };
    {
        ChunkStore store = open_required(scratch, default_options());
        OpenWorld world(config);
        world.set_generator(generator);
        world.set_store(store.loader(), store.saver());
        T2D_CHECK_EQ(world.update(view), 16u);   // 2x2 chunks plus the load ring
        T2D_CHECK(world.set(5, 5, 9));           // chunk (0, 0)
        T2D_CHECK(world.set(33, 5, 8));          // chunk (1, 0)
        T2D_CHECK(world.flush());
        T2D_CHECK_EQ(store.chunk_count(), 2u);
        T2D_CHECK(store.flush(true));
    }
    corrupt_record_payload(scratch.file, 16u, ChunkKey{0, 0});

    {
        ChunkStore store = open_required(scratch, default_options());
        T2D_CHECK_EQ(store.chunk_count(), 2u);
        T2D_CHECK(store.fetch(ChunkKey{0, 0}).status == ReadStatus::Corrupt);
        T2D_CHECK(store.fetch(ChunkKey{1, 0}).ok());
        const ScanReport report = store.scan();
        T2D_CHECK(report_has_key(report, ChunkKey{0, 0}));
        T2D_CHECK(report_has_kind(report, CorruptChunk::Kind::RecordPayload));

        OpenWorld world(config);
        world.set_generator(generator);
        world.set_store(store.loader(), store.saver());
        T2D_CHECK_EQ(world.update(view), 16u);
        T2D_CHECK_EQ(world.at(5, 5), 1);    // the corrupt chunk was regenerated by the loader fallback
        T2D_CHECK_EQ(world.at(33, 5), 8);   // the healthy edit came back from the file

        // The game decides to drop the damaged chunk, then writes a replacement.
        CompactReport compact_report;
        const CorruptPolicy drop = [](const CorruptChunk&) { return CorruptAction::Drop; };
        T2D_CHECK(store.compact(drop, &compact_report));
        T2D_CHECK_EQ(compact_report.dropped.size(), 1u);
        T2D_CHECK(compact_report.dropped.size() == 1u &&
                  compact_report.dropped[0] == (ChunkKey{0, 0}));
        T2D_CHECK_EQ(store.chunk_count(), 1u);
        T2D_CHECK(store.fetch(ChunkKey{0, 0}).status == ReadStatus::Missing);
        T2D_CHECK_EQ(world.at(33, 5), 8);
        T2D_CHECK(world.set(5, 5, 7));
        T2D_CHECK(world.flush());
        T2D_CHECK_EQ(store.chunk_count(), 2u);
        T2D_CHECK(store.fetch(ChunkKey{0, 0}).ok());
    }
}

T2D_TEST(version_one_files_are_refused_with_a_version_two_error) {
    Scratch scratch("v1.t2dk");
    {
        ChunkStore store = open_required(scratch, default_options());
        T2D_CHECK(store.write(ChunkKey{1, 1}, make_payload(121u, 10u)));
        T2D_CHECK(store.flush(true));
    }
    rewrite_header_version(scratch.file, 1u);
    const std::vector<u8> before = read_whole_file(scratch.file);
    std::string error;
    T2D_CHECK_FALSE(ChunkStore::open(scratch.string(), default_options(), &error).has_value());
    T2D_CHECK(error.find("version 2") != std::string::npos);
    T2D_CHECK(read_whole_file(scratch.file) == before);   // rejecting a v1 file does not touch it
}

T2D_TEST_MAIN
