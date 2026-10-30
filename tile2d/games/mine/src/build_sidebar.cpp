#include <mine/build_sidebar.h>

#include <mine/types/tile_definition.h>

#include <algorithm>
#include <memory>

namespace mine {

void BuildSidebar::rebuild(const ContentRegistry& registry, const ContentDefinitions& definitions,
                           const BuildingUnlocks& unlocks) {
    all_.clear();
    categories_.clear();
    // Registration order, so the grid is the order the designer's files declare: a picker that shuffles
    // itself between reloads is a picker nobody learns.
    for (const ContentKind kind : {ContentKind::Structure, ContentKind::Machine, ContentKind::Drill,
                                   ContentKind::Warehouse, ContentKind::Floor, ContentKind::Ore}) {
        for (const ContentEntry& entry : registry.entries(kind)) {
            const types::TileDefinition* definition = definitions.find_tile(kind, entry.name);
            if (definition == nullptr || definition->building.empty()) continue;
            // Only what this save has unlocked is on offer; a locked building is not a building the player
            // may pick yet (docs/GAME_DESIGN.md section 1.44).
            if (!unlocks.unlocked(kind, entry.id)) continue;
            BuildEntry building;
            building.kind = kind;
            building.id = entry.id;
            building.name = entry.name;
            building.category = definition->building.category;
            building.materials = definition->building.materials;
            building.seconds = definition->building.seconds;
            // What the entry is drawn as is the plot's own answer, not a second reading of the definition's
            // picture fields here: the sidebar builds the plot it would place and asks it for its complete
            // render preview (docs/GAME_DESIGN.md section 1.39). A definition that is not a plot at all
            // answers with nothing to draw, which is what the name alone is for.
            const std::unique_ptr<types::SceneTile> plot =
                types::make_tile(*definition, entry.id, 0, GridPos{0, 0});
            if (plot != nullptr) building.preview = plot->get_preview();
            all_.push_back(std::move(building));
        }
    }
    for (const BuildEntry& building : all_) {
        if (std::find(categories_.begin(), categories_.end(), building.category) == categories_.end()) {
            categories_.push_back(building.category);
        }
    }
    // The selection survives a reload when the category is still there; the building selection does not,
    // because the ids it was made of may not be the ids it would be made of now.
    if (std::find(categories_.begin(), categories_.end(), category_) == categories_.end()) {
        category_ = categories_.empty() ? types::BuildingCategory::None : categories_.front();
    }
    page_ = 0;
    select_category(category_);
}

void BuildSidebar::select_category(types::BuildingCategory category) {
    category_ = category;
    selected_ = kNone;
    page_ = 0;
    shown_.clear();
    for (const BuildEntry& building : all_) {
        if (building.category == category) shown_.push_back(building);
    }
}

const BuildEntry* BuildSidebar::selected() const {
    return selected_ < shown_.size() ? &shown_[selected_] : nullptr;
}

void BuildSidebar::select(usize index) { selected_ = index < shown_.size() ? index : kNone; }

BuildSidebar::Layout BuildSidebar::layout(t2d::Vec2 screen, t2d::f32 unit) const {
    Layout out;
    // The button is always there: closed, it is the only part of the sidebar on the screen, and it is what
    // opens it again. The panel itself is only laid out when it is open.
    const t2d::f32 unit_padding = 6.0f * unit;
    const t2d::f32 button = 22.0f * unit;
    if (!open_) {
        // Closed: a tab flush against the right edge of the screen, vertically centred - the only part of
        // the sidebar that is there, and the way back in.
        out.toggle = t2d::Aabb2{t2d::Vec2{screen.x - button, (screen.y - button) * 0.5f},
                                t2d::Vec2{screen.x, (screen.y + button) * 0.5f}};
        return out;
    }
    out.visible = true;
    // A **fixed** vertical rectangle, **flush against the right edge** of the screen and vertically centred:
    // its size does not depend on how much is in it (docs/GAME_DESIGN.md section 1.36). What does not fit
    // is on another page.
    const t2d::f32 padding = unit_padding;
    const t2d::f32 category_width = 56.0f * unit;
    const t2d::f32 cell = 34.0f * unit;
    const t2d::f32 line = 18.0f * unit;
    const t2d::f32 width = category_width + static_cast<t2d::f32>(kColumns) * cell + padding * 3.0f;
    const t2d::f32 height = padding * 2.0f + line * 3.5f + static_cast<t2d::f32>(kRows) * cell;
    const t2d::f32 top = std::max(0.0f, (screen.y - height) * 0.5f);
    out.panel = t2d::Aabb2{t2d::Vec2{screen.x - width, top}, t2d::Vec2{screen.x, top + height}};
    // Open: the button is the first thing on the panel's **left**, above the categories - a close button
    // belongs at the near edge of the thing it closes, not at the far one (docs/GAME_DESIGN.md 1.36).
    out.toggle = t2d::Aabb2{t2d::Vec2{out.panel.min.x + padding, out.panel.min.y + padding},
                            t2d::Vec2{out.panel.min.x + padding + button, out.panel.min.y + padding + line}};
    // The technology tree button sits right of the close button on the same line (section 1.50).
    const t2d::f32 tech_width = 48.0f * unit;
    out.tech = t2d::Aabb2{t2d::Vec2{out.toggle.max.x + padding * 0.5f, out.toggle.min.y},
                          t2d::Vec2{out.toggle.max.x + padding * 0.5f + tech_width, out.toggle.max.y}};
    const t2d::f32 body_top = out.panel.min.y + padding + line * 1.5f;
    for (usize index = 0; index < categories_.size(); ++index) {
        const t2d::f32 y = body_top + static_cast<t2d::f32>(index) * line;
        out.categories.push_back(t2d::Aabb2{t2d::Vec2{out.panel.min.x + padding, y},
                                            t2d::Vec2{out.panel.min.x + padding + category_width, y + line}});
    }
    const t2d::f32 grid_x = out.panel.min.x + padding * 2.0f + category_width;
    const usize first = page_ * kPageSize;
    for (usize index = first; index < shown_.size() && index - first < kPageSize; ++index) {
        const usize slot = index - first;
        const t2d::f32 x = grid_x + static_cast<t2d::f32>(slot % kColumns) * cell;
        const t2d::f32 y = body_top + static_cast<t2d::f32>(slot / kColumns) * cell;
        out.entries.push_back(
            t2d::Aabb2{t2d::Vec2{x, y}, t2d::Vec2{x + cell - 3.0f * unit, y + cell - 3.0f * unit}});
    }
    out.details = t2d::Aabb2{t2d::Vec2{out.panel.min.x + padding, out.panel.max.y - padding - line * 2.0f},
                             t2d::Vec2{out.panel.max.x - padding, out.panel.max.y - padding}};
    out.page_label = t2d::Aabb2{t2d::Vec2{out.panel.max.x - 44.0f * unit, out.panel.max.y - padding - line},
                                t2d::Vec2{out.panel.max.x - padding, out.panel.max.y - padding}};
    return out;
}

void BuildSidebar::turn_page(i32 by) {
    const t2d::i64 wanted = static_cast<t2d::i64>(page_) + by;
    const t2d::i64 last = static_cast<t2d::i64>(page_count()) - 1;
    page_ = static_cast<usize>(std::clamp<t2d::i64>(wanted, 0, last < 0 ? 0 : last));
}
usize BuildSidebar::entry_at(const Layout& layout, t2d::Vec2 point) const {
    for (usize slot = 0; slot < layout.entries.size(); ++slot) {
        if (layout.entries[slot].contains(point)) return page_ * kPageSize + slot;
    }
    return kNone;
}

std::optional<types::BuildingCategory> BuildSidebar::category_at(const Layout& layout,
                                                                 t2d::Vec2 point) const {
    for (usize index = 0; index < layout.categories.size(); ++index) {
        if (layout.categories[index].contains(point)) return categories_[index];
    }
    return std::nullopt;
}

} // namespace mine
