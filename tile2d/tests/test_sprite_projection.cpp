// The projection the sprite batch hands to the GPU, verified without one.
//
// This is the test for a bug that produced a perfectly clear image while the renderer reported
// hundreds of triangles: the 2D world sits at z = 0 and the projection used a 0.1 .. 100 depth
// range, so every quad was behind the near plane and Vulkan clipped the whole frame away.
#include <t2d/render/sprite_batch.h>

#include <support/test_support.h>

#include <glm/glm.hpp>

using namespace t2d;

namespace {

struct ClipPoint {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 w = 0.0f;
};

/// World position (at the given world depth) -> normalised device coordinates.
[[nodiscard]] ClipPoint project(const ore::Mat4& view_projection, f32 x, f32 y, f32 z = 0.0f) {
    const glm::vec4 clip = view_projection * glm::vec4{x, y, z, 1.0f};
    ClipPoint point;
    point.x = clip.x / clip.w;
    point.y = clip.y / clip.w;
    point.z = clip.z / clip.w;
    point.w = clip.w;
    return point;
}

} // namespace

T2D_TEST(the_camera_rectangle_maps_onto_the_whole_viewport) {
    const Vec2 center{512.0f, 256.0f};
    const Vec2 size{1280.0f, 720.0f};
    const ore::Mat4 view_projection = sprite_view_projection(center, size);
    const f32 half_width = size.x * 0.5f;
    const f32 half_height = size.y * 0.5f;

    const ClipPoint top_left = project(view_projection, center.x - half_width, center.y - half_height);
    T2D_CHECK_NEAR(top_left.x, -1.0f, 1e-5f);
    T2D_CHECK_NEAR(top_left.y, -1.0f, 1e-5f);

    const ClipPoint bottom_right = project(view_projection, center.x + half_width, center.y + half_height);
    T2D_CHECK_NEAR(bottom_right.x, 1.0f, 1e-5f);
    T2D_CHECK_NEAR(bottom_right.y, 1.0f, 1e-5f);

    // Top-left origin: a larger world y is lower on the screen, and +X is to the right.
    const ClipPoint middle = project(view_projection, center.x, center.y);
    T2D_CHECK_NEAR(middle.x, 0.0f, 1e-5f);
    T2D_CHECK_NEAR(middle.y, 0.0f, 1e-5f);
    T2D_CHECK_GT(project(view_projection, center.x, center.y + half_height * 0.5f).y, 0.0f);
    T2D_CHECK_LT(project(view_projection, center.x - half_width * 0.5f, center.y).x, 0.0f);
    T2D_CHECK_EQ(middle.w, 1.0f);
}

T2D_TEST(sprite_geometry_at_world_depth_zero_is_inside_the_clip_volume) {
    const ore::Mat4 view_projection = sprite_view_projection(Vec2{100.0f, 100.0f}, Vec2{320.0f, 180.0f});
    for (const f32 offset : {-1.0f, 0.0f, 1.0f}) {
        const ClipPoint point = project(view_projection, 100.0f + offset * 50.0f, 100.0f + offset * 30.0f, 0.0f);
        T2D_CHECK_GE(point.z, 0.0f);
        T2D_CHECK_LT(point.z, 1.0f);
        T2D_CHECK_GE(point.x, -1.0f);
        T2D_CHECK_LT(point.x, 1.0f);
        T2D_CHECK_GE(point.y, -1.0f);
        T2D_CHECK_LT(point.y, 1.0f);
    }
    // Small negative depths (the debug overlays) stay visible too.
    T2D_CHECK_LT(project(view_projection, 100.0f, 100.0f, -0.5f).z, 1.0f);
    T2D_CHECK_GE(project(view_projection, 100.0f, 100.0f, -0.5f).z, 0.0f);
}

T2D_TEST_MAIN
