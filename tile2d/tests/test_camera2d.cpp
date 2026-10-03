// Tile2D - the camera: screen and world coordinates, zooming about an anchor, fitting a map of
// hundreds of cells into a viewport and keeping a bounded view inside it. Pure arithmetic, so the
// whole suite runs in milliseconds and the numbers can be checked exactly.
#include <support/test_support.h>

#include <t2d/core/camera2d.h>

#include <cmath>

using namespace t2d;

namespace {

/// A viewport the size of the documentation screenshots.
constexpr Vec2 kViewport{1280.0f, 720.0f};

/// The world of a 512x512 cell map, one unit per cell: "hundreds of cells per side" is the size the
/// game is built for (docs/GAME_DESIGN.md section 2).
[[nodiscard]] constexpr Aabb2 big_map() { return Aabb2{Vec2{0.0f, 0.0f}, Vec2{512.0f, 512.0f}}; }

} // namespace

T2D_TEST(screen_and_world_coordinates_are_inverse) {
    Camera2D camera(kViewport, 16.0f, Vec2{20.0f, 12.0f});
    for (i32 y = 0; y < 24; ++y) {
        for (i32 x = 0; x < 40; ++x) {
            const Vec2 screen = camera.screen_of(Vec2{static_cast<f32>(x), static_cast<f32>(y)});
            const Vec2 back = camera.world_of(screen);
            T2D_CHECK_NEAR(back.x, static_cast<f32>(x), 0.0005f);
            T2D_CHECK_NEAR(back.y, static_cast<f32>(y), 0.0005f);
        }
    }
    // The centre of the viewport is the camera's world position.
    const Vec2 middle = camera.world_of(camera.viewport_centre());
    T2D_CHECK_NEAR(middle.x, camera.centre().x, 0.0005f);
    T2D_CHECK_NEAR(middle.y, camera.centre().y, 0.0005f);
}

T2D_TEST(the_visible_rectangle_and_the_culling_range_follow_the_view) {
    Camera2D camera(kViewport, 20.0f, Vec2{40.0f, 30.0f});
    const Aabb2 visible = camera.visible_world();
    // 1280 / 20 = 64 cells wide, 720 / 20 = 36 cells tall, centred on (40, 30).
    T2D_CHECK_NEAR(visible.min.x, 8.0f, 0.0005f);
    T2D_CHECK_NEAR(visible.max.x, 72.0f, 0.0005f);
    T2D_CHECK_NEAR(visible.min.y, 12.0f, 0.0005f);
    T2D_CHECK_NEAR(visible.max.y, 48.0f, 0.0005f);

    const TileRect cells = camera.visible_cells();
    T2D_CHECK_EQ(cells.x, 8);
    T2D_CHECK_EQ(cells.y, 12);
    T2D_CHECK_EQ(cells.width, 64);
    T2D_CHECK_EQ(cells.height, 36);
    // The range is half open and covers everything visible: the cell the view ends inside counts.
    T2D_CHECK(cells.contains(71, 47));
    T2D_CHECK(!cells.contains(72, 48));

    // A camera looking past the edge of the map reports negative cells instead of clamping: a cursor
    // may sit outside the map, and only the caller knows what to draw there.
    camera.set_centre(Vec2{-10.0f, -10.0f});
    T2D_CHECK_EQ(camera.visible_cells().x, -42);
    T2D_CHECK_EQ(camera.visible_cells().y, -28);

    // A fractional edge cell is included rather than dropped, or a column of tiles would flicker at
    // the edge of the screen while the camera moves: the view covers [-21.5, 42.5) cells here.
    camera.set_centre(Vec2{10.5f, 10.5f});
    const TileRect fractional = camera.visible_cells();
    T2D_CHECK_EQ(fractional.x, -22);
    T2D_CHECK_EQ(fractional.width, 65);
    T2D_CHECK_NEAR(static_cast<f32>(fractional.x), std::floor(camera.visible_world().min.x), 0.0005f);
}

