// The build sidebar: which categories have buildings, which buildings each holds, what is picked, where its
// parts are on a screen, and how the pages turn (docs/GAME_DESIGN.md section 1.36).
#include <mine/build_sidebar.h>

#include <support/test_support.h>

#include <string>

using namespace mine;
using namespace mine::types;

namespace {

/// A registry and its definitions, with buildings in them.
struct Fixture {
    ContentRegistry registry;
    ContentDefinitions definitions;

    void add(std::string_view name, BuildingCategory category, usize materials = 1,
             std::string_view image = {}, bool default_unlock = true) {
        registry.register_content(ContentKind::Structure, name);
        TileDefinition definition;
        definition.kind = ContentKind::Structure;
        definition.name = std::string(name);
        definition.image = std::string(image);
        definition.building.category = category;
        definition.building.seconds = 5.0f;
        definition.building.default_unlock = default_unlock;
        for (usize index = 0; index < materials; ++index) {
            definition.building.materials.push_back(ContentStack{std::format("item{}", index), 10});
        }
        definitions.add(std::move(definition));
    }

    /// The unlock state a new save over this fixture's content starts with: every building at the default
    /// its definition gives it (docs/GAME_DESIGN.md section 1.44).
    [[nodiscard]] BuildingUnlocks unlocks_from_defaults() const {
        BuildingUnlocks unlocks;
        unlocks.reset(registry, definitions);
        return unlocks;
    }

    /// A building whose content is a drill: two pictures, the second of which turns on the first
    /// (docs/GAME_DESIGN.md section 1.38).
    void add_drill(std::string_view name, BuildingCategory category, std::string_view image,
                   std::string_view top_image) {
        registry.register_content(ContentKind::Drill, name);
        TileDefinition definition;
        definition.kind = ContentKind::Drill;
        definition.name = std::string(name);
        definition.image = std::string(image);
        definition.top_image = std::string(top_image);
        definition.spin = 4.0f;
        definition.building.category = category;
        definition.building.seconds = 5.0f;
        definitions.add(std::move(definition));
    }
};

constexpr t2d::f32 kUnit = 1.0f;
const t2d::Vec2 kScreen{1280.0f, 720.0f};

/// The middle of a rectangle: where a click lands when a test means "this one".
t2d::Vec2 middle_of(const t2d::Aabb2& box) {
    return t2d::Vec2{(box.min.x + box.max.x) * 0.5f, (box.min.y + box.max.y) * 0.5f};
}

} // namespace

T2D_TEST(the_build_sidebar_lists_buildings_by_category_picks_one_and_pages_the_rest) {
    Fixture content;
    content.add("bin", BuildingCategory::Storage, 2);
    content.add("shed", BuildingCategory::Storage, 1);
    content.add("drill", BuildingCategory::Extraction, 1);
    content.add("crusher", BuildingCategory::Processing, 1);
    content.registry.register_content(ContentKind::Floor, "dirt");   // no building sub-object: not offered
    TileDefinition floor;
    floor.kind = ContentKind::Floor;
    floor.name = "dirt";
    content.definitions.add(std::move(floor));

    BuildSidebar sidebar;
    sidebar.rebuild(content.registry, content.definitions, content.unlocks_from_defaults());

    // --- what it offers ----------------------------------------------------------------------------
    T2D_REQUIRE(sidebar.categories().size() == 3u);
    T2D_CHECK(sidebar.categories()[0] == BuildingCategory::Storage);
    T2D_CHECK(sidebar.categories()[1] == BuildingCategory::Extraction);
    T2D_CHECK(sidebar.categories()[2] == BuildingCategory::Processing);
    T2D_CHECK(sidebar.category() == BuildingCategory::Storage);   // the first that has something
    T2D_REQUIRE(sidebar.entries().size() == 2u);
    T2D_CHECK_EQ(sidebar.entries()[0].name, std::string("bin"));
    T2D_CHECK_EQ(sidebar.entries()[0].materials.size(), 2u);
    T2D_CHECK_EQ(sidebar.entries()[0].seconds, 5.0f);
    T2D_CHECK(sidebar.selected() == nullptr);   // nothing is picked until the player picks

    sidebar.select_category(BuildingCategory::Extraction);
    T2D_REQUIRE(sidebar.entries().size() == 1u);
    T2D_CHECK_EQ(sidebar.entries()[0].name, std::string("drill"));
    T2D_CHECK_EQ(sidebar.page(), 0u);   // a new category starts on its first page

    // --- where its parts are -----------------------------------------------------------------------
    sidebar.set_open(false);
    BuildSidebar::Layout closed = sidebar.layout(kScreen, kUnit);
    T2D_CHECK_FALSE(closed.visible);
    T2D_CHECK(closed.toggle.max.x == kScreen.x);                       // the way in, on the right edge
    T2D_CHECK(closed.toggle.min.x > kScreen.x - 40.0f * kUnit);
    T2D_CHECK(closed.panel.max.x == 0.0f && closed.panel.max.y == 0.0f);   // nothing else is laid out

    sidebar.set_open(true);
    const BuildSidebar::Layout layout = sidebar.layout(kScreen, kUnit);
    T2D_CHECK(layout.visible);
    T2D_CHECK(layout.panel.max.x == kScreen.x);                        // flush against the right edge
    T2D_CHECK(layout.panel.min.x > kScreen.x * 0.5f);                  // and a column, not the whole screen
    T2D_CHECK(layout.panel.max.y - layout.panel.min.y > layout.panel.max.x - layout.panel.min.x);
    T2D_REQUIRE(layout.categories.size() == 3u);   // every category is on screen; the picked one is lit
    T2D_REQUIRE(layout.entries.size() == 1u);
    T2D_CHECK(layout.categories[0].max.x <= layout.entries[0].min.x);  // categories left of the buildings
    T2D_CHECK(layout.details.min.y >= layout.entries[0].max.y);        // and what it costs below both
    // The button is on the **left** of the panel when it is open: it closes what it is near.
    T2D_CHECK(layout.toggle.min.x < layout.panel.min.x + (layout.panel.max.x - layout.panel.min.x) * 0.5f);
    T2D_CHECK(layout.toggle.max.y <= layout.categories[0].min.y);

    // --- what a click lands on ---------------------------------------------------------------------
    T2D_CHECK(sidebar.category_at(layout, middle_of(layout.toggle)) == std::nullopt);
    T2D_CHECK_EQ(sidebar.entry_at(layout, middle_of(layout.toggle)), BuildSidebar::kNone);
    T2D_CHECK(sidebar.category_at(layout, middle_of(layout.categories[0])) == sidebar.categories()[0]);
    T2D_CHECK_EQ(sidebar.entry_at(layout, middle_of(layout.categories[0])), BuildSidebar::kNone);
    T2D_CHECK_EQ(sidebar.entry_at(layout, middle_of(layout.entries[0])), 0u);
    T2D_CHECK_EQ(sidebar.entry_at(layout, t2d::Vec2{10.0f, 10.0f}), BuildSidebar::kNone);   // over the map

    // --- picking ----------------------------------------------------------------------------------
    sidebar.select(0);
    T2D_REQUIRE(sidebar.selected() != nullptr);
    T2D_CHECK_EQ(sidebar.selected()->name, std::string("drill"));
    sidebar.clear_selection();
    T2D_CHECK(sidebar.selected() == nullptr);
    sidebar.select(99);   // past the end is nothing, not the last one
    T2D_CHECK(sidebar.selected() == nullptr);
}

