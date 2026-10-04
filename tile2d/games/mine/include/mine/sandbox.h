// Mine - the single layer sandbox: a content debugger, not a game mode.
//
// The sandbox shows one map, with no demands and no progression, and it holds no content of its own.
// The palette is whatever the designer's data registered (docs/GAME_DESIGN.md section 7); with an
// empty registry the map stays empty and says so. What it is for is the loop a designer actually
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
// The map has one or more *tile layers* (the same concept the framework's TileMap has): what a layer
// means is the designer's business, and the sandbox only numbers them. Painting writes into the active
// layer, erasing removes what is on top, and the screen draws the stack bottom to top so a ground
// layer, an ore layer and a structures layer can be told apart.
//
// The storage is the one the game itself will use (content_grid.h): four bytes per cell, so a map of
// hundreds of cells per side is cheap, and no name per cell, so a reload has to go through the same
// name -> id translation a save does. The view is the framework's camera (t2d/core/camera2d.h): the
// sandbox is a god view like the game is - there is no player character anywhere in this file.
//
// Nothing in this file is content: no resource, structure, recipe or machine is named here, and none
// ever will be.
#pragma once

#include <mine/content_grid.h>
#include <mine/registry.h>

#include <t2d/core/camera2d.h>
#include <t2d/core/math2d.h>
#include <t2d/core/types.h>

#include <optional>
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

/// One cell as a reader sees it: the stored reference plus the name the table in force gives it. The
/// name is a view into that table, so it stays valid for as long as the registry is not reloaded.
struct CellView {
    ContentKind kind = ContentKind::Count;
    ContentId id = kNoContent;
    bool stale = false;
    std::string_view name{};

    [[nodiscard]] bool empty() const { return kind == ContentKind::Count; }
    [[nodiscard]] bool missing() const { return !empty() && stale; }
    [[nodiscard]] ContentRef ref() const { return ContentRef{kind, id, stale}; }
    /// The id a reader should be shown: kNoContent once the content is gone. The cell keeps the number
    /// it was placed with internally - that is what lets a returning entry repair it - but that number
    /// may have been handed to something else since, so showing it would be a lie.
    [[nodiscard]] ContentId shown_id() const { return missing() ? kNoContent : id; }

    friend bool operator==(const CellView&, const CellView&) = default;
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

/// How many cells one drawn quad should cover so that \p visible_cells cells on every one of
/// \p layers layers fit inside \p max_quads: 1 while the map is small enough to be drawn cell by
/// cell, larger when it is not. A map of hundreds of cells per side at a zoom that shows all of it
/// is a million cells; one quad per cell would be a million quads for a screen of pixels, so the
/// frame draws a sample instead - one quad per block of cells, named by the cell in its middle.
[[nodiscard]] i32 draw_step_for(usize visible_cells, i32 layers, usize max_quads);

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

/// One map with several tile layers: a grid per layer, a palette built from the registry, a cursor, a
/// camera and an active layer. Pure logic - no window, no GPU, no clock - so the whole model is
/// testable in milliseconds.
class SandboxModel {
public:
    /// A map larger than this is refused rather than allocated: a corrupt layout file must not be able
    /// to ask for a gigabyte.
    static constexpr u32 kMaxDimension = 1024;
    static constexpr u32 kDefaultWidth = 40;
    static constexpr u32 kDefaultHeight = 24;
    /// Mirrors TileMap::kMaxLayers: how many tile layers a sandbox map can hold.
    static constexpr i32 kMaxLayers = ContentGrid::kMaxLayers;
    /// The zoom range of the camera, in pixels per cell. The low end is what makes a map of hundreds
    /// of cells per side visible as a whole: at 0.05 pixels per cell a 4096 cell map is 205 pixels
    /// wide, and the frame draws it as a sampled overview instead of one quad per cell.
    static constexpr f32 kMinCellPx = 0.05f;
    static constexpr f32 kMaxCellPx = 512.0f;

