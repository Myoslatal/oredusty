// Two presses on one cell are one gesture: what makes a double click, and what does not.
//
// A double click is the game's own idea of a gesture (docs/GAME_DESIGN.md section 1.31): the designer put
// a second meaning on the pointer, and telling it apart from two clicks is a rule of its own - two
// presses, one cell, one window. There is no content in here and nothing to draw: the tracker is the
// whole subject.
#include <mine/double_click.h>

#include <support/test_support.h>

#include <format>
#include <string>

using namespace mine;

T2D_TEST(a_second_press_on_the_same_cell_inside_the_window_is_a_double_click) {
    DoubleClick tracker;
    T2D_CHECK_FALSE(tracker.pending());
    T2D_CHECK_FALSE(tracker.press(GridPos{4, 7}));   // the first press is a single click
    T2D_CHECK(tracker.pending());
    T2D_CHECK(tracker.press(GridPos{4, 7}));        // and the second one is the double click
}

T2D_TEST(a_press_on_another_cell_is_a_first_press_again) {
    // Two presses on two cells are two clicks: the cell is half of the rule, not just the timing.
    DoubleClick tracker;
    T2D_CHECK_FALSE(tracker.press(GridPos{4, 7}));
    T2D_CHECK_FALSE(tracker.press(GridPos{5, 7}));
    T2D_CHECK(tracker.pending());
    T2D_CHECK(tracker.press(GridPos{5, 7}));
}

T2D_TEST(the_window_runs_out) {
    DoubleClick tracker{0.4f};
    T2D_CHECK_NEAR(tracker.window(), 0.4f, 1e-6f);
    T2D_CHECK_FALSE(tracker.press(GridPos{1, 1}));
    tracker.advance(0.3f);
    T2D_CHECK(tracker.pending());                   // still inside the window
    tracker.advance(0.2f);                          // 0.5 > 0.4: the first press is forgotten
    T2D_CHECK_FALSE(tracker.pending());
    T2D_CHECK_FALSE(tracker.press(GridPos{1, 1}));  // so this press starts over
}

T2D_TEST(a_press_just_inside_the_window_still_counts) {
    DoubleClick tracker{0.4f};
    T2D_CHECK_FALSE(tracker.press(GridPos{2, 2}));
    tracker.advance(0.39f);
    T2D_CHECK(tracker.press(GridPos{2, 2}));
}

T2D_TEST(two_presses_far_apart_in_time_are_two_clicks) {
    DoubleClick tracker;
    T2D_CHECK_FALSE(tracker.press(GridPos{0, 0}));
    for (int frame = 0; frame < 10; ++frame) tracker.advance(0.1f);   // a second of the game running
    T2D_CHECK_FALSE(tracker.press(GridPos{0, 0}));
}

T2D_TEST(a_third_press_can_pair_with_the_second) {
    // Double clicking twice in a row is something a player does, so a completed double click arms the
    // window again rather than leaving it empty.
    DoubleClick tracker;
    T2D_CHECK_FALSE(tracker.press(GridPos{3, 3}));
    T2D_CHECK(tracker.press(GridPos{3, 3}));
    T2D_CHECK(tracker.pending());
    T2D_CHECK(tracker.press(GridPos{3, 3}));
}

T2D_TEST(reset_forgets_a_press_that_is_waiting) {
    // A layer change, a screen change: whatever was half pressed belongs to something else now.
    DoubleClick tracker;
    T2D_CHECK_FALSE(tracker.press(GridPos{9, 9}));
    tracker.reset();
    T2D_CHECK_FALSE(tracker.pending());
    T2D_CHECK_FALSE(tracker.press(GridPos{9, 9}));
}

T2D_TEST(time_passing_with_nothing_waiting_changes_nothing) {
    DoubleClick tracker{0.2f};
    tracker.advance(10.0f);
    T2D_CHECK_FALSE(tracker.pending());
    T2D_CHECK_FALSE(tracker.press(GridPos{1, 2}));
    tracker.advance(0.5f);
    tracker.advance(0.5f);
    T2D_CHECK_FALSE(tracker.pending());
    T2D_CHECK_FALSE(tracker.press(GridPos{1, 2}));
}

T2D_TEST_MAIN
