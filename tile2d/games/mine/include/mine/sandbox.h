// Mine - the single layer sandbox: a content debugger, not a game mode.
//
// The sandbox shows exactly one layer, with no demands and no progression, and it holds no content of
// its own. The palette is whatever the designer's data registered (docs/GAME_DESIGN.md section 7); with
// an empty registry the layer stays empty and says so. What it is for is the loop a designer actually
// runs - edit the data file, reload it, look at the result:
//
//   * the palette lists the registered content with the ids the registry handed out, so the names and
//     the numbers a save would store can both be read off the screen;
//   * a reload rebuilds the registry from the files and re-points every placed cell at its content *by
//     name*, so inserting an entry in the middle of a file - which shifts every id after it - cannot
//     silently turn one structure into another;
//   * content that disappeared is reported, and its cells are marked missing instead of being handed to
//     whatever now happens to hold that id.
//
// Nothing in this file is content: no resource, structure, recipe or machine is named here, and none
// ever will be.
#pragma once

#include <mine/registry.h>

#include <t2d/core/math2d.h>
#include <t2d/core/types.h>

#include <string>
#include <string_view>
#include <vector>

namespace mine {

using t2d::f32;
using t2d::i32;
using t2d::u32;
using t2d::u64;
using t2d::u8;
using t2d::usize;
using t2d::Vec2;

/// A cell coordinate. Signed, because a cursor can be dragged outside the layer and a camera can look
/// past its edge; only inside() cells exist in the grid.
struct GridPos {
    i32 x = 0;
    i32 y = 0;

    friend bool operator==(const GridPos&, const GridPos&) = default;
};

/// One cell of the sandbox layer. The name is kept beside the id on purpose: it is what makes the cell
/// re-pointable after the registry changed, and what lets the screen say "this was sample_a and it is
/// gone" instead of showing a bare 0.
struct SandboxCell {
    /// ContentKind::Count means the cell is empty.
    ContentKind kind = ContentKind::Count;
    /// The id the running registry handed out; kNoContent when the content is no longer registered.
    ContentId id = kNoContent;
    std::string name;

    [[nodiscard]] bool empty() const { return kind == ContentKind::Count; }
    [[nodiscard]] bool missing() const { return !empty() && id == kNoContent; }

    friend bool operator==(const SandboxCell&, const SandboxCell&) = default;
};

/// One selectable piece of registered content.
struct PaletteEntry {
    ContentKind kind = ContentKind::Item;
    ContentId id = kNoContent;
    std::string name;
};

/// The kinds a cell can hold. This is the engineering kind list of registry.h, not content: the kinds
/// whose instances occupy a tile. Items, recipes, layers and channels are referenced *by* cells and
/// never placed on one, so they stay out of the palette.
[[nodiscard]] bool is_placeable_kind(ContentKind kind);

/// A stable colour for a content name, packed the way ore::make_rgba packs (0xAABBGGRR). Art arrives
/// with the designer's data; until then a colour derived from the name is what makes a cell readable,
/// and deriving it from the name means a cell keeps its colour across reloads.
[[nodiscard]] u32 debug_color_for(std::string_view name);

/// What one reload of the content files did.
struct SandboxReloadReport {
    /// False when a file could not be read or parsed; the cells are then left exactly as they were.
    bool parsed = true;
    /// One line per failure, "path:line:column: message".
    std::vector<std::string> errors;
    /// Tables that are not named after a content kind (a typo in the data file, usually).
    std::vector<std::string> unknown_tables;

    usize added = 0;    ///< content the new files have and the old registry did not
    usize removed = 0;  ///< content that is gone
    usize kept = 0;     ///< content present before and after
    usize remapped_cells = 0; ///< placed cells whose id moved because the registry changed
    usize lost_cells = 0;     ///< placed cells whose content no longer exists
    usize palette_size = 0;

    [[nodiscard]] bool clean() const { return parsed && errors.empty() && unknown_tables.empty(); }
};

/// What loading a saved layout did.
struct SandboxLoadReport {
    bool ok = false;
    std::string error;      ///< why the file was refused
    usize translated = 0;   ///< cells whose saved id still exists, by name
    usize missing = 0;      ///< cells whose content the running registry does not have
    usize unknown_kind = 0; ///< cells whose saved kind is not one a cell can hold
};

/// The single layer sandbox: a grid, a palette built from the registry, a cursor and a camera. Pure
/// logic - no window, no GPU, no clock - so the whole model is testable in milliseconds.
class SandboxModel {
public:
    /// A layer larger than this is refused rather than allocated: a corrupt layout file must not be
    /// able to ask for a gigabyte.
    static constexpr u32 kMaxDimension = 1024;
    static constexpr u32 kDefaultWidth = 40;
    static constexpr u32 kDefaultHeight = 24;

    SandboxModel(u32 width = kDefaultWidth, u32 height = kDefaultHeight);

