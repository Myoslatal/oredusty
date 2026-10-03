#include <ore/core/time.h>

#include <support/test_support.h>

using ore::FixedTimestep;
using ore::FrameTimer;

ORE_TEST(fixed_timestep_produces_whole_steps) {
    FixedTimestep timestep(1.0f / 60.0f);
    ORE_CHECK_EQ(timestep.advance(1.0f / 60.0f), 1u);
    ORE_CHECK_EQ(timestep.advance(1.0f / 120.0f), 0u);
    ORE_CHECK_EQ(timestep.advance(1.0f / 120.0f), 1u);
    ORE_CHECK_EQ(timestep.advance(0.0f), 0u);
    ORE_CHECK_NEAR(timestep.simulated_seconds(), 2.0 / 60.0, 1e-6);
    ORE_CHECK(timestep.alpha() >= 0.0f && timestep.alpha() < 1.0f);
}

ORE_TEST(fixed_timestep_clamps_long_frames) {
    FixedTimestep timestep(1.0f / 60.0f, 4);
    // A one second frame would be 60 steps; the clamp allows only 4 and drops the backlog.
    ORE_CHECK_EQ(timestep.advance(1.0f), 4u);
    ORE_CHECK(timestep.alpha() < 1.0f);
    ORE_CHECK_EQ(timestep.advance(0.0f), 0u);
}

ORE_TEST(frame_timer_reports_deltas) {
    FrameTimer timer;
    ORE_CHECK_EQ(timer.frame_index(), 0u);
    timer.tick();
    ORE_CHECK(timer.delta_seconds() >= 0.0f);
    ORE_CHECK_EQ(timer.frame_index(), 1u);
    ORE_CHECK(timer.average_ms() >= 0.0f);
    ORE_CHECK(timer.max_ms() >= timer.min_ms());
    ORE_CHECK(timer.average_fps() >= 0.0f);
    timer.reset();
    ORE_CHECK_EQ(timer.frame_index(), 0u);
}

ORE_TEST_MAIN
