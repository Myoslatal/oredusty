// Tile2D - the open world: a relative-coordinate streamed tile plane.
//
// TileMap is a rectangle with edges. It answers "what is here" for the cells of an authored width x
// height grid, and off the grid it answers out of bounds - solid walls by default - which is exactly
// what a fixed level wants and exactly what an open world cannot afford. An open world has no authored
// size: a generator decides what a cell is, the same cell must come back the same way however far the
// player has walked, and only the chunks near the view stay in memory.
//
// This is that layer, and it is an OPTIONAL engine facility intended for the Mefemma shell. It is not
// the mine's map model and it does not touch TileMap: ordinary maps keep their absolute authored cell
// coordinates and do not use any of this. The facility splits coordinates in two on purpose:
//
//   * ChunkKey - the ABSOLUTE chunk identity (i64). Generation and the byte store see it, so the same
//     chunk always regenerates the same content and keeps the same store payload, however the world
//     has been rebased or reloaded.
//   * ChunkCoord and cells - coordinates RELATIVE to origin(), the absolute chunk that relative (0, 0)
//     currently maps to. Streaming, queries, bounds and rendering use them, so f32 geometry stays
//     small and precise no matter how far the world extends. Moving the origin (a "floating origin",
//     OpenWorld::set_origin() / recenter()) changes only that mapping, never the resident content.
//
// Everything here is single threaded and deterministic: the engine owns no threads and no global
// state, generation is seeded from (world seed, absolute chunk key) alone, and two worlds with the same
// seed generate identical chunks in any load order or origin. See docs/OPEN_WORLD.md for the lifecycle,
// the origin pattern and the byte format.
#pragma once

#include <t2d/core/camera2d.h>
#include <t2d/core/math2d.h>
#include <t2d/core/rng.h>
#include <t2d/core/types.h>
#include <t2d/sim/tilemap.h>
#include <t2d/sim/tileset.h>

#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace t2d {

/// ABSOLUTE chunk identity, in the same unbounded lattice as the world itself. Generation and the byte
/// store use it, so it must never depend on the current origin: a chunk keeps its key and therefore its
/// content and its store payload when the world is rebased.
struct ChunkKey {
    i64 x = 0;
    i64 y = 0;

    friend constexpr bool operator==(const ChunkKey&, const ChunkKey&) = default;
};

/// Hash for absolute chunk keys (the resident map and store-keyed containers use it).
struct ChunkKeyHash {
    [[nodiscard]] usize operator()(const ChunkKey& key) const noexcept;
};

/// A chunk coordinate RELATIVE to OpenWorld::origin(): the absolute chunk is origin() + this offset.
/// i32 on purpose - the open-world API is a streaming/render space, not an authored map's coordinate
/// system. The practical range is +/-OpenWorld::kMaxRelativeChunk.
struct ChunkCoord {
    i32 x = 0;
    i32 y = 0;

    friend constexpr bool operator==(const ChunkCoord&, const ChunkCoord&) = default;
};

/// Hash for relative chunk coordinates (sets of view chunks inside the streamer).
struct ChunkCoordHash {
    [[nodiscard]] usize operator()(const ChunkCoord& coord) const noexcept;
};

/// One resident chunk: kSize x kSize cells on each of 1..kMaxLayers layers, in row major order.
///
/// A chunk never has its own dimensions. That is what makes a chunk blob a pure tile payload: the
/// reader is told how many layers to expect, and every layer is exactly kSize * kSize cells. The blob
/// format is the same T2DC v1 whether the chunk is addressed absolutely or by a relative coordinate.
class WorldChunk {
public:
    static constexpr i32 kSize = TileMap::kChunkSize;
    static constexpr usize kCellCount = static_cast<usize>(kSize) * static_cast<usize>(kSize);

    /// An empty chunk with one layer, which is what every caller that does not name a layer count
    /// means by "a chunk".
    WorldChunk() = default;
    /// The layer count is clamped to [1, TileMap::kMaxLayers].
    explicit WorldChunk(i32 layers);