T2D_TEST(a_category_with_more_buildings_than_fit_is_paged) {
    Fixture content;
    for (usize index = 0; index < BuildSidebar::kPageSize + 3; ++index) {
        content.add(std::format("shed{}", index), BuildingCategory::Storage);
    }

    BuildSidebar sidebar;
    sidebar.rebuild(content.registry, content.definitions, content.unlocks_from_defaults());
    sidebar.set_open(true);
    T2D_REQUIRE(sidebar.entries().size() == BuildSidebar::kPageSize + 3);
    T2D_CHECK_EQ(sidebar.page_count(), 2u);

    // The first page is full, the second holds what is left, and a click on it answers with the index in the
    // **whole** list - the world view places the building the player is looking at, not the one on slot 0.
    const BuildSidebar::Layout first = sidebar.layout(kScreen, kUnit);
    T2D_REQUIRE(first.entries.size() == BuildSidebar::kPageSize);
    T2D_CHECK_EQ(sidebar.entry_at(first, middle_of(first.entries[0])), 0u);
    T2D_CHECK_EQ(sidebar.entry_at(first, middle_of(first.entries[BuildSidebar::kPageSize - 1])),
                 BuildSidebar::kPageSize - 1);

    sidebar.turn_page(1);
    T2D_CHECK_EQ(sidebar.page(), 1u);
    const BuildSidebar::Layout second = sidebar.layout(kScreen, kUnit);
    T2D_REQUIRE(second.entries.size() == 3u);
    T2D_CHECK_EQ(sidebar.entry_at(second, middle_of(second.entries[0])), BuildSidebar::kPageSize);
    T2D_CHECK_EQ(sidebar.entry_at(second, middle_of(second.entries[2])), BuildSidebar::kPageSize + 2);
    // The panel is the same size on both pages: the sidebar has a fixed size (section 1.36).
    T2D_CHECK(second.panel.max.x == first.panel.max.x && second.panel.max.y == first.panel.max.y);
    T2D_CHECK(second.panel.min.x == first.panel.min.x && second.panel.min.y == first.panel.min.y);

    // Turning past either end stays on the pages there are.
    sidebar.turn_page(1);
    T2D_CHECK_EQ(sidebar.page(), 1u);
    sidebar.turn_page(-5);
    T2D_CHECK_EQ(sidebar.page(), 0u);
    // A category with one page has nothing to turn to.
    sidebar.select_category(BuildingCategory::Processing);
    T2D_CHECK_EQ(sidebar.page_count(), 1u);
    sidebar.turn_page(1);
    T2D_CHECK_EQ(sidebar.page(), 0u);
}

