#include <ore/platform/input.h>

#include <support/test_support.h>

using namespace ore;

namespace {

/// Fills an input state the way the window layer would.
void press(InputState& state, Key key) {
    state.begin_frame();
    state.set_key(key, true);
}

} // namespace

ORE_TEST(input_state_tracks_edges) {
    InputState state;
    state.begin_frame();
    ORE_CHECK_FALSE(state.key_down(Key::W));
    state.set_key(Key::W, true);
    ORE_CHECK(state.key_down(Key::W));
    ORE_CHECK(state.key_pressed(Key::W));
    ORE_CHECK_FALSE(state.key_released(Key::W));

    // A new frame clears the pressed edge but keeps the key held.
    state.begin_frame();
    ORE_CHECK(state.key_down(Key::W));
    ORE_CHECK_FALSE(state.key_pressed(Key::W));

    state.set_key(Key::W, false);
    ORE_CHECK_FALSE(state.key_down(Key::W));
    ORE_CHECK(state.key_released(Key::W));

    state.begin_frame();
    ORE_CHECK_FALSE(state.key_released(Key::W));
}

ORE_TEST(input_state_mouse_and_text) {
    InputState state;
    state.begin_frame();
    state.set_mouse_position(120.0f, 80.0f);
    state.add_mouse_delta(3.0f, -4.0f);
    state.add_scroll(0.0f, 1.0f);
    state.set_mouse_button(MouseButton::Right, true);
    state.add_text("hi");
    state.set_cursor_inside(true);

    ORE_CHECK_EQ(state.mouse_x(), 120.0f);
    ORE_CHECK_EQ(state.mouse_y(), 80.0f);
    ORE_CHECK_EQ(state.mouse_delta_x(), 3.0f);
    ORE_CHECK_EQ(state.mouse_delta_y(), -4.0f);
    ORE_CHECK_EQ(state.scroll_y(), 1.0f);
    ORE_CHECK(state.mouse_down(MouseButton::Right));
    ORE_CHECK(state.mouse_pressed(MouseButton::Right));
    ORE_CHECK(state.cursor_inside_window());
    ORE_CHECK_EQ(state.text_input(), std::string("hi"));

    state.begin_frame();
    ORE_CHECK_EQ(state.mouse_delta_x(), 0.0f);
    ORE_CHECK_EQ(state.scroll_y(), 0.0f);
    ORE_CHECK_FALSE(state.mouse_pressed(MouseButton::Right));
    ORE_CHECK(state.text_input().empty());
}

ORE_TEST(pointer_events_arrive_in_framebuffer_pixels) {
    // A 2x display: the window system reports the pointer in screen coordinates (512x384), the
    // framebuffer it draws into is twice that. A hit test in framebuffer pixels must see the doubled
    // position, or the pointer lands on whatever is up and to the left of it.
    const PixelScale doubled = PixelScale::of(512, 384, 1024, 768);
    ORE_CHECK_EQ(doubled.x, 2.0f);
    ORE_CHECK_EQ(doubled.y, 2.0f);

    InputState state;
    state.begin_frame();
    state.set_cursor_inside(true);
    state.add_pointer_event(400.0f, 300.0f, doubled);
    ORE_CHECK_EQ(state.mouse_x(), 800.0f);
    ORE_CHECK_EQ(state.mouse_y(), 600.0f);
    ORE_CHECK_EQ(state.mouse_delta_x(), 0.0f); // appearing over the window is not a movement
    ORE_CHECK_EQ(state.mouse_delta_y(), 0.0f);

    state.begin_frame();
    state.add_pointer_event(410.0f, 290.0f, doubled);
    ORE_CHECK_EQ(state.mouse_x(), 820.0f);
    ORE_CHECK_EQ(state.mouse_y(), 580.0f);
    ORE_CHECK_EQ(state.mouse_delta_x(), 20.0f);
    ORE_CHECK_EQ(state.mouse_delta_y(), -20.0f);

    // Leaving and re-entering: the pointer reappears somewhere else, which is not a movement either.
    state.begin_frame();
    state.set_cursor_inside(false);
    state.set_cursor_inside(true);
    state.add_pointer_event(10.0f, 10.0f, doubled);
    ORE_CHECK_EQ(state.mouse_x(), 20.0f);
    ORE_CHECK_EQ(state.mouse_delta_x(), 0.0f);

    // A fractional scale: a 2133x1200 window is a 3555x2000 framebuffer (the two ratios differ in the
    // last digits because the sizes are rounded independently), so the corner of the window is the
    // corner of the framebuffer.
    const PixelScale fractional = PixelScale::of(2133, 1200, 3555, 2000);
    state.begin_frame();
    state.add_pointer_event(2133.0f, 1200.0f, fractional);
    ORE_CHECK_NEAR(state.mouse_x(), 3555.0f, 0.5f);
    ORE_CHECK_NEAR(state.mouse_y(), 2000.0f, 0.5f);

    // An unscaled display is the identity, and a minimised window (no size to divide by) cannot
    // produce a scale at all.
    ORE_CHECK(PixelScale::of(1280, 720, 1280, 720).identity());
    ORE_CHECK(PixelScale::of(0, 0, 1024, 768).identity());
    ORE_CHECK(PixelScale::of(1280, 720, 0, 0).identity());
}

