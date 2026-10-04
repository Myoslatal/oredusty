// The single layer sandbox, without a window: the grid, the palette the designer's data produces, the
// camera, and - the part that matters - what a reload does to a layer that is already painted.
#include <mine/content_loader.h>
#include <mine/sandbox.h>

#include <support/test_support.h>

#include <t2d/core/bitstream.h>

#include <algorithm>
#include <format>
#include <string>
#include <vector>

using namespace mine;
using t2d::ConstSpan;
using t2d::EcfgError;

namespace {

// Placeholder names on purpose: this file must not look like game content. The designer's own names
// arrive in their own file (docs/GAME_DESIGN.md section 7).
constexpr const char* kContentV1 = R"(
structure::
    sample_a::
        note:"the fields under a name are the designer's, the engine only reads the name"
    sample_b::
machine::
    sample_c::
)";

/// The same file with one entry inserted at the top of the structure table: every structure id after
/// it moves by one. This is the change that makes a save without a name table unreadable.
constexpr const char* kContentV2 = R"(
structure::
    sample_zero::
    sample_a::
    sample_b::
machine::
    sample_c::
)";

/// And one with an entry deleted.
constexpr const char* kContentV3 = R"(
structure::
    sample_a::
machine::
    sample_c::
)";

constexpr const char* kBroken = "structure::\n    sample_a::\n        note:unquoted\n";
constexpr const char* kTypoTable = "structurs::\n    sample_a::\n";

void load(ContentRegistry& registry, const char* text) {
    EcfgError error;
    const ContentLoadReport report = register_content_from_ecfg_text(registry, text, &error);
    T2D_REQUIRE(registry.total_count() > 0 || report.registered > 0);
}

[[nodiscard]] ConstSpan<const u8> span_of(const std::vector<u8>& bytes) {
    return ConstSpan<const u8>(bytes.data(), bytes.size());
}

/// Paints one cell with a named structure, so the tests read as "the layer had a sample_b here".
void paint_named(SandboxModel& model, GridPos pos, ContentKind kind, std::string_view name,
                 const ContentRegistry& registry) {
    const ContentId id = registry.find(kind, name);
    T2D_REQUIRE(id != kNoContent);
    model.set_cell(pos, ContentRef{kind, id, false});
}

} // namespace

T2D_TEST(a_new_layer_is_empty_and_the_cursor_stays_inside_it) {
    SandboxModel model(8, 4);
    T2D_CHECK_EQ(model.width(), 8u);
    T2D_CHECK_EQ(model.height(), 4u);
    T2D_CHECK_EQ(model.filled_cells(), 0u);
    T2D_CHECK_EQ(model.cursor(), (GridPos{0, 0}));
    T2D_CHECK(model.inside(GridPos{7, 3}));
    T2D_CHECK_FALSE(model.inside(GridPos{8, 3}));
    T2D_CHECK_FALSE(model.inside(GridPos{-1, 0}));
    // A cell outside the grid reads as empty instead of running off the end of the vector.
    T2D_CHECK(model.cell(GridPos{99, 99}).empty());

    model.set_cursor(GridPos{100, -5});
    T2D_CHECK_EQ(model.cursor(), (GridPos{7, 0}));
    model.move_cursor(-1, 0);
    T2D_CHECK_EQ(model.cursor(), (GridPos{6, 0}));
    model.set_cursor(GridPos{0, 0});
    model.move_cursor(-1, -1);
    T2D_CHECK_EQ(model.cursor(), (GridPos{0, 0}));

    // A zero sized layer is refused rather than allocated empty.
    SandboxModel clamped(0, 0);
    T2D_CHECK_EQ(clamped.width(), 1u);
    T2D_CHECK_EQ(clamped.height(), 1u);
}

T2D_TEST(the_palette_is_the_registered_content_that_can_sit_on_a_tile) {
    T2D_CHECK(is_placeable_kind(ContentKind::Structure));
    T2D_CHECK(is_placeable_kind(ContentKind::Machine));
    T2D_CHECK_FALSE(is_placeable_kind(ContentKind::Item));
    T2D_CHECK_FALSE(is_placeable_kind(ContentKind::Recipe));
    T2D_CHECK_FALSE(is_placeable_kind(ContentKind::Layer));
    T2D_CHECK_FALSE(is_placeable_kind(ContentKind::Channel));

    ContentRegistry registry;
    load(registry, kContentV1);
    SandboxModel model;
    T2D_CHECK_EQ(model.rebind(registry).palette_size, 3u);
    T2D_CHECK_EQ(model.palette_count(), 3u);
    T2D_CHECK_EQ(model.palette(0).name, std::string("sample_a"));
    T2D_CHECK_EQ(model.palette(0).kind, ContentKind::Structure);
    T2D_CHECK_EQ(model.palette(0).id, 1u);
    T2D_CHECK_EQ(model.palette(1).name, std::string("sample_b"));
    T2D_CHECK_EQ(model.palette(2).name, std::string("sample_c"));
    T2D_CHECK_EQ(model.palette(2).kind, ContentKind::Machine);
    T2D_CHECK_EQ(model.palette(2).id, 1u); // ids are per kind

    // An empty registry leaves the palette empty and the accessors safe.
    ContentRegistry empty;
    SandboxModel bare;
    T2D_CHECK_EQ(bare.rebind(empty).palette_size, 0u);
    T2D_CHECK(bare.selected_entry() == nullptr);
    T2D_CHECK_FALSE(bare.paint());
    T2D_CHECK_EQ(bare.palette(7).name, std::string{});
}

T2D_TEST(painting_places_the_selected_entry_and_erasing_takes_it_away) {
    ContentRegistry registry;
    load(registry, kContentV1);
    SandboxModel model(4, 4);
    model.rebind(registry);
    model.set_cursor(GridPos{2, 1});

    T2D_CHECK(model.paint());
    T2D_CHECK_EQ(model.cell(GridPos{2, 1}).name, std::string("sample_a"));
    T2D_CHECK_EQ(model.cell(GridPos{2, 1}).id, 1u);
    T2D_CHECK_EQ(model.cell(GridPos{2, 1}).kind, ContentKind::Structure);
    T2D_CHECK_EQ(model.filled_cells(), 1u);
    // Painting the same content again changes nothing, and says so.
    T2D_CHECK_FALSE(model.paint());

    model.cycle_palette(1);
    T2D_CHECK(model.paint());
    T2D_CHECK_EQ(model.cell(GridPos{2, 1}).name, std::string("sample_b"));
    T2D_CHECK_EQ(model.filled_cells(), 1u);

    T2D_CHECK(model.erase());
    T2D_CHECK(model.cell(GridPos{2, 1}).empty());
    T2D_CHECK_FALSE(model.erase());
    T2D_CHECK_EQ(model.filled_cells(), 0u);

    // A cell is only ever reachable through the cursor, which is always inside the grid.
    model.set_cursor(GridPos{3, 3});
    T2D_CHECK(model.paint());
    T2D_CHECK_EQ(model.filled_cells(), 1u);
    model.clear_cells();
    T2D_CHECK_EQ(model.filled_cells(), 0u);
}