T2D_TEST(a_buildings_icon_is_the_complete_preview_the_plot_answers_with) {
    Fixture content;
    content.add("bin", BuildingCategory::Storage, 1, "art/bin.png");
    content.add("shed", BuildingCategory::Storage, 1);   // no picture at all
    content.add_drill("rig", BuildingCategory::Extraction, "art/rig_base.png", "art/rig_top.png");

    BuildSidebar sidebar;
    sidebar.rebuild(content.registry, content.definitions, content.unlocks_from_defaults());

    // The picture is the plot's own answer (types/tile.h): rebuild builds the plot it would place and asks
    // it, instead of reading the definition's picture fields a second time (section 1.39).
    T2D_REQUIRE(sidebar.entries().size() == 2u);
    T2D_CHECK_EQ(sidebar.entries()[0].name, std::string("bin"));
    T2D_REQUIRE(sidebar.entries()[0].preview.images.size() == 1u);
    T2D_CHECK(sidebar.entries()[0].preview.images[0].part.empty());
    T2D_CHECK_FALSE(sidebar.entries()[0].preview.images[0].mirrored);
    // A building whose entry names no picture previews as nothing: the name alone says it.
    T2D_CHECK(sidebar.entries()[1].preview.empty());

    // A drill's icon is both of its pictures, in drawing order: the base, then the top that turns on it -
    // which is the first time the sidebar can show a drill whole (section 1.38).
    sidebar.select_category(BuildingCategory::Extraction);
    T2D_REQUIRE(sidebar.entries().size() == 1u);
    const BuildEntry& rig = sidebar.entries()[0];
    T2D_REQUIRE(rig.preview.images.size() == 2u);
    T2D_CHECK(rig.preview.images[0].part.empty());
    T2D_CHECK_EQ(rig.preview.images[1].part, std::string("top"));
    // A still frame at zero seconds: the icon does not start at whatever moment the frame happens to be,
    // and the pivot is the middle when the entry names no spin_origin.
    T2D_CHECK_EQ(rig.preview.images[1].radians, 0.0f);
    T2D_CHECK_EQ(rig.preview.images[0].radians, 0.0f);
    const t2d::Vec2 middle{0.5f, 0.5f};
    T2D_CHECK_EQ(rig.preview.images[1].pivot, middle);
}

T2D_TEST(only_unlocked_buildings_are_offered) {
    Fixture content;
    content.add("bin", BuildingCategory::Storage, 1);                     // unlocked
    content.add("shed", BuildingCategory::Storage, 1, {}, false);         // locked by the data
    content.add("rig", BuildingCategory::Extraction, 1);                  // unlocked
    content.add("crusher", BuildingCategory::Processing, 1, {}, false);   // the only one of its category

    BuildingUnlocks unlocks;
    unlocks.reset(content.registry, content.definitions);
    BuildSidebar sidebar;
    sidebar.rebuild(content.registry, content.definitions, unlocks);

    // Locked buildings are not on offer, and a category with nothing unlocked in it does not appear at all
    // (docs/GAME_DESIGN.md section 1.44).
    T2D_REQUIRE(sidebar.categories().size() == 2u);
    T2D_CHECK(sidebar.categories()[0] == BuildingCategory::Storage);
    T2D_CHECK(sidebar.categories()[1] == BuildingCategory::Extraction);
    T2D_REQUIRE(sidebar.entries().size() == 1u);
    T2D_CHECK_EQ(sidebar.entries()[0].name, std::string("bin"));

    // Unlocking is a state of the save, not a change of the content: the next rebuild offers it, in
    // registration order, and picking works exactly as before (section 1.36).
    const ContentId shed = content.registry.find(ContentKind::Structure, "shed");
    const ContentId crusher = content.registry.find(ContentKind::Structure, "crusher");
    T2D_CHECK(unlocks.set_unlocked(ContentKind::Structure, shed, true));
    T2D_CHECK(unlocks.set_unlocked(ContentKind::Structure, crusher, true));
    sidebar.rebuild(content.registry, content.definitions, unlocks);
    T2D_REQUIRE(sidebar.categories().size() == 3u);
    T2D_REQUIRE(sidebar.entries().size() == 2u);   // Storage: bin, then shed
    T2D_CHECK_EQ(sidebar.entries()[0].name, std::string("bin"));
    T2D_CHECK_EQ(sidebar.entries()[1].name, std::string("shed"));
    T2D_CHECK_EQ(sidebar.page(), 0u);
    sidebar.select(1);
    T2D_REQUIRE(sidebar.selected() != nullptr);
    T2D_CHECK_EQ(sidebar.selected()->name, std::string("shed"));

    // Locking one back takes it out again, and the rest of the sidebar is what it was.
    const ContentId bin = content.registry.find(ContentKind::Structure, "bin");
    T2D_CHECK(unlocks.set_unlocked(ContentKind::Structure, bin, false));
    sidebar.rebuild(content.registry, content.definitions, unlocks);
    T2D_REQUIRE(sidebar.entries().size() == 1u);
    T2D_CHECK_EQ(sidebar.entries()[0].name, std::string("shed"));
}

T2D_TEST_MAIN