    [[nodiscard]] i32 layers() const;
    /// Local coordinates are 0..kSize-1. An invalid layer or coordinate reads kEmptyTile.
    [[nodiscard]] TileId at(i32 layer, i32 local_x, i32 local_y) const;
    /// An invalid layer or coordinate is ignored. A valid write marks the chunk dirty.
    void set(i32 layer, i32 local_x, i32 local_y, TileId id);
    /// Fills one layer with id and marks the chunk dirty.
    void fill(i32 layer, TileId id);
    /// The raw layer storage (kCellCount ids). This is the generator's and the serialiser's access;
    /// writing through it does NOT mark the chunk dirty - a generated chunk has to come back clean,
    /// or every chunk it touches would be saved on eviction.
    [[nodiscard]] Span<TileId> tiles(i32 layer);
    [[nodiscard]] ConstSpan<const TileId> tiles(i32 layer) const;

    [[nodiscard]] bool dirty() const;
    void mark_dirty();
    void clear_dirty();
    [[nodiscard]] usize count_non_empty() const;

    /// FNV-1a over the layer count (u32, little endian) followed by every tile id (u16, little
    /// endian) in row major order, bottom to top. Two chunks with the same tiles and the same layer
    /// count hash the same; the layer count is in the hash so a one layer chunk and a two layer chunk
    /// with identical base layers do not collide.
    [[nodiscard]] u64 checksum() const;

    /// The chunk blob docs/OPEN_WORLD.md documents:
    ///   'T' '2' 'D' 'C' | u8 version (1) | varint layer count
    ///   | per layer, bottom to top: (varint run length > 0 | u16 tile id)* over row major 32 x 32.
    /// A run never crosses a layer, and an empty chunk still encodes valid runs.
    [[nodiscard]] std::vector<u8> serialize() const;
    /// Reads a blob written by serialize(). expected_layers is the world's layer count.
    /// Rejects a bad magic or version, a layer count that is outside 1..kMaxLayers or different from
    /// expected_layers, a zero run, a run longer than the cells left on its layer, a truncated blob
    /// and trailing bytes.
    [[nodiscard]] static std::optional<WorldChunk> deserialize(ConstSpan<const u8> data,
                                                               i32 expected_layers);

private:
    i32 layers_ = 1;
    std::vector<TileId> cells_ = std::vector<TileId>(kCellCount, kEmptyTile);
    bool dirty_ = false;
};

/// Everything a world needs to know before it exists. The constructor clamps the values and
/// config() returns the clamped result, so the rest of the class can take the stored config at face
/// value.
struct OpenWorldConfig {
    /// World size of one cell, in pixels. A non-finite or non-positive value becomes 16.
    f32 tile_size = 16.0f;
    /// 1..TileMap::kMaxLayers; clamped.
    i32 layers = 1;
    /// Mixed into chunk_seed(); the generator's only source of randomness.
    u64 seed = 0;
    /// Ring of relative chunks around the view that update() keeps loaded. Negative values become 0.
    i32 load_margin_chunks = 1;
    /// Ring that has to be left before a chunk is evicted. The effective value is
    /// max(load_margin_chunks, unload_margin_chunks), never negative: the hysteresis band.
    i32 unload_margin_chunks = 2;
    /// Soft cap on resident chunks; 0 means unlimited. update() refuses to grow past it rather than
    /// evicting the chunks the view still needs.
    usize max_loaded_chunks = 2048;
};

/// The streaming world in relative coordinates: resident chunks around a relative view, deterministic
/// generation keyed by absolute chunks, edits that survive eviction through a game supplied store, and
/// queries that never load implicitly.
///
/// Two coordinate spaces, and the split is the point of the class:
///   * every gameplay/streaming/render call takes RELATIVE cells and relative chunk coordinates,
///     measured from origin(). They stay small, so f32 world geometry stays precise;
///   * generation, chunk_seed()/chunk_rng() and the store callbacks receive the ABSOLUTE ChunkKey.
///     That is what keeps a save deterministic when the game rebases the world: moving the origin
///     changes the mapping, never the identity or the content of a chunk.
///
/// The default origin is chunk (0, 0). set_origin() and recenter() move it and return the shift so the
/// caller can move its own camera and entity positions; resident chunks are re-addressed, not reloaded.
///
/// Practical ranges: absolute cells +/-2^40 and absolute chunks +/-2^35 (set_origin clamps to that);
/// relative cells +/-kMaxRelativeCell (2^30) and relative chunks +/-kMaxRelativeChunk (2^25). A view
/// outside the relative range, or one touching more than 2^20 chunks, makes chunks_in() return nothing
/// and update() a no-op. Direct cell calls take any i32; they are the caller's coordinate space, not a
/// claim that the world extends that far.
class OpenWorld {
public:
    using LayerMask = TileMap::LayerMask;
    /// Absolute chunk key: what generation sees, so content is addressable independently of the
    /// current origin.
    using ChunkGenerator = std::function<void(const ChunkKey&, WorldChunk&)>;
    /// Absolute chunk key -> stored bytes, or nullopt/empty when the store has none.
    using ChunkLoader = std::function<std::optional<std::vector<u8>>(const ChunkKey&)>;
    /// Absolute chunk key + serialised chunk bytes -> whether they were durably written.
    using ChunkSaver = std::function<bool(const ChunkKey&, ConstSpan<const u8>)>;