T2D_TEST(the_palette_selection_wraps_and_its_window_follows_it) {
    ContentRegistry registry;
    load(registry, kContentV1);
    SandboxModel model;
    model.rebind(registry);

    T2D_CHECK_EQ(model.selected_palette(), 0u);
    model.cycle_palette(-1);
    T2D_CHECK_EQ(model.selected_palette(), 2u); // wraps backwards
    model.cycle_palette(1);
    T2D_CHECK_EQ(model.selected_palette(), 0u);
    model.cycle_palette(5);
    T2D_CHECK_EQ(model.selected_palette(), 2u);

    model.select_palette(99);
    T2D_CHECK_EQ(model.selected_palette(), 2u); // clamped, never out of range
    T2D_CHECK_EQ(model.palette_window_start(3), 0u);
    T2D_CHECK_EQ(model.palette_window_start(0), 0u);
    T2D_CHECK_EQ(model.palette_window_start(1), 2u); // a one row window shows the selection
    T2D_CHECK_EQ(model.selected_entry()->name, std::string("sample_c"));
}

T2D_TEST(screen_and_cell_coordinates_are_inverse_and_zoom_keeps_the_anchor) {
    SandboxModel model(8, 6);
    model.set_cell_px(16.0f);
    model.set_origin(t2d::Vec2{100.0f, 50.0f});
    for (i32 y = 0; y < 6; ++y) {
        for (i32 x = 0; x < 8; ++x) {
            const t2d::Vec2 screen = model.screen_of_cell(GridPos{x, y});
            T2D_CHECK_EQ(model.cell_at_screen(screen), (GridPos{x, y}));
            T2D_CHECK_EQ(model.cell_at_screen(t2d::Vec2{screen.x + 15.0f, screen.y + 15.0f}),
                         (GridPos{x, y}));
        }
    }
    // Outside the layer the coordinates go negative instead of clamping: the caller decides.
    T2D_CHECK_EQ(model.cell_at_screen(t2d::Vec2{99.0f, 49.0f}), (GridPos{-1, -1}));

    model.set_cell_px(20.0f);
    model.set_origin(t2d::Vec2{0.0f, 0.0f});
    const t2d::Vec2 anchor{45.0f, 65.0f};
    const GridPos under_anchor = model.cell_at_screen(anchor);
    T2D_CHECK_EQ(under_anchor, (GridPos{2, 3}));
    model.zoom_at(anchor, 2.0f);
    T2D_CHECK_NEAR(model.cell_px(), 40.0f, 0.001f);
    T2D_CHECK_EQ(model.cell_at_screen(anchor), under_anchor);
    model.zoom_at(anchor, 0.25f);
    T2D_CHECK_NEAR(model.cell_px(), 10.0f, 0.001f);
    T2D_CHECK_EQ(model.cell_at_screen(anchor), under_anchor);
    // The zoom is clamped, and a zoom that changes nothing must not move the layer either.
    model.zoom_at(anchor, 0.0001f);
    T2D_CHECK_NEAR(model.cell_px(), SandboxModel::kMinCellPx, 0.001f);
    const t2d::Vec2 origin_before = model.origin();
    model.zoom_at(anchor, 1.0f);
    T2D_CHECK_NEAR(model.origin().x, origin_before.x, 0.001f);
    T2D_CHECK_NEAR(model.origin().y, origin_before.y, 0.001f);
}

T2D_TEST(the_camera_can_centre_the_layer_and_keep_the_cursor_visible) {
    SandboxModel model(40, 24);
    model.set_cell_px(20.0f);
    const t2d::Vec2 viewport{800.0f, 600.0f};
    model.center_view(viewport);
    T2D_CHECK_NEAR(model.origin().x, 0.0f, 0.001f);
    T2D_CHECK_NEAR(model.origin().y, 60.0f, 0.001f);
    T2D_CHECK_EQ(model.cell_at_screen(t2d::Vec2{0.0f, 60.0f}), (GridPos{0, 0}));

    // A cursor that ran off the right and bottom edges is scrolled back in, with a margin.
    model.set_cursor(GridPos{39, 23});
    model.scroll_to_show(model.cursor(), viewport);
    const t2d::Vec2 top_left = model.screen_of_cell(model.cursor());
    T2D_CHECK_GE(top_left.x, 20.0f - 0.01f);
    T2D_CHECK_GE(top_left.y, 20.0f - 0.01f);
    T2D_CHECK_LT(top_left.x + 20.0f, viewport.x - 20.0f + 0.01f);
    T2D_CHECK_LT(top_left.y + 20.0f, viewport.y - 20.0f + 0.01f);
    T2D_CHECK_EQ(model.cell_at_screen(t2d::Vec2{top_left.x + 19.0f, top_left.y + 19.0f}), model.cursor());

    // A cursor at the top left is brought back too, and panning moves the layer by the same amount.
    model.set_cursor(GridPos{0, 0});
    model.scroll_to_show(model.cursor(), viewport);
    T2D_CHECK_GE(model.origin().x, 20.0f - 0.01f);
    const t2d::Vec2 before = model.origin();
    model.pan(t2d::Vec2{-5.0f, 7.0f});
    T2D_CHECK_NEAR(model.origin().x, before.x - 5.0f, 0.001f);
    T2D_CHECK_NEAR(model.origin().y, before.y + 7.0f, 0.001f);
}

T2D_TEST(a_reload_repoints_painted_cells_by_name_when_ids_move) {
    ContentRegistry registry;
    load(registry, kContentV1);
    SandboxModel model(8, 8);
    model.rebind(registry);
    T2D_CHECK_EQ(registry.find(ContentKind::Structure, "sample_a"), 1u);
    paint_named(model, GridPos{1, 1}, ContentKind::Structure, "sample_a", registry);
    paint_named(model, GridPos{2, 2}, ContentKind::Structure, "sample_b", registry);
    paint_named(model, GridPos{3, 3}, ContentKind::Machine, "sample_c", registry);

    const SandboxReloadReport report =
        model.reload_texts(registry, {kContentV2}, {"v2.ecfg"});
    T2D_CHECK(report.parsed);
    T2D_CHECK(report.clean());
    T2D_CHECK_EQ(report.added, 1u);
    T2D_CHECK_EQ(report.removed, 0u);
    T2D_CHECK_EQ(report.kept, 3u);
    T2D_CHECK_EQ(report.remapped_cells, 2u); // sample_a and sample_b both moved by one
    T2D_CHECK_EQ(report.lost_cells, 0u);
    T2D_CHECK_EQ(report.palette_size, 4u);

    // The ids really did move, and the cells followed their names rather than their numbers.
    T2D_CHECK_EQ(registry.find(ContentKind::Structure, "sample_a"), 2u);
    T2D_CHECK_EQ(registry.find(ContentKind::Structure, "sample_b"), 3u);
    T2D_CHECK_EQ(model.cell(GridPos{1, 1}).id, 2u);
    T2D_CHECK_EQ(model.cell(GridPos{1, 1}).name, std::string("sample_a"));
    T2D_CHECK_EQ(model.cell(GridPos{2, 2}).id, 3u);
    T2D_CHECK_EQ(model.cell(GridPos{3, 3}).id, 1u); // machines are numbered on their own
    T2D_CHECK_FALSE(model.cell(GridPos{1, 1}).missing());

    // The brush keeps pointing at the same content across the reload.
    model.select_palette(1);
    const std::string selected = model.selected_entry()->name;
    (void)model.reload_texts(registry, {kContentV1}, {"v1.ecfg"});
    T2D_CHECK_EQ(model.selected_entry()->name, selected);
}

T2D_TEST(a_reload_reports_content_that_is_gone_and_never_reuses_its_id) {
    ContentRegistry registry;
    load(registry, kContentV2);
    SandboxModel model(8, 8);
    model.rebind(registry);
    paint_named(model, GridPos{1, 1}, ContentKind::Structure, "sample_b", registry);

    const SandboxReloadReport report = model.reload_texts(registry, {kContentV3}, {"v3.ecfg"});
    T2D_CHECK(report.parsed);
    T2D_CHECK_EQ(report.removed, 2u); // sample_zero and sample_b
    T2D_CHECK_EQ(report.kept, 2u);
    T2D_CHECK_EQ(report.lost_cells, 1u);
    T2D_CHECK_EQ(report.added, 0u);

    const CellView cell = model.cell(GridPos{1, 1});
    T2D_CHECK(cell.missing());
    // The name survives: "this was sample_b and the file no longer has it" is the useful message.
    T2D_CHECK_EQ(cell.name, std::string("sample_b"));
    // And the id it used to hold is not handed to whatever now holds that number.
    T2D_CHECK_EQ(registry.find(ContentKind::Structure, "sample_b"), kNoContent);
    T2D_CHECK_EQ(registry.find(ContentKind::Structure, "sample_a"), 1u);
    T2D_CHECK_EQ(model.palette_count(), 2u);

    // Putting the entry back repairs the cell: it is matched by name again.
    const SandboxReloadReport restored = model.reload_texts(registry, {kContentV2}, {"v2.ecfg"});
    T2D_CHECK_EQ(restored.lost_cells, 0u);
    T2D_CHECK_FALSE(model.cell(GridPos{1, 1}).missing());
    T2D_CHECK_EQ(model.cell(GridPos{1, 1}).id, registry.find(ContentKind::Structure, "sample_b"));
}

T2D_TEST(a_broken_file_changes_neither_the_registry_nor_the_layer) {
    ContentRegistry registry;
    load(registry, kContentV1);
    SandboxModel model(8, 8);
    model.rebind(registry);
    paint_named(model, GridPos{1, 1}, ContentKind::Structure, "sample_a", registry);

    const SandboxReloadReport report = model.reload_texts(registry, {kBroken}, {"broken.ecfg"});
    T2D_CHECK_FALSE(report.parsed);
    T2D_CHECK_EQ(report.errors.size(), 1u);
    T2D_CHECK(report.errors[0].find("broken.ecfg") != std::string::npos);
    T2D_CHECK_MSG(report.errors[0].find("unquoted") != std::string::npos, "{}", report.errors[0]);

    // Nothing was cleared: a half applied reload would leave the layer pointing at a registry no file
    // describes.
    T2D_CHECK_EQ(registry.count(ContentKind::Structure), 2u);
    T2D_CHECK_EQ(model.cell(GridPos{1, 1}).id, 1u);
    T2D_CHECK_EQ(model.palette_count(), 3u);

    // A file that cannot be read at all is reported the same way.
    const SandboxReloadReport missing = model.reload(registry, {"/nonexistent/content.ecfg"});
    T2D_CHECK_FALSE(missing.parsed);
    T2D_CHECK_EQ(missing.errors.size(), 1u);
    T2D_CHECK_EQ(registry.count(ContentKind::Structure), 2u);
}

T2D_TEST(a_table_that_is_not_a_content_kind_is_reported_not_ignored) {
    ContentRegistry registry;
    SandboxModel model;
    const SandboxReloadReport report = model.reload_texts(registry, {kTypoTable}, {"typo.ecfg"});
    T2D_CHECK(report.parsed);
    T2D_CHECK_FALSE(report.clean()); // a typo must not look like a clean load
    T2D_CHECK_EQ(report.unknown_tables.size(), 1u);
    T2D_CHECK_EQ(report.unknown_tables[0], std::string("structurs"));
    T2D_CHECK_EQ(registry.total_count(), 0u);
    T2D_CHECK_EQ(report.palette_size, 0u);
}

T2D_TEST(a_layout_round_trips_through_the_save_path) {
    ContentRegistry registry;
    load(registry, kContentV1);
    SandboxModel model(6, 5);
    model.rebind(registry);
    paint_named(model, GridPos{0, 0}, ContentKind::Structure, "sample_a", registry);
    paint_named(model, GridPos{5, 4}, ContentKind::Structure, "sample_b", registry);
    paint_named(model, GridPos{3, 2}, ContentKind::Machine, "sample_c", registry);

    const std::vector<u8> bytes = model.serialize();
    T2D_CHECK_GT(bytes.size(), 16u);

    SandboxModel loaded(1, 1);
    const SandboxLoadReport report = loaded.deserialize(span_of(bytes), registry);
    T2D_CHECK_MSG(report.ok, "{}", report.error);
    T2D_CHECK_EQ(report.translated, 3u);
    T2D_CHECK_EQ(report.missing, 0u);
    T2D_CHECK_EQ(loaded.width(), 6u);
    T2D_CHECK_EQ(loaded.height(), 5u);
    for (u32 y = 0; y < 5; ++y) {
        for (u32 x = 0; x < 6; ++x) {
            const GridPos pos{static_cast<i32>(x), static_cast<i32>(y)};
            T2D_CHECK(loaded.cell(pos) == model.cell(pos));
        }
    }
    T2D_CHECK_EQ(loaded.dump_text(), model.dump_text());
}