    SandboxModel(u32 width = kDefaultWidth, u32 height = kDefaultHeight, i32 layers = 1);

    // --- the map ---
    [[nodiscard]] u32 width() const { return cells_.width(); }
    [[nodiscard]] u32 height() const { return cells_.height(); }
    [[nodiscard]] i32 layer_count() const { return cells_.layer_count(); }
    [[nodiscard]] bool inside(GridPos pos) const { return cells_.inside(pos); }
    /// The topmost non-empty cell at \p pos: what the inspector shows, and what erase() removes.
    [[nodiscard]] CellView cell(GridPos pos) const;
    [[nodiscard]] CellView cell(i32 layer, GridPos pos) const;
    /// Writes into the active layer.
    void set_cell(GridPos pos, ContentRef value);
    void set_cell(i32 layer, GridPos pos, ContentRef value);
    void clear_cells();                                   ///< every layer
    void clear_layer(i32 layer);
    [[nodiscard]] usize filled_cells() const;             ///< every layer, O(1)
    [[nodiscard]] usize filled_cells(i32 layer) const;
    /// Bytes the cells occupy: what a map of this size costs, for the status line.
    [[nodiscard]] usize cell_bytes() const { return cells_.bytes(); }
    /// Resizes, keeping the cells of every layer that still fit.
    void resize(u32 width, u32 height);

    // --- the active layer (where the brush writes) ---
    [[nodiscard]] i32 active_layer() const { return active_layer_; }
    void set_active_layer(i32 layer);
    /// Moves the active layer, wrapping at both ends (the way a key press walks the stack).
    void cycle_layer(i32 delta);

    // --- the palette (the designer's data, as registered) ---
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
    /// Moves the cursor, clamped to the map: the cursor always points at a cell that exists.
    void move_cursor(i32 dx, i32 dy);
    /// Places the selected palette entry at the cursor, on the active layer; false when there is
    /// nothing to place or the cell already holds it.
    bool paint();
    /// Empties the topmost non-empty cell at the cursor; false when it is already empty everywhere.
    bool erase();

    // --- the camera (screen space, through the framework's camera) ---
    [[nodiscard]] const t2d::Camera2D& camera() const { return camera_; }
    [[nodiscard]] f32 cell_px() const { return camera_.zoom(); }
    void set_cell_px(f32 pixels);
    [[nodiscard]] Vec2 origin() const { return camera_.screen_of(Vec2{0.0f, 0.0f}); }
    void set_origin(Vec2 origin);
    void pan(Vec2 delta) { camera_.pan(delta); }
    /// Zooms by \p factor while keeping the point under \p anchor on screen: the reason to zoom is to
    /// look closer at the cell you are already looking at.
    void zoom_at(Vec2 anchor, f32 factor) { camera_.zoom_at(anchor, factor); }
    [[nodiscard]] GridPos cell_at_screen(Vec2 point) const;
    [[nodiscard]] Vec2 screen_of_cell(GridPos cell) const;
    /// The cells the viewport touches, half open: what a renderer walks instead of the whole map.
    [[nodiscard]] t2d::TileRect visible_cells() const { return camera_.visible_cells(); }
    /// Centres the whole map in a viewport of \p viewport pixels.
    void center_view(Vec2 viewport);
    /// Puts \p cell in the middle of the view at \p pixels_per_cell (0 keeps the current zoom): a map
    /// of hundreds of cells per side does not fit a window at a readable zoom, so a caller has to be
    /// able to say where to look.
    void look_at(GridPos cell, f32 pixels_per_cell = 0.0f);
    /// Scrolls the least amount that brings \p cell inside a viewport, keeping a margin of one cell.
    void scroll_to_show(GridPos cell, Vec2 viewport);
    /// Changes the viewport without moving the view: the world point at the centre stays there and the
    /// new space simply shows more of the map. Going from the editor's chrome to the playtest's full
    /// window is a viewport change, not a camera reset.
    void set_viewport(Vec2 size) { camera_.set_viewport(size); }

