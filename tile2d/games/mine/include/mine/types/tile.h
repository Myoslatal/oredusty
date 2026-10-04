// Mine - what every plot of a mine has in common.
//
// A **plot** (地块) is a fixed thing that occupies cells of one layer: a floor, an ore vein, a machine.
// It is placed where it is and does not move. A plot is often one cell, but not always - a big
// structure is 2x2, 3x3 or 2x3 (docs/GAME_DESIGN.md section 1.12) - so a plot's footprint is a
// rectangle of cells whose top left corner is the plot's anchor. Nothing here assumes one cell.
//
// What a plot *is* - which content it stands for - is the registry's business (mine/registry.h): a
// plot holds the kind and the id a save stores, never a name. What it *does* is in the two kinds of
// plot below (types/scene_tile.h, types/entity_tile.h), which exist because of how often it has to be
// done.
//
// This class is the base of that family: identity, footprint, and whether the content behind it is
// still there. It has no constructor a caller can reach, because a plot is always one of the two
// kinds - there is no such thing as a plot that is neither scenery nor a machine.
#pragma once

#include <mine/content_grid.h>
#include <mine/registry.h>

#include <t2d/core/math2d.h>

#include <string>

namespace mine::types {

using t2d::i32;
using t2d::TileRect;

/// The cells a plot of \p width x \p height anchored at \p anchor covers. A plot always covers at
/// least the cell it is anchored at, so a size below one cell is read as one cell rather than as a
/// plot that is nowhere.
[[nodiscard]] constexpr TileRect footprint(GridPos anchor, i32 width = 1, i32 height = 1) {
    return TileRect{anchor.x, anchor.y, width < 1 ? 1 : width, height < 1 ? 1 : height};
}

/// The base class of everything that occupies cells of a mine.
class Tile {
public:
    virtual ~Tile() = default;

    // --- what it is -------------------------------------------------------------------------------
    [[nodiscard]] ContentKind kind() const { return kind_; }
    /// The id the table in force handed out. Kept even when the content is gone, so content that comes
    /// back repairs the plot by name instead of losing it.
    [[nodiscard]] ContentId id() const { return id_; }
    /// The name the registry has for this plot's content, or "#<id>" when the content is gone - the
    /// same text the sandbox shows for a cell whose content disappeared.
    [[nodiscard]] std::string name(const ContentRegistry& registry) const;
    /// The content behind the plot is not in the registry any more (a reload can take it away).
    [[nodiscard]] bool missing() const { return missing_; }
    void set_missing(bool missing) { missing_ = missing; }
    /// There is content behind it and that content is here.
    [[nodiscard]] bool valid() const { return id_ != kNoContent && !missing_; }

    // --- where it is ------------------------------------------------------------------------------
    /// The tile layer it was placed on. What a layer means is the designer's business; the map only
    /// numbers them from zero.
    [[nodiscard]] i32 layer() const { return layer_; }
    /// The cells it covers, half open like every rectangle in the framework.
    [[nodiscard]] const TileRect& cells() const { return cells_; }
    /// The cell its footprint starts at, top left.
    [[nodiscard]] GridPos anchor() const { return GridPos{cells_.x, cells_.y}; }
    [[nodiscard]] i32 width() const { return cells_.width; }
    [[nodiscard]] i32 height() const { return cells_.height; }
    [[nodiscard]] i32 cell_count() const { return cells_.width * cells_.height; }
    [[nodiscard]] bool covers(GridPos cell) const { return cells_.contains(cell.x, cell.y); }
    /// True when the two plots are on the same layer and share at least one cell. Two 3x3 structures
    /// overlap; a structure and the floor under it do not, because they are on different layers.
    [[nodiscard]] bool overlaps(const Tile& other) const;
    /// True when every cell of the footprint is inside a map of \p map_width x \p map_height cells:
    /// what a placement check asks before a 2x2 is put down near the edge.
    [[nodiscard]] bool within(i32 map_width, i32 map_height) const;

protected:
    /// A plot is constructed where it is: it has no place until it has one. \p width and \p height
    /// are the footprint in cells (at least one).
    Tile(ContentKind kind, ContentId id, i32 layer, GridPos anchor, i32 width = 1, i32 height = 1);

private:
    ContentKind kind_ = ContentKind::Item;
    ContentId id_ = kNoContent;
    i32 layer_ = 0;
    TileRect cells_{};
    bool missing_ = false;
};

} // namespace mine::types