T2D_TEST(a_saved_layout_survives_the_ids_moving_underneath_it) {
    ContentRegistry old_registry;
    load(old_registry, kContentV1);
    SandboxModel model(6, 5);
    model.rebind(old_registry);
    paint_named(model, GridPos{1, 1}, ContentKind::Structure, "sample_a", old_registry);
    paint_named(model, GridPos{2, 2}, ContentKind::Structure, "sample_b", old_registry);
    const std::vector<u8> bytes = model.serialize();

    // The designer inserted an entry: every structure id moved, so the saved numbers now mean
    // something else. The layout still loads, because it carried the table it was written with.
    ContentRegistry new_registry;
    load(new_registry, kContentV2);
    T2D_CHECK_EQ(new_registry.find(ContentKind::Structure, "sample_a"), 2u);

    SandboxModel loaded(1, 1);
    const SandboxLoadReport report = loaded.deserialize(span_of(bytes), new_registry);
    T2D_CHECK_MSG(report.ok, "{}", report.error);
    T2D_CHECK_EQ(report.translated, 2u);
    T2D_CHECK_EQ(report.missing, 0u);
    T2D_CHECK_EQ(loaded.cell(GridPos{1, 1}).name, std::string("sample_a"));
    T2D_CHECK_EQ(loaded.cell(GridPos{1, 1}).id, 2u);
    T2D_CHECK_EQ(loaded.cell(GridPos{2, 2}).name, std::string("sample_b"));
    T2D_CHECK_EQ(loaded.cell(GridPos{2, 2}).id, 3u);

    // Content the new version dropped is reported, and the cell keeps the name it had.
    ContentRegistry reduced;
    load(reduced, kContentV3);
    SandboxModel older(1, 1);
    const SandboxLoadReport missing = older.deserialize(span_of(bytes), reduced);
    T2D_CHECK(missing.ok);
    T2D_CHECK_EQ(missing.translated, 1u);
    T2D_CHECK_EQ(missing.missing, 1u);
    T2D_CHECK(older.cell(GridPos{2, 2}).missing());
    T2D_CHECK_EQ(older.cell(GridPos{2, 2}).name, std::string("sample_b"));
}

T2D_TEST(a_layout_file_it_cannot_trust_is_refused_and_nothing_moves) {
    ContentRegistry registry;
    load(registry, kContentV1);
    SandboxModel model(4, 4);
    model.rebind(registry);
    paint_named(model, GridPos{1, 1}, ContentKind::Structure, "sample_a", registry);
    const std::vector<u8> good = model.serialize();

    SandboxModel target(4, 4);
    const auto refused = [&](ConstSpan<const u8> data, std::string_view expected) {
        const SandboxLoadReport report = target.deserialize(data, registry);
        T2D_CHECK_FALSE(report.ok);
        T2D_CHECK_FALSE(report.error.empty());
        T2D_CHECK_MSG(report.error.find(expected) != std::string::npos, "'{}' does not mention '{}'",
                      report.error, expected);
        // The layer it was asked to replace is untouched.
        T2D_CHECK(target.cell(GridPos{0, 0}).empty());
        T2D_CHECK_EQ(target.filled_cells(), 0u);
        T2D_CHECK_EQ(target.width(), 4u);
    };

    // at() rather than []: GCC 16 at -O3 decides that indexing a vector copy could be a null
    // dereference and refuses to build the release preset.
    std::vector<u8> wrong_magic = good;
    wrong_magic.at(0) ^= 0xFFu;
    refused(span_of(wrong_magic), "magic");

    std::vector<u8> wrong_version = good;
    wrong_version.at(4) = 99u;
    refused(span_of(wrong_version), "version");

    refused(ConstSpan<const u8>(good.data(), good.size() - 4), "table");

    std::vector<u8> trailing = good;
    trailing.push_back(0u);
    refused(span_of(trailing), "trailing");

    // A map size that would ask for a gigabyte, and a cell count that does not match it.
    const auto write_header = [](t2d::ByteWriter& writer, u32 width, u32 height, u32 layers, u32 count) {
        writer.write_u32(0x3242534Du);
        writer.write_u8(3u);
        writer.write_varint(width);
        writer.write_varint(height);
        writer.write_varint(layers);
        writer.write_varint(count);
    };
    std::vector<u8> absurd;
    t2d::ByteWriter writer(absurd);
    write_header(writer, 100000u, 100000u, 1u, 1u);
    refused(span_of(absurd), "size");

    std::vector<u8> mismatched;
    t2d::ByteWriter mismatch_writer(mismatched);
    write_header(mismatch_writer, 2u, 2u, 1u, 5u);
    refused(span_of(mismatched), "cell count");

    // A layer count no mask could address.
    std::vector<u8> absurd_layers;
    t2d::ByteWriter layers_writer(absurd_layers);
    write_header(layers_writer, 1u, 1u, 33u, 1u);
    refused(span_of(absurd_layers), "layer count");

    std::vector<u8> bad_kind;
    t2d::ByteWriter kind_writer(bad_kind);
    write_header(kind_writer, 1u, 1u, 1u, 1u);
    kind_writer.write_u8(200u);
    kind_writer.write_varint(1u);
    refused(span_of(bad_kind), "unknown kind");

    // A table that is not a table: the cells read fine, the name -> id table does not.
    std::vector<u8> bad_table;
    t2d::ByteWriter table_writer(bad_table);
    write_header(table_writer, 1u, 1u, 1u, 1u);
    table_writer.write_u8(0xFFu);
    const u8 garbage[4] = {0xDEu, 0xADu, 0xBEu, 0xEFu};
    table_writer.write_varint(4u);
    table_writer.write_bytes(ConstSpan<const u8>(garbage, 4));
    refused(span_of(bad_table), "unreadable");

    // An empty buffer is refused too, and the valid one still loads after all of that.
    refused(ConstSpan<const u8>(), "magic");
    T2D_CHECK(target.deserialize(span_of(good), registry).ok);
    T2D_CHECK_EQ(target.filled_cells(), 1u);
}

T2D_TEST(the_dump_lists_the_cells_and_the_palette) {
    ContentRegistry registry;
    load(registry, kContentV1);
    SandboxModel model(4, 4);
    model.rebind(registry);
    model.set_content_paths({"content.ecfg"});
    paint_named(model, GridPos{2, 3}, ContentKind::Structure, "sample_a", registry);

    const std::string dump = model.dump_text();
    T2D_CHECK(dump.find("sandbox map 4x4, 1 tile layer(s)") != std::string::npos);
    T2D_CHECK(dump.find("content.ecfg") != std::string::npos);
    T2D_CHECK(dump.find("palette: 3 entries") != std::string::npos);
    T2D_CHECK(dump.find("L0 2,3 structure #1 sample_a") != std::string::npos);
    T2D_CHECK(dump.find("sample_c") != std::string::npos);
    T2D_CHECK(dump.find("(missing)") == std::string::npos);
}

