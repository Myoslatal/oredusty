// Mine - a plot, as data.
//
// A plot is a fixed thing that occupies cells of one layer: a floor, an ore vein, a machine, a
// warehouse. It has the core fields every plot shares - content identity, footprint, layer, mirroring,
// the definition it was made from - and it owns one runtime ObjectData: the bytes of the components its
// tile type is made of. What a plot *does* is in the component function tables; this class only asks
// them. There is no vtable and no concrete Floor/Warehouse/Drill class: the tile type says what a plot
// is made of, and an interface query says what it can do.
#pragma once

#include <mine/content_grid.h>
#include <mine/registry.h>

#include <mine/runtime/content/components.h>

#include <mine/types/click.h>
#include <mine/types/container.h>
#include <mine/types/task.h>

#include <t2d/core/math2d.h>
#include <t2d/core/rng.h>
#include <t2d/core/types.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mine {

class MineLayer;

namespace runtime {
class ObjectData;
class TileType;
} // namespace runtime

namespace types {

class TileRuntimeLibrary;
struct RuntimeTileDefinition;
struct TileDefinition;
struct Building;

using t2d::f32;
using t2d::i32;
using t2d::TileRect;
using t2d::usize;

/// One picture of a plot's complete render preview: which part it is, whether it is drawn mirrored, how
/// far it is turned and about which point of itself. Kept as the screen's vocabulary; the component
/// table answers in C types and get_preview() converts.
struct PreviewImage {
    /// Empty is the entry's own "image"; "top" is the part a drill turns over its base.
    std::string part;
    bool mirrored = false;
    f32 radians = 0.0f;
    t2d::Vec2 pivot{0.5f, 0.5f};
};

struct TilePreview {
    std::vector<PreviewImage> images;

    [[nodiscard]] bool empty() const { return images.empty(); }
};

/// The cells a plot of \p width x \p height anchored at \p anchor covers. A plot always covers at least
/// the cell it is anchored at.
[[nodiscard]] constexpr TileRect footprint(GridPos anchor, i32 width = 1, i32 height = 1) {
    return TileRect{anchor.x, anchor.y, width < 1 ? 1 : width, height < 1 ? 1 : height};
}

/// A plot: core data plus the runtime object its components live in.
class Tile {
public:
    Tile(std::shared_ptr<const TileRuntimeLibrary> library,
         std::shared_ptr<const RuntimeTileDefinition> runtime_definition, ContentId id, i32 layer,
         GridPos anchor, i32 width = 1, i32 height = 1);
    ~Tile();

    Tile(const Tile&) = delete;
    Tile& operator=(const Tile&) = delete;
    Tile(Tile&&) noexcept = default;
    Tile& operator=(Tile&&) noexcept = default;

    // --- what it is -------------------------------------------------------------------------------
    [[nodiscard]] ContentKind kind() const { return kind_; }
    [[nodiscard]] ContentId id() const { return id_; }
    [[nodiscard]] std::string name(const ContentRegistry& registry) const;
    [[nodiscard]] bool is_floor() const { return kind_ == ContentKind::Floor; }
    [[nodiscard]] bool is_ore() const { return kind_ == ContentKind::Ore; }
    [[nodiscard]] bool missing() const { return missing_; }
    void set_missing(bool missing) { missing_ = missing; }
    [[nodiscard]] bool valid() const { return id_ != kNoContent && !missing_; }
    /// The definition this plot was made from. Owned by the runtime library the plot holds, so it
    /// survives a content reload.
    [[nodiscard]] const TileDefinition* definition() const;
    [[nodiscard]] const Building* building() const;
    /// The tile type the plot was built from: the layout every plot of this content shares.
    [[nodiscard]] const runtime::TileType* tile_type() const { return tile_type_; }

