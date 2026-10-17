#include <mine/build_sidebar.h>

#include <algorithm>

namespace mine {

void BuildSidebar::rebuild(const ContentRegistry& registry, const ContentDefinitions& definitions) {
    all_.clear();
    categories_.clear();
    // Registration order, so the grid is the order the designer's files declare: a picker that shuffles
    // itself between reloads is a picker nobody learns.
    for (const ContentKind kind : {ContentKind::Structure, ContentKind::Machine, ContentKind::Drill,
                                   ContentKind::Warehouse, ContentKind::Floor, ContentKind::Ore}) {
        for (const ContentEntry& entry : registry.entries(kind)) {
            const types::TileDefinition* definition = definitions.find_tile(kind, entry.name);
            if (definition == nullptr || definition->building.empty()) continue;
            BuildEntry building;
            building.kind = kind;
            building.id = entry.id;
            building.name = entry.name;
            building.category = definition->building.category;
            building.materials = definition->building.materials;
            building.seconds = definition->building.seconds;
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
    select_category(category_);
}

void BuildSidebar::select_category(types::BuildingCategory category) {
    category_ = category;
    selected_ = kNone;
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
    if (!open_) return out;
    out.visible = true;
    // A column at the **right, vertically centred** (the designer's words): wide enough for a category
    // name beside a grid of pictures, and never more than half the screen - a sidebar that covers the map
    // is not a sidebar.
    const t2d::f32 padding = 8.0f * unit;
    const t2d::f32 category_width = 92.0f * unit;
    const t2d::f32 cell = 44.0f * unit;
    const t2d::f32 line = 22.0f * unit;
    const t2d::f32 grid_width = cell * 3.0f + padding;
    const t2d::f32 width = std::min(category_width + grid_width + padding * 2.0f, screen.x * 0.5f);
    const t2d::f32 rows = static_cast<t2d::f32>(std::max<usize>(1, categories_.size()));
    const t2d::f32 grid_rows = static_cast<t2d::f32>(std::max<usize>(1, (shown_.size() + 2) / 3));
    const t2d::f32 body = std::max(rows * line, grid_rows * cell);
    const t2d::f32 height = body + line * 2.0f + padding * 2.0f;
    const t2d::f32 top = std::max(padding, (screen.y - height) * 0.5f);
    out.panel = t2d::Aabb2{t2d::Vec2{screen.x - width - padding, top},
                           t2d::Vec2{screen.x - padding, top + height}};
    for (usize index = 0; index < categories_.size(); ++index) {
        const t2d::f32 y = out.panel.min.y + padding + static_cast<t2d::f32>(index) * line;
        out.categories.push_back(t2d::Aabb2{t2d::Vec2{out.panel.min.x + padding, y},
                                            t2d::Vec2{out.panel.min.x + padding + category_width, y + line}});
    }
    const t2d::f32 grid_x = out.panel.min.x + padding + category_width;
    for (usize index = 0; index < shown_.size(); ++index) {
        const t2d::f32 x = grid_x + static_cast<t2d::f32>(index % 3) * cell;
        const t2d::f32 y = out.panel.min.y + padding + static_cast<t2d::f32>(index / 3) * cell;
        out.entries.push_back(
            t2d::Aabb2{t2d::Vec2{x, y}, t2d::Vec2{x + cell - 4.0f * unit, y + cell - 4.0f * unit}});
    }
    out.details = t2d::Aabb2{t2d::Vec2{out.panel.min.x + padding, out.panel.max.y - padding - line * 2.0f},
                             t2d::Vec2{out.panel.max.x - padding, out.panel.max.y - padding}};
    return out;
}

usize BuildSidebar::entry_at(const Layout& layout, t2d::Vec2 point) const {
    for (usize index = 0; index < layout.entries.size(); ++index) {
        if (layout.entries[index].contains(point)) return index;
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