T2D_TEST(debug_colours_are_stable_readable_and_spread_out) {
    T2D_CHECK_EQ(debug_color_for("sample_a"), debug_color_for("sample_a"));
    T2D_CHECK_NE(debug_color_for("sample_a"), debug_color_for("sample_b"));
    T2D_CHECK_EQ(debug_color_for("sample_a") >> 24, 0xFFu);

    std::vector<u32> colours;
    colours.reserve(64);
    for (u32 index = 0; index < 64; ++index) colours.push_back(debug_color_for(std::format("name_{}", index)));
    for (const u32 colour : colours) {
        const u32 sum = (colour & 0xFFu) + ((colour >> 8) & 0xFFu) + ((colour >> 16) & 0xFFu);
        T2D_CHECK_GT(sum, 60u);  // never near black on a dark background
        T2D_CHECK_LT(sum, 700u); // never white either
    }
    std::vector<u32> sorted = colours;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    T2D_CHECK_GE(sorted.size(), 48u); // 64 names must not collapse into a handful of colours
    T2D_CHECK_EQ(debug_color_for(""), 0xFF3C3C3Cu);
}

T2D_TEST(the_debug_fills_are_content_agnostic_and_deterministic) {
    ContentRegistry registry;
    load(registry, kContentV1);
    SandboxModel model(12, 8);
    model.rebind(registry);

    // One band per palette entry, in palette order.
    model.fill_bands();
    T2D_CHECK_EQ(model.filled_cells(), 12u * 8u);
    T2D_CHECK_EQ(model.cell(GridPos{0, 0}).name, std::string("sample_a"));
    T2D_CHECK_EQ(model.cell(GridPos{0, 7}).name, std::string("sample_c"));
    T2D_CHECK_EQ(model.cell(GridPos{11, 4}).name, std::string("sample_b"));

    model.fill_scatter(7u);
    const std::string first = model.dump_text();
    T2D_CHECK_GT(model.filled_cells(), 0u);
    T2D_CHECK_LT(model.filled_cells(), 12u * 8u);
    model.fill_scatter(7u);
    T2D_CHECK_EQ(model.dump_text(), first); // same seed, same layer
    model.fill_scatter(8u);
    T2D_CHECK_NE(model.dump_text(), first);

    // Nothing registered: both fills empty the layer instead of inventing content.
    ContentRegistry empty;
    model.rebind(empty);
    model.fill_scatter(7u);
    T2D_CHECK_EQ(model.filled_cells(), 0u);
    model.fill_bands();
    T2D_CHECK_EQ(model.filled_cells(), 0u);
}

T2D_TEST(resizing_keeps_what_still_fits) {
    ContentRegistry registry;
    load(registry, kContentV1);
    SandboxModel model(4, 4);
    model.rebind(registry);
    paint_named(model, GridPos{3, 3}, ContentKind::Structure, "sample_a", registry);
    paint_named(model, GridPos{0, 0}, ContentKind::Structure, "sample_b", registry);
    model.set_cursor(GridPos{3, 3});

    model.resize(2, 2);
    T2D_CHECK_EQ(model.width(), 2u);
    T2D_CHECK_EQ(model.height(), 2u);
    T2D_CHECK_EQ(model.filled_cells(), 1u); // the cell at 3,3 fell outside
    T2D_CHECK_EQ(model.cell(GridPos{0, 0}).name, std::string("sample_b"));
    T2D_CHECK_EQ(model.cursor(), (GridPos{1, 1})); // the cursor is pulled back inside

    model.resize(6, 6);
    T2D_CHECK_EQ(model.width(), 6u);
    T2D_CHECK_EQ(model.cell(GridPos{0, 0}).name, std::string("sample_b"));
    T2D_CHECK_EQ(model.cell(GridPos{5, 5}).name, std::string{});
}

T2D_TEST(the_map_has_tile_layers_and_the_brush_writes_into_the_active_one) {
    ContentRegistry registry;
    load(registry, kContentV1);
    SandboxModel model(8, 6, 3);
    T2D_CHECK_EQ(model.layer_count(), 3);
    T2D_CHECK_EQ(model.active_layer(), 0);

    model.rebind(registry);
    model.set_cursor(GridPos{2, 2});
    T2D_CHECK(model.paint());                                  // sample_a on layer 0
    T2D_CHECK_EQ(model.cell(0, GridPos{2, 2}).name, std::string("sample_a"));
    T2D_CHECK_EQ(model.cell(1, GridPos{2, 2}).name, std::string{});

    model.set_active_layer(2);
    model.cycle_palette(2);                                    // sample_c, a machine
    T2D_CHECK(model.paint());
    T2D_CHECK_EQ(model.cell(2, GridPos{2, 2}).name, std::string("sample_c"));
    T2D_CHECK_EQ(model.cell(0, GridPos{2, 2}).name, std::string("sample_a"));   // layer 0 is untouched

    // The unqualified cell() is the topmost non-empty one: what the screen shows is what it reports.
    T2D_CHECK_EQ(model.cell(GridPos{2, 2}).name, std::string("sample_c"));
    model.clear_layer(2);
    T2D_CHECK_EQ(model.cell(GridPos{2, 2}).name, std::string("sample_a"));      // only layer 0 is left

    // Painting on a higher layer puts that layer on top of the stack.
    model.set_active_layer(1);
    model.cycle_palette(-1);                                   // sample_b
    T2D_CHECK(model.paint());
    T2D_CHECK_EQ(model.cell(1, GridPos{2, 2}).name, std::string("sample_b"));
    T2D_CHECK_EQ(model.cell(GridPos{2, 2}).name, std::string("sample_b"));
    model.set_active_layer(2);
    model.cycle_palette(1);                                    // sample_c again
    T2D_CHECK(model.paint());
    T2D_CHECK_EQ(model.cell(GridPos{2, 2}).name, std::string("sample_c"));

    T2D_CHECK_EQ(model.filled_cells(), 3u);
    T2D_CHECK_EQ(model.filled_cells(0), 1u);
    T2D_CHECK_EQ(model.filled_cells(1), 1u);
    T2D_CHECK_EQ(model.filled_cells(2), 1u);
    T2D_CHECK_EQ(model.filled_cells(7), 0u);                   // a layer that does not exist

    // Clearing one layer leaves the others alone.
    model.clear_layer(1);
    T2D_CHECK_EQ(model.filled_cells(), 2u);
    T2D_CHECK_EQ(model.cell(GridPos{2, 2}).name, std::string("sample_c"));
    T2D_CHECK_EQ(model.cell(1, GridPos{2, 2}).name, std::string{});
    model.clear_cells();
    T2D_CHECK_EQ(model.filled_cells(), 0u);

    // Layer numbers wrap, and the count is clamped to what the framework's maps can hold.
    model.set_active_layer(0);
    model.cycle_layer(-1);
    T2D_CHECK_EQ(model.active_layer(), 2);
    model.cycle_layer(1);
    T2D_CHECK_EQ(model.active_layer(), 0);
    T2D_CHECK_EQ(SandboxModel(4, 4, 99).layer_count(), SandboxModel::kMaxLayers);
    T2D_CHECK_EQ(SandboxModel(4, 4, 0).layer_count(), 1);
    T2D_CHECK_EQ(SandboxModel(4, 4).layer_count(), 1);         // the one layer default

    // Writing to a layer that does not exist is refused, not redirected.
    model.set_cell(9, GridPos{1, 1}, ContentRef{ContentKind::Structure, 1, false});
    T2D_CHECK_EQ(model.filled_cells(), 0u);
}

