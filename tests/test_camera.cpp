// Ore framework - math + camera unit tests.
//
// The Vulkan conventions this module promises are checked behaviourally (where do
// points land after the perspective divide) and by comparing against glm itself.
// GLM_FORCE_DEPTH_ZERO_TO_ONE is defined here, in this test translation unit only,
// so that glm::perspective() produces the zero-to-one depth form to compare with;
// <ore/math/math.h> itself never defines any GLM macro.
#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include <ore/math/camera.h>
#include <ore/math/math.h>

#include <support/test_support.h>

#include <cmath>
#include <format>
#include <string>
#include <string_view>

using ore::Aabb;
using ore::CameraInputState;
using ore::f32;
using ore::FlyCameraController;
using ore::i32;
using ore::Mat3;
using ore::Mat4;
using ore::OrbitCameraController;
using ore::PerspectiveCamera;
using ore::Quat;
using ore::Vec2;
using ore::Vec3;
using ore::Vec4;

using ore::compose;
using ore::degrees;
using ore::euler_angles;
using ore::kPi;
using ore::look_at;
using ore::orthographic;
using ore::perspective;
using ore::perspective_flipped;
using ore::radians;

namespace {

constexpr f32 kTolerance = 1e-5f;

[[nodiscard]] bool nearly_equal(f32 a, f32 b, f32 epsilon) {
    return std::abs(a - b) <= epsilon;
}

[[nodiscard]] bool vec3_near(const Vec3& a, const Vec3& b, f32 epsilon) {
    return nearly_equal(a.x, b.x, epsilon) && nearly_equal(a.y, b.y, epsilon) &&
           nearly_equal(a.z, b.z, epsilon);
}

[[nodiscard]] bool mat4_near(const Mat4& a, const Mat4& b, f32 epsilon) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!nearly_equal(a[column][row], b[column][row], epsilon)) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] std::string vec3_string(const Vec3& value) {
    return std::format("({:.5f}, {:.5f}, {:.5f})", value.x, value.y, value.z);
}

[[nodiscard]] std::string mat4_string(const Mat4& matrix) {
    std::string text;
    for (int column = 0; column < 4; ++column) {
        text += std::format("[{:8.4f} {:8.4f} {:8.4f} {:8.4f}]", matrix[column][0], matrix[column][1],
                            matrix[column][2], matrix[column][3]);
    }
    return text;
}

// The harness macros take a fixed number of arguments, so a brace initialiser
// such as Vec3{1.0f, 2.0f, 3.0f} would split into several arguments. These
// variadic wrappers forward the whole comparison instead and report both values.
void check_near(std::string_view expression, f32 a, f32 b, f32 epsilon, const char* file, i32 line) {
    ::ore::test::report(nearly_equal(a, b, epsilon), expression, file, line, std::format("{} vs {}", a, b));
}

void check_vec3_near(std::string_view expression, const Vec3& a, const Vec3& b, f32 epsilon, const char* file,
                     i32 line) {
    ::ore::test::report(vec3_near(a, b, epsilon), expression, file, line,
                        std::format("{} vs {}", vec3_string(a), vec3_string(b)));
}

void check_mat4_near(std::string_view expression, const Mat4& a, const Mat4& b, f32 epsilon, const char* file,
                     i32 line) {
    ::ore::test::report(mat4_near(a, b, epsilon), expression, file, line,
                        std::format("{} vs {}", mat4_string(a), mat4_string(b)));
}

} // namespace

#define CHECK_NEAR(...) check_near(#__VA_ARGS__, __VA_ARGS__, __FILE__, __LINE__)
#define CHECK_VEC3_NEAR(...) check_vec3_near(#__VA_ARGS__, __VA_ARGS__, __FILE__, __LINE__)
#define CHECK_MAT4_NEAR(...) check_mat4_near(#__VA_ARGS__, __VA_ARGS__, __FILE__, __LINE__)

// ------------------------------------------------------------------------ math --
ORE_TEST(math_angle_conversions) {
    CHECK_NEAR(radians(0.0f), 0.0f, 1e-6f);
    CHECK_NEAR(radians(90.0f), kPi * 0.5f, 1e-6f);
    CHECK_NEAR(radians(180.0f), kPi, 1e-6f);
    CHECK_NEAR(degrees(kPi), 180.0f, 1e-4f);
    CHECK_NEAR(degrees(radians(37.5f)), 37.5f, 1e-4f);
    ORE_CHECK(kPi > 3.14159f && kPi < 3.14160f);
}