    // --- the playtest pointer (the game's pointer, not the editor's cursor) ---
    /// Points at \p screen and remembers the cell under it; a pointer outside the map remembers
    /// nothing, because the game's pointer selects what is under it and there is nothing there
    /// (docs/GAME_DESIGN.md section 1.11). The camera can move under a still pointer, so a caller
    /// points again every frame it draws rather than only when the mouse moves.
    void point_at(Vec2 screen);
    /// Points at \p cell directly: what a scripted run needs, because a headless one has no mouse to
    /// move. A cell outside the map leaves the pointer pointing at nothing, like a real one would.
    void point_at_cell(GridPos cell);
    void clear_pointer() { hovered_.reset(); }
    [[nodiscard]] std::optional<GridPos> hovered() const { return hovered_; }

    // --- content-agnostic debug fills (they fill the active layer) ---
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
    /// Adopts \p registry as the one in force: every placed cell is re-pointed at its content by name,
    /// the palette is rebuilt from it, and its name table becomes the one the cells refer to. A caller
    /// that fills the registry from more than one source (the game's own content, then its mods) calls
    /// this once, after the last source, so a single pass sees the whole picture.
    SandboxReloadReport rebind(const ContentRegistry& registry);
    [[nodiscard]] const std::vector<std::string>& content_paths() const { return content_paths_; }
    void set_content_paths(std::vector<std::string> paths) { content_paths_ = std::move(paths); }

    /// The map as a save would store it: every layer's cells as ids, plus the name -> id table in
    /// force, so a later version can translate them back by name (registry.h).
    [[nodiscard]] std::vector<u8> serialize() const;
    /// Loads a saved layout and translates its ids into the running registry. A file it cannot fully
    /// trust is refused and the map is left untouched; a file that loads rebuilds the palette from
    /// \p registry, so the map is ready to paint on.
    SandboxLoadReport deserialize(t2d::ConstSpan<const u8> data, const ContentRegistry& registry);

    /// A readable listing of the map: coordinates, layer, kind, id and name per placed cell, plus the
    /// palette. This is what goes into a bug report.
    [[nodiscard]] std::string dump_text() const;

private:
    /// Rebuilds the palette from \p registry. Called by rebind() and deserialize(), which are the two
    /// places that know the ids in the cells have just been re-pointed.
    usize rebuild_palette(const ContentRegistry& registry);
    /// The name the table in force gives \p ref, or an empty view when nothing names it.
    [[nodiscard]] std::string_view name_of(ContentRef ref) const;
    /// Remembers the name of a cell whose content went missing, so the screen can still say what used
    /// to be there after the registry dropped it.
    void remember_missing(ContentKind kind, ContentId id, std::string_view name);
    /// The table a save of this map carries: the one in force, plus the names of what went missing.
    [[nodiscard]] ContentTable save_table() const;

    ContentGrid cells_{};
    /// The name -> id table the placed cells refer to. A cell stores no name of its own (that is what
    /// makes it four bytes), so this is what turns its id back into a name - and what makes a reload
    /// able to re-point it after ids moved.
    ContentTable table_{};
    /// Names of content that went missing, kept so a lost cell can still be reported by name.
    ContentTable retired_{};
    std::vector<PaletteEntry> palette_;
    usize selected_ = 0;
    /// Which layer the brush writes into. What a layer means is the designer's business.
    i32 active_layer_ = 0;
    GridPos cursor_{};
    /// What the playtest's pointer is over, if anything. Not the cursor: the cursor is the editor's
    /// brush, this is the game's pointer, and the two never move each other.
    std::optional<GridPos> hovered_{};
    t2d::Camera2D camera_{};
    std::vector<std::string> content_paths_;
};

} // namespace mine