ORE_TEST(input_map_actions_and_axes) {
    InputMap map;
    map.bind("jump", Key::Space);
    map.bind("jump", Key::C);
    map.bind("fire", MouseButton::Left);
    map.bind_axis("move_x", Key::D, Key::A);
    map.bind_axis("move_z", Key::S, Key::W);
    ORE_CHECK_EQ(map.action_count(), 2u);
    ORE_CHECK_EQ(map.axis_count(), 2u);

    InputState state;
    map.update(state);
    ORE_CHECK_FALSE(map.action_down("jump"));
    ORE_CHECK_EQ(map.axis("move_x"), 0.0f);
    ORE_CHECK_EQ(map.axis("unbound"), 0.0f);
    ORE_CHECK_FALSE(map.action_down("unbound"));

    press(state, Key::Space);
    map.update(state);
    ORE_CHECK(map.action_down("jump"));
    ORE_CHECK(map.action_pressed("jump"));

    // Releasing the key produces the released edge; a new frame alone does not.
    state.begin_frame();
    state.set_key(Key::Space, false);
    map.update(state);
    ORE_CHECK(map.action_released("jump"));

    state.begin_frame();
    state.set_key(Key::C, true); // the second binding of the same action
    map.update(state);
    ORE_CHECK(map.action_down("jump"));

    state.begin_frame();
    state.set_mouse_button(MouseButton::Left, true);
    map.update(state);
    ORE_CHECK(map.action_down("fire"));

    state.begin_frame();
    state.set_key(Key::D, true);
    state.set_key(Key::W, true);
    map.update(state);
    ORE_CHECK_EQ(map.axis("move_x"), 1.0f);
    ORE_CHECK_EQ(map.axis("move_z"), -1.0f);

    // Held keys persist across frames, so the opposite key must be released to flip the axis.
    state.begin_frame();
    state.set_key(Key::D, false);
    state.set_key(Key::A, true);
    map.update(state);
    ORE_CHECK_EQ(map.axis("move_x"), -1.0f);
}

ORE_TEST(input_map_unbind_and_keys_of) {
    InputMap map;
    map.bind("crouch", Key::LeftControl);
    map.bind("crouch", Key::C);
    ORE_REQUIRE(map.keys_of("crouch").size() == 2u);
    ORE_CHECK_EQ(map.keys_of("crouch")[0], Key::LeftControl);
    map.unbind("crouch");
    ORE_CHECK_EQ(map.action_count(), 0u);
    ORE_CHECK(map.keys_of("crouch").empty());
    map.bind("x", Key::X);
    map.clear();
    ORE_CHECK_EQ(map.action_count(), 0u);
}

ORE_TEST(input_map_without_state_is_safe) {
    InputMap map;
    map.bind("jump", Key::Space);
    map.bind_axis("move_x", Key::D, Key::A);
    // No update() yet: everything reads as released / centred instead of crashing.
    ORE_CHECK_FALSE(map.action_down("jump"));
    ORE_CHECK_FALSE(map.action_pressed("jump"));
    ORE_CHECK_FALSE(map.action_released("jump"));
    ORE_CHECK_EQ(map.axis("move_x"), 0.0f);
}

ORE_TEST(key_codes_are_distinct) {
    // The platform layer forwards GLFW codes directly, so collisions would silently merge keys.
    ORE_CHECK_NE(static_cast<i32>(Key::W), static_cast<i32>(Key::A));
    ORE_CHECK_NE(static_cast<i32>(Key::Escape), static_cast<i32>(Key::Enter));
    ORE_CHECK_NE(static_cast<i32>(Key::LeftShift), static_cast<i32>(Key::RightShift));
    ORE_CHECK(static_cast<i32>(Key::Count) > static_cast<i32>(Key::Z));
    ORE_CHECK_EQ(static_cast<usize>(MouseButton::Count), 7u);
}

ORE_TEST_MAIN