    /// Absolute cell range the origin may be placed in (chunk-aligned): +/-2^40 cells, i.e. +/-2^35
    /// chunk keys. Wider than f32 can render precisely; the bound only keeps i64 arithmetic safe.
    static constexpr i64 kMaxAbsoluteCell = i64{1} << 40;
    static constexpr i64 kMaxAbsoluteChunk = kMaxAbsoluteCell / WorldChunk::kSize;
    /// Practical relative range: +/-2^30 cells, i.e. +/-2^25 relative chunks. A view is refused
    /// outside it, so relative f32 geometry stays meaningful.
    static constexpr i32 kMaxRelativeCell = i32{1} << 30;
    static constexpr i32 kMaxRelativeChunk = kMaxRelativeCell / WorldChunk::kSize;

    /// What the world did since the last reset_stats(). chunk_seed()/chunk_rng() are pure. The
    /// counters make streaming behaviour measurable instead of something to guess at in a debugger.
    struct Stats {
        u64 chunks_generated = 0;   ///< new resident chunks with no store payload (generated or empty)
        u64 chunks_restored = 0;    ///< new resident chunks read back from the store
        u64 chunks_saved = 0;       ///< successful saver callbacks (update, unload_chunk, flush)
        u64 chunks_evicted = 0;     ///< chunks dropped from residency by update() (clean or saved)
        u64 restore_failures = 0;   ///< store payloads that were not a valid chunk blob
        u64 save_failures = 0;      ///< saver callbacks that returned false
        u64 budget_refusals = 0;    ///< loads skipped because max_loaded_chunks had no room
    };

    /// The shift set_origin()/recenter() performed, measured in absolute cells:
    /// new_origin - old_origin, multiplied by kSize. position_delta() is the amount to add to every
    /// relative position to keep it at the same absolute place.
    struct OriginShift {
        i64 x = 0;
        i64 y = 0;

        [[nodiscard]] Vec2 position_delta(f32 tile_size) const;
    };

    OpenWorld() = default;
    explicit OpenWorld(const OpenWorldConfig& config);

    [[nodiscard]] const OpenWorldConfig& config() const;
    [[nodiscard]] f32 tile_size() const;
    [[nodiscard]] i32 layers() const;
    [[nodiscard]] u64 seed() const;

    // --- origin: the moving zero of the relative space ------------------------
    /// The absolute chunk that relative chunk (0, 0) maps to. Default {0, 0}.
    [[nodiscard]] ChunkKey origin() const;
    /// The absolute chunk a relative chunk names under the current origin.
    [[nodiscard]] ChunkKey absolute_of(const ChunkCoord& relative) const;
    /// The relative chunk an absolute chunk has under the current origin, or nullopt when the offset
    /// does not fit the i32 relative space (never truncates).
    [[nodiscard]] std::optional<ChunkCoord> relative_of(const ChunkKey& absolute) const;
    /// Absolute cell of a relative cell: origin() * kSize + relative.
    [[nodiscard]] i64 absolute_cell_x(i32 relative_x) const;
    [[nodiscard]] i64 absolute_cell_y(i32 relative_y) const;
    /// Moves the origin to new_origin (clamped to +/-kMaxAbsoluteChunk) and returns the shift. The
    /// resident chunks are re-addressed by the new mapping; nothing is reloaded, saved or lost.
    [[nodiscard]] OriginShift set_origin(const ChunkKey& new_origin);
    /// set_origin() to the absolute chunk that contains the camera centre (a relative world position),
    /// i.e. the convenience behind a floating-origin camera. Apply OriginShift::position_delta() to the
    /// camera and the game's own positions to keep looking at the same absolute place.
    [[nodiscard]] OriginShift recenter(const Camera2D& camera);

