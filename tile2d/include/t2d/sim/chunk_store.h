// Tile2D - the file-backed chunk store for the relative-coordinate open world.
//
// OpenWorld's ChunkLoader/ChunkSaver are callbacks: the engine says which ABSOLUTE ChunkKey a payload
// belongs to, the game decides where the bytes live. This class is the engine's own answer for the
// common case - one file - designed so that locating a chunk never scans the file AND one damaged
// chunk never takes the others down with it:
//
//   * a lookup is hash -> one 72-byte index slot -> one self-describing record;
//   * every slot half carries its own CRC, so a flipped bit in the index cannot make a slot look
//     empty (which would cut a probe chain and hide later keys) or impersonate another key;
//   * fetch() distinguishes Ok / Missing / Corrupt, and scan() reports every damaged slot, record
//     header and record payload it can find, resynchronising past a corrupt record header so the rest
//     of the log stays reachable;
//   * the engine never silently decides what happens to damaged data: compact() aborts by default,
//     while compact(policy) lets the game see each corruption and choose Abort or Drop.
//
// File layout (little endian, one file, format version 2):
//
//   [header copy A: 4096 B][header copy B: 4096 B][index slots: slots * 72 B][data records ...]
//
// Header copies: magic T2DK, version 2, sequence, index_offset (8192), slots (power of two 16..2^24),
// live/tombstone counts, reserved, data_start (= 8192 + slots*72), data_end and a header CRC over the
// first 56 bytes. Every metadata commit writes the copy that is NOT current with sequence + 1, so a
// torn header never makes the file unopenable. Version 1 files are refused with a clear error; there
// is no migration.
//
// Index slot: two 36-byte halves. Half: tag u32 (bits 0-1 state: 0 empty / 1 live / 2 tombstone,
// bits 2-31 generation), key_x i64, key_y i64, record_offset u64, payload_size u32, half_crc32 u32
// (CRC-32 over the first 32 bytes). An all-zero half is the valid empty. Anything else that fails the
// field checks or the CRC is POISONED: it never terminates a probe, never matches a key and never
// shadows another entry - it is reported and counted instead.
//
// Record: magic T2R1, state u32 (1 live / 2 deleted), payload_size u32, key_x i64, key_y i64,
// payload_crc32 u32, header_crc32 u32 (over the first 32 bytes), reserved u32 = 0, then the payload.
// Records are append-only; overwrites and erases append new records. A corrupt record header has no
// trustworthy length, so scanners search forward for the next candidate whose header CRC AND payload
// CRC both hold; a corrupt payload with a good header is reported with its key and skipped by its
// known length.
//
// The full format, the protection story and the policy-compaction contract live in docs/OPEN_WORLD.md
// (section "存储文件：t2d::ChunkStore"). This is part of the optional Mefemma-oriented open-world
// facility; ordinary TileMaps do not use it.
#pragma once

#include <t2d/core/types.h>
#include <t2d/save/archive.h>
#include <t2d/sim/open_world.h>

#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace t2d {

/// What fetch() found.
enum class ReadStatus : u8 {
    Ok,       ///< the exact payload bytes were read and passed their checksums
    Missing,  ///< no reachable valid entry (not an error); a poisoned index slot on the probe path also
              ///< reads as Missing - scan() reports that slot because it cannot name the lost key
    Corrupt,  ///< a reachable live entry for the key exists but its record/checksum data is damaged
    NotOpen,  ///< the store has been moved from or closed
    IoError,  ///< the file could not be read
};

struct ReadResult {
    ReadStatus status = ReadStatus::Missing;
    std::vector<u8> bytes;
    std::string error;

    [[nodiscard]] bool ok() const { return status == ReadStatus::Ok; }
};

/// One piece of damage scan() and policy compaction can report. The engine only reports; what to do
/// about a damaged chunk (drop, regenerate, abort) is the game's decision.
struct CorruptChunk {
    enum class Kind : u8 {
        IndexSlot,      ///< a poisoned slot half (CRC/state/field check failed)
        RecordHeader,   ///< a record header that failed its structure/CRC (key usually unknown)
        RecordPayload,  ///< a good header whose payload failed its CRC
    };