T2D_TEST(erasing_removes_what_is_on_top) {
    ContentRegistry registry;
    load(registry, kContentV1);
    SandboxModel model(4, 4, 2);
    model.rebind(registry);
    model.set_cursor(GridPos{1, 1});
    paint_named(model, GridPos{1, 1}, ContentKind::Structure, "sample_a", registry);   // active layer 0
    model.set_active_layer(1);
    paint_named(model, GridPos{1, 1}, ContentKind::Structure, "sample_b", registry);

    // Erase takes the topmost cell, whichever layer holds it: the visible one goes first.
    T2D_CHECK(model.erase());
    T2D_CHECK(model.cell(1, GridPos{1, 1}).empty());
    T2D_CHECK_EQ(model.cell(0, GridPos{1, 1}).name, std::string("sample_a"));
    T2D_CHECK(model.erase());
    T2D_CHECK(model.cell(0, GridPos{1, 1}).empty());
    T2D_CHECK_FALSE(model.erase());
    T2D_CHECK_EQ(model.filled_cells(), 0u);
}

T2D_TEST(a_layout_keeps_every_tile_layer) {
    ContentRegistry registry;
    load(registry, kContentV1);
    SandboxModel model(6, 5, 3);
    model.rebind(registry);
    paint_named(model, GridPos{0, 0}, ContentKind::Structure, "sample_a", registry);
    model.set_active_layer(1);
    paint_named(model, GridPos{1, 1}, ContentKind::Structure, "sample_b", registry);
    paint_named(model, GridPos{5, 4}, ContentKind::Structure, "sample_b", registry);
    model.set_active_layer(2);
    paint_named(model, GridPos{2, 2}, ContentKind::Machine, "sample_c", registry);
    model.set_active_layer(2);

    const std::vector<u8> bytes = model.serialize();
    SandboxModel loaded(1, 1);
    const SandboxLoadReport report = loaded.deserialize(span_of(bytes), registry);
    T2D_CHECK_MSG(report.ok, "{}", report.error);
    T2D_CHECK_EQ(report.translated, 4u);
    T2D_CHECK_EQ(report.missing, 0u);
    T2D_CHECK_EQ(loaded.layer_count(), 3);
    T2D_CHECK_EQ(loaded.active_layer(), 0);      // a loaded map starts on its base layer
    T2D_CHECK_EQ(loaded.width(), 6u);
    T2D_CHECK_EQ(loaded.height(), 5u);
    for (i32 layer = 0; layer < 3; ++layer) {
        for (u32 y = 0; y < 5; ++y) {
            for (u32 x = 0; x < 6; ++x) {
                const GridPos pos{static_cast<i32>(x), static_cast<i32>(y)};
                T2D_CHECK(loaded.cell(layer, pos) == model.cell(layer, pos));
            }
        }
    }
    T2D_CHECK_EQ(loaded.filled_cells(0), 1u);
    T2D_CHECK_EQ(loaded.filled_cells(1), 2u);
    T2D_CHECK_EQ(loaded.filled_cells(2), 1u);

    // The dump says which layer each cell is on.
    const std::string dump = loaded.dump_text();
    T2D_CHECK(dump.find("3 tile layer(s)") != std::string::npos);
    T2D_CHECK(dump.find("L0 0,0 structure #1 sample_a") != std::string::npos);
    T2D_CHECK(dump.find("L1 1,1 structure #2 sample_b") != std::string::npos);
    T2D_CHECK(dump.find("L2 2,2 machine #1 sample_c") != std::string::npos);
}

T2D_TEST(a_reload_repoints_cells_on_every_tile_layer) {
    ContentRegistry registry;
    load(registry, kContentV1);
    SandboxModel model(6, 5, 2);
    model.rebind(registry);
    paint_named(model, GridPos{1, 1}, ContentKind::Structure, "sample_a", registry);
    model.set_active_layer(1);
    paint_named(model, GridPos{2, 2}, ContentKind::Structure, "sample_b", registry);

    // v2 inserts a structure at the top of the table: both layers' ids move, both must follow.
    const SandboxReloadReport report = model.reload_texts(registry, {kContentV2}, {"v2.ecfg"});
    T2D_CHECK(report.parsed);
    T2D_CHECK_EQ(report.remapped_cells, 2u);
    T2D_CHECK_EQ(report.lost_cells, 0u);
    T2D_CHECK_EQ(model.cell(0, GridPos{1, 1}).id, registry.find(ContentKind::Structure, "sample_a"));
    T2D_CHECK_EQ(model.cell(1, GridPos{2, 2}).id, registry.find(ContentKind::Structure, "sample_b"));
    T2D_CHECK_EQ(model.cell(0, GridPos{1, 1}).name, std::string("sample_a"));
    T2D_CHECK_EQ(model.cell(1, GridPos{2, 2}).name, std::string("sample_b"));

    // And content that disappeared is marked missing wherever it sits.
    const SandboxReloadReport removed = model.reload_texts(registry, {kContentV3}, {"v3.ecfg"});
    T2D_CHECK_EQ(removed.lost_cells, 1u);
    T2D_CHECK(model.cell(1, GridPos{2, 2}).missing());
    T2D_CHECK_FALSE(model.cell(0, GridPos{1, 1}).missing());
}

