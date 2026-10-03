// Mine - the map grid: what content sits on every cell of a map.
//
// The game is built for maps of hundreds of cells per side (docs/GAME_DESIGN.md section 2), so a cell
// is packed rather than convenient: 32 bits, four bytes, one cell. A 512x512 layer is then 1 MiB, and
// a map of a hundred such layers still fits in memory; a std::string per cell - the shape the sandbox
// started with - is 40 bytes and one heap allocation per painted cell.
//
// A cell stores *what*, never *which name*: names live in the registry, and in the table a save
// carries (registry.h). That is also what lets a reload re-point a cell by name after ids moved.
//
// Layers are allocated on first write, so a map that uses one of its eight layers pays for one. What
// a layer means is the designer's business; the grid only numbers them from 0 upwards, the same way
// the framework's TileMap does.
#pragma once

#include <mine/registry.h>

#include <t2d/core/types.h>

#include <string_view>
#include <vector>

namespace mine {

using t2d::i32;
using t2d::u32;
using t2d::usize;

/// A cell coordinate. Signed, because a cursor can be dragged outside the map and a camera can look
/// past its edge; only inside() cells exist in the grid.
struct GridPos {
    i32 x = 0;
    i32 y = 0;

    friend bool operator==(const GridPos&, const GridPos&) = default;
};

/// Bits of one packed cell: the id in the low bits, then the kind, then the "gone" flag. The id field
/// is exactly as wide as registry.h promises ids stay (kMaxContentId), and the kind field has room
/// for the kinds that do not exist yet.
inline constexpr u32 kContentRefIdBits = 20;
inline constexpr u32 kContentRefKindShift = kContentRefIdBits;
inline constexpr u32 kContentRefStaleShift = kContentRefIdBits + 4;
inline constexpr u32 kContentRefIdMask = (1u << kContentRefIdBits) - 1u;
static_assert(kMaxContentId <= (1u << kContentRefIdBits), "a content id must fit the packed cell");
static_assert(kContentKindCount <= 16, "a content kind must fit the packed cell");

/// One cell: which piece of content sits here, in four bytes.
struct ContentRef {
    /// ContentKind::Count means the cell is empty.
    ContentKind kind = ContentKind::Count;
    /// The id the table in force handed out; kNoContent when nothing is there.
    ContentId id = kNoContent;
    /// The id is one the table in force no longer resolves: the content is gone, the cell is not. The
    /// id is kept, so a piece of content that comes back re-points its cells instead of losing them.
    bool stale = false;

    [[nodiscard]] constexpr bool empty() const { return kind == ContentKind::Count; }
    [[nodiscard]] constexpr bool missing() const { return !empty() && stale; }

    [[nodiscard]] constexpr u32 packed() const {
        if (empty()) return 0;
        return (id & kContentRefIdMask) | (static_cast<u32>(kind) << kContentRefKindShift) |
               (stale ? (1u << kContentRefStaleShift) : 0u);
    }
    [[nodiscard]] static constexpr ContentRef unpack(u32 bits) {
        if (bits == 0) return ContentRef{};
        ContentRef ref;
        ref.kind = static_cast<ContentKind>((bits >> kContentRefKindShift) & 0xFu);
        ref.id = bits & kContentRefIdMask;
        ref.stale = (bits & (1u << kContentRefStaleShift)) != 0;
        return ref;
    }

    friend constexpr bool operator==(const ContentRef&, const ContentRef&) = default;
};

/// A grid of cells over one or more tile layers: the storage a map is made of.
class ContentGrid {
public:
    /// A map larger than this per side is refused rather than allocated: a corrupt layout file must not
    /// be able to ask for a gigabyte. 4096x4096 is 64 MiB per written layer.
    static constexpr u32 kMaxDimension = 4096;
    /// Mirrors TileMap::kMaxLayers: how many tile layers one map can hold.
    static constexpr i32 kMaxLayers = 32;

    ContentGrid() = default;
    /// \p width, \p height and \p layers are clamped into range; nothing is allocated until a cell
    /// is written to.
    ContentGrid(u32 width, u32 height, i32 layers = 1);

    [[nodiscard]] u32 width() const { return width_; }
    [[nodiscard]] u32 height() const { return height_; }
    [[nodiscard]] i32 layer_count() const { return layer_count_; }
    [[nodiscard]] usize cell_count() const { return static_cast<usize>(width_) * height_; }
    [[nodiscard]] bool inside(GridPos pos) const {
        return pos.x >= 0 && pos.y >= 0 && static_cast<u32>(pos.x) < width_ && static_cast<u32>(pos.y) < height_;
    }

    /// The cell at \p pos in \p layer; empty for anything outside the grid or the layer range.
    [[nodiscard]] ContentRef at(i32 layer, GridPos pos) const;
    void set(i32 layer, GridPos pos, ContentRef value);
    /// Empties one layer, or every layer. Storage is released: a cleared layer costs nothing until it
    /// is painted again.
    void clear_layer(i32 layer);
    void clear();
    /// Keeps the cells that still fit: growing adds empty cells, shrinking drops what falls outside.
    void resize(u32 width, u32 height);

    /// How many cells hold content - O(1), not a walk: the status line asks for this every frame, and
    /// a 512x512x8 map is two million cells.
    [[nodiscard]] usize filled() const;
    [[nodiscard]] usize filled(i32 layer) const;
    /// Whether \p layer has storage yet: a layer nobody painted on has none.
    [[nodiscard]] bool allocated(i32 layer) const;
    /// Bytes the cells actually occupy, for a status line or a bug report.
    [[nodiscard]] usize bytes() const;

    /// Visits every cell of \p layer in row major order, with a mutable reference: this is how a
    /// remap, a save and a dump walk a layer without a bounds check per cell. A layer nobody wrote to
    /// is not visited at all, and the visitor is free to write through the reference.
    template <typename Visitor>
    void for_each(i32 layer, Visitor&& visit) {
        if (layer < 0 || layer >= layer_count_) return;
        std::vector<u32>& cells = layers_[static_cast<usize>(layer)];
        if (cells.empty()) return;
        for (usize index = 0; index < cells.size(); ++index) {
            ContentRef ref = ContentRef::unpack(cells[index]);
            visit(GridPos{static_cast<i32>(index % width_), static_cast<i32>(index / width_)}, ref);
            cells[index] = ref.packed();
        }
    }

    /// The same walk over a grid that is not being changed (a save, a dump, a report).
    template <typename Visitor>
    void for_each(i32 layer, Visitor&& visit) const {
        if (layer < 0 || layer >= layer_count_) return;
        const std::vector<u32>& cells = layers_[static_cast<usize>(layer)];
        if (cells.empty()) return;
        for (usize index = 0; index < cells.size(); ++index) {
            visit(GridPos{static_cast<i32>(index % width_), static_cast<i32>(index / width_)},
                  ContentRef::unpack(cells[index]));
        }
    }

private:
    [[nodiscard]] usize index_of(GridPos pos) const {
        return static_cast<usize>(pos.y) * width_ + static_cast<usize>(pos.x);
    }
    /// Allocates \p layer if it has no storage yet; false when the layer is out of range.
    [[nodiscard]] bool reserve_layer(i32 layer);

    u32 width_ = 1;
    u32 height_ = 1;
    i32 layer_count_ = 1;
    /// One vector per layer, allocated on first write: layer major, row major inside a layer.
    std::vector<std::vector<u32>> layers_{};
    std::vector<usize> filled_{};
};

} // namespace mine
