// The frame's input: the difference between a key being held and a key being pressed this frame, pointer
// deltas that survive a begin_frame(), and the wheel and typed text that belong to one frame only.
#include <mefemma/input.h>

#include <support/test_support.h>

using namespace mefemma;

T2D_TEST(a_press_is_an_edge_and_holding_is_a_situation) {
    InputState input;
    input.begin_frame();
    T2D_CHECK_FALSE(input.key_down(Key::A));
    T2D_CHECK_FALSE(input.key_pressed(Key::A));

    input.set_key(Key::A, true);
    T2D_CHECK(input.key_down(Key::A));
    T2D_CHECK(input.key_pressed(Key::A));
    T2D_CHECK_FALSE(input.key_released(Key::A));

    // Setting it down again inside the same frame is not a second press.
    input.set_key(Key::A, true);
    T2D_CHECK(input.key_pressed(Key::A));

    input.begin_frame();
    T2D_CHECK(input.key_down(Key::A));
    T2D_CHECK_FALSE(input.key_pressed(Key::A));

    input.set_key(Key::A, false);
    T2D_CHECK_FALSE(input.key_down(Key::A));
    T2D_CHECK(input.key_released(Key::A));
    input.begin_frame();
    T2D_CHECK_FALSE(input.key_released(Key::A));
}

T2D_TEST(a_tap_between_two_frames_is_still_a_press) {
    // The platform can deliver a down and an up for the same key before the frame loop looks again (the
    // debug input server's "key press" is exactly that). The frame has to see both edges and the key up.
    InputState input;
    input.begin_frame();
    input.set_key(Key::Enter, true);
    input.set_key(Key::Enter, false);
    T2D_CHECK_FALSE(input.key_down(Key::Enter));
    T2D_CHECK(input.key_pressed(Key::Enter));
    T2D_CHECK(input.key_released(Key::Enter));
    input.begin_frame();
    T2D_CHECK_FALSE(input.key_pressed(Key::Enter));
    T2D_CHECK_FALSE(input.key_released(Key::Enter));
}

T2D_TEST(an_unknown_key_is_never_down_and_never_presses) {
    InputState input;
    input.begin_frame();
    T2D_CHECK_FALSE(input.key_down(Key::Count));
    T2D_CHECK_FALSE(input.key_pressed(Key::Count));
    input.set_key(Key::Count, true);   // ignored, not an out of bounds write
    T2D_CHECK_FALSE(input.key_down(Key::Count));
}

T2D_TEST(mouse_buttons_have_the_same_edges) {
    InputState input;
    input.begin_frame();
    input.set_mouse_button(MouseButton::Left, true);
    T2D_CHECK(input.mouse_down(MouseButton::Left));
    T2D_CHECK(input.mouse_pressed(MouseButton::Left));
    input.begin_frame();
    T2D_CHECK(input.mouse_down(MouseButton::Left));
    T2D_CHECK_FALSE(input.mouse_pressed(MouseButton::Left));
    input.set_mouse_button(MouseButton::Left, false);
    T2D_CHECK(input.mouse_released(MouseButton::Left));
    T2D_CHECK_FALSE(input.mouse_down(MouseButton::Left));
}

T2D_TEST(the_first_pointer_position_has_no_delta_and_the_next_ones_do) {
    InputState input;
    input.begin_frame();
    input.set_mouse_position(100.0f, 50.0f);
    T2D_CHECK_EQ(input.mouse_x(), 100.0f);
    T2D_CHECK_EQ(input.mouse_y(), 50.0f);
    T2D_CHECK_EQ(input.mouse_delta_x(), 0.0f);
    T2D_CHECK_EQ(input.mouse_delta_y(), 0.0f);

    input.begin_frame();
    input.set_mouse_position(130.0f, 40.0f);
    T2D_CHECK_EQ(input.mouse_delta_x(), 30.0f);
    T2D_CHECK_EQ(input.mouse_delta_y(), -10.0f);

    // Two events in one frame add up; begin_frame() is what starts a new one.
    input.set_mouse_position(150.0f, 45.0f);
    T2D_CHECK_EQ(input.mouse_delta_x(), 50.0f);
    T2D_CHECK_EQ(input.mouse_delta_y(), -5.0f);
    input.begin_frame();
    T2D_CHECK_EQ(input.mouse_delta_x(), 0.0f);
}

T2D_TEST(leaving_the_window_forgets_the_position_so_re_entering_does_not_drag) {
    InputState input;
    input.begin_frame();
    input.set_cursor_inside(true);
    input.set_mouse_position(100.0f, 100.0f);
    input.begin_frame();
    input.set_mouse_position(120.0f, 100.0f);
    T2D_CHECK_EQ(input.mouse_delta_x(), 20.0f);

    input.begin_frame();
    input.set_cursor_inside(false);
    T2D_CHECK_FALSE(input.cursor_inside_window());
    T2D_CHECK_EQ(input.mouse_delta_x(), 0.0f);

    input.begin_frame();
    input.set_cursor_inside(true);
    input.set_mouse_position(600.0f, 300.0f);
    T2D_CHECK_EQ(input.mouse_delta_x(), 0.0f);   // no drag across the screen
    T2D_CHECK_EQ(input.mouse_x(), 600.0f);
}

T2D_TEST(the_wheel_and_typed_text_belong_to_one_frame) {
    InputState input;
    input.begin_frame();
    input.add_scroll(0.0f, 1.0f);
    input.add_scroll(0.5f, 0.5f);
    T2D_CHECK_EQ(input.scroll_x(), 0.5f);
    T2D_CHECK_EQ(input.scroll_y(), 1.5f);
    input.set_text("ab");
    T2D_CHECK_EQ(input.text_input(), std::string_view("ab"));

    input.begin_frame();
    T2D_CHECK_EQ(input.scroll_x(), 0.0f);
    T2D_CHECK_EQ(input.scroll_y(), 0.0f);
    T2D_CHECK(input.text_input().empty());
}

T2D_TEST_MAIN
