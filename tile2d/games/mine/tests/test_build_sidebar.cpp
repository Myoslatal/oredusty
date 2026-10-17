// The build sidebar: which categories have buildings, which buildings each holds, what is picked, and where
// its parts are on a screen (docs/GAME_DESIGN.md section 1.36).
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

    void add(std::string_view name, BuildingCategory category, usize materials = 1) {
        registry.register_content(ContentKind::Structure, name);
        TileDefinition definition;
        definition.kind = ContentKind::Structure;
        definition.name = std::string(name);
        definition.building.category = category;
        definition.building.seconds = 5.0f;
        for (usize index = 0; index < materials; ++index) {
            definition.building.materials.push_back(ContentStack{std::format("item{}", index), 10});
        }
        definitions.add(std::move(definition));
    }
};

constexpr t2d::f32 kUnit = 1.0f;
const t2d::Vec2 kScreen{1280.0f, 720.0f};

} // namespace

T2D_TEST(the_sidebar_lists_the_buildings_by_category) {
    Fixture content;
    content.add("bin", BuildingCategory::Storage, 2);
    content.add("drill", BuildingCategory::Extraction, 1);
    content.add("crusher", BuildingCategory::Processing, 1);

    BuildSidebar sidebar;
    sidebar.rebuild(content.registry, content.definitions);
    T2D_REQUIRE(sidebar.categories().size() == 3u);
    T2D_CHECK(sidebar.categories()[0] == BuildingCategory::Storage);
    T2D_CHECK(sidebar.categories()[1] == BuildingCategory::Extraction);
    T2D_CHECK(sidebar.categories()[2] == BuildingCategory::Processing);
    // The first category with something in it is the one it starts on, and only its buildings are shown.
    T2D_CHECK(sidebar.category() == BuildingCategory::Storage);
    T2D_REQUIRE(sidebar.entries().size() == 1u);
    T2D_CHECK_EQ(sidebar.entries()[0].name, std::string("bin"));
    T2D_CHECK_EQ(sidebar.entries()[0].materials.size(), 2u);
    T2D_CHECK_EQ(sidebar.entries()[0].seconds, 5.0f);

    sidebar.select_category(BuildingCategory::Extraction);
    T2D_REQUIRE(sidebar.entries().size() == 1u);
    T2D_CHECK_EQ(sidebar.entries()[0].name, std::string("drill"));
}

T2D_TEST(content_that_is_not_a_building_is_not_offered) {
    Fixture content;
    content.add("bin", BuildingCategory::Storage, 1);
    content.registry.register_content(ContentKind::Floor, "dirt");   // no building sub-object
    TileDefinition floor;
    floor.kind = ContentKind::Floor;
    floor.name = "dirt";
    content.definitions.add(std::move(floor));

    BuildSidebar sidebar;
    sidebar.rebuild(content.registry, content.definitions);
    T2D_REQUIRE(sidebar.categories().size() == 1u);
    T2D_CHECK(sidebar.categories()[0] == BuildingCategory::Storage);
    T2D_REQUIRE(sidebar.entries().size() == 1u);
    T2D_CHECK_EQ(sidebar.entries()[0].name, std::string("bin"));
}

T2D_TEST(picking_a_building_is_a_box_and_a_line_under_the_grid) {
    Fixture content;
    content.add("bin", BuildingCategory::Storage, 2);
    content.add("shed", BuildingCategory::Storage, 1);

    BuildSidebar sidebar;
    sidebar.rebuild(content.registry, content.definitions);
    T2D_CHECK(sidebar.selected() == nullptr);   // nothing is picked until the player picks

    sidebar.set_open(true);
    const BuildSidebar::Layout layout = sidebar.layout(kScreen, kUnit);
    T2D_CHECK(layout.visible);
    T2D_REQUIRE(layout.categories.size() == 1u);
    T2D_REQUIRE(layout.entries.size() == 2u);
    // The panel is on the **right** of the screen and inside it, and the categories are to the left of the
    // buildings: the two columns the designer asked for.
    T2D_CHECK(layout.panel.max.x <= kScreen.x);
    T2D_CHECK(layout.panel.min.x > kScreen.x * 0.5f);
    T2D_CHECK(layout.panel.min.y >= 0.0f && layout.panel.max.y <= kScreen.y);
    T2D_CHECK(layout.categories[0].max.x <= layout.entries[0].min.x);
    T2D_CHECK(layout.details.min.y >= layout.entries[0].max.y);

    // What a click lands on, which is what the world view asks.
    const t2d::Vec2 middle{(layout.entries[1].min.x + layout.entries[1].max.x) * 0.5f,
                           (layout.entries[1].min.y + layout.entries[1].max.y) * 0.5f};
    T2D_CHECK_EQ(sidebar.entry_at(layout, middle), 1u);
    T2D_CHECK(sidebar.category_at(layout, middle) == std::nullopt);
    const t2d::Vec2 on_category{(layout.categories[0].min.x + layout.categories[0].max.x) * 0.5f,
                                (layout.categories[0].min.y + layout.categories[0].max.y) * 0.5f};
    T2D_CHECK(sidebar.category_at(layout, on_category) == BuildingCategory::Storage);
    T2D_CHECK_EQ(sidebar.entry_at(layout, on_category), BuildSidebar::kNone);
    T2D_CHECK_EQ(sidebar.entry_at(layout, t2d::Vec2{10.0f, 10.0f}), BuildSidebar::kNone);   // over the map

    sidebar.select(1);
    T2D_REQUIRE(sidebar.selected() != nullptr);
    T2D_CHECK_EQ(sidebar.selected()->name, std::string("shed"));
    sidebar.clear_selection();
    T2D_CHECK(sidebar.selected() == nullptr);

    // Closed is closed: nothing is laid out and nothing is hit.
    sidebar.set_open(false);
    T2D_CHECK_FALSE(sidebar.layout(kScreen, kUnit).visible);
}

T2D_TEST_MAIN