T2D_TEST(zooming_keeps_the_world_point_under_the_anchor_and_respects_the_limits) {
    Camera2D camera(kViewport, 20.0f, Vec2{40.0f, 30.0f});
    const Vec2 anchor{300.0f, 200.0f};
    const Vec2 under = camera.world_of(anchor);
    camera.zoom_at(anchor, 2.0f);
    T2D_CHECK_NEAR(camera.zoom(), 40.0f, 0.0005f);
    const Vec2 after = camera.world_of(anchor);
    T2D_CHECK_NEAR(after.x, under.x, 0.0005f);
    T2D_CHECK_NEAR(after.y, under.y, 0.0005f);

    // Zooming about the centre of the viewport keeps whatever the centre is showing.
    const Vec2 centre_world = camera.world_of(camera.viewport_centre());
    camera.zoom_by(0.25f);
    T2D_CHECK_NEAR(camera.zoom(), 10.0f, 0.0005f);
    T2D_CHECK_NEAR(camera.world_of(camera.viewport_centre()).x, centre_world.x, 0.0005f);
    T2D_CHECK_NEAR(camera.world_of(camera.viewport_centre()).y, centre_world.y, 0.0005f);

    camera.set_zoom_limits(2.0f, 64.0f);
    camera.set_zoom(1000.0f);
    T2D_CHECK_NEAR(camera.zoom(), 64.0f, 0.0005f);
    camera.set_zoom(0.0f);
    T2D_CHECK_NEAR(camera.zoom(), 2.0f, 0.0005f);
    // A factor the limits swallow must not move the view either.
    const Vec2 centre_before = camera.centre();
    camera.zoom_at(anchor, 0.0001f);
    T2D_CHECK_NEAR(camera.zoom(), 2.0f, 0.0005f);
    T2D_CHECK_NEAR(camera.centre().x, centre_before.x, 0.0005f);
    T2D_CHECK_NEAR(camera.centre().y, centre_before.y, 0.0005f);
}

T2D_TEST(a_map_of_hundreds_of_cells_fits_the_viewport) {
    Camera2D camera;
    camera.set_viewport(kViewport);
    camera.fit(big_map());
    // 720 / 512 is the tighter axis, so that is the zoom: the whole map is on screen.
    T2D_CHECK_NEAR(camera.zoom(), 720.0f / 512.0f, 0.0005f);
    T2D_CHECK_NEAR(camera.centre().x, 256.0f, 0.0005f);
    T2D_CHECK_NEAR(camera.centre().y, 256.0f, 0.0005f);
    // Fitting means the map is inside the view - the height is the tighter axis, so the view is
    // wider than the map on the left and on the right, and exactly as tall.
    const Aabb2 visible = camera.visible_world();
    T2D_CHECK_NEAR(visible.height(), 512.0f, 0.0005f);
    T2D_CHECK_LT(visible.min.x, big_map().min.x + 0.0005f);
    T2D_CHECK_GT(visible.max.x, big_map().max.x - 0.0005f);
    T2D_CHECK(visible.contains(big_map().min));
    T2D_CHECK(visible.contains(big_map().max));
    // Culling at that zoom walks every cell of the map, and only the margin of empty world around it.
    const TileRect cells = camera.visible_cells();
    T2D_CHECK(cells.contains(0, 0));
    T2D_CHECK(cells.contains(511, 511));
    T2D_CHECK_LT(cells.width * cells.height, 1024 * 1024);
    T2D_CHECK_LT(static_cast<f32>(cells.width * cells.height),
                 1.25f * 512.0f * 512.0f * (1280.0f / 720.0f));

    // A margin leaves that much screen space free on every side.
    camera.fit(big_map(), 40.0f);
    T2D_CHECK_NEAR(camera.zoom(), 640.0f / 512.0f, 0.0005f);
    // A degenerate box moves the camera without dividing by zero.
    const f32 zoom_before = camera.zoom();
    camera.fit(Aabb2{Vec2{10.0f, 10.0f}, Vec2{10.0f, 10.0f}});
    T2D_CHECK_NEAR(camera.zoom(), zoom_before, 0.0005f);
    T2D_CHECK_NEAR(camera.centre().x, 10.0f, 0.0005f);
}