ORE_TEST(math_perspective_uses_vulkan_depth_and_y_down) {
    const f32 aspect = 4.0f / 3.0f;
    const f32 near_plane = 0.1f;
    const f32 far_plane = 100.0f;
    const Mat4 projection = perspective(radians(60.0f), aspect, near_plane, far_plane);

    const auto project = [&projection](const Vec3& view_point) {
        const Vec4 clip = projection * Vec4{view_point, 1.0f};
        return Vec3{clip.x / clip.w, clip.y / clip.w, clip.z / clip.w};
    };

    // Vulkan depth range: 0 at the near plane, 1 at the far plane.
    CHECK_NEAR(project(Vec3{0.0f, 0.0f, -near_plane}).z, 0.0f, 1e-5f);
    CHECK_NEAR(project(Vec3{0.0f, 0.0f, -far_plane}).z, 1.0f, 1e-5f);
    CHECK_NEAR(project(Vec3{0.0f, 0.0f, -1.0f}).z, 0.900901f, 1e-5f);

    // Vulkan clip space is Y-down, so a point above the view axis maps to negative
    // NDC y. It is the Y axis that is negated (X keeps its sign); for mirrored
    // points that shows up as ndc.y == -ndc.x * aspect.
    const Vec3 above = project(Vec3{0.0f, 1.0f, -5.0f});
    const Vec3 beside = project(Vec3{1.0f, 0.0f, -5.0f});
    ORE_CHECK(above.y < 0.0f);
    ORE_CHECK(beside.x > 0.0f);
    CHECK_NEAR(above.y, -beside.x * aspect, 1e-5f);
    CHECK_NEAR(above.x, 0.0f, 1e-6f);
    CHECK_NEAR(beside.y, 0.0f, 1e-6f);

    // glm agrees on everything except the depth convention (see this file's
    // GLM_FORCE_DEPTH_ZERO_TO_ONE) and the negated [1][1] entry.
    const Mat4 gl = glm::perspective(radians(60.0f), aspect, near_plane, far_plane);
    Mat4 expected = gl;
    expected[1][1] = -expected[1][1];
    CHECK_MAT4_NEAR(projection, expected, 1e-6f);

    // Sanity: glm really handed us the Y-up zero-to-one form above.
    CHECK_NEAR(gl[1][1], 1.0f / std::tan(radians(30.0f)), 1e-5f);
    CHECK_NEAR(gl[2][3], -1.0f, 1e-6f);
}

ORE_TEST(math_perspective_flipped_keeps_y_up) {
    const f32 aspect = 16.0f / 9.0f;
    const Mat4 flipped = perspective_flipped(radians(45.0f), aspect, 0.5f, 250.0f);
    const Mat4 vulkan = perspective(radians(45.0f), aspect, 0.5f, 250.0f);

    // Only [1][1] differs, and the flipped form is exactly glm's Y-up matrix.
    CHECK_NEAR(flipped[1][1], -vulkan[1][1], 1e-6f);
    CHECK_MAT4_NEAR(flipped, glm::perspective(radians(45.0f), aspect, 0.5f, 250.0f), 1e-6f);

    const Vec4 above = flipped * Vec4{0.0f, 1.0f, -2.0f, 1.0f};
    ORE_CHECK(above.y > 0.0f); // Y up in clip space, for a negative-height viewport

    // Depth is still the Vulkan 0..1 range.
    const Vec4 at_near = flipped * Vec4{0.0f, 0.0f, -0.5f, 1.0f};
    const Vec4 at_far = flipped * Vec4{0.0f, 0.0f, -250.0f, 1.0f};
    CHECK_NEAR(at_near.z / at_near.w, 0.0f, 1e-5f);
    CHECK_NEAR(at_far.z / at_far.w, 1.0f, 1e-5f);
}

ORE_TEST(math_orthographic_maps_box_to_clip_space) {
    const Mat4 ortho = orthographic(-2.0f, 6.0f, -1.0f, 3.0f, 0.5f, 20.0f);
    const auto project = [&ortho](const Vec3& point) {
        const Vec4 clip = ortho * Vec4{point, 1.0f};
        return Vec4{clip.x / clip.w, clip.y / clip.w, clip.z / clip.w, clip.w};
    };

    // x runs from left (-1) to right (+1).
    CHECK_NEAR(project(Vec3{-2.0f, 0.0f, 0.0f}).x, -1.0f, 1e-5f);
    CHECK_NEAR(project(Vec3{6.0f, 0.0f, 0.0f}).x, 1.0f, 1e-5f);
    // Vulkan Y-down: "top" (up on screen) maps to -1.
    CHECK_NEAR(project(Vec3{0.0f, 3.0f, 0.0f}).y, -1.0f, 1e-5f);
    CHECK_NEAR(project(Vec3{0.0f, -1.0f, 0.0f}).y, 1.0f, 1e-5f);
    // Depth 0..1 along the view axis.
    CHECK_NEAR(project(Vec3{0.0f, 0.0f, -0.5f}).z, 0.0f, 1e-5f);
    CHECK_NEAR(project(Vec3{0.0f, 0.0f, -20.0f}).z, 1.0f, 1e-5f);
    CHECK_NEAR(project(Vec3{0.0f, 0.0f, -20.0f}).w, 1.0f, 1e-5f);
    // The centre of the box lands in the centre of the depth range.
    CHECK_VEC3_NEAR(Vec3{project(Vec3{2.0f, 1.0f, -10.25f})}, Vec3{0.0f, 0.0f, 0.5f}, 1e-5f);
}