T2D_TEST(the_debug_fills_only_touch_the_active_layer) {
    ContentRegistry registry;
    load(registry, kContentV1);
    SandboxModel model(12, 8, 2);
    model.rebind(registry);
    model.set_active_layer(1);
    model.fill_bands();
    T2D_CHECK_EQ(model.filled_cells(1), 12u * 8u);
    T2D_CHECK_EQ(model.filled_cells(0), 0u);
    T2D_CHECK_EQ(model.cell(GridPos{0, 0}).name, std::string("sample_a"));   // topmost is layer 1

    model.fill_scatter(7u);
    const std::string first = model.dump_text();
    T2D_CHECK_GT(model.filled_cells(1), 0u);
    T2D_CHECK_EQ(model.filled_cells(0), 0u);
    model.fill_scatter(7u);
    T2D_CHECK_EQ(model.dump_text(), first);

    // A fill with nothing in the palette empties the active layer and leaves the others alone.
    model.set_active_layer(0);
    paint_named(model, GridPos{3, 3}, ContentKind::Structure, "sample_a", registry);
    T2D_CHECK_EQ(model.filled_cells(0), 1u);
    const usize scattered = model.filled_cells(1);
    T2D_CHECK_GT(scattered, 0u);
    ContentRegistry empty;
    model.rebind(empty);
    model.fill_scatter(7u);
    T2D_CHECK_EQ(model.filled_cells(0), 0u);      // the active layer was cleared
    T2D_CHECK_EQ(model.filled_cells(1), scattered);
}

T2D_TEST(a_map_of_hundreds_of_cells_per_side_costs_four_bytes_a_cell) {
    SandboxModel model(512, 512, 4);
    T2D_CHECK_EQ(model.width(), 512u);
    T2D_CHECK_EQ(model.height(), 512u);
    T2D_CHECK_EQ(model.layer_count(), 4);
    // Nothing painted yet: a layer nobody wrote to has no storage at all, and the fill count is a
    // counter rather than a walk over 262144 cells (the status line asks for it every frame).
    T2D_CHECK_EQ(model.cell_bytes(), 0u);
    T2D_CHECK_EQ(model.filled_cells(), 0u);

    ContentRegistry registry;
    load(registry, kContentV1);
    model.rebind(registry);
    model.set_cursor(GridPos{511, 511});
    T2D_CHECK(model.paint());
    T2D_CHECK_EQ(model.cell_bytes(), 512u * 512u * 4u);   // one layer of the four
    T2D_CHECK_EQ(model.filled_cells(), 1u);
    T2D_CHECK_EQ(model.cell(GridPos{511, 511}).name, std::string("sample_a"));

    model.set_active_layer(3);
    model.set_cursor(GridPos{0, 0});
    T2D_CHECK(model.paint());
    T2D_CHECK_EQ(model.cell_bytes(), 2u * 512u * 512u * 4u);
    T2D_CHECK_EQ(model.filled_cells(), 2u);
    T2D_CHECK_EQ(model.filled_cells(3), 1u);

    // The same map under the old storage - a name per cell - was forty bytes a cell and a heap
    // allocation for every painted one; the whole map is four bytes a cell per layer used.
    model.clear_cells();
    T2D_CHECK_EQ(model.cell_bytes(), 0u);
    T2D_CHECK_EQ(model.filled_cells(), 0u);
}

T2D_TEST(the_camera_can_show_a_map_of_hundreds_of_cells_at_once) {
    SandboxModel model(512, 512);
    const t2d::Vec2 viewport{1280.0f, 720.0f};
    // Fitting a map this size needs a zoom below one pixel per cell. The zoom range has to allow it,
    // or the whole feature is "you can never see your map". The frame then draws a sampled overview.
    T2D_CHECK_LT(static_cast<f32>(SandboxModel::kMaxDimension) * SandboxModel::kMinCellPx, 1280.0f);
    const f32 cell = std::min(viewport.x / 512.0f, viewport.y / 512.0f);
    model.set_cell_px(cell);
    T2D_CHECK_NEAR(model.cell_px(), cell, 0.0005f);
    model.center_view(viewport);

    // Every cell of the map is inside the view. The height is the tighter axis, so the view is wider
    // than the map on both sides - which is exactly what fitting means.
    const t2d::TileRect visible = model.visible_cells();
    T2D_CHECK(visible.contains(0, 0));
    T2D_CHECK(visible.contains(511, 511));
    T2D_CHECK_LT(visible.x, 0);
    T2D_CHECK_NEAR(static_cast<f32>(visible.height), 512.0f, 1.0f);

    // A cursor walked to the far corner is brought on screen at that zoom, and the cell under a
    // screen point survives zooming in on it.
    model.set_cursor(GridPos{511, 511});
    model.scroll_to_show(model.cursor(), viewport);
    const t2d::Vec2 at = model.screen_of_cell(model.cursor());
    T2D_CHECK_GE(at.x, 0.0f);
    T2D_CHECK_LT(at.x, viewport.x);
    T2D_CHECK_GE(at.y, 0.0f);
    T2D_CHECK_LT(at.y, viewport.y);

    const t2d::Vec2 anchor{100.0f, 100.0f};
    const GridPos under = model.cell_at_screen(anchor);
    model.zoom_at(anchor, 8.0f);
    T2D_CHECK_EQ(model.cell_at_screen(anchor), under);
    T2D_CHECK_NEAR(model.cell_px(), cell * 8.0f, 0.001f);
    T2D_CHECK_LT(model.visible_cells().width, visible.width);   // zooming in shows fewer cells
}

T2D_TEST(the_draw_step_keeps_a_big_map_inside_the_quad_budget) {
    // A small map is drawn cell by cell, whatever the layer count.
    T2D_CHECK_EQ(draw_step_for(100, 1, 16384), 1);
    T2D_CHECK_EQ(draw_step_for(16384, 1, 16384), 1);
    // Past the budget one quad covers a block of cells, and the block grows with the square root.
    T2D_CHECK_EQ(draw_step_for(16385, 1, 16384), 2);
    T2D_CHECK_EQ(draw_step_for(65536, 1, 16384), 2);
    T2D_CHECK_EQ(draw_step_for(65537, 1, 16384), 3);
    // Every layer draws the same range, so eight layers need eight times the step.
    T2D_CHECK_EQ(draw_step_for(16385, 4, 16384), 3);
    T2D_CHECK_EQ(draw_step_for(16385, 8, 16384), 3);
    // The whole of a 512x512 map at a zoom that shows all of it.
    const i32 step = draw_step_for(512u * 512u, 4, 16384);
    T2D_CHECK_GE(step, 8);
    const i32 per_axis = (512 + step - 1) / step;   // how many blocks one row of the map becomes
    T2D_CHECK_GE(16384, per_axis * per_axis * 4);   // the whole stack stays inside the budget
    // A caller that asks for nothing still gets a usable step rather than a division by zero.
    T2D_CHECK_EQ(draw_step_for(0, 0, 0), 1);
    T2D_CHECK_EQ(draw_step_for(1000000, -3, 16384), draw_step_for(1000000, 1, 16384));
}