T2D_TEST(a_bounded_view_stops_at_the_edge_of_the_map_and_centres_a_small_one) {
    Camera2D camera(kViewport, 20.0f, Vec2{256.0f, 256.0f});
    // Far outside the map: the view is pulled back until its edge touches the map's edge.
    camera.set_centre(Vec2{-500.0f, -500.0f});
    camera.clamp_to(big_map());
    const Aabb2 visible = camera.visible_world();
    T2D_CHECK_NEAR(visible.min.x, 0.0f, 0.0005f);
    T2D_CHECK_NEAR(visible.min.y, 0.0f, 0.0005f);

    camera.set_centre(Vec2{5000.0f, 5000.0f});
    camera.clamp_to(big_map());
    T2D_CHECK_NEAR(camera.visible_world().max.x, 512.0f, 0.0005f);
    T2D_CHECK_NEAR(camera.visible_world().max.y, 512.0f, 0.0005f);

    // A map smaller than the viewport stays centred instead of being pushed to a corner.
    camera.set_centre(Vec2{1000.0f, 1000.0f});
    camera.clamp_to(Aabb2{Vec2{0.0f, 0.0f}, Vec2{20.0f, 10.0f}});
    T2D_CHECK_NEAR(camera.centre().x, 10.0f, 0.0005f);
    T2D_CHECK_NEAR(camera.centre().y, 5.0f, 0.0005f);
}

T2D_TEST(scrolling_brings_a_cell_back_with_the_least_movement) {
    Camera2D camera(kViewport, 20.0f, Vec2{40.0f, 30.0f});
    // A cell that is already visible must not move the view at all.
    const Vec2 before = camera.centre();
    camera.scroll_to_show(Aabb2{Vec2{40.0f, 30.0f}, Vec2{41.0f, 31.0f}}, 20.0f);
    T2D_CHECK_NEAR(camera.centre().x, before.x, 0.0005f);
    T2D_CHECK_NEAR(camera.centre().y, before.y, 0.0005f);

    // One that ran off the right edge is brought back with a one cell margin, and nothing else moves.
    camera.scroll_to_show(Aabb2{Vec2{300.0f, 30.0f}, Vec2{301.0f, 31.0f}}, 20.0f);
    const Aabb2 visible = camera.visible_world();
    T2D_CHECK_NEAR(visible.max.x, 301.0f + 1.0f, 0.0005f);
    T2D_CHECK_NEAR(camera.centre().y, before.y, 0.0005f);
    // The cell is on screen afterwards, with its margin.
    const Vec2 screen = camera.screen_of(Vec2{301.0f, 31.0f});
    T2D_CHECK_GE(kViewport.x - 20.0f + 0.0005f, screen.x);
    T2D_CHECK_GE(screen.x, 0.0f);
}

T2D_TEST(panning_moves_the_world_with_the_pointer_and_survives_a_zero_viewport) {
    Camera2D camera(kViewport, 20.0f, Vec2{40.0f, 30.0f});
    const Vec2 at = camera.screen_of(Vec2{45.0f, 33.0f});
    camera.pan(Vec2{5.0f, -7.0f});
    const Vec2 moved = camera.screen_of(Vec2{45.0f, 33.0f});
    T2D_CHECK_NEAR(moved.x, at.x + 5.0f, 0.0005f);
    T2D_CHECK_NEAR(moved.y, at.y - 7.0f, 0.0005f);

    camera.move(Vec2{1.0f, 2.0f});
    T2D_CHECK_NEAR(camera.centre().x, 40.0f - 0.25f + 1.0f, 0.0005f);
    T2D_CHECK_NEAR(camera.centre().y, 30.0f + 0.35f + 2.0f, 0.0005f);

    // A window being resized can report a silly size for a frame: everything stays finite.
    camera.set_viewport(Vec2{-10.0f, -10.0f});
    T2D_CHECK_EQ(camera.viewport().x, 0.0f);
    T2D_CHECK_EQ(camera.viewport().y, 0.0f);
    T2D_CHECK_EQ(camera.visible_world().width(), 0.0f);
    T2D_CHECK(camera.visible_cells().empty());
    T2D_CHECK(std::isfinite(camera.screen_of(Vec2{1.0f, 2.0f}).x));
    camera.set_viewport(kViewport);
    camera.fit(Aabb2{Vec2{0.0f, 0.0f}, Vec2{0.0f, 0.0f}});
    T2D_CHECK(std::isfinite(camera.zoom()));
    T2D_CHECK_GT(camera.zoom(), 0.0f);
}

T2D_TEST_MAIN