    // --- generation and store -------------------------------------------------
    /// Sets the chunk generator; the world does not call it until a chunk is needed. The generator
    /// receives the ABSOLUTE chunk key and must write complete chunk content for it: it may use
    /// set(), fill() or tiles() and the world returns the chunk clean either way (generation is not an
    /// edit). Generation is deterministic by contract, not enforced - the same seed and absolute key
    /// are expected to produce the same chunk in any load order or origin.
    void set_generator(ChunkGenerator generator);
    /// Either callback may be nullptr. Both receive the ABSOLUTE chunk key, so a save stays valid when
    /// the origin moves. The loader is asked before the generator; the saver receives the
    /// WorldChunk::serialize() bytes and returns whether they were durably written.
    void set_store(ChunkLoader loader, ChunkSaver saver);

    /// Splitmix64-style mixing of the world seed and the ABSOLUTE chunk key. The same three values
    /// always produce the same seed on every platform; this is the identity the store and generation
    /// agree on, independent of the current origin.
    [[nodiscard]] static u64 chunk_seed(u64 world_seed, const ChunkKey& key);
    /// Rng(seed(), chunk_seed(seed(), key)), the generator's random stream.
    [[nodiscard]] Rng chunk_rng(const ChunkKey& key) const;

    // --- streaming ------------------------------------------------------------
    /// Every RELATIVE chunk the finite view touches, plus a ring of margin_chunks chunks, in ascending
    /// (y, x) order. A view with a non-finite coordinate or a negative size yields nothing, and so does
    /// a view outside the practical relative range or one touching more than 2^20 chunks. The view is
    /// in relative world units, the same units as a Camera2D looking at relative space.
    [[nodiscard]] std::vector<ChunkCoord> chunks_in(const Aabb2& view, i32 margin_chunks = 0) const;

    /// Streams the world around the RELATIVE view and returns the resident chunk count:
    ///   1. evict every resident chunk whose ABSOLUTE key is outside the absolute keep set derived
    ///      from chunks_in(view, unload margin): a clean chunk is dropped, a dirty one is saved first
    ///      and kept resident when the save fails or no saver exists;
    ///   2. load every missing chunk in chunks_in(view, load margin), nearest the view centre first
    ///      (Manhattan distance, ties by y then x), honouring max_loaded_chunks by evicting clean
    ///      chunks the load does not need and otherwise counting a budget refusal.
    /// An invalid (or refused) view is a no-op. The two margins are what stops a camera sitting on a
    /// chunk boundary from loading and unloading the same chunk every frame. The origin is not part of
    /// this decision: it only names the chunks, it does not change which absolute ones they are.
    usize update(const Aabb2& view);
    usize update(const Camera2D& camera);

    [[nodiscard]] WorldChunk* find_chunk(const ChunkCoord& relative);
    [[nodiscard]] const WorldChunk* find_chunk(const ChunkCoord& relative) const;
    [[nodiscard]] bool is_loaded(const ChunkCoord& relative) const;

    /// Makes the relative chunk resident, loading it from the store first when the store has a payload
    /// for its ABSOLUTE key and generating it otherwise. A malformed payload is reported with T2D_WARN
    /// naming the absolute key and falls back to generation. A generated chunk is clean; this is the
    /// one place that creates chunks, and every other operation (queries included) only reads what it
    /// made resident.
    WorldChunk& ensure_chunk(const ChunkCoord& relative);

    /// Drops a chunk: clean ones are erased at once; a dirty one is serialised and saved under its
    /// absolute key first, and stays resident (returning false) when the save fails or no saver exists.
    bool unload_chunk(const ChunkCoord& relative);

    /// The resident ABSOLUTE keys, sorted by y then x, so anything that walks a world is deterministic.
    [[nodiscard]] std::vector<ChunkKey> resident_chunks() const;
    [[nodiscard]] usize resident_chunk_count() const;