    // --- where it is ------------------------------------------------------------------------------
    [[nodiscard]] i32 layer() const { return layer_; }
    [[nodiscard]] const TileRect& cells() const { return cells_; }
    [[nodiscard]] GridPos anchor() const { return GridPos{cells_.x, cells_.y}; }
    [[nodiscard]] i32 width() const { return cells_.width; }
    [[nodiscard]] i32 height() const { return cells_.height; }
    [[nodiscard]] i32 cell_count() const { return cells_.width * cells_.height; }
    [[nodiscard]] bool covers(GridPos cell) const { return cells_.contains(cell.x, cell.y); }
    [[nodiscard]] bool overlaps(const Tile& other) const;
    [[nodiscard]] bool within(i32 map_width, i32 map_height) const;
    /// The layer this plot is in, or nullptr while it is in none.
    [[nodiscard]] MineLayer* owner() const { return owner_; }

    // --- how it draws itself ----------------------------------------------------------------------
    [[nodiscard]] bool random_reverse() const { return random_reverse_; }
    void set_random_reverse(bool value) { random_reverse_ = value; }
    [[nodiscard]] bool mirrored() const { return mirrored_; }
    void set_mirrored(bool value) { mirrored_ = value; }
    void randomise_mirror(t2d::Rng& rng);
    /// The complete preview, from the PreviewFunctions table. \p seconds is the world clock: only a
    /// turning top reads it.
    [[nodiscard]] TilePreview get_preview(f32 seconds = 0.0f) const;

    // --- runtime object ---------------------------------------------------------------------------
    [[nodiscard]] bool has_component(runtime::ComponentTypeId type_id) const;
    [[nodiscard]] const void* instance(runtime::ComponentTypeId type_id) const;
    [[nodiscard]] const void* shared(runtime::ComponentTypeId type_id) const;
    /// The active table of an interface, or nullptr. Objects cache nothing: an override installed
    /// before the query is what this answers with.
    [[nodiscard]] const void* interface(runtime::ComponentTypeId type_id,
                                        runtime::InterfaceId interface_id) const;
    [[nodiscard]] runtime::ObjectData* object() { return object_.get(); }
    [[nodiscard]] const runtime::ObjectData* object() const { return object_.get(); }

    // --- container / click ------------------------------------------------------------------------
    [[nodiscard]] bool has_container() const;
    [[nodiscard]] Container container() const;
    [[nodiscard]] usize capacity() const;
    [[nodiscard]] bool holds(GridPos cell) const { return covers(cell); }
    [[nodiscard]] bool has_click() const;
    [[nodiscard]] Click click() const;
    /// A click arrived at \p cell. A plot still going up takes none, and closes a panel it had open:
    /// it is not finished, so it is not something the player has.
    bool on_click(GridPos cell);

    // --- drag -------------------------------------------------------------------------------------
    [[nodiscard]] bool has_drag() const;
    [[nodiscard]] bool drag_accepts(ContentKind kind, ContentId id, usize count) const;
    /// Takes a drop in whole or not at all.
    bool drag_receive(ContentKind kind, ContentId id, usize count);

    // --- the passes -------------------------------------------------------------------------------
    [[nodiscard]] bool dirty() const { return dirty_; }
    void mark_dirty() { dirty_ = true; }
    /// Brings the plot up to date if it is dirty and not still being built. There is no virtual hook:
    /// the per-content refresh logic is a component table when content needs it.
    bool refresh();
    [[nodiscard]] bool under_construction() const;
    void set_under_construction(bool value);

    // --- ticker -----------------------------------------------------------------------------------
    [[nodiscard]] bool is_ticking() const;
    [[nodiscard]] f32 period_seconds() const;
    void set_period_seconds(f32 seconds);
    [[nodiscard]] f32 since_last_update() const;
    void set_since_last_update(f32 seconds);
    /// Hands the ticker the frame's time. The optional is the span this run accounts for: one run per
    /// call, with the whole time that had piled up, so a long frame is not replayed and a drill can
    /// produce what the span is worth.
    [[nodiscard]] std::optional<f32> advance(f32 delta_seconds);