    Kind kind = Kind::IndexSlot;
    bool has_key = false;
    ChunkKey key{};
    u64 slot_index = 0;     ///< IndexSlot: which slot position
    u64 record_offset = 0;  ///< RecordHeader/RecordPayload: where in the data area
    std::string detail;
};

struct ScanReport {
    std::vector<CorruptChunk> corruptions;
    usize live_chunks = 0;
    usize checked_slots = 0;
    usize checked_records = 0;
    std::string error;   ///< non-empty only for a fatal I/O failure; corruption is data

    [[nodiscard]] bool ok() const { return error.empty(); }
};

/// What a game wants done with one damaged chunk during policy compaction.
enum class CorruptAction : u8 {
    Abort,  ///< stop, leave the original file untouched, return false
    Drop,   ///< skip this damaged record/region, continue, and make the affected key missing
};

using CorruptPolicy = std::function<CorruptAction(const CorruptChunk&)>;

struct CompactReport {
    std::vector<CorruptChunk> corruptions;   ///< everything the rewrite scan found
    std::vector<ChunkKey> dropped;           ///< keys dropped because of corruption (known keys only)
    usize kept = 0;                          ///< chunks written to the new file
    bool aborted = false;                    ///< true when the policy (or the default) stopped it
};

/// A single-file store for OpenWorld chunk payloads, keyed by the ABSOLUTE ChunkKey. Move-only: the
/// FILE handle moves with the object; a moved-from store reports is_open() == false.
class ChunkStore {
public:
    struct Options {
        /// Index slots at create time; clamped to [kMinSlots, kMaxSlots] and rounded up to a power of
        /// two. The index is allowed to grow past this while the store is written to.
        u32 initial_slots = 1024;
        /// Maximum live entries / slots before write() grows the index; clamped to [0.1, 0.95].
        f32 max_load_factor = 0.7f;
    };

    /// Instrumentation. reads counts every file read a lookup performs (one per index slot probed plus
    /// one per record read), so a clean hit sequence satisfies reads == probes + hits; a lookup never
    /// scans the file. bytes_read counts the slot and record bytes those reads moved; bytes_written
    /// counts appended record bytes (index and header writes are bookkeeping, not payload).
    /// lookups/hits/misses count fetch() calls by outcome (Corrupt is neither a hit nor a miss);
    /// probes counts slot positions read; corrupt_slots counts poisoned slot halves encountered by
    /// lookups; corrupt_records counts record validations that failed; resyncs counts recovery scans
    /// that resynchronised past a corrupt record header; dropped_records counts records dropped by a
    /// policy-driven compact. slots and chunk_count are state, so reset_stats() keeps them.
    struct Stats {
        u64 lookups = 0;
        u64 hits = 0;
        u64 misses = 0;
        u64 probes = 0;
        u64 reads = 0;
        u64 read_failures = 0;
        u64 writes = 0;
        u64 overwrites = 0;
        u64 erases = 0;
        u64 bytes_read = 0;
        u64 bytes_written = 0;
        u64 rewrites = 0;
        u64 tail_recovered = 0;
        u64 corrupt_slots = 0;
        u64 corrupt_records = 0;
        u64 resyncs = 0;
        u64 dropped_records = 0;
        u32 slots = 0;
        usize chunk_count = 0;
    };

    static constexpr u32 kHeaderCopySize = 4096;
    static constexpr u64 kIndexOffset = 2 * static_cast<u64>(kHeaderCopySize);
    static constexpr u32 kSlotHalfSize = 36;
    static constexpr u32 kSlotSize = 2 * kSlotHalfSize;   // 72
    static constexpr u32 kRecordHeaderSize = 40;
    static constexpr u32 kMaxPayloadSize = 64u << 20;
    static constexpr u32 kMinSlots = 16;
    static constexpr u32 kMaxSlots = 1u << 24;
    static constexpr u32 kFormatVersion = 2;
    static constexpr u32 kMagic = save::fourcc('T', '2', 'D', 'K');
    static constexpr u32 kRecordMagic = save::fourcc('T', '2', 'R', '1');

    /// Opens (creating when the path is missing or empty) the path. On failure returns nullopt and,
    /// when given, a readable reason in the error. The two argument form uses the default Options.
    /// A format version other than 2 is refused (there is no migration).
    [[nodiscard]] static std::optional<ChunkStore> open(const std::string& path, std::string* error = nullptr);
    [[nodiscard]] static std::optional<ChunkStore> open(const std::string& path, const Options& options,
                                                        std::string* error = nullptr);