    // --- relative cell queries ------------------------------------------------
    /// Cell queries read RESIDENT chunks only: a chunk that is not loaded reads as empty space, and
    /// these are const on purpose - the frame-thread queries never start a load. Coordinates are
    /// RELATIVE cells; layer masks work exactly as they do on TileMap (bit i is layer i).
    [[nodiscard]] TileId at(i32 cell_x, i32 cell_y) const;
    [[nodiscard]] TileId at(i32 layer, i32 cell_x, i32 cell_y) const;
    [[nodiscard]] TileId topmost(i32 cell_x, i32 cell_y, LayerMask mask = TileMap::kAllLayers) const;

    /// Writes one relative cell, making its chunk resident first (generating it if needed), and marks
    /// that chunk dirty. The two argument form writes the base layer, exactly like TileMap. Returns
    /// false only when the layer does not exist.
    bool set(i32 cell_x, i32 cell_y, TileId id);
    bool set(i32 layer, i32 cell_x, i32 cell_y, TileId id);

    [[nodiscard]] bool is_solid(i32 cell_x, i32 cell_y, const Tileset& tileset,
                                LayerMask mask = TileMap::kAllLayers) const;
    [[nodiscard]] bool is_one_way(i32 cell_x, i32 cell_y, const Tileset& tileset,
                                  LayerMask mask = TileMap::kAllLayers) const;
    [[nodiscard]] bool is_hazard(i32 cell_x, i32 cell_y, const Tileset& tileset,
                                 LayerMask mask = TileMap::kAllLayers) const;
    [[nodiscard]] bool overlaps_solid(const Aabb2& box, const Tileset& tileset,
                                      LayerMask mask = TileMap::kAllLayers) const;
    [[nodiscard]] bool overlaps_hazard(const Aabb2& box, const Tileset& tileset,
                                       LayerMask mask = TileMap::kAllLayers) const;

    [[nodiscard]] usize dirty_chunk_count() const;

    /// Saves every dirty chunk it can, in deterministic absolute (y, x) order, clearing each chunk's
    /// dirty flag only when the saver accepted it. Returns true iff nothing is dirty afterwards: dirty
    /// chunks with no saver at all therefore make this false rather than silently dropping edits.
    bool flush();
    bool flush(const ChunkCoord& relative);

    [[nodiscard]] const Stats& stats() const;
    void reset_stats();

    /// Floor division by kSize for relative cells, negative safe (INT32_MIN included).
    [[nodiscard]] static ChunkCoord chunk_of(i32 relative_x, i32 relative_y) noexcept;
    /// The 0..kSize-1 position of a relative cell inside its chunk, negative safe.
    [[nodiscard]] static i32 local_of(i32 relative_cell) noexcept;

    /// RELATIVE world space bounds, the same convention as TileMap::tile_bounds(): +Y down, top left
    /// corner plus size. The origin does not leak into these: a chunk at relative (0, 0) is always at
    /// relative world (0, 0), which is what keeps f32 drawing precise far from the absolute origin.
    [[nodiscard]] Aabb2 chunk_bounds(const ChunkCoord& relative) const;
    [[nodiscard]] Aabb2 cell_bounds(i32 cell_x, i32 cell_y) const;

private:
    [[nodiscard]] ChunkKey key_of(const ChunkCoord& relative) const;
    /// Drops a resident chunk through the unload rules (update()'s eviction pass). A dirty chunk whose
    /// save fails stays where it is.
    void evict_resident(const ChunkKey& key);
    /// Serialises and saves one dirty chunk under its absolute key. Returns false (and counts
    /// save_failures) when the saver rejects it; a missing saver is not a save failure.
    bool save_chunk(WorldChunk& chunk, const ChunkKey& key);
    /// Budget pressure: evicts the furthest clean resident chunk whose absolute key is not in wanted,
    /// or returns false when there is none to give up. The victim order is deterministic.
    bool make_room(const std::unordered_set<ChunkKey, ChunkKeyHash>& wanted, const ChunkCoord& centre);

    OpenWorldConfig config_{};
    ChunkKey origin_{};
    ChunkGenerator generator_;
    ChunkLoader loader_;
    ChunkSaver saver_;
    std::unordered_map<ChunkKey, WorldChunk, ChunkKeyHash> chunks_;
    Stats stats_{};
};

} // namespace t2d