ORE_TEST(math_look_at_builds_view_matrix) {
    const Mat4 view = look_at(Vec3{0.0f, 0.0f, 5.0f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});

    // The target ends up 5 units down the -Z axis of view space.
    const Vec4 origin = view * Vec4{0.0f, 0.0f, 0.0f, 1.0f};
    CHECK_VEC3_NEAR(Vec3{origin}, Vec3{0.0f, 0.0f, -5.0f}, 1e-5f);
    CHECK_NEAR(origin.w, 1.0f, 1e-5f);

    // ... and the camera position maps to the view-space origin.
    const Vec4 eye = view * Vec4{0.0f, 0.0f, 5.0f, 1.0f};
    CHECK_VEC3_NEAR(Vec3{eye}, Vec3{0.0f, 0.0f, 0.0f}, 1e-5f);

    // Right-handed view space: +Y stays up and +X stays right.
    CHECK_VEC3_NEAR(Mat3{view} * Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1e-5f);
    CHECK_VEC3_NEAR(Mat3{view} * Vec3{1.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, 1e-5f);

    // An off-axis target lands on the -Z axis as well.
    const Mat4 tilted = look_at(Vec3{0.0f, 0.0f, 5.0f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
    const Vec4 target = tilted * Vec4{1.0f, 0.0f, 0.0f, 1.0f};
    CHECK_NEAR(target.x, 0.0f, 1e-5f);
    CHECK_NEAR(target.y, 0.0f, 1e-5f);
    ORE_CHECK(target.z < 0.0f);
}

ORE_TEST(math_compose_is_translation_rotation_scale) {
    const Vec3 position{1.0f, 2.0f, 3.0f};
    const Quat rotation = euler_angles(radians(90.0f), 0.0f, 0.0f); // +90 degrees about +X
    const Vec3 scale{2.0f, 3.0f, 4.0f};
    const Mat4 matrix = compose(position, rotation, scale);

    // R_x(90) maps (x, y, z) to (x, -z, y), so with the scale applied first:
    //   column 0 = R * (2, 0, 0) = (2, 0, 0)
    //   column 1 = R * (0, 3, 0) = (0, 0, 3)      <- R * S, not S * R
    //   column 2 = R * (0, 0, 4) = (0, -4, 0)
    //   column 3 = position
    Mat4 expected{0.0f};
    expected[0] = Vec4{2.0f, 0.0f, 0.0f, 0.0f};
    expected[1] = Vec4{0.0f, 0.0f, 3.0f, 0.0f};
    expected[2] = Vec4{0.0f, -4.0f, 0.0f, 0.0f};
    expected[3] = Vec4{position, 1.0f};
    CHECK_MAT4_NEAR(matrix, expected, 1e-5f);

    // Scale, then rotate, then translate: (1, 1, 1) -> (2, 3, 4) -> (2, -4, 3) -> (3, -2, 6).
    CHECK_VEC3_NEAR(Vec3{matrix * Vec4{1.0f, 1.0f, 1.0f, 1.0f}}, Vec3{3.0f, -2.0f, 6.0f}, 1e-5f);

    // Identity rotation and unit scale leave only the translation.
    const Mat4 translated = compose(Vec3{-4.0f, 0.5f, 8.0f}, Quat{1.0f, 0.0f, 0.0f, 0.0f}, Vec3{1.0f});
    CHECK_VEC3_NEAR(Vec3{translated * Vec4{1.0f, 2.0f, 3.0f, 1.0f}}, Vec3{-3.0f, 2.5f, 11.0f}, 1e-5f);
}

ORE_TEST(math_euler_angles_are_yxz) {
    const Vec3 forward{0.0f, 0.0f, -1.0f};
    CHECK_VEC3_NEAR(euler_angles(0.0f, 0.0f, 0.0f) * forward, forward, 1e-6f);

    // Positive pitch looks up, positive yaw turns left (towards -X).
    CHECK_VEC3_NEAR(euler_angles(radians(90.0f), 0.0f, 0.0f) * forward, Vec3{0.0f, 1.0f, 0.0f}, 1e-5f);
    CHECK_VEC3_NEAR(euler_angles(0.0f, radians(90.0f), 0.0f) * forward, Vec3{-1.0f, 0.0f, 0.0f}, 1e-5f);

    // YXZ order: yaw is applied in world space around +Y, then pitch around the
    // rotated +X. With XYZ order the same angles would give (-1, 0, 0) instead.
    CHECK_VEC3_NEAR(euler_angles(radians(90.0f), radians(90.0f), 0.0f) * forward, Vec3{0.0f, 1.0f, 0.0f},
                    1e-5f);

    // Roll rotates about the local +Z and the result stays unit length.
    const Quat rolled = euler_angles(0.0f, 0.0f, radians(90.0f));
    CHECK_VEC3_NEAR(rolled * Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1e-5f);
    CHECK_NEAR(glm::length(euler_angles(0.3f, 0.7f, -1.1f)), 1.0f, 1e-6f);
}

ORE_TEST(math_aabb_basics) {
    const Aabb box = Aabb::from_center_extents(Vec3{1.0f, 2.0f, 3.0f}, Vec3{0.5f, 1.0f, 2.0f});
    CHECK_VEC3_NEAR(box.min, Vec3{0.5f, 1.0f, 1.0f}, 1e-6f);
    CHECK_VEC3_NEAR(box.max, Vec3{1.5f, 3.0f, 5.0f}, 1e-6f);
    CHECK_VEC3_NEAR(box.center(), Vec3{1.0f, 2.0f, 3.0f}, 1e-6f);
    CHECK_VEC3_NEAR(box.extents(), Vec3{0.5f, 1.0f, 2.0f}, 1e-6f);
    CHECK_VEC3_NEAR(box.size(), Vec3{1.0f, 2.0f, 4.0f}, 1e-6f);

    // Negative extents are treated as their absolute value.
    const Aabb negative = Aabb::from_center_extents(Vec3{1.0f, 2.0f, 3.0f}, Vec3{-0.5f, -1.0f, -2.0f});
    CHECK_VEC3_NEAR(negative.min, box.min, 1e-6f);
    CHECK_VEC3_NEAR(negative.max, box.max, 1e-6f);

    // contains() is inclusive on the boundary.
    ORE_CHECK(box.contains(Vec3{1.0f, 2.0f, 3.0f}));
    ORE_CHECK(box.contains(Vec3{0.5f, 3.0f, 5.0f}));
    ORE_CHECK_FALSE(box.contains(Vec3{0.5f, 3.0f, 5.001f}));
    ORE_CHECK_FALSE(box.contains(Vec3{0.0f, 2.0f, 3.0f}));

    // expand() grows the box so that every point ends up inside it.
    Aabb grown;
    grown.expand(Vec3{-1.0f, 0.0f, 0.0f});
    grown.expand(Vec3{2.0f, 1.0f, -3.0f});
    CHECK_VEC3_NEAR(grown.min, Vec3{-1.0f, 0.0f, -3.0f}, 1e-6f);
    CHECK_VEC3_NEAR(grown.max, Vec3{2.0f, 1.0f, 0.0f}, 1e-6f);
    ORE_CHECK(grown.contains(Vec3{-1.0f, 0.0f, 0.0f}));
    ORE_CHECK(grown.contains(Vec3{2.0f, 1.0f, -3.0f}));
}

ORE_TEST(math_aabb_transformed_covers_rotated_corners) {
    const Aabb unit_box{Vec3{-1.0f}, Vec3{1.0f}};

    // Rotating a cube by 45 degrees about +Y widens it to sqrt(2) in x and z.
    const Mat4 rotate_y = glm::rotate(Mat4{1.0f}, radians(45.0f), Vec3{0.0f, 1.0f, 0.0f});
    const Aabb rotated = unit_box.transformed(rotate_y);
    CHECK_VEC3_NEAR(rotated.min, Vec3{-std::sqrt(2.0f), -1.0f, -std::sqrt(2.0f)}, 1e-5f);
    CHECK_VEC3_NEAR(rotated.max, Vec3{std::sqrt(2.0f), 1.0f, std::sqrt(2.0f)}, 1e-5f);
    CHECK_VEC3_NEAR(rotated.center(), Vec3{0.0f}, 1e-5f);
    CHECK_VEC3_NEAR(rotated.extents(), Vec3{std::sqrt(2.0f), 1.0f, std::sqrt(2.0f)}, 1e-5f);

    // Translation and uniform scale through compose().
    const Vec3 offset{10.0f, 0.0f, -5.0f};
    const Aabb moved = unit_box.transformed(compose(offset, Quat{1.0f, 0.0f, 0.0f, 0.0f}, Vec3{2.0f}));
    CHECK_VEC3_NEAR(moved.min, Vec3{8.0f, -2.0f, -7.0f}, 1e-5f);
    CHECK_VEC3_NEAR(moved.max, Vec3{12.0f, 2.0f, -3.0f}, 1e-5f);

    // A projective matrix is divided by w: a view-space point 5 units in front of
    // a 0.1 / 100 camera lands at depth 0.980981, not at the undivided 4.9049.
    const PerspectiveCamera camera;
    const Vec3 point{0.0f, 0.0f, -5.0f};
    const Aabb projected = Aabb{point, point}.transformed(camera.projection());
    CHECK_VEC3_NEAR(projected.min, Vec3{0.0f, 0.0f, 0.980981f}, 1e-5f);
    CHECK_VEC3_NEAR(projected.max, Vec3{0.0f, 0.0f, 0.980981f}, 1e-5f);
}

ORE_TEST(math_aabb_intersects) {
    const Aabb a{Vec3{0.0f}, Vec3{2.0f}};
    const Aabb overlapping{Vec3{1.0f}, Vec3{3.0f}};
    const Aabb touching{Vec3{2.0f}, Vec3{4.0f}};
    const Aabb disjoint{Vec3{2.001f}, Vec3{4.0f}};
    const Aabb inside{Vec3{0.5f}, Vec3{1.5f}};

    ORE_CHECK(a.intersects(overlapping));
    ORE_CHECK(overlapping.intersects(a));
    ORE_CHECK(a.intersects(touching)); // touching counts as intersecting
    ORE_CHECK(touching.intersects(a));
    ORE_CHECK_FALSE(a.intersects(disjoint));
    ORE_CHECK_FALSE(disjoint.intersects(a));
    ORE_CHECK(a.intersects(inside));
    ORE_CHECK(inside.intersects(a));
    ORE_CHECK(a.intersects(a));

    // Separated on a single axis is enough to be disjoint.
    const Aabb offset_in_y{Vec3{0.0f, 5.0f, 0.0f}, Vec3{2.0f, 6.0f, 2.0f}};
    ORE_CHECK_FALSE(a.intersects(offset_in_y));
}

// ---------------------------------------------------------------------- camera --
ORE_TEST(camera_defaults_match_the_documented_values) {
    const PerspectiveCamera camera;
    ORE_CHECK_EQ(camera.fov_y_degrees, 60.0f);
    ORE_CHECK_EQ(camera.aspect, 16.0f / 9.0f);
    ORE_CHECK_EQ(camera.near_plane, 0.1f);
    ORE_CHECK_EQ(camera.far_plane, 100.0f);
    CHECK_VEC3_NEAR(camera.position, Vec3{0.0f}, 0.0f);
    ORE_CHECK_EQ(camera.orientation.w, 1.0f);
    CHECK_VEC3_NEAR(Vec3{camera.orientation.x, camera.orientation.y, camera.orientation.z}, Vec3{0.0f}, 0.0f);

    const CameraInputState input;
    ORE_CHECK_FALSE(input.move_forward || input.move_backward || input.move_left || input.move_right ||
                    input.move_up || input.move_down || input.look_active);
    ORE_CHECK_EQ(input.look_delta_x, 0.0f);
    ORE_CHECK_EQ(input.look_delta_y, 0.0f);
    ORE_CHECK_EQ(input.scroll_delta, 0.0f);
    ORE_CHECK_EQ(input.speed_multiplier, 1.0f);
    ORE_CHECK_EQ(input.sensitivity, 0.0015f);

    PerspectiveCamera shared;
    const FlyCameraController fly(shared);
    ORE_CHECK_EQ(fly.move_speed(), 4.0f);

    const OrbitCameraController orbit(shared);
    CHECK_VEC3_NEAR(orbit.target(), Vec3{0.0f}, 0.0f);
    ORE_CHECK_EQ(orbit.distance(), 5.0f);
    ORE_CHECK_EQ(orbit.smoothing(), 0.0f);
}

ORE_TEST(camera_basis_view_and_projection) {
    PerspectiveCamera camera;
    CHECK_VEC3_NEAR(camera.forward(), Vec3{0.0f, 0.0f, -1.0f}, 1e-6f);
    CHECK_VEC3_NEAR(camera.right(), Vec3{1.0f, 0.0f, 0.0f}, 1e-6f);
    CHECK_VEC3_NEAR(camera.up(), Vec3{0.0f, 1.0f, 0.0f}, 1e-6f);

    // At the origin the view matrix is the identity, so world space is view space.
    CHECK_MAT4_NEAR(camera.view(), Mat4{1.0f}, 1e-6f);

    camera.position = Vec3{0.0f, 0.0f, 5.0f};
    const Vec3 eye{0.0f, 0.0f, 5.0f};
    CHECK_MAT4_NEAR(camera.view(), look_at(eye, Vec3{0.0f}, Vec3{0.0f, 1.0f, 0.0f}), 1e-5f);
    CHECK_VEC3_NEAR(Vec3{camera.view() * Vec4{0.0f, 0.0f, 0.0f, 1.0f}}, Vec3{0.0f, 0.0f, -5.0f}, 1e-5f);

    // A 90 degree yaw about +Y turns left: forward -> -X and right -> -Z.
    camera.orientation = euler_angles(0.0f, radians(90.0f), 0.0f);
    CHECK_VEC3_NEAR(camera.forward(), Vec3{-1.0f, 0.0f, 0.0f}, 1e-5f);
    CHECK_VEC3_NEAR(camera.right(), Vec3{0.0f, 0.0f, -1.0f}, 1e-5f);
    CHECK_VEC3_NEAR(camera.up(), Vec3{0.0f, 1.0f, 0.0f}, 1e-5f);

    // view_projection() == projection() * view(), and a point straight ahead of
    // the camera projects to the centre of the screen.
    CHECK_MAT4_NEAR(camera.view_projection(), camera.projection() * camera.view(), 1e-6f);
    const Vec3 ahead = camera.position + camera.forward() * 3.0f;
    const Vec4 clip = camera.view_projection() * Vec4{ahead, 1.0f};
    ORE_CHECK(clip.w > 0.0f);
    CHECK_NEAR(clip.x / clip.w, 0.0f, 1e-5f);
    CHECK_NEAR(clip.y / clip.w, 0.0f, 1e-5f);
    ORE_CHECK(clip.z / clip.w > 0.0f && clip.z / clip.w < 1.0f);

    // A rotated, translated camera agrees with look_at() built from its own basis.
    PerspectiveCamera posed;
    posed.position = Vec3{2.0f, 1.0f, -3.0f};
    posed.orientation = euler_angles(radians(-20.0f), radians(35.0f), 0.0f);
    CHECK_MAT4_NEAR(posed.view(), look_at(posed.position, posed.position + posed.forward(), posed.up()),
                    1e-5f);
}

ORE_TEST(camera_aspect_and_projection) {
    PerspectiveCamera camera;
    camera.set_aspect(1920.0f, 1080.0f);
    CHECK_NEAR(camera.aspect, 16.0f / 9.0f, 1e-6f);

    camera.set_aspect_from_size(2560u, 1440u);
    CHECK_NEAR(camera.aspect, 16.0f / 9.0f, 1e-6f);

    camera.set_aspect_from_size(800u, 600u);
    CHECK_NEAR(camera.aspect, 4.0f / 3.0f, 1e-6f);

    // projection() is ore::perspective() with the camera's own parameters.
    const Mat4 direct =
        perspective(radians(camera.fov_y_degrees), camera.aspect, camera.near_plane, camera.far_plane);
    CHECK_MAT4_NEAR(camera.projection(), direct, 1e-7f);

    // The Y-down flip makes [1][1] the negated, aspect-scaled [0][0].
    const Mat4 projection = camera.projection();
    CHECK_NEAR(projection[0][0], 1.0f / (camera.aspect * std::tan(radians(30.0f))), 1e-5f);
    CHECK_NEAR(projection[1][1], -projection[0][0] * camera.aspect, 1e-5f);
    CHECK_NEAR(projection[2][3], -1.0f, 1e-6f);

    // A wider vertical field of view shrinks the focal terms.
    camera.fov_y_degrees = 90.0f;
    ORE_CHECK(camera.projection()[0][0] < projection[0][0]);
    ORE_CHECK(camera.projection()[1][1] > projection[1][1]);
}

ORE_TEST(camera_world_from_screen_round_trip) {
    PerspectiveCamera camera;
    camera.position = Vec3{2.0f, 3.0f, -4.0f};
    camera.orientation = euler_angles(radians(-15.0f), radians(40.0f), 0.0f);

    // Depth 0 and 1 unproject onto the near and far plane centres.
    const Vec3 at_near = camera.world_from_screen(Vec2{0.0f, 0.0f}, 0.0f);
    const Vec3 at_far = camera.world_from_screen(Vec2{0.0f, 0.0f}, 1.0f);
    CHECK_NEAR(glm::distance(at_near, camera.position), camera.near_plane, 1e-4f);
    // Depth values crowd towards 1, so the far end is only accurate to ~1e-4 relative.
    CHECK_NEAR(glm::distance(at_far, camera.position), camera.far_plane, 0.05f);
    CHECK_VEC3_NEAR(glm::normalize(at_near - camera.position), camera.forward(), 1e-4f);

    // An off-axis world point survives project -> unproject.
    const Vec3 world_point =
        camera.position + camera.forward() * 6.0f + camera.right() * 0.75f - camera.up() * 0.5f;
    const Vec4 clip = camera.view_projection() * Vec4{world_point, 1.0f};
    ORE_REQUIRE(clip.w > 0.0f);
    const Vec2 ndc{clip.x / clip.w, clip.y / clip.w};
    const f32 depth = clip.z / clip.w;
    ORE_CHECK(ndc.x > -1.0f && ndc.x < 1.0f);
    ORE_CHECK(ndc.y > -1.0f && ndc.y < 1.0f);
    ORE_CHECK(depth > 0.0f && depth < 1.0f);
    CHECK_VEC3_NEAR(camera.world_from_screen(ndc, depth), world_point, 1e-3f);

    // NDC +Y points down, so a positive ndc.y unprojects below the view axis.
    const Vec3 below = camera.world_from_screen(Vec2{0.0f, 0.5f}, 0.5f);
    ORE_CHECK(glm::dot(below - camera.position, camera.up()) < 0.0f);
}

// ------------------------------------------------------------- fly controller --
ORE_TEST(fly_camera_forward_moves_along_forward) {
    PerspectiveCamera camera;
    FlyCameraController controller(camera, 2.0f);
    ORE_CHECK_EQ(controller.move_speed(), 2.0f);

    CameraInputState input;
    input.move_forward = true;
    controller.update(input, 1.0f);

    // One second at two units per second = two units along forward().
    CHECK_VEC3_NEAR(camera.position, Vec3{0.0f, 0.0f, -2.0f}, 1e-5f);
    CHECK_NEAR(glm::distance(camera.position, Vec3{0.0f}), 2.0f, 1e-5f);
    CHECK_VEC3_NEAR(camera.forward(), Vec3{0.0f, 0.0f, -1.0f}, 1e-6f);
    CHECK_NEAR(glm::dot(camera.position, camera.forward()), 2.0f, 1e-5f);

    // Movement follows the current orientation, not the world axes.
    camera.position = Vec3{0.0f};
    camera.orientation = euler_angles(0.0f, radians(90.0f), 0.0f); // facing -X
    controller.update(input, 0.5f);
    CHECK_VEC3_NEAR(camera.position, Vec3{-1.0f, 0.0f, 0.0f}, 1e-5f);
}

ORE_TEST(fly_camera_strafe_vertical_and_normalization) {
    PerspectiveCamera camera;
    FlyCameraController controller(camera, 2.0f);
    CameraInputState input;

    input.move_right = true;
    controller.update(input, 1.0f);
    CHECK_VEC3_NEAR(camera.position, Vec3{2.0f, 0.0f, 0.0f}, 1e-5f);

    input = CameraInputState{};
    input.move_up = true;
    controller.update(input, 0.5f);
    CHECK_VEC3_NEAR(camera.position, Vec3{2.0f, 1.0f, 0.0f}, 1e-5f);

    // A diagonal move covers the same distance as a straight one.
    input = CameraInputState{};
    input.move_left = true;
    input.move_backward = true;
    controller.update(input, 1.0f);
    const Vec3 corner{2.0f, 1.0f, 0.0f};
    CHECK_NEAR(glm::distance(camera.position, corner), 2.0f, 1e-5f);
    CHECK_VEC3_NEAR(camera.position, Vec3{2.0f - std::sqrt(2.0f), 1.0f, std::sqrt(2.0f)}, 1e-5f);

    // No keys and a zero frame time are both no-ops.
    controller.update(CameraInputState{}, 1.0f);
    input = CameraInputState{};
    input.move_forward = true;
    controller.update(input, 0.0f);
    CHECK_VEC3_NEAR(camera.position, Vec3{2.0f - std::sqrt(2.0f), 1.0f, std::sqrt(2.0f)}, 1e-5f);
}

ORE_TEST(fly_camera_look_changes_orientation_not_position) {
    PerspectiveCamera camera;
    camera.position = Vec3{1.0f, 2.0f, 3.0f};
    FlyCameraController controller(camera, 4.0f);

    const Quat before = camera.orientation;
    CameraInputState input;
    input.look_active = true;
    input.look_delta_x = 100.0f; // mouse moved right
    input.look_delta_y = 50.0f;  // ... and down
    controller.update(input, 1.0f / 60.0f);

    CHECK_VEC3_NEAR(camera.position, Vec3{1.0f, 2.0f, 3.0f}, 1e-6f);
    ORE_CHECK(glm::dot(camera.orientation, before) < 0.9999f);

    // Default sensitivity 0.0015 rad/px: yaw -0.15 rad, pitch -0.075 rad.
    CHECK_NEAR(camera.forward().x, std::sin(0.15f) * std::cos(0.075f), 1e-5f);
    CHECK_NEAR(camera.forward().y, -std::sin(0.075f), 1e-5f);
    CHECK_NEAR(camera.forward().z, -std::cos(0.15f) * std::cos(0.075f), 1e-5f);
    CHECK_NEAR(glm::length(camera.forward()), 1.0f, 1e-6f);

    // No roll: the local up axis keeps pointing into the upper hemisphere.
    ORE_CHECK(camera.up().y > 0.0f);

    // Without the look button the orientation is left alone.
    const Quat looked = camera.orientation;
    input.look_active = false;
    controller.update(input, 1.0f / 60.0f);
    ORE_CHECK(glm::dot(camera.orientation, looked) > 0.999999f);
}

ORE_TEST(fly_camera_speed_multiplier_scroll_and_rebinding) {
    PerspectiveCamera camera;
    FlyCameraController controller(camera, 10.0f);

    CameraInputState input;
    input.move_forward = true;
    input.speed_multiplier = 0.25f;
    controller.update(input, 1.0f);
    CHECK_VEC3_NEAR(camera.position, Vec3{0.0f, 0.0f, -2.5f}, 1e-5f);

    // The wheel scales the flight speed (documented behaviour of this controller).
    const f32 initial_speed = controller.move_speed();
    input = CameraInputState{};
    input.scroll_delta = 1.0f;
    controller.update(input, 0.0f);
    ORE_CHECK(controller.move_speed() > initial_speed);
    const f32 faster = controller.move_speed();

    input.scroll_delta = -1.0f;
    controller.update(input, 0.0f);
    ORE_CHECK(controller.move_speed() < faster);
    CHECK_NEAR(controller.move_speed(), initial_speed, 1e-4f);

    controller.set_move_speed(7.5f);
    ORE_CHECK_EQ(controller.move_speed(), 7.5f);

    // Rebinding leaves the first camera alone.
    PerspectiveCamera other;
    controller.set_camera(other);
    ORE_CHECK(&controller.camera() == &other);
    input = CameraInputState{};
    input.move_forward = true;
    controller.update(input, 1.0f);
    CHECK_VEC3_NEAR(other.position, Vec3{0.0f, 0.0f, -7.5f}, 1e-5f);
    CHECK_VEC3_NEAR(camera.position, Vec3{0.0f, 0.0f, -2.5f}, 1e-5f);
}

// ----------------------------------------------------------- orbit controller --
ORE_TEST(orbit_camera_update_places_camera_at_distance) {
    PerspectiveCamera camera;
    OrbitCameraController controller(camera, Vec3{0.0f}, 5.0f);
    ORE_CHECK_EQ(controller.distance(), 5.0f);
    CHECK_VEC3_NEAR(controller.target(), Vec3{0.0f}, 1e-6f);

    controller.update(0.0f);
    CHECK_VEC3_NEAR(camera.position, Vec3{0.0f, 0.0f, 5.0f}, 1e-5f);
    ORE_CHECK_NEAR(glm::distance(camera.position, controller.target()), 5.0f, 1e-5f);

    // The camera looks at the target with an upright up axis.
    const Vec3 to_target = glm::normalize(controller.target() - camera.position);
    ORE_CHECK_NEAR(glm::dot(camera.forward(), to_target), 1.0f, 1e-5f);
    CHECK_NEAR(camera.up().y, 1.0f, 1e-5f);

    // A target elsewhere: the camera keeps the direction it already had from the
    // target and is pulled to the requested distance.
    PerspectiveCamera placed;
    placed.position = Vec3{1.0f, 2.0f, 5.0f};
    OrbitCameraController orbit(placed, Vec3{1.0f, 2.0f, 3.0f}, 5.0f);
    orbit.update(0.0f);
    CHECK_VEC3_NEAR(placed.position, Vec3{1.0f, 2.0f, 8.0f}, 1e-5f);

    // set_target() moves the orbit centre on the next update().
    orbit.set_target(Vec3{1.0f, 2.0f, -2.0f});
    CHECK_VEC3_NEAR(orbit.target(), Vec3{1.0f, 2.0f, -2.0f}, 1e-6f);
    orbit.update(0.0f);
    CHECK_VEC3_NEAR(placed.position, Vec3{1.0f, 2.0f, 3.0f}, 1e-5f);
}

ORE_TEST(orbit_camera_rotate_moves_to_expected_axis) {
    PerspectiveCamera camera;
    OrbitCameraController controller(camera, Vec3{0.0f}, 5.0f);

    // +90 degrees of yaw about +Y swings the camera from +Z onto +X.
    controller.rotate(kPi * 0.5f, 0.0f);
    controller.update(0.0f);
    CHECK_VEC3_NEAR(camera.position, Vec3{5.0f, 0.0f, 0.0f}, 1e-5f);
    ORE_CHECK_NEAR(glm::distance(camera.position, Vec3{0.0f}), 5.0f, 1e-5f);

    // ... and another +90 degrees onto -Z.
    controller.rotate(kPi * 0.5f, 0.0f);
    controller.update(0.0f);
    CHECK_VEC3_NEAR(camera.position, Vec3{0.0f, 0.0f, -5.0f}, 1e-5f);

    // The camera keeps looking at the target from every angle.
    const Vec3 to_target = glm::normalize(-camera.position);
    ORE_CHECK_NEAR(glm::dot(camera.forward(), to_target), 1.0f, 1e-5f);
}

ORE_TEST(orbit_camera_pitch_is_clamped_and_keeps_looking_at_target) {
    PerspectiveCamera camera;
    OrbitCameraController controller(camera, Vec3{0.0f}, 2.0f);

    // The default pitch limit is 85 degrees, so a huge input saturates there.
    controller.rotate(0.0f, radians(1000.0f));
    controller.update(0.0f);
    CHECK_NEAR(camera.position.x, 0.0f, 1e-4f);
    CHECK_NEAR(camera.position.y, 2.0f * std::sin(radians(85.0f)), 1e-5f);
    CHECK_NEAR(camera.position.z, 2.0f * std::cos(radians(85.0f)), 1e-5f);

    const Vec3 to_target = glm::normalize(controller.target() - camera.position);
    ORE_CHECK_NEAR(glm::dot(camera.forward(), to_target), 1.0f, 1e-5f);
    ORE_CHECK(camera.up().y > 0.0f);

    // Custom limits clamp the pitch, and the current pose follows immediately.
    controller.set_limits(-0.5f, 0.5f, 0.5f, 10.0f);
    controller.update(0.0f);
    CHECK_NEAR(camera.position.y, 2.0f * std::sin(0.5f), 1e-5f);
    CHECK_NEAR(camera.position.z, 2.0f * std::cos(0.5f), 1e-5f);

    controller.rotate(0.0f, -10.0f);
    controller.update(0.0f);
    CHECK_NEAR(camera.position.y, 2.0f * std::sin(-0.5f), 1e-5f);
}

ORE_TEST(orbit_camera_zoom_and_distance_limits) {
    PerspectiveCamera camera;
    OrbitCameraController controller(camera, Vec3{0.0f}, 5.0f);
    controller.set_limits(-1.0f, 1.0f, 2.0f, 8.0f);
    ORE_CHECK_EQ(controller.distance(), 5.0f);

    // Wheel up zooms in (distance shrinks), wheel down zooms back out.
    controller.zoom(1.0f);
    ORE_CHECK(controller.distance() < 5.0f);
    ORE_CHECK(controller.distance() > 2.0f);
    controller.zoom(-1.0f);
    CHECK_NEAR(controller.distance(), 5.0f, 1e-4f);

    // The distance is clamped on both ends.
    controller.zoom(100.0f);
    ORE_CHECK_EQ(controller.distance(), 2.0f);
    controller.zoom(-100.0f);
    ORE_CHECK_EQ(controller.distance(), 8.0f);

    controller.set_distance(100.0f);
    ORE_CHECK_EQ(controller.distance(), 8.0f);
    controller.set_distance(0.5f);
    ORE_CHECK_EQ(controller.distance(), 2.0f);

    controller.set_distance(4.0f);
    controller.update(0.0f);
    ORE_CHECK_NEAR(glm::distance(camera.position, controller.target()), 4.0f, 1e-5f);
}

ORE_TEST(orbit_camera_pan_moves_the_target_in_the_camera_plane) {
    PerspectiveCamera camera;
    OrbitCameraController controller(camera, Vec3{0.0f}, 10.0f);
    controller.update(0.0f); // camera at (0, 0, 10): right = +X, up = +Y

    // Dragging right pulls the target towards -X so the content follows the cursor.
    controller.pan(100.0f, 0.0f);
    ORE_CHECK(controller.target().x < 0.0f);
    CHECK_NEAR(controller.target().y, 0.0f, 1e-6f);
    CHECK_NEAR(controller.target().z, 0.0f, 1e-6f);

    controller.set_target(Vec3{0.0f});
    controller.pan(0.0f, 50.0f); // drag down
    ORE_CHECK(controller.target().y > 0.0f);
    CHECK_NEAR(controller.target().x, 0.0f, 1e-6f);
    CHECK_NEAR(controller.target().z, 0.0f, 1e-6f);

    // The world-space step scales with the distance.
    controller.set_target(Vec3{0.0f});
    controller.set_distance(20.0f);
    controller.pan(100.0f, 0.0f);
    const f32 far_pan = controller.target().x;
    controller.set_target(Vec3{0.0f});
    controller.set_distance(5.0f);
    controller.pan(100.0f, 0.0f);
    CHECK_NEAR(controller.target().x, far_pan * 0.25f, 1e-5f);
}

ORE_TEST(orbit_camera_smoothing_approaches_the_orbit_pose) {
    PerspectiveCamera camera;
    OrbitCameraController controller(camera, Vec3{0.0f}, 5.0f);
    controller.update(0.0f);
    CHECK_VEC3_NEAR(camera.position, Vec3{0.0f, 0.0f, 5.0f}, 1e-5f);

    controller.set_smoothing(8.0f);
    ORE_CHECK_EQ(controller.smoothing(), 8.0f);
    controller.rotate(kPi * 0.5f, 0.0f); // the new orbit pose is (5, 0, 0)

    // The default (smoothing 0) snaps; with smoothing the camera eases in.
    controller.update(0.05f);
    ORE_CHECK(camera.position.x > 0.0f);
    ORE_CHECK(camera.position.x < 5.0f);
    ORE_CHECK(camera.position.z > 0.0f);

    for (int step = 0; step < 200; ++step) {
        controller.update(0.05f);
    }
    CHECK_VEC3_NEAR(camera.position, Vec3{5.0f, 0.0f, 0.0f}, 1e-3f);
    const Vec3 to_target = glm::normalize(-camera.position);
    ORE_CHECK_NEAR(glm::dot(camera.forward(), to_target), 1.0f, 1e-3f);

    // Turning smoothing off snaps again.
    controller.set_smoothing(0.0f);
    controller.rotate(kPi * 0.5f, 0.0f); // -> (0, 0, -5)
    controller.update(0.0f);
    CHECK_VEC3_NEAR(camera.position, Vec3{0.0f, 0.0f, -5.0f}, 1e-5f);
}

ORE_TEST_MAIN