    ~ChunkStore();
    ChunkStore(ChunkStore&& other) noexcept;
    ChunkStore& operator=(ChunkStore&& other) noexcept;
    T2D_NON_COPYABLE(ChunkStore);

    [[nodiscard]] bool is_open() const;
    [[nodiscard]] const std::string& path() const;

    /// The status-and-bytes lookup: Ok/Missing/Corrupt/NotOpen/IoError. Corrupt and IoError carry a
    /// reason; Missing is not an error. Damage is never returned as wrong bytes.
    [[nodiscard]] ReadResult fetch(const ChunkKey& key);

    /// Convenience wrapper: Ok -> bytes; Missing -> nullopt with no error; Corrupt/IoError -> nullopt
    /// plus a reason. Non-const because the lookup updates stats, like fetch().
    [[nodiscard]] std::optional<std::vector<u8>> read(const ChunkKey& key, std::string* error = nullptr);

    /// Appends a record for the key and commits it to the index and the header. An existing key is
    /// overwritten (the old bytes stay as garbage for compact()). Returns false and a reason (plus a
    /// possibly inconsistent file) on I/O failure; the documented recovery is to reopen the store.
    [[nodiscard]] bool write(const ChunkKey& key, ConstSpan<const u8> payload, std::string* error = nullptr);

    /// Appends a deleted record and writes a tombstone slot. Idempotent: a missing (or poisoned) key
    /// returns true; a write after it inserts a fresh valid entry. The deleted record is appended even
    /// when the key's index entry is missing/poisoned, so a later compact() cannot resurrect a chunk the
    /// game decided to drop from an older record.
    [[nodiscard]] bool erase(const ChunkKey& key, std::string* error = nullptr);

    /// True when fetch() says Ok (a corrupt entry reads as absent here; fetch() gives the reason).
    [[nodiscard]] bool contains(const ChunkKey& key);

    [[nodiscard]] usize chunk_count() const;
    /// The current size of the file on disk (0 when the handle cannot be measured).
    [[nodiscard]] u64 file_size() const;

    /// Adapters for OpenWorld::set_store(). They capture this; the store must outlive the callbacks.
    /// A corrupt or missing chunk becomes nullopt, which is exactly the world's "regenerate it".
    [[nodiscard]] OpenWorld::ChunkLoader loader();
    [[nodiscard]] OpenWorld::ChunkSaver saver();

    /// Read-only corruption report: every poisoned slot half, every live entry whose record fails,
    /// and an independent walk of the record log (with resynchronisation past corrupt headers). Fatal
    /// file I/O goes in report.error; corruption is data. Does not modify the file.
    [[nodiscard]] ScanReport scan();

    /// fflush()es the file; when durable, also fsyncs (or _commit on Windows). Returns false with a
    /// reason when either step fails.
    [[nodiscard]] bool flush(bool durable = true, std::string* error = nullptr);

    /// Rewrites the file compactly: scans the record log, keeps the latest state per key, writes a temp
    /// file with a fresh index in deterministic (y, x) key order and renames it over the original.
    /// The default aborts on the first corrupt RECORD and leaves the original untouched; use the policy
    /// overload to let the game decide per corrupt record. A poisoned index slot does not reach the
    /// policy: the record log is the source of truth and the rebuilt index recovers it (erase(key) is the
    /// way for the game to drop a known key instead of recovering it). This is the index-recovery path.
    [[nodiscard]] bool compact(std::string* error = nullptr);
    [[nodiscard]] bool compact(const CorruptPolicy& policy, CompactReport* report = nullptr,
                               std::string* error = nullptr);