    // --- the layer ---
    [[nodiscard]] u32 width() const { return width_; }
    [[nodiscard]] u32 height() const { return height_; }
    [[nodiscard]] bool inside(GridPos pos) const;
    [[nodiscard]] const SandboxCell& cell(GridPos pos) const;
    void set_cell(GridPos pos, const SandboxCell& value);
    void clear_cells();
    [[nodiscard]] usize filled_cells() const;
    /// Resizes, keeping the cells that still fit.
    void resize(u32 width, u32 height);

    // --- the palette (the designer's data, as registered) ---
    /// Rebuilds the palette from \p registry and returns how many entries it holds. The selection is
    /// kept when it still points at the same content, and clamped otherwise.
    usize rebuild_palette(const ContentRegistry& registry);
    [[nodiscard]] usize palette_count() const { return palette_.size(); }
    [[nodiscard]] const PaletteEntry& palette(usize index) const;
    [[nodiscard]] usize selected_palette() const { return selected_; }
    void select_palette(usize index);
    /// Moves the selection, wrapping at both ends (the way a key press walks the list).
    void cycle_palette(i32 delta);
    /// First palette index that has to be drawn so that the selection is inside a window of \p rows.
    [[nodiscard]] usize palette_window_start(usize rows) const;
    [[nodiscard]] const PaletteEntry* selected_entry() const;

    // --- the cursor and the brush ---
    [[nodiscard]] GridPos cursor() const { return cursor_; }
    void set_cursor(GridPos pos);
    /// Moves the cursor, clamped to the layer: the cursor always points at a cell that exists.
    void move_cursor(i32 dx, i32 dy);
    /// Places the selected palette entry at the cursor; false when there is nothing to place.
    bool paint();
    /// Empties the cursor cell; false when it was already empty.
    bool erase();

    // --- the camera (screen space) ---
    [[nodiscard]] f32 cell_px() const { return cell_px_; }
    void set_cell_px(f32 pixels);
    [[nodiscard]] Vec2 origin() const { return origin_; }
    void set_origin(Vec2 origin) { origin_ = origin; }
    void pan(Vec2 delta) { origin_.x += delta.x; origin_.y += delta.y; }
    /// Zooms by \p factor while keeping the point under \p anchor on screen: the reason to zoom is to
    /// look closer at the cell you are already looking at.
    void zoom_at(Vec2 anchor, f32 factor);
    [[nodiscard]] GridPos cell_at_screen(Vec2 point) const;
    [[nodiscard]] Vec2 screen_of_cell(GridPos cell) const;
    /// Centres the whole layer in a viewport of \p viewport pixels.
    void center_view(Vec2 viewport);
    /// Scrolls the least amount that brings \p cell inside a viewport, keeping a margin of one cell.
    void scroll_to_show(GridPos cell, Vec2 viewport);

    // --- content-agnostic debug fills ---
    /// One horizontal band per palette entry, filled with it: a legible catalogue of what is registered.
    void fill_bands();
    /// A deterministic scatter of palette entries, from \p seed: what a layer of this content could
    /// look like. Same seed, same layer.
    void fill_scatter(u64 seed, u32 percent = 25);

    // --- the designer's files ---
    /// Re-reads the content files into \p registry and re-points every cell by name. The registry is
    /// cleared first, so content that was deleted from a file really disappears and the ids in force
    /// are the ones a fresh registration hands out. On a parse failure nothing is touched.
    SandboxReloadReport reload(ContentRegistry& registry, const std::vector<std::string>& paths);
    /// The same, over texts already in memory (\p names are used in the error messages).
    SandboxReloadReport reload_texts(ContentRegistry& registry, const std::vector<std::string>& texts,
                                     const std::vector<std::string>& names);
    [[nodiscard]] const std::vector<std::string>& content_paths() const { return content_paths_; }
    void set_content_paths(std::vector<std::string> paths) { content_paths_ = std::move(paths); }

    /// The layer as a save would store it: the cells as ids, plus the name -> id table in force, so a
    /// later version can translate them back by name (registry.h).
    [[nodiscard]] std::vector<u8> serialize(const ContentRegistry& registry) const;
    /// Loads a saved layout and translates its ids into the running registry. A file it cannot fully
    /// trust is refused and the layer is left untouched; a file that loads rebuilds the palette from
    /// \p registry, so the layer is ready to paint on.
    SandboxLoadReport deserialize(t2d::ConstSpan<const u8> data, const ContentRegistry& registry);

    /// A readable listing of the layer: coordinates, kind, id and name per placed cell, plus the
    /// palette. This is what goes into a bug report.
    [[nodiscard]] std::string dump_text() const;

private:
    [[nodiscard]] usize index(GridPos pos) const { return static_cast<usize>(pos.y) * width_ + static_cast<usize>(pos.x); }

    u32 width_ = kDefaultWidth;
    u32 height_ = kDefaultHeight;
    std::vector<SandboxCell> cells_;
    std::vector<PaletteEntry> palette_;
    usize selected_ = 0;
    GridPos cursor_{};
    Vec2 origin_{};
    f32 cell_px_ = 24.0f;
    std::vector<std::string> content_paths_;
};

} // namespace mine
