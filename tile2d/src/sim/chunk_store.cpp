// Tile2D - the file-backed chunk store, format version 2: per-slot CRCs and corruption isolation.
//
// The file is [header A 4096][header B 4096][index slots * 72][append-only records]. A lookup hashes the
// absolute ChunkKey, reads one slot at a time and - only for matching live halves - reads a record.
// Every slot half carries its own CRC, so a damaged slot is POISONED: it never terminates a probe (which
// would hide later keys), never matches a key and never shadows a real entry; the probe keeps going and
// reports the damage. Damage in a record is reported by fetch()/scan() and never returned as bytes. The
// engine does not choose a policy: compact() aborts by default, compact(policy) lets the game decide
// Abort or Drop for each damaged chunk, and a resynchronising record-log scan keeps the rest of the log
// reachable past a corrupt header.
#include <t2d/sim/chunk_store.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  include <io.h>
#else
#  include <unistd.h>
#endif

namespace t2d {
namespace {

constexpr u8 kStateEmpty = 0;
constexpr u8 kStateLive = 1;
constexpr u8 kStateTombstone = 2;
constexpr u8 kStatePoisoned = 3;
constexpr u32 kMaxGeneration = (1u << 30) - 1;

constexpr u32 kHeaderMagicOffset = 0;
constexpr u32 kHeaderVersionOffset = 4;
constexpr u32 kHeaderSequenceOffset = 8;
constexpr u32 kHeaderIndexOffsetOffset = 16;
constexpr u32 kHeaderSlotsOffset = 24;
constexpr u32 kHeaderLiveOffset = 28;
constexpr u32 kHeaderTombstoneOffset = 32;
constexpr u32 kHeaderReservedOffset = 36;
constexpr u32 kHeaderDataStartOffset = 40;
constexpr u32 kHeaderDataEndOffset = 48;
constexpr u32 kHeaderCrcOffset = 56;
constexpr u32 kHeaderCrcBytes = 56;

constexpr u32 kRecordMagicOffset = 0;
constexpr u32 kRecordStateOffset = 4;
constexpr u32 kRecordSizeOffset = 8;
constexpr u32 kRecordKeyXOffset = 12;
constexpr u32 kRecordKeyYOffset = 20;
constexpr u32 kRecordPayloadCrcOffset = 28;
constexpr u32 kRecordHeaderCrcOffset = 32;
constexpr u32 kRecordReservedOffset = 36;
constexpr u32 kRecordHeaderCrcBytes = 32;

void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

void put_u32(u8* out, u32 value) {
    for (u32 index = 0; index < 4u; ++index) out[index] = static_cast<u8>((value >> (index * 8u)) & 0xFFu);
}
void put_u64(u8* out, u64 value) {
    for (u32 index = 0; index < 8u; ++index) out[index] = static_cast<u8>((value >> (index * 8u)) & 0xFFu);
}
void put_i64(u8* out, i64 value) { put_u64(out, std::bit_cast<u64>(value)); }

[[nodiscard]] u32 get_u32(const u8* in) {
    u32 value = 0;
    for (u32 index = 0; index < 4u; ++index) value |= static_cast<u32>(in[index]) << (index * 8u);
    return value;
}
[[nodiscard]] u64 get_u64(const u8* in) {
    u64 value = 0;
    for (u32 index = 0; index < 8u; ++index) value |= static_cast<u64>(in[index]) << (index * 8u);
    return value;
}
[[nodiscard]] i64 get_i64(const u8* in) { return std::bit_cast<i64>(get_u64(in)); }

#if defined(_WIN32)
[[nodiscard]] bool seek_file(std::FILE* file, u64 offset) {
    return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0;
}
[[nodiscard]] i64 file_position(std::FILE* file) { return static_cast<i64>(_ftelli64(file)); }
#else
[[nodiscard]] bool seek_file(std::FILE* file, u64 offset) {
    if (offset > static_cast<u64>(std::numeric_limits<off_t>::max())) return false;
    return ::fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
}
[[nodiscard]] i64 file_position(std::FILE* file) {
    const off_t position = ::ftello(file);
    return position < 0 ? -1 : static_cast<i64>(position);
}
#endif

[[nodiscard]] bool file_length(std::FILE* file, u64& size) {
#if defined(_WIN32)
    if (_fseeki64(file, 0, SEEK_END) != 0) return false;
#else
    if (::fseeko(file, 0, SEEK_END) != 0) return false;
#endif
    const i64 end = file_position(file);
    if (end < 0) return false;
    size = static_cast<u64>(end);
    return true;
}

[[nodiscard]] bool read_at(std::FILE* file, u64 offset, Span<u8> out) {
    if (out.empty()) return true;
    if (!seek_file(file, offset)) return false;
    return std::fread(out.data(), 1, out.size(), file) == out.size();
}

[[nodiscard]] bool write_at(std::FILE* file, u64 offset, ConstSpan<const u8> data) {
    if (data.empty()) return true;
    if (!seek_file(file, offset)) return false;
    return std::fwrite(data.data(), 1, data.size(), file) == data.size();
}

[[nodiscard]] bool sync_file(std::FILE* file) {
#if defined(_WIN32)
    return _commit(_fileno(file)) == 0;
#else
    return ::fsync(::fileno(file)) == 0;
#endif
}

[[nodiscard]] bool is_power_of_two(u32 value) { return value != 0u && (value & (value - 1u)) == 0u; }

[[nodiscard]] u32 next_power_of_two(u64 value) {
    if (value <= 1u) return 1u;
    if (value > 0x8000'0000ull) return 0x8000'0000u;
    u64 result = 1;
    while (result < value) result <<= 1u;
    return static_cast<u32>(result);
}

[[nodiscard]] u32 clamp_slots(u32 slots) {
    if (slots < ChunkStore::kMinSlots) slots = ChunkStore::kMinSlots;
    if (slots > ChunkStore::kMaxSlots) slots = ChunkStore::kMaxSlots;
    const u32 rounded = next_power_of_two(slots);
    return rounded > ChunkStore::kMaxSlots ? ChunkStore::kMaxSlots : rounded;
}

[[nodiscard]] ChunkStore::Options clamp_options(const ChunkStore::Options& options) {
    ChunkStore::Options clamped = options;
    clamped.initial_slots = clamp_slots(options.initial_slots);
    f32 load = options.max_load_factor;
    if (!std::isfinite(load) || !(load > 0.05f)) load = 0.7f;
    clamped.max_load_factor = clamp(load, 0.1f, 0.95f);
    return clamped;
}

[[nodiscard]] u64 mix64(u64 value) {
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

/// The stable slot hash: splitmix-style mixing of the two key bit patterns. Part of the on-disk format.
[[nodiscard]] u64 hash_key(const ChunkKey& key) {
    u64 hash = mix64(static_cast<u64>(key.x) * 0x9E3779B97F4A7C15ull);
    hash ^= mix64(static_cast<u64>(key.y) * 0xC2B2AE3D27D4EB4Full + 0x165667B19E3779F9ull);
    return mix64(hash);
}

struct HeaderFields {
    u64 sequence = 0;
    u32 slots = 0;
    u32 live_count = 0;
    u32 tombstone_count = 0;
    u64 data_start = 0;
    u64 data_end = 0;
};

[[nodiscard]] std::array<u8, ChunkStore::kHeaderCopySize> encode_header(const HeaderFields& header) {
    std::array<u8, ChunkStore::kHeaderCopySize> page{};
    put_u32(page.data() + kHeaderMagicOffset, ChunkStore::kMagic);
    put_u32(page.data() + kHeaderVersionOffset, ChunkStore::kFormatVersion);
    put_u64(page.data() + kHeaderSequenceOffset, header.sequence);
    put_u64(page.data() + kHeaderIndexOffsetOffset, ChunkStore::kIndexOffset);
    put_u32(page.data() + kHeaderSlotsOffset, header.slots);
    put_u32(page.data() + kHeaderLiveOffset, header.live_count);
    put_u32(page.data() + kHeaderTombstoneOffset, header.tombstone_count);
    put_u32(page.data() + kHeaderReservedOffset, 0);
    put_u64(page.data() + kHeaderDataStartOffset, header.data_start);
    put_u64(page.data() + kHeaderDataEndOffset, header.data_end);
    const u32 checksum = save::crc32(ConstSpan<const u8>(page.data(), kHeaderCrcBytes));
    put_u32(page.data() + kHeaderCrcOffset, checksum);
    return page;
}

[[nodiscard]] bool decode_header(const u8* page, HeaderFields& header, std::string& why) {
    if (get_u32(page + kHeaderMagicOffset) != ChunkStore::kMagic) {
        why = "magic does not match";
        return false;
    }
    const u32 version = get_u32(page + kHeaderVersionOffset);
    if (version != ChunkStore::kFormatVersion) {
        why = "format version is " + std::to_string(version) + ", this store needs version " +
              std::to_string(ChunkStore::kFormatVersion) + " (version 1 files are not migrated)";
        return false;
    }
    const u32 expected_crc = get_u32(page + kHeaderCrcOffset);
    if (save::crc32(ConstSpan<const u8>(page, kHeaderCrcBytes)) != expected_crc) {
        why = "header checksum does not match";
        return false;
    }
    if (get_u64(page + kHeaderIndexOffsetOffset) != ChunkStore::kIndexOffset) {
        why = "index offset is not 8192";
        return false;
    }
    header.slots = get_u32(page + kHeaderSlotsOffset);
    if (!is_power_of_two(header.slots) || header.slots < ChunkStore::kMinSlots ||
        header.slots > ChunkStore::kMaxSlots) {
        why = "index slot count is not a power of two in range";
        return false;
    }
    if (get_u32(page + kHeaderReservedOffset) != 0u) {
        why = "reserved header field is not zero";
        return false;
    }
    header.sequence = get_u64(page + kHeaderSequenceOffset);
    header.live_count = get_u32(page + kHeaderLiveOffset);
    header.tombstone_count = get_u32(page + kHeaderTombstoneOffset);
    header.data_start = get_u64(page + kHeaderDataStartOffset);
    header.data_end = get_u64(page + kHeaderDataEndOffset);
    if (header.data_start !=
        ChunkStore::kIndexOffset + static_cast<u64>(header.slots) * ChunkStore::kSlotSize) {
        why = "data start does not follow the index";
        return false;
    }
    if (header.data_end < header.data_start) {
        why = "data end is before data start";
        return false;
    }
    const u64 slot_capacity = static_cast<u64>(header.slots) * 2u;
    if (static_cast<u64>(header.live_count) > slot_capacity ||
        static_cast<u64>(header.tombstone_count) > slot_capacity) {
        why = "header counts do not fit the index";
        return false;
    }
    return true;
}

struct RecordFields {
    u32 state = 0;
    u32 payload_size = 0;
    i64 key_x = 0;
    i64 key_y = 0;
    u32 payload_crc = 0;
};

[[nodiscard]] std::array<u8, ChunkStore::kRecordHeaderSize> encode_record_header(const RecordFields& record) {
    std::array<u8, ChunkStore::kRecordHeaderSize> raw{};
    put_u32(raw.data() + kRecordMagicOffset, ChunkStore::kRecordMagic);
    put_u32(raw.data() + kRecordStateOffset, record.state);
    put_u32(raw.data() + kRecordSizeOffset, record.payload_size);
    put_i64(raw.data() + kRecordKeyXOffset, record.key_x);
    put_i64(raw.data() + kRecordKeyYOffset, record.key_y);
    put_u32(raw.data() + kRecordPayloadCrcOffset, record.payload_crc);
    const u32 checksum = save::crc32(ConstSpan<const u8>(raw.data(), kRecordHeaderCrcBytes));
    put_u32(raw.data() + kRecordHeaderCrcOffset, checksum);
    put_u32(raw.data() + kRecordReservedOffset, 0);
    return raw;
}

[[nodiscard]] bool decode_record_header(const u8* raw, RecordFields& record, std::string& why) {
    if (get_u32(raw + kRecordMagicOffset) != ChunkStore::kRecordMagic) {
        why = "record magic does not match";
        return false;
    }
    record.state = get_u32(raw + kRecordStateOffset);
    if (record.state != kStateLive && record.state != kStateTombstone) {
        why = "record state is neither live nor deleted";
        return false;
    }
    record.payload_size = get_u32(raw + kRecordSizeOffset);
    if (record.payload_size > ChunkStore::kMaxPayloadSize) {
        why = "record payload is larger than the 64 MiB limit";
        return false;
    }
    record.key_x = get_i64(raw + kRecordKeyXOffset);
    record.key_y = get_i64(raw + kRecordKeyYOffset);
    record.payload_crc = get_u32(raw + kRecordPayloadCrcOffset);
    if (get_u32(raw + kRecordReservedOffset) != 0u) {
        why = "record reserved field is not zero";
        return false;
    }
    const u32 expected_crc = get_u32(raw + kRecordHeaderCrcOffset);
    if (save::crc32(ConstSpan<const u8>(raw, kRecordHeaderCrcBytes)) != expected_crc) {
        why = "record header checksum does not match";
        return false;
    }
    return true;
}

[[nodiscard]] u32 auto_slots_for(u64 live_count, f32 max_load_factor, u32 initial_slots) {
    const f64 load = static_cast<f64>(max_load_factor);
    const u64 wanted = live_count == 0 ? initial_slots
                                       : static_cast<u64>(std::ceil(static_cast<f64>(live_count) / load));
    u32 slots = next_power_of_two(std::max<u64>(wanted, ChunkStore::kMinSlots));
    if (slots < initial_slots) slots = initial_slots;
    if (slots > ChunkStore::kMaxSlots) slots = ChunkStore::kMaxSlots;
    return slots;
}

struct KeyLess {
    [[nodiscard]] bool operator()(const ChunkKey& a, const ChunkKey& b) const {
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    }
};

struct Survivor {
    u64 offset = 0;
    u32 payload_size = 0;
    u32 payload_crc = 0;
};

/// Deduplicates scan()'s index pass against its independent record-log pass.
[[nodiscard]] bool same_corruption(const CorruptChunk& a, const CorruptChunk& b) {
    if (a.kind != b.kind) return false;
    if (a.kind == CorruptChunk::Kind::IndexSlot) return a.slot_index == b.slot_index;
    return a.record_offset == b.record_offset;
}

} // namespace

// --- ChunkStore: slot encoding -----------------------------------------------------

ChunkStore::Slot ChunkStore::decode_slot(const u8* raw) {
    Slot slot;
    for (int half = 0; half < 2; ++half) {
        const u8* at = raw + static_cast<usize>(half) * kSlotHalfSize;
        Half& value = slot.halves[half];
        value.tag = get_u32(at);
        value.key_x = get_i64(at + 4);
        value.key_y = get_i64(at + 12);
        value.record_offset = get_u64(at + 20);
        value.payload_size = get_u32(at + 28);
        value.half_crc = get_u32(at + 32);
        value.all_zero = true;
        for (u32 index = 0; index < kSlotHalfSize; ++index) {
            if (at[index] != 0u) {
                value.all_zero = false;
                break;
            }
        }
        value.crc_ok = save::crc32(ConstSpan<const u8>(at, 32)) == value.half_crc;
    }
    return slot;
}

void ChunkStore::encode_slot(const Slot& slot, u8* raw) {
    for (int half = 0; half < 2; ++half) {
        u8* at = raw + static_cast<usize>(half) * kSlotHalfSize;
        const Half& value = slot.halves[half];
        if (value.all_zero) {
            // The valid empty is all 36 bytes zero, CRC bytes included: a computed CRC over zeros is
            // not zero and would make the half look poisoned after reopening.
            std::memset(at, 0, kSlotHalfSize);
            continue;
        }
        put_u32(at, value.tag);
        put_i64(at + 4, value.key_x);
        put_i64(at + 12, value.key_y);
        put_u64(at + 20, value.record_offset);
        put_u32(at + 28, value.payload_size);
        put_u32(at + 32, save::crc32(ConstSpan<const u8>(at, 32)));
    }
}

u8 ChunkStore::state_of(const Half& half) {
    if (half.all_zero) return kStateEmpty;   // the valid empty: all 36 bytes zero
    if (!half.crc_ok) return kStatePoisoned;
    const u8 state = static_cast<u8>(half.tag & 3u);
    if (state != kStateLive && state != kStateTombstone) return kStatePoisoned;
    if (half.payload_size > kMaxPayloadSize) return kStatePoisoned;
    return state;
}

bool ChunkStore::is_empty_half(const Half& half) { return state_of(half) == kStateEmpty; }

bool ChunkStore::is_poisoned(const Half& half) { return state_of(half) == kStatePoisoned; }

u32 ChunkStore::generation_of(const Half& half) {
    const u8 state = state_of(half);
    return (state == kStatePoisoned || state == kStateEmpty) ? 0u : half.tag >> 2u;
}

u32 ChunkStore::max_generation(const Slot& slot) {
    return std::max(generation_of(slot.halves[0]), generation_of(slot.halves[1]));
}

int ChunkStore::write_target_half(const Slot& slot) {
    const u8 first = state_of(slot.halves[0]);
    const u8 second = state_of(slot.halves[1]);
    // Repair a poisoned half first: it is the one that must not be trusted.
    if (first == kStatePoisoned && second != kStatePoisoned) return 0;
    if (second == kStatePoisoned && first != kStatePoisoned) return 1;
    if (first == kStatePoisoned) return 0;
    // Then reuse a tombstone half.
    if (first == kStateTombstone && second != kStateTombstone) return 0;
    if (second == kStateTombstone && first != kStateTombstone) return 1;
    if (first == kStateTombstone) return generation_of(slot.halves[0]) <= generation_of(slot.halves[1]) ? 0 : 1;
    // Prefer a valid empty half over a live one, so another key's live half is not overwritten.
    if (first == kStateEmpty && second != kStateEmpty) return 0;
    if (second == kStateEmpty && first != kStateEmpty) return 1;
    if (first == kStateEmpty && second == kStateEmpty) return 1;   // fresh slot: generation 1 in half 1
    // Two live halves (a repaired/torn slot): write the older generation, keep the newer readable.
    return generation_of(slot.halves[0]) <= generation_of(slot.halves[1]) ? 1 : 0;
}

ChunkStore::Half ChunkStore::make_half(u32 generation, u8 state, const ChunkKey& key, u64 record_offset,
                                       u32 payload_size) {
    Half half;
    half.tag = (generation << 2u) | static_cast<u32>(state);
    half.key_x = key.x;
    half.key_y = key.y;
    half.record_offset = record_offset;
    half.payload_size = payload_size;
    half.crc_ok = true;
    half.all_zero = false;
    return half;
}

// --- ChunkStore: construction and file plumbing --------------------------------------

ChunkStore::~ChunkStore() { close_file(); }

ChunkStore::ChunkStore(ChunkStore&& other) noexcept
    : file_(other.file_),
      path_(std::move(other.path_)),
      options_(other.options_),
      slots_(other.slots_),
      live_count_(other.live_count_),
      tombstone_count_(other.tombstone_count_),
      data_end_(other.data_end_),
      sequence_(other.sequence_),
      active_header_(other.active_header_),
      stats_(other.stats_) {
    other.file_ = nullptr;
    other.slots_ = 0;
    other.live_count_ = 0;
    other.tombstone_count_ = 0;
    other.data_end_ = 0;
}

ChunkStore& ChunkStore::operator=(ChunkStore&& other) noexcept {
    if (this == &other) return *this;
    close_file();
    file_ = other.file_;
    path_ = std::move(other.path_);
    options_ = other.options_;
    slots_ = other.slots_;
    live_count_ = other.live_count_;
    tombstone_count_ = other.tombstone_count_;
    data_end_ = other.data_end_;
    sequence_ = other.sequence_;
    active_header_ = other.active_header_;
    stats_ = other.stats_;
    other.file_ = nullptr;
    other.slots_ = 0;
    other.live_count_ = 0;
    other.tombstone_count_ = 0;
    other.data_end_ = 0;
    return *this;
}

void ChunkStore::close_file() {
    if (file_ != nullptr) {
        std::fflush(file_);
        std::fclose(file_);
        file_ = nullptr;
    }
}

bool ChunkStore::is_open() const { return file_ != nullptr; }

const std::string& ChunkStore::path() const { return path_; }

u32 ChunkStore::data_start() const {
    return static_cast<u32>(kIndexOffset + static_cast<u64>(slots_) * kSlotSize);
}

u32 ChunkStore::hash_low(const ChunkKey& key) const { return static_cast<u32>(hash_key(key)); }

bool ChunkStore::read_slot(u32 slot_index, Slot& slot, std::string* error) {
    std::array<u8, kSlotSize> raw{};
    if (!read_at(file_, kIndexOffset + static_cast<u64>(slot_index) * kSlotSize, raw)) {
        set_error(error, "cannot read an index slot from '" + path_ + "'");
        return false;
    }
    slot = decode_slot(raw.data());
    return true;
}

bool ChunkStore::write_slot_half(u32 slot_index, const Slot& slot, int half_index, std::string* error) {
    // Only one half is written; the other half's bytes (and CRC) stay exactly as they were.
    std::array<u8, kSlotSize> raw{};
    encode_slot(slot, raw.data());
    const u64 offset = kIndexOffset + static_cast<u64>(slot_index) * kSlotSize +
                       static_cast<u64>(half_index) * kSlotHalfSize;
    if (!write_at(file_, offset,
                  ConstSpan<const u8>(raw.data() + static_cast<usize>(half_index) * kSlotHalfSize,
                                      kSlotHalfSize))) {
        set_error(error, "cannot write an index slot to '" + path_ + "'");
        return false;
    }
    return true;
}

bool ChunkStore::write_header_copy(u32 copy_index, u64 sequence, std::string* error) {
    HeaderFields header;
    header.sequence = sequence;
    header.slots = slots_;
    header.live_count = live_count_;
    header.tombstone_count = tombstone_count_;
    header.data_start = data_start();
    header.data_end = data_end_;
    const auto page = encode_header(header);
    if (!write_at(file_, static_cast<u64>(copy_index) * kHeaderCopySize, page)) {
        set_error(error, "cannot write a header copy to '" + path_ + "'");
        return false;
    }
    return true;
}

bool ChunkStore::commit_header(std::string* error) {
    const u32 next_copy = active_header_ == 0u ? 1u : 0u;
    if (!write_header_copy(next_copy, sequence_ + 1u, error)) return false;
    active_header_ = next_copy;
    ++sequence_;
    if (std::fflush(file_) != 0) {
        set_error(error, "cannot flush '" + path_ + "'");
        return false;
    }
    return true;
}

// --- ChunkStore: open / create / load ------------------------------------------------

std::optional<ChunkStore> ChunkStore::open(const std::string& path, std::string* error) {
    return open(path, Options{}, error);
}

std::optional<ChunkStore> ChunkStore::open(const std::string& path, const Options& options, std::string* error) {
    ChunkStore store;
    store.path_ = path;
    store.options_ = clamp_options(options);

    std::FILE* file = std::fopen(path.c_str(), "r+b");
    if (file == nullptr) {
        if (errno != ENOENT) {
            set_error(error, "cannot open '" + path + "' for reading and writing");
            return std::nullopt;
        }
        file = std::fopen(path.c_str(), "w+b");
        if (file == nullptr) {
            set_error(error, "cannot create '" + path + "'");
            return std::nullopt;
        }
        store.file_ = file;
        if (!store.create_empty(error)) {
            store.close_file();
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            return std::nullopt;
        }
        return std::optional<ChunkStore>{std::move(store)};
    }

    store.file_ = file;
    u64 size = 0;
    if (!file_length(file, size)) {
        set_error(error, "cannot measure '" + path + "'");
        store.close_file();
        return std::nullopt;
    }
    if (size == 0) {
        if (!store.create_empty(error)) {
            store.close_file();
            return std::nullopt;
        }
        return std::optional<ChunkStore>{std::move(store)};
    }
    if (!store.load_existing(error)) {
        store.close_file();
        return std::nullopt;
    }
    return std::optional<ChunkStore>{std::move(store)};
}

bool ChunkStore::create_empty(std::string* error) {
    slots_ = clamp_slots(options_.initial_slots);
    live_count_ = 0;
    tombstone_count_ = 0;
    data_end_ = data_start();
    sequence_ = 0;
    active_header_ = 0;

    const std::array<u8, kHeaderCopySize> zeros{};
    if (!write_at(file_, 0, zeros) || !write_at(file_, kHeaderCopySize, zeros)) {
        set_error(error, "cannot zero the header copies of '" + path_ + "'");
        return false;
    }
    // A zeroed index is a valid empty index: every half is all-zero (valid empty).
    const std::vector<u8> zero_buffer(static_cast<usize>(kSlotSize) * 512u, 0u);
    u64 written = 0;
    const u64 index_bytes = static_cast<u64>(slots_) * kSlotSize;
    while (written < index_bytes) {
        const usize amount =
            static_cast<usize>(std::min<u64>(static_cast<u64>(zero_buffer.size()), index_bytes - written));
        if (!write_at(file_, kIndexOffset + written, ConstSpan<const u8>(zero_buffer.data(), amount))) {
            set_error(error, "cannot zero the index of '" + path_ + "'");
            return false;
        }
        written += amount;
    }
    if (!write_header_copy(0, 0, error) || !write_header_copy(1, 0, error)) return false;
    if (std::fflush(file_) != 0) {
        set_error(error, "cannot flush the new store '" + path_ + "'");
        return false;
    }
    stats_.slots = slots_;
    stats_.chunk_count = 0;
    return true;
}

bool ChunkStore::load_existing(std::string* error) {
    u64 size = 0;
    if (!file_length(file_, size)) {
        set_error(error, "cannot measure '" + path_ + "'");
        return false;
    }
    if (size < kIndexOffset) {
        set_error(error, "not a chunk store: '" + path_ + "' is shorter than its two headers");
        return false;
    }

    std::array<u8, kHeaderCopySize> pages[2];
    for (u32 copy = 0; copy < 2u; ++copy) {
        if (!read_at(file_, static_cast<u64>(copy) * kHeaderCopySize, pages[copy])) {
            set_error(error, "cannot read a header copy of '" + path_ + "'");
            return false;
        }
    }

    HeaderFields candidates[2];
    bool valid[2] = {false, false};
    std::string why[2];
    for (u32 copy = 0; copy < 2u; ++copy) {
        if (!decode_header(pages[copy].data(), candidates[copy], why[copy])) continue;
        if (candidates[copy].data_end > size) {
            why[copy] = "data end runs past the end of the file";
            continue;
        }
        valid[copy] = true;
    }
    if (!valid[0] && !valid[1]) {
        set_error(error, "not a chunk store: both header copies of '" + path_ + "' are invalid (" + why[0] +
                             " / " + why[1] + ")");
        return false;
    }

    u32 chosen = 0;
    if (valid[0] && valid[1]) {
        chosen = candidates[1].sequence > candidates[0].sequence ? 1u : 0u;
    } else {
        chosen = valid[0] ? 0u : 1u;
    }

    slots_ = candidates[chosen].slots;
    live_count_ = candidates[chosen].live_count;
    tombstone_count_ = candidates[chosen].tombstone_count;
    data_end_ = candidates[chosen].data_end;
    sequence_ = candidates[chosen].sequence;
    active_header_ = chosen;
    stats_.slots = slots_;
    stats_.chunk_count = live_count_;

    if (data_end_ != size) {
        if (!recover_tail(size, error)) return false;
    }
    return true;
}

bool ChunkStore::recover_tail(u64 file_size, std::string* error) {
    struct Recovered {
        u64 offset = 0;
        u32 payload_size = 0;
        u32 state = 0;
        i64 key_x = 0;
        i64 key_y = 0;
        u32 payload_crc = 0;
    };
    std::vector<Recovered> records;
    u64 offset = data_end_;
    while (offset < file_size) {
        if (file_size - offset < kRecordHeaderSize) break;
        std::array<u8, kRecordHeaderSize> raw{};
        if (!read_at(file_, offset, raw)) {
            set_error(error, "cannot read the tail of '" + path_ + "'");
            return false;
        }
        RecordFields fields;
        std::string why;
        if (!decode_record_header(raw.data(), fields, why)) break;
        if (static_cast<u64>(fields.payload_size) > file_size - offset - kRecordHeaderSize) break;
        if (fields.payload_size > 0u) {
            std::vector<u8> payload(fields.payload_size);
            if (!read_at(file_, offset + kRecordHeaderSize, payload)) {
                set_error(error, "cannot read the tail of '" + path_ + "'");
                return false;
            }
            if (save::crc32(ConstSpan<const u8>(payload)) != fields.payload_crc) break;
        }
        records.push_back(Recovered{offset, fields.payload_size, fields.state, fields.key_x, fields.key_y,
                                    fields.payload_crc});
        offset += kRecordHeaderSize + fields.payload_size;
    }
    if (records.empty()) return true;

    for (const Recovered& record : records) {
        if (!apply_recovered_record(ChunkKey{record.key_x, record.key_y}, record.state, record.offset,
                                    record.payload_size, error)) {
            return false;
        }
    }
    data_end_ = offset;
    if (!recompute_counts(error)) return false;
    if (!commit_header(error)) return false;
    stats_.tail_recovered += records.size();
    stats_.chunk_count = live_count_;
    return true;
}

bool ChunkStore::apply_recovered_record(const ChunkKey& key, u32 state, u64 record_offset,
                                        u32 payload_size, std::string* error) {
    const u32 mask = slots_ - 1u;
    const u32 start = hash_low(key) & mask;
    bool have_target = false;
    u32 target_index = 0;
    int target_half_index = -1;
    Slot target_slot;

    for (u32 step = 0; step < slots_; ++step) {
        const u32 index = (start + step) & mask;
        Slot slot;
        if (!read_slot(index, slot, error)) return false;
        for (int half = 0; half < 2; ++half) {
            if (state_of(slot.halves[half]) != kStateLive) continue;
            if (slot.halves[half].key_x != key.x || slot.halves[half].key_y != key.y) continue;
            slot.halves[half] =
                make_half(max_generation(slot) + 1u, static_cast<u8>(state), key, record_offset, payload_size);
            return write_slot_half(index, slot, half, error);
        }
        if (!have_target) {
            const u8 first = state_of(slot.halves[0]);
            const u8 second = state_of(slot.halves[1]);
            if (first == kStateTombstone) {
                have_target = true;
                target_index = index;
                target_half_index = 0;
                target_slot = slot;
            } else if (second == kStateTombstone) {
                have_target = true;
                target_index = index;
                target_half_index = 1;
                target_slot = slot;
            } else if (first == kStatePoisoned) {
                have_target = true;
                target_index = index;
                target_half_index = 0;
                target_slot = slot;
            } else if (second == kStatePoisoned) {
                have_target = true;
                target_index = index;
                target_half_index = 1;
                target_slot = slot;
            }
        }
        if (state_of(slot.halves[0]) == kStateEmpty && state_of(slot.halves[1]) == kStateEmpty) {
            if (!have_target) {
                have_target = true;
                target_index = index;
                target_half_index = 1;
                target_slot = slot;
            }
            break;
        }
    }
    if (!have_target) {
        set_error(error, "cannot recover a tail record: the index of '" + path_ + "' is full");
        return false;
    }
    target_slot.halves[target_half_index] = make_half(max_generation(target_slot) + 1u,
                                                      static_cast<u8>(state), key, record_offset, payload_size);
    return write_slot_half(target_index, target_slot, target_half_index, error);
}

bool ChunkStore::recompute_counts(std::string* error) {
    u32 live = 0;
    u32 tombstones = 0;
    for (u32 index = 0; index < slots_; ++index) {
        Slot slot;
        if (!read_slot(index, slot, error)) return false;
        for (const Half& half : slot.halves) {
            const u8 state = state_of(half);
            if (state == kStateLive) ++live;
            else if (state == kStateTombstone) ++tombstones;
        }
    }
    live_count_ = live;
    tombstone_count_ = tombstones;
    stats_.slots = slots_;
    stats_.chunk_count = live_count_;
    return true;
}

// --- ChunkStore: probing and record checks ------------------------------------------

bool ChunkStore::probe_key(const ChunkKey& key, Probe& probe, std::string* error) {
    probe = Probe{};
    const u32 mask = slots_ - 1u;
    const u32 start = hash_low(key) & mask;
    bool have_tombstone = false;
    u32 tombstone_index = 0;
    int tombstone_half = 0;
    Slot tombstone_slot;
    bool have_poison = false;
    u32 poison_index = 0;
    int poison_half = 0;
    Slot poison_slot;

    for (u32 step = 0; step < slots_; ++step) {
        const u32 index = (start + step) & mask;
        Slot slot;
        ++stats_.probes;
        ++stats_.reads;
        stats_.bytes_read += kSlotSize;
        if (!read_slot(index, slot, error)) return false;

        const u8 first = state_of(slot.halves[0]);
        const u8 second = state_of(slot.halves[1]);
        if (first == kStatePoisoned) {
            ++stats_.corrupt_slots;
            probe.corruption_seen = true;
            if (!have_poison) {
                have_poison = true;
                poison_index = index;
                poison_half = 0;
                poison_slot = slot;
            }
        } else if (first == kStateTombstone && !have_tombstone) {
            have_tombstone = true;
            tombstone_index = index;
            tombstone_half = 0;
            tombstone_slot = slot;
        }
        if (second == kStatePoisoned) {
            ++stats_.corrupt_slots;
            probe.corruption_seen = true;
            if (!have_poison) {
                have_poison = true;
                poison_index = index;
                poison_half = 1;
                poison_slot = slot;
            }
        } else if (second == kStateTombstone && !have_tombstone) {
            have_tombstone = true;
            tombstone_index = index;
            tombstone_half = 1;
            tombstone_slot = slot;
        }

        for (int half = 0; half < 2; ++half) {
            if (state_of(slot.halves[half]) != kStateLive) continue;
            if (slot.halves[half].key_x != key.x || slot.halves[half].key_y != key.y) continue;
            Match match;
            match.slot_index = index;
            match.half_index = half;
            match.record_offset = slot.halves[half].record_offset;
            match.payload_size = slot.halves[half].payload_size;
            match.half = slot.halves[half];
            probe.matches.push_back(match);
        }

        if (first == kStateEmpty && second == kStateEmpty) {
            // Only a slot whose BOTH halves are valid empty terminates the probe. A poisoned half
            // never cuts a chain short, so it cannot hide a later key.
            probe.result = probe.matches.empty() ? Probe::Result::Missing : Probe::Result::Found;
            if (have_tombstone) {
                probe.has_target = true;
                probe.target_slot = tombstone_index;
                probe.target_half = tombstone_half;
                probe.target_data = tombstone_slot;
            } else if (have_poison) {
                probe.has_target = true;
                probe.target_slot = poison_index;
                probe.target_half = poison_half;
                probe.target_data = poison_slot;
            } else {
                probe.has_target = true;
                probe.target_slot = index;
                probe.target_half = -1;
                probe.target_data = slot;
                probe.target_is_empty_slot = true;
            }
            return true;
        }
    }

    probe.result = probe.matches.empty() ? Probe::Result::Missing : Probe::Result::Found;
    if (have_tombstone) {
        probe.has_target = true;
        probe.target_slot = tombstone_index;
        probe.target_half = tombstone_half;
        probe.target_data = tombstone_slot;
    } else if (have_poison) {
        probe.has_target = true;
        probe.target_slot = poison_index;
        probe.target_half = poison_half;
        probe.target_data = poison_slot;
    } else {
        probe.result = Probe::Result::Full;
    }
    return true;
}

ChunkStore::RecordCheck ChunkStore::read_record_payload(const RecordInfo& info, std::vector<u8>* payload,
                                                        std::string* detail, bool count_stats) {
    const u64 total = kRecordHeaderSize + static_cast<u64>(info.payload_size);
    if (info.payload_size > kMaxPayloadSize || info.offset > data_end_ || total > data_end_ - info.offset) {
        if (count_stats) ++stats_.corrupt_records;
        set_error(detail, "a chunk record lies outside the committed data");
        return RecordCheck::HeaderCorrupt;
    }
    if (count_stats) {
        ++stats_.reads;
        stats_.bytes_read += total;
    }
    std::vector<u8> buffer(static_cast<usize>(total));
    if (!read_at(file_, info.offset, buffer)) {
        if (count_stats) {
            ++stats_.read_failures;
            ++stats_.corrupt_records;
        }
        set_error(detail, "cannot read a chunk record from '" + path_ + "'");
        return RecordCheck::IoError;
    }
    if (save::crc32(ConstSpan<const u8>(buffer.data() + kRecordHeaderSize, info.payload_size)) !=
        info.payload_crc) {
        if (count_stats) ++stats_.corrupt_records;
        set_error(detail, "chunk payload checksum does not match in '" + path_ + "'");
        return RecordCheck::PayloadCorrupt;
    }
    if (payload != nullptr) {
        payload->assign(buffer.begin() + static_cast<std::ptrdiff_t>(kRecordHeaderSize), buffer.end());
    }
    return RecordCheck::Ok;
}

ChunkStore::RecordCheck ChunkStore::check_record(const Half& half, const ChunkKey& key,
                                                 std::vector<u8>* payload, std::string* detail,
                                                 bool count_stats) {
    RecordInfo info;
    info.offset = half.record_offset;
    info.payload_size = half.payload_size;
    info.state = kStateLive;
    info.key_x = key.x;
    info.key_y = key.y;
    info.payload_crc = 0;
    const u64 total = kRecordHeaderSize + static_cast<u64>(half.payload_size);
    if (half.payload_size > kMaxPayloadSize || half.record_offset > data_end_ ||
        total > data_end_ - half.record_offset) {
        if (count_stats) ++stats_.corrupt_records;
        set_error(detail, "a chunk record for the requested key lies outside the committed data");
        return RecordCheck::HeaderCorrupt;
    }
    if (count_stats) {
        ++stats_.reads;
        stats_.bytes_read += total;
    }
    std::vector<u8> buffer(static_cast<usize>(total));
    if (!read_at(file_, half.record_offset, buffer)) {
        if (count_stats) {
            ++stats_.read_failures;
            ++stats_.corrupt_records;
        }
        set_error(detail, "cannot read a chunk record from '" + path_ + "'");
        return RecordCheck::IoError;
    }
    RecordFields fields;
    std::string why;
    if (!decode_record_header(buffer.data(), fields, why)) {
        if (count_stats) ++stats_.corrupt_records;
        set_error(detail, "corrupt chunk record in '" + path_ + "': " + why);
        return RecordCheck::HeaderCorrupt;
    }
    if (fields.state != kStateLive || fields.key_x != key.x || fields.key_y != key.y ||
        fields.payload_size != half.payload_size) {
        if (count_stats) ++stats_.corrupt_records;
        set_error(detail, "chunk record does not match its index entry in '" + path_ + "'");
        return RecordCheck::HeaderCorrupt;
    }
    const u32 checksum =
        save::crc32(ConstSpan<const u8>(buffer.data() + kRecordHeaderSize, half.payload_size));
    if (checksum != fields.payload_crc) {
        if (count_stats) ++stats_.corrupt_records;
        set_error(detail, "chunk payload checksum does not match in '" + path_ + "'");
        return RecordCheck::PayloadCorrupt;
    }
    if (payload != nullptr) {
        payload->assign(buffer.begin() + static_cast<std::ptrdiff_t>(kRecordHeaderSize), buffer.end());
    }
    return RecordCheck::Ok;
}

ChunkStore::RecordCheck ChunkStore::read_record_header(u64 offset, RecordInfo& info, std::string* detail) {
    if (offset > data_end_ || data_end_ - offset < kRecordHeaderSize) {
        set_error(detail, "a record header lies outside the committed data");
        return RecordCheck::HeaderCorrupt;
    }
    std::array<u8, kRecordHeaderSize> raw{};
    if (!read_at(file_, offset, raw)) {
        set_error(detail, "cannot read a record header from '" + path_ + "'");
        return RecordCheck::IoError;
    }
    RecordFields fields;
    std::string why;
    if (!decode_record_header(raw.data(), fields, why)) {
        set_error(detail, why);
        return RecordCheck::HeaderCorrupt;
    }
    if (static_cast<u64>(fields.payload_size) > data_end_ - offset - kRecordHeaderSize) {
        set_error(detail, "a record runs past the committed data");
        return RecordCheck::HeaderCorrupt;
    }
    info.offset = offset;
    info.payload_size = fields.payload_size;
    info.payload_crc = fields.payload_crc;
    info.state = fields.state;
    info.key_x = fields.key_x;
    info.key_y = fields.key_y;
    return RecordCheck::Ok;
}

bool ChunkStore::find_resync(u64 from, u64& found, bool& io_error, std::string& detail) {
    io_error = false;
    constexpr u64 kWindow = 1u << 20;
    u64 scan = from;
    while (scan + kRecordHeaderSize <= data_end_) {
        const u64 window = std::min<u64>(kWindow, data_end_ - scan);
        std::vector<u8> buffer(static_cast<usize>(window));
        if (!read_at(file_, scan, buffer)) {
            io_error = true;
            detail = "cannot read the record log of '" + path_ + "' while resynchronising";
            return false;
        }
        for (u64 offset = 0; offset + kRecordHeaderSize <= window; ++offset) {
            RecordFields fields;
            std::string why;
            if (!decode_record_header(buffer.data() + offset, fields, why)) continue;
            const u64 candidate = scan + offset;
            if (static_cast<u64>(fields.payload_size) > data_end_ - candidate - kRecordHeaderSize) continue;
            std::vector<u8> payload(fields.payload_size);
            if (fields.payload_size > 0u &&
                !read_at(file_, candidate + kRecordHeaderSize, payload)) {
                io_error = true;
                detail = "cannot read the record log of '" + path_ + "' while resynchronising";
                return false;
            }
            if (save::crc32(ConstSpan<const u8>(payload)) != fields.payload_crc) continue;
            found = candidate;
            return true;
        }
        if (scan + window >= data_end_) break;
        scan += window - (kRecordHeaderSize - 1u);
    }
    return false;
}

// --- ChunkStore: fetch / read / contains ---------------------------------------------

ReadResult ChunkStore::fetch(const ChunkKey& key) {
    ReadResult result;
    ++stats_.lookups;
    if (!is_open()) {
        result.status = ReadStatus::NotOpen;
        result.error = "the chunk store is not open";
        return result;
    }
    const u32 mask = slots_ - 1u;
    const u32 start = hash_low(key) & mask;
    bool saw_corrupt = false;
    bool saw_io = false;
    std::string io_error;
    std::string last_detail;

    for (u32 step = 0; step < slots_; ++step) {
        const u32 index = (start + step) & mask;
        Slot slot;
        ++stats_.probes;
        ++stats_.reads;
        stats_.bytes_read += kSlotSize;
        if (!read_slot(index, slot, &last_detail)) {
            ++stats_.read_failures;
            result.status = ReadStatus::IoError;
            result.error = last_detail;
            return result;
        }
        const u8 first = state_of(slot.halves[0]);
        const u8 second = state_of(slot.halves[1]);
        if (first == kStatePoisoned) ++stats_.corrupt_slots;
        if (second == kStatePoisoned) ++stats_.corrupt_slots;

        for (int half = 0; half < 2; ++half) {
            const Half& value = slot.halves[half];
            if (state_of(value) != kStateLive) continue;
            if (value.key_x != key.x || value.key_y != key.y) continue;
            std::vector<u8> payload;
            std::string detail;
            const RecordCheck check = check_record(value, key, &payload, &detail, true);
            if (check == RecordCheck::Ok) {
                ++stats_.hits;
                result.status = ReadStatus::Ok;
                result.bytes = std::move(payload);
                return result;
            }
            if (check == RecordCheck::IoError) {
                saw_io = true;
                io_error = detail;
            } else {
                // A matching live half whose record fails does not shadow a later valid entry: keep
                // probing and let a later entry win. Only if nothing valid is found is it Corrupt.
                saw_corrupt = true;
                last_detail = detail;
            }
        }
        if (first == kStateEmpty && second == kStateEmpty) break;
    }
    if (saw_io) {
        result.status = ReadStatus::IoError;
        result.error = io_error;
        return result;
    }
    if (saw_corrupt) {
        result.status = ReadStatus::Corrupt;
        result.error = last_detail.empty() ? "the entry for the requested key is corrupt" : last_detail;
        return result;
    }
    ++stats_.misses;
    result.status = ReadStatus::Missing;
    return result;
}

std::optional<std::vector<u8>> ChunkStore::read(const ChunkKey& key, std::string* error) {
    ReadResult result = fetch(key);
    if (result.status == ReadStatus::Ok) return std::move(result.bytes);
    if (result.status != ReadStatus::Missing) set_error(error, result.error);
    return std::nullopt;
}

bool ChunkStore::contains(const ChunkKey& key) { return fetch(key).status == ReadStatus::Ok; }

// --- ChunkStore: write / erase -------------------------------------------------------

bool ChunkStore::append_record(const ChunkKey& key, u32 state, ConstSpan<const u8> payload,
                               u64& record_offset, std::string* error) {
    RecordFields fields;
    fields.state = state;
    fields.payload_size = static_cast<u32>(payload.size());
    fields.key_x = key.x;
    fields.key_y = key.y;
    fields.payload_crc = save::crc32(payload);
    const auto raw = encode_record_header(fields);

    record_offset = data_end_;
    if (!write_at(file_, record_offset, raw)) {
        set_error(error, "cannot append a chunk record to '" + path_ + "'");
        return false;
    }
    if (!payload.empty() && !write_at(file_, record_offset + kRecordHeaderSize, payload)) {
        set_error(error, "cannot append a chunk record payload to '" + path_ + "'");
        return false;
    }
    if (std::fflush(file_) != 0) {
        set_error(error, "cannot flush an appended record to '" + path_ + "'");
        return false;
    }
    data_end_ += kRecordHeaderSize + static_cast<u64>(payload.size());
    stats_.bytes_written += kRecordHeaderSize + payload.size();
    return true;
}

bool ChunkStore::write(const ChunkKey& key, ConstSpan<const u8> payload, std::string* error) {
    if (!is_open()) {
        set_error(error, "the chunk store is not open");
        return false;
    }
    if (payload.size() > kMaxPayloadSize) {
        set_error(error, "the payload is larger than the 64 MiB chunk store limit");
        return false;
    }

    Probe probe;
    for (u32 attempt = 0;; ++attempt) {
        if (!probe_key(key, probe, error)) return false;
        if (probe.result == Probe::Result::Found) break;   // an overwrite does not grow the index
        const bool over_load =
            static_cast<f64>(live_count_ + 1u) > static_cast<f64>(slots_) * static_cast<f64>(options_.max_load_factor);
        if (!over_load && probe.result != Probe::Result::Full) break;
        if (attempt >= 2u) {
            set_error(error, "the chunk store index of '" + path_ + "' is full");
            return false;
        }
        u64 target = std::max<u64>(static_cast<u64>(slots_) * 2u,
                                   auto_slots_for(live_count_ + 1u, options_.max_load_factor, options_.initial_slots));
        if (target > kMaxSlots) target = kMaxSlots;
        if (target == slots_) {
            set_error(error, "the chunk store index of '" + path_ + "' is full");
            return false;
        }
        if (!rewrite(static_cast<u32>(target), nullptr, nullptr, error)) return false;
    }

    bool overwrite = false;
    u32 slot_index = probe.target_slot;
    int half_index = probe.target_half;
    Slot slot = probe.target_data;
    if (probe.result == Probe::Result::Found) {
        const Match* best = &probe.matches.front();
        for (const Match& match : probe.matches) {
            if (match.record_offset > best->record_offset) best = &match;
        }
        overwrite = true;
        slot_index = best->slot_index;
        half_index = best->half_index;
        if (!read_slot(slot_index, slot, error)) return false;
    } else {
        if (probe.result == Probe::Result::Full || !probe.has_target) {
            set_error(error, "the chunk store index of '" + path_ + "' is full");
            return false;
        }
        if (half_index < 0) half_index = write_target_half(slot);
    }

    if (max_generation(slot) >= kMaxGeneration) {
        if (!rewrite(slots_, nullptr, nullptr, error)) return false;
        return write(key, payload, error);   // generations were reset; retry once with a fresh index
    }

    const u8 target_state = state_of(slot.halves[half_index]);
    u64 record_offset = 0;
    if (!append_record(key, kStateLive, payload, record_offset, error)) return false;

    slot.halves[half_index] =
        make_half(max_generation(slot) + 1u, kStateLive, key, record_offset, static_cast<u32>(payload.size()));
    if (!write_slot_half(slot_index, slot, half_index, error)) return false;

    if (overwrite) {
        ++stats_.overwrites;
    } else {
        ++live_count_;
        if (target_state == kStateTombstone && tombstone_count_ > 0) --tombstone_count_;
    }
    ++stats_.writes;
    stats_.slots = slots_;
    stats_.chunk_count = live_count_;
    return commit_header(error);
}

bool ChunkStore::erase(const ChunkKey& key, std::string* error) {
    if (!is_open()) {
        set_error(error, "the chunk store is not open");
        return false;
    }
    Probe probe;
    if (!probe_key(key, probe, error)) return false;
    if (probe.matches.empty()) {
        // The key is absent, or its only index entry was poisoned and is no longer reachable. Record the
        // deletion in the log anyway: a later compact() rebuilds the index from records and must not
        // resurrect the damaged chunk. Dropping a damaged chunk then stays the game's decision.
        u64 record_offset = 0;
        if (!append_record(key, kStateTombstone, ConstSpan<const u8>{}, record_offset, error)) return false;
        return commit_header(error);
    }

    u32 generation = 0;
    for (const Match& match : probe.matches) generation = std::max(generation, generation_of(match.half));
    if (generation >= kMaxGeneration) {
        if (!rewrite(slots_, nullptr, nullptr, error)) return false;
        if (!probe_key(key, probe, error)) return false;
        if (probe.matches.empty()) return true;
    }

    std::vector<Match> matches = probe.matches;
    std::sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) {
        if (a.slot_index != b.slot_index) return a.slot_index < b.slot_index;
        return a.half_index < b.half_index;
    });
    u64 record_offset = 0;
    if (!append_record(key, kStateTombstone, ConstSpan<const u8>{}, record_offset, error)) return false;
    for (const Match& match : matches) {
        Slot slot;
        if (!read_slot(match.slot_index, slot, error)) return false;
        slot.halves[match.half_index] =
            make_half(max_generation(slot) + 1u, kStateTombstone, key, record_offset, 0u);
        if (!write_slot_half(match.slot_index, slot, match.half_index, error)) return false;
        if (live_count_ > 0) --live_count_;
        ++tombstone_count_;
    }
    ++stats_.erases;
    stats_.chunk_count = live_count_;
    return commit_header(error);
}

// --- ChunkStore: flush, stats, adapters ----------------------------------------------

bool ChunkStore::flush(bool durable, std::string* error) {
    if (!is_open()) {
        set_error(error, "the chunk store is not open");
        return false;
    }
    if (std::fflush(file_) != 0) {
        set_error(error, "cannot flush '" + path_ + "'");
        return false;
    }
    if (durable && !sync_file(file_)) {
        set_error(error, "cannot sync '" + path_ + "' to disk");
        return false;
    }
    return true;
}

usize ChunkStore::chunk_count() const { return live_count_; }

u64 ChunkStore::file_size() const {
    if (file_ == nullptr) return 0;
    u64 size = 0;
    if (!file_length(file_, size)) return 0;
    return size;
}

const ChunkStore::Stats& ChunkStore::stats() const { return stats_; }

void ChunkStore::reset_stats() {
    stats_ = Stats{};
    stats_.slots = slots_;
    stats_.chunk_count = live_count_;
}

OpenWorld::ChunkLoader ChunkStore::loader() {
    return [this](const ChunkKey& key) -> std::optional<std::vector<u8>> { return read(key, nullptr); };
}

OpenWorld::ChunkSaver ChunkStore::saver() {
    return [this](const ChunkKey& key, ConstSpan<const u8> payload) { return write(key, payload, nullptr); };
}

// --- ChunkStore: scan ----------------------------------------------------------------

ScanReport ChunkStore::scan() {
    ScanReport report;
    if (!is_open()) {
        report.error = "the chunk store is not open";
        return report;
    }
    const auto add_corruption = [&report](CorruptChunk chunk) {
        for (const CorruptChunk& existing : report.corruptions) {
            if (same_corruption(existing, chunk)) return;
        }
        report.corruptions.push_back(std::move(chunk));
    };

    // 1. Every index slot half: poisoned halves and live entries whose record fails.
    for (u32 index = 0; index < slots_; ++index) {
        Slot slot;
        std::string error;
        if (!read_slot(index, slot, &error)) {
            report.error = error;
            return report;
        }
        ++report.checked_slots;
        for (int half = 0; half < 2; ++half) {
            const Half& value = slot.halves[half];
            const u8 state = state_of(value);
            if (state == kStatePoisoned) {
                CorruptChunk chunk;
                chunk.kind = CorruptChunk::Kind::IndexSlot;
                chunk.slot_index = index;
                chunk.detail = "index slot half is poisoned (state/field check or CRC failed)";
                add_corruption(std::move(chunk));
                continue;
            }
            if (state != kStateLive) continue;
            const ChunkKey key{value.key_x, value.key_y};
            std::string detail;
            std::vector<u8> payload;
            const RecordCheck check = check_record(value, key, &payload, &detail, false);
            if (check == RecordCheck::Ok) {
                ++report.live_chunks;
            } else if (check == RecordCheck::IoError) {
                report.error = detail;
                return report;
            } else {
                CorruptChunk chunk;
                chunk.kind = check == RecordCheck::PayloadCorrupt ? CorruptChunk::Kind::RecordPayload
                                                                  : CorruptChunk::Kind::RecordHeader;
                chunk.has_key = true;
                chunk.key = key;
                chunk.slot_index = index;
                chunk.record_offset = value.record_offset;
                chunk.detail = detail;
                add_corruption(std::move(chunk));
            }
        }
    }

    // 2. Walk the record log independently of the index, resynchronising past corrupt headers.
    u64 offset = data_start();
    while (offset < data_end_) {
        if (data_end_ - offset < kRecordHeaderSize) {
            CorruptChunk chunk;
            chunk.kind = CorruptChunk::Kind::RecordHeader;
            chunk.record_offset = offset;
            chunk.detail = "truncated record header at the end of the data area";
            add_corruption(std::move(chunk));
            break;
        }
        RecordInfo info;
        std::string detail;
        const RecordCheck header_check = read_record_header(offset, info, &detail);
        if (header_check == RecordCheck::IoError) {
            report.error = detail;
            return report;
        }
        if (header_check == RecordCheck::HeaderCorrupt) {
            CorruptChunk chunk;
            chunk.kind = CorruptChunk::Kind::RecordHeader;
            chunk.record_offset = offset;
            chunk.detail = detail;
            add_corruption(std::move(chunk));

            u64 found = 0;
            bool io_error = false;
            if (!find_resync(offset + 1u, found, io_error, detail)) {
                if (io_error) report.error = detail;
                break;
            }
            ++stats_.resyncs;
            offset = found;
            continue;
        }
        ++report.checked_records;
        std::vector<u8> payload;
        const RecordCheck payload_check = read_record_payload(info, &payload, &detail, false);
        if (payload_check == RecordCheck::IoError) {
            report.error = detail;
            return report;
        }
        if (payload_check == RecordCheck::PayloadCorrupt) {
            CorruptChunk chunk;
            chunk.kind = CorruptChunk::Kind::RecordPayload;
            chunk.has_key = true;
            chunk.key = ChunkKey{info.key_x, info.key_y};
            chunk.record_offset = offset;
            chunk.detail = detail;
            add_corruption(std::move(chunk));
        }
        offset += kRecordHeaderSize + info.payload_size;
    }
    return report;
}

// --- ChunkStore: rewrite (grow / compact / index recovery / policy) ------------------

bool ChunkStore::rewrite(u32 requested_slots, const CorruptPolicy* policy, CompactReport* report,
                         std::string* error) {
    if (report != nullptr) *report = CompactReport{};
    std::map<ChunkKey, Survivor, KeyLess> survivors;
    std::set<ChunkKey, KeyLess> dropped;

    const auto record_corruption = [&](const CorruptChunk& chunk) -> bool {
        if (report != nullptr) report->corruptions.push_back(chunk);
        if (policy == nullptr) {
            set_error(error, "cannot rewrite '" + path_ + "': corruption found at record offset " +
                                 std::to_string(chunk.record_offset) +
                                 "; call scan() and compact(policy) to decide what to do with it");
            return false;
        }
        if ((*policy)(chunk) == CorruptAction::Abort) {
            set_error(error, "compaction of '" + path_ + "' aborted by the game policy at record offset " +
                                 std::to_string(chunk.record_offset));
            if (report != nullptr) report->aborted = true;
            return false;
        }
        ++stats_.dropped_records;
        if (chunk.has_key) {
            survivors.erase(chunk.key);
            dropped.insert(chunk.key);
        }
        return true;
    };

    u64 offset = data_start();
    while (offset < data_end_) {
        if (data_end_ - offset < kRecordHeaderSize) {
            CorruptChunk chunk;
            chunk.kind = CorruptChunk::Kind::RecordHeader;
            chunk.record_offset = offset;
            chunk.detail = "truncated record header";
            if (!record_corruption(chunk)) return false;
            break;
        }
        RecordInfo info;
        std::string detail;
        const RecordCheck header_check = read_record_header(offset, info, &detail);
        if (header_check == RecordCheck::IoError) {
            set_error(error, detail);
            return false;
        }
        if (header_check == RecordCheck::HeaderCorrupt) {
            CorruptChunk chunk;
            chunk.kind = CorruptChunk::Kind::RecordHeader;
            chunk.record_offset = offset;
            chunk.detail = detail;
            if (!record_corruption(chunk)) return false;

            u64 found = 0;
            bool io_error = false;
            if (!find_resync(offset + 1u, found, io_error, detail)) {
                if (io_error) {
                    set_error(error, detail);
                    return false;
                }
                break;
            }
            ++stats_.resyncs;
            offset = found;
            continue;
        }
        std::vector<u8> payload;
        const RecordCheck payload_check = read_record_payload(info, &payload, &detail, false);
        if (payload_check == RecordCheck::IoError) {
            set_error(error, detail);
            return false;
        }
        if (payload_check == RecordCheck::PayloadCorrupt) {
            CorruptChunk chunk;
            chunk.kind = CorruptChunk::Kind::RecordPayload;
            chunk.has_key = true;
            chunk.key = ChunkKey{info.key_x, info.key_y};
            chunk.record_offset = offset;
            chunk.detail = detail;
            if (!record_corruption(chunk)) return false;
            offset += kRecordHeaderSize + info.payload_size;
            continue;
        }
        const ChunkKey key{info.key_x, info.key_y};
        if (info.state == kStateLive) {
            survivors[key] = Survivor{offset, info.payload_size, info.payload_crc};
            dropped.erase(key);   // a later valid record wins over an earlier dropped one
        } else {
            survivors.erase(key);
            dropped.erase(key);
        }
        offset += kRecordHeaderSize + info.payload_size;
    }

    u32 new_slots = requested_slots != 0u ? clamp_slots(requested_slots)
                                          : auto_slots_for(survivors.size(), options_.max_load_factor,
                                                           options_.initial_slots);
    while (new_slots < kMaxSlots &&
           static_cast<f64>(survivors.size()) >
               static_cast<f64>(new_slots) * static_cast<f64>(options_.max_load_factor)) {
        new_slots = clamp_slots(new_slots * 2u);
    }

    const std::string temp = path_ + ".tmp";
    std::FILE* out = std::fopen(temp.c_str(), "w+b");
    if (out == nullptr) {
        set_error(error, "cannot create the rewrite file '" + temp + "'");
        return false;
    }
    const auto abort_rewrite = [&](const std::string& message) {
        set_error(error, message);
        if (out != nullptr) std::fclose(out);
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        return false;
    };

    std::vector<Slot> index(static_cast<usize>(new_slots));
    const u32 mask = new_slots - 1u;
    u64 out_offset = kIndexOffset + static_cast<u64>(new_slots) * kSlotSize;
    if (!seek_file(out, out_offset)) return abort_rewrite("cannot seek in the rewrite file '" + temp + "'");
    for (const auto& entry : survivors) {
        const ChunkKey& key = entry.first;
        const Survivor& survivor = entry.second;
        std::vector<u8> payload(survivor.payload_size);
        if (survivor.payload_size > 0u && !read_at(file_, survivor.offset + kRecordHeaderSize, payload)) {
            return abort_rewrite("cannot read a surviving payload from '" + path_ + "'");
        }
        if (save::crc32(ConstSpan<const u8>(payload)) != survivor.payload_crc) {
            return abort_rewrite("cannot rewrite '" + path_ + "': a surviving payload fails its checksum");
        }

        RecordFields fields;
        fields.state = kStateLive;
        fields.payload_size = survivor.payload_size;
        fields.key_x = key.x;
        fields.key_y = key.y;
        fields.payload_crc = survivor.payload_crc;
        const auto raw = encode_record_header(fields);
        if (std::fwrite(raw.data(), 1, raw.size(), out) != raw.size()) {
            return abort_rewrite("cannot write the rewrite file '" + temp + "'");
        }
        if (!payload.empty() && std::fwrite(payload.data(), 1, payload.size(), out) != payload.size()) {
            return abort_rewrite("cannot write the rewrite file '" + temp + "'");
        }

        const u32 hash = hash_low(key) & mask;
        bool inserted = false;
        for (u32 probe = 0; probe < new_slots; ++probe) {
            Slot& slot = index[static_cast<usize>((hash + probe) & mask)];
            if (state_of(slot.halves[0]) != kStateEmpty || state_of(slot.halves[1]) != kStateEmpty) continue;
            slot.halves[0] = make_half(1u, kStateLive, key, out_offset, survivor.payload_size);
            inserted = true;
            break;
        }
        if (!inserted) return abort_rewrite("cannot build the new index for '" + path_ + "'");
        out_offset += kRecordHeaderSize + survivor.payload_size;
    }

    if (!seek_file(out, kIndexOffset)) return abort_rewrite("cannot seek in the rewrite file '" + temp + "'");
    std::vector<u8> index_buffer(static_cast<usize>(kSlotSize) * 512u, 0u);
    usize buffered = 0;
    for (u32 slot_index = 0; slot_index < new_slots; ++slot_index) {
        encode_slot(index[slot_index], index_buffer.data() + buffered * kSlotSize);
        ++buffered;
        if (buffered == 512u || slot_index + 1u == new_slots) {
            const usize bytes = buffered * kSlotSize;
            if (std::fwrite(index_buffer.data(), 1, bytes, out) != bytes) {
                return abort_rewrite("cannot write the new index to '" + temp + "'");
            }
            buffered = 0;
        }
    }

    HeaderFields header;
    header.sequence = sequence_ + 1u;
    header.slots = new_slots;
    header.live_count = static_cast<u32>(survivors.size());
    header.tombstone_count = 0;
    header.data_start = kIndexOffset + static_cast<u64>(new_slots) * kSlotSize;
    header.data_end = out_offset;
    const auto header_page = encode_header(header);
    if (!write_at(out, 0, header_page) || !write_at(out, kHeaderCopySize, header_page)) {
        return abort_rewrite("cannot write the headers to '" + temp + "'");
    }
    if (std::fflush(out) != 0 || !sync_file(out)) {
        return abort_rewrite("cannot flush the rewrite file '" + temp + "'");
    }
    if (std::fclose(out) != 0) {
        out = nullptr;
        return abort_rewrite("cannot close the rewrite file '" + temp + "'");
    }
    out = nullptr;

    if (std::rename(temp.c_str(), path_.c_str()) != 0) {
        return abort_rewrite("cannot replace '" + path_ + "' with the rewritten store");
    }
    close_file();
    file_ = std::fopen(path_.c_str(), "r+b");
    if (file_ == nullptr) {
        set_error(error, "the chunk store was rewritten but '" + path_ + "' cannot be reopened");
        return false;
    }
    slots_ = new_slots;
    live_count_ = static_cast<u32>(survivors.size());
    tombstone_count_ = 0;
    data_end_ = out_offset;
    sequence_ = header.sequence;
    active_header_ = 0;
    stats_.slots = slots_;
    stats_.chunk_count = live_count_;
    ++stats_.rewrites;
    if (report != nullptr) {
        report->kept = survivors.size();
        report->dropped.assign(dropped.begin(), dropped.end());
        report->aborted = false;
    }
    return true;
}

bool ChunkStore::compact(std::string* error) {
    if (!is_open()) {
        set_error(error, "the chunk store is not open");
        return false;
    }
    return rewrite(0u, nullptr, nullptr, error);
}

bool ChunkStore::compact(const CorruptPolicy& policy, CompactReport* report, std::string* error) {
    if (!is_open()) {
        set_error(error, "the chunk store is not open");
        return false;
    }
    return rewrite(0u, &policy, report, error);
}

} // namespace t2d