    [[nodiscard]] const Stats& stats() const;
    /// Clears the counters; the slot count and live chunk count are state, so they stay accurate.
    void reset_stats();

private:
    struct Half {
        u32 tag = 0;
        i64 key_x = 0;
        i64 key_y = 0;
        u64 record_offset = 0;
        u32 payload_size = 0;
        u32 half_crc = 0;
        // An in-memory default-constructed Half is the valid empty: no bytes have been written yet.
        // decode_slot() and make_half() set these explicitly from the bytes/fields they hold.
        bool crc_ok = true;
        bool all_zero = true;
    };
    struct Slot {
        Half halves[2];
    };
    struct Match {
        u32 slot_index = 0;
        int half_index = 0;
        u64 record_offset = 0;
        u32 payload_size = 0;
        Half half;
    };
    struct Probe {
        enum class Result : u8 { Found, Missing, Full } result = Result::Missing;
        std::vector<Match> matches;              ///< every live half for the key found in the chain
        bool has_target = false;                 ///< insertion target (tombstone/poisoned/empty)
        u32 target_slot = 0;
        int target_half = -1;                    ///< -1: choose a half of an all-empty slot
        Slot target_data;
        bool target_is_empty_slot = false;
        bool corruption_seen = false;            ///< poison in a slot on this chain
    };
    enum class RecordCheck : u8 { Ok, HeaderCorrupt, PayloadCorrupt, IoError };
    struct RecordInfo {
        u64 offset = 0;
        u32 payload_size = 0;
        u32 payload_crc = 0;
        u32 state = 0;
        i64 key_x = 0;
        i64 key_y = 0;
    };

    ChunkStore() = default;

    bool create_empty(std::string* error);
    bool load_existing(std::string* error);
    bool recover_tail(u64 file_size, std::string* error);
    bool apply_recovered_record(const ChunkKey& key, u32 state, u64 record_offset, u32 payload_size,
                                std::string* error);
    bool recompute_counts(std::string* error);
    [[nodiscard]] bool probe_key(const ChunkKey& key, Probe& probe, std::string* error);
    [[nodiscard]] RecordCheck check_record(const Half& half, const ChunkKey& key, std::vector<u8>* payload,
                                           std::string* detail, bool count_stats = true);
    [[nodiscard]] RecordCheck read_record_payload(const RecordInfo& info, std::vector<u8>* payload,
                                                  std::string* detail, bool count_stats = true);
    bool read_slot(u32 slot_index, Slot& slot, std::string* error);
    bool write_slot_half(u32 slot_index, const Slot& slot, int half_index, std::string* error);
    bool append_record(const ChunkKey& key, u32 state, ConstSpan<const u8> payload, u64& record_offset,
                       std::string* error);
    bool write_header_copy(u32 copy_index, u64 sequence, std::string* error);
    bool commit_header(std::string* error);
    /// The single rewrite path. policy == nullptr means the safe default (abort on corruption).
    bool rewrite(u32 requested_slots, const CorruptPolicy* policy, CompactReport* report, std::string* error);
    /// Finds the next byte offset in [from, data_end - 40] whose header AND payload checksums hold.
    /// Returns false when there is none; io_error distinguishes "no candidate" from a read failure.
    [[nodiscard]] bool find_resync(u64 from, u64& found, bool& io_error, std::string& detail);
    [[nodiscard]] RecordCheck read_record_header(u64 offset, RecordInfo& info, std::string* detail);
    [[nodiscard]] static Slot decode_slot(const u8* raw);
    static void encode_slot(const Slot& slot, u8* raw);
    /// 0 = valid empty, 1 = live, 2 = tombstone, 3 = poisoned.
    [[nodiscard]] static u8 state_of(const Half& half);
    [[nodiscard]] static bool is_empty_half(const Half& half);
    [[nodiscard]] static bool is_poisoned(const Half& half);
    [[nodiscard]] static u32 generation_of(const Half& half);
    [[nodiscard]] static u32 max_generation(const Slot& slot);
    /// Which half a write should replace: a poisoned half first (repair), then a tombstone, then the
    /// lower-generation half; an all-empty slot uses half 1 (generation 1).
    [[nodiscard]] static int write_target_half(const Slot& slot);
    [[nodiscard]] static Half make_half(u32 generation, u8 state, const ChunkKey& key, u64 record_offset,
                                        u32 payload_size);
    [[nodiscard]] u32 data_start() const;
    [[nodiscard]] u32 hash_low(const ChunkKey& key) const;
    void close_file();

    std::FILE* file_ = nullptr;
    std::string path_;
    Options options_{};
    u32 slots_ = 0;
    u32 live_count_ = 0;
    u32 tombstone_count_ = 0;
    u64 data_end_ = 0;
    u64 sequence_ = 0;
    u32 active_header_ = 0;   ///< 0 = copy A at offset 0, 1 = copy B at offset 4096
    Stats stats_{};
};

} // namespace t2d