    // --- drill ------------------------------------------------------------------------------------
    [[nodiscard]] bool is_drill() const;
    [[nodiscard]] bool ready() const;
    [[nodiscard]] f32 dig_power() const;
    [[nodiscard]] runtime::content::DrillJob job() const;
    void set_job(runtime::content::DrillJob job);
    [[nodiscard]] usize produced() const;
    void set_produced(usize count);
    [[nodiscard]] f32 spin_seconds() const;
    [[nodiscard]] f32 spin_angle(f32 seconds) const;
    [[nodiscard]] t2d::Vec2 spin_origin() const;
    /// Accounts for \p elapsed_seconds of mining; returns how many drops it threw.
    usize drill_tick(f32 elapsed_seconds);
    [[nodiscard]] t2d::u64 drill_rng() const;
    [[nodiscard]] t2d::u32 drill_side() const;

    // --- task -------------------------------------------------------------------------------------
    /// True when the plot carries the task component. A plot whose content declares no tasks still answers
    /// true when its tile type says it has one: an empty chain is a chain, and every question below says
    /// so (docs/GAME_DESIGN.md section 1.52).
    [[nodiscard]] bool has_task() const;
    /// The plot's task chain: a view over its own progress and the content's shared steps. A plot with no
    /// task component answers an invalid view (count/current 0, no step).
    [[nodiscard]] Task task() const;
    /// Delivers whatever the container holds toward the current step, as far as it goes, and consumes it.
    /// Returns how many steps this call completed. Every path that can put items in the container ends
    /// with it: a drag, a click, a placement's starting stock, a load.
    usize run_tasks();
    /// Restores how many steps a save says are done (docs/SAVE.md). Returns false when the plot has no
    /// task component; a value past the chain's length is clamped to it.
    bool set_task_completed(usize completed);
    /// Remembers that \p count of \p kind / \p id just arrived, at the layer's clock, for a **rate**
    /// step's sliding window. A deliver step records nothing: its progress is what its container holds.
    /// Callers that put items into the container outside drag_receive (a placement's starting stock, a
    /// load) note them the same way, so a rate window counts every way the items got there.
    void note_delivery(ContentKind kind, ContentId id, usize count);
    /// Hands the frame's seconds to the plot's task chain: a satisfied **rate** step's hold grows, an
    /// unsatisfied one resets it (docs/GAME_DESIGN.md section 1.54). The layer's tick pass calls this;
    /// nothing else has to.
    void task_tick(f32 delta_seconds);
    /// How many steps are done, as the plot's own data says (before the Task view's clamp).
    [[nodiscard]] usize task_completed() const;

private:
    friend class mine::MineLayer;

    [[nodiscard]] const runtime::content::ContainerFunctions* container_functions() const;
    [[nodiscard]] const runtime::content::PreviewFunctions* preview_functions() const;
    [[nodiscard]] const runtime::content::ClickFunctions* click_functions() const;
    [[nodiscard]] const runtime::content::DragFunctions* drag_functions() const;
    [[nodiscard]] const runtime::content::BuildingFunctions* building_functions() const;
    [[nodiscard]] const runtime::content::TickerFunctions* ticker_functions() const;
    [[nodiscard]] const runtime::content::DrillFunctions* drill_functions() const;
    [[nodiscard]] const runtime::content::TaskFunctions* task_functions() const;
    /// The layer's clock in seconds, or 0 for a plot in no layer: what a task window is measured against.
    [[nodiscard]] f32 now_seconds() const;

    std::shared_ptr<const TileRuntimeLibrary> library_;
    std::shared_ptr<const RuntimeTileDefinition> runtime_definition_;
    const runtime::TileType* tile_type_ = nullptr;
    std::unique_ptr<runtime::ObjectData> object_;
    ContentKind kind_ = ContentKind::Item;
    ContentId id_ = kNoContent;
    i32 layer_ = 0;
    TileRect cells_{};
    bool missing_ = false;
    bool random_reverse_ = false;
    bool mirrored_ = false;
    bool dirty_ = true;
    MineLayer* owner_ = nullptr;
};

} // namespace types
} // namespace mine
