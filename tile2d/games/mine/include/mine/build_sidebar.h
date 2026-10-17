// Mine - the build sidebar: the buildings the player may pick (docs/GAME_DESIGN.md section 1.36).
//
// It is a **model**, not a drawing. Which categories have buildings, which buildings are in each, which
// one is selected, whether the sidebar is open, and where its parts are on a screen of a given size - the
// world view draws that and hands it clicks, and everything above the pixels is testable without a
// renderer (the rule the rest of the game's screen code follows).
#pragma once

#include <mine/content_loader.h>
#include <mine/registry.h>
#include <mine/types/building.h>

#include <t2d/core/math2d.h>

#include <optional>
#include <string>
#include <vector>

namespace mine {

/// One building the sidebar offers: a content entry that declares a `building::` sub-object.
struct BuildEntry {
    ContentKind kind = ContentKind::Structure;
    ContentId id = kNoContent;
    std::string name;                        ///< the name the registry knows it by
    types::BuildingCategory category = types::BuildingCategory::None;
    std::vector<ContentStack> materials;     ///< what it costs, for the line under the grid
    t2d::f32 seconds = 0.0f;                 ///< how long it takes to build
};

/// The sidebar's state and its layout. The categories are its left column, the buildings of the selected
/// category its right, and what the selected one costs is a line under both.
class BuildSidebar {
public:
    /// Fills it from what is registered: every plot content whose definition is a building, in registration
    /// order, and the categories that end up with something in them. A reload calls this again.
    void rebuild(const ContentRegistry& registry, const ContentDefinitions& definitions);

    [[nodiscard]] bool open() const { return open_; }
    void set_open(bool open) { open_ = open; }
    void toggle() { open_ = !open_; }

    /// The categories that have buildings, in the order the data declares them.
    [[nodiscard]] const std::vector<types::BuildingCategory>& categories() const { return categories_; }
    [[nodiscard]] types::BuildingCategory category() const { return category_; }
    /// Selects \p category and forgets the building selection: a building of another category is not one
    /// the player is looking at any more.
    void select_category(types::BuildingCategory category);

    /// The buildings of the selected category, in registration order.
    [[nodiscard]] const std::vector<BuildEntry>& entries() const { return shown_; }
    [[nodiscard]] const BuildEntry* selected() const;
    void select(usize index);
    void clear_selection() { selected_ = kNone; }

    /// Where the parts are on a screen of \p screen, with \p unit the interface scale the game uses.
    struct Layout {
        bool visible = false;
        t2d::Aabb2 panel{};
        std::vector<t2d::Aabb2> categories{};
        std::vector<t2d::Aabb2> entries{};
        t2d::Aabb2 details{};
    };
    [[nodiscard]] Layout layout(t2d::Vec2 screen, t2d::f32 unit) const;

    /// What a point is over, for the clicks the world view hands over. npos / nullopt when it is over
    /// nothing.
    [[nodiscard]] usize entry_at(const Layout& layout, t2d::Vec2 point) const;
    [[nodiscard]] std::optional<types::BuildingCategory> category_at(const Layout& layout,
                                                                     t2d::Vec2 point) const;
    static constexpr usize kNone = static_cast<usize>(-1);

private:
    std::vector<BuildEntry> all_;                            ///< every building, in registration order
    std::vector<types::BuildingCategory> categories_;        ///< the ones with something in them
    std::vector<BuildEntry> shown_;                          ///< the selected category's buildings
    types::BuildingCategory category_ = types::BuildingCategory::None;
    usize selected_ = kNone;
    bool open_ = false;
};

} // namespace mine