T2D_TEST(a_layout_written_while_content_was_missing_repairs_itself) {
    ContentRegistry full;
    load(full, kContentV2);
    SandboxModel model(4, 4);
    model.rebind(full);
    paint_named(model, GridPos{1, 1}, ContentKind::Structure, "sample_b", full);

    // The designer deletes sample_b and reloads: the cell is marked missing and keeps the id it was
    // placed with, so the entry coming back can repair it.
    ContentRegistry reduced;
    (void)model.reload_texts(reduced, {kContentV3}, {"v3.ecfg"});
    T2D_CHECK(model.cell(GridPos{1, 1}).missing());
    T2D_CHECK_EQ(model.cell(GridPos{1, 1}).name, std::string("sample_b"));

    // A layout written now carries the name of what went missing, so loading it into a registry that
    // has the content again brings the cell back instead of losing it for good.
    const std::vector<u8> bytes = model.serialize();
    ContentRegistry restored;
    load(restored, kContentV2);
    SandboxModel loaded(1, 1);
    const SandboxLoadReport report = loaded.deserialize(span_of(bytes), restored);
    T2D_CHECK_MSG(report.ok, "{}", report.error);
    T2D_CHECK_EQ(report.missing, 0u);
    T2D_CHECK_EQ(report.translated, 1u);
    T2D_CHECK_FALSE(loaded.cell(GridPos{1, 1}).missing());
    T2D_CHECK_EQ(loaded.cell(GridPos{1, 1}).name, std::string("sample_b"));
    T2D_CHECK_EQ(loaded.cell(GridPos{1, 1}).id, restored.find(ContentKind::Structure, "sample_b"));
}

T2D_TEST(the_playtest_pointer_selects_what_is_under_it_and_nothing_off_the_map) {
    SandboxModel model(8, 6);
    model.set_cell_px(10.0f);
    model.set_origin(t2d::Vec2{0.0f, 0.0f});

    // Nothing is pointed at until a pointer arrives: a playtest that has not seen a mouse is not
    // pointing at cell (0,0) by accident.
    T2D_CHECK_FALSE(model.hovered().has_value());

    // The middle of a cell selects that cell, and so does its far corner.
    T2D_CHECK_EQ(model.cell_at_screen(t2d::Vec2{25.0f, 35.0f}), (GridPos{2, 3}));
    model.point_at(t2d::Vec2{25.0f, 35.0f});
    T2D_REQUIRE(model.hovered().has_value());
    T2D_CHECK_EQ(*model.hovered(), (GridPos{2, 3}));
    model.point_at(t2d::Vec2{29.9f, 39.9f});
    T2D_CHECK_EQ(*model.hovered(), (GridPos{2, 3}));

    // Off the map is not a cell, and it is not clamped to the nearest edge either: pointing at the
    // void beside a small map must not select its last column.
    model.point_at(t2d::Vec2{-1.0f, 5.0f});
    T2D_CHECK_FALSE(model.hovered().has_value());
    model.point_at(t2d::Vec2{5.0f, -1.0f});
    T2D_CHECK_FALSE(model.hovered().has_value());
    model.point_at(t2d::Vec2{80.0f, 35.0f});   // one pixel past the right edge of an 8 wide map
    T2D_CHECK_FALSE(model.hovered().has_value());
    model.point_at(t2d::Vec2{79.0f, 35.0f});
    T2D_CHECK_EQ(*model.hovered(), (GridPos{7, 3}));

    // A scripted run has no mouse, so it can point at a cell directly. A cell that is not on the map
    // leaves the pointer pointing at nothing, exactly like a real one would.
    model.point_at_cell(GridPos{1, 1});
    T2D_CHECK_EQ(*model.hovered(), (GridPos{1, 1}));
    model.point_at_cell(GridPos{8, 1});
    T2D_CHECK_FALSE(model.hovered().has_value());
    model.point_at_cell(GridPos{-1, 1});
    T2D_CHECK_FALSE(model.hovered().has_value());

    // The pointer is a screen position, not a remembered cell: the same mouse position after a pan is
    // over whatever the camera has moved under it. This is why the app points again every frame.
    model.point_at_cell(GridPos{2, 3});
    model.point_at(t2d::Vec2{25.0f, 35.0f});
    T2D_CHECK_EQ(*model.hovered(), (GridPos{2, 3}));
    model.pan(t2d::Vec2{-30.0f, 0.0f});   // dragging left moves the view right by three cells
    T2D_CHECK_EQ(model.cell_at_screen(t2d::Vec2{25.0f, 35.0f}), (GridPos{5, 3}));
    model.point_at(t2d::Vec2{25.0f, 35.0f});
    T2D_CHECK_EQ(*model.hovered(), (GridPos{5, 3}));

    // And leaving the playtest forgets it: a pointer from before the chrome came back would be drawn
    // over a cell nobody is pointing at.
    model.clear_pointer();
    T2D_CHECK_FALSE(model.hovered().has_value());

    // The pointer is not the cursor: pointing moves nothing the brush writes at.
    model.set_cursor(GridPos{4, 4});
    model.point_at(t2d::Vec2{25.0f, 35.0f});
    T2D_CHECK_EQ(model.cursor(), (GridPos{4, 4}));
}

T2D_TEST(changing_the_viewport_keeps_the_cell_the_view_is_centred_on) {
    SandboxModel model(40, 24);
    model.set_cell_px(20.0f);
    const t2d::Vec2 editor{800.0f, 500.0f};
    model.center_view(editor);
    // Somewhere specific, so the check is not about the middle of the map.
    model.look_at(GridPos{11, 7});
    const GridPos centred = model.cell_at_screen(editor * 0.5f);
    T2D_CHECK_EQ(centred, (GridPos{11, 7}));

    // The playtest has the whole window instead of the editor's chrome: the viewport grows, the cell
    // in the middle stays the one that was being looked at, and the extra space shows more map.
    const t2d::Vec2 playtest{1280.0f, 720.0f};
    model.set_viewport(playtest);
    T2D_CHECK_EQ(model.cell_at_screen(playtest * 0.5f), centred);
    const t2d::TileRect wider = model.visible_cells();
    model.set_viewport(editor);
    const t2d::TileRect narrower = model.visible_cells();
    T2D_CHECK_GT(wider.width, narrower.width);
    T2D_CHECK_GT(wider.height, narrower.height);
}

T2D_TEST_MAIN