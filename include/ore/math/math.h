// Ore framework - math types and Vulkan-convention matrix builders.
//
// Conventions shared by the whole framework (see also camera.h):
//   * Right-handed world space: +X right, +Y up, +Z towards the viewer.
//   * A camera looks down its local -Z axis, local +X is right, local +Y is up.
//   * Vulkan clip space: x and y in [-1, 1] with +Y pointing DOWN, z in [0, 1].
//     perspective() and orthographic() bake both rules in, so a default Vulkan
//     viewport (positive height) presents an upright image.
//   * Column-major matrices and column vectors: clip = projection * view * model * position.
//
// This header is a thin layer over glm. It deliberately does not define any
// GLM_FORCE_* macro (those would leak into every translation unit that includes
// Ore); the Vulkan depth range and the Y flip are applied by hand.
#pragma once

#include <ore/core/assert.h>
#include <ore/core/types.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace ore {

using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using Mat3 = glm::mat3;
using Mat4 = glm::mat4;
using Quat = glm::quat;

inline constexpr f32 kPi = 3.14159265358979323846f;

/// Decimal degrees -> radians.
[[nodiscard]] constexpr f32 radians(f32 degrees_value) {
    return degrees_value * (kPi / 180.0f);
}

/// Radians -> decimal degrees.
[[nodiscard]] constexpr f32 degrees(f32 radians_value) {
    return radians_value * (180.0f / kPi);
}

/// Right-handed perspective projection into Vulkan clip space (depth 0..1, Y down).
///
/// p fov_y_radians is the full vertical field of view. A view-space point at
/// z = -near_plane lands on depth 0, a point at z = -far_plane on depth 1.
/// Matches glm::perspective() built with GLM_FORCE_DEPTH_ZERO_TO_ONE except that
/// the [1][1] entry is negated, so +Y in view space maps to -Y in clip space.
[[nodiscard]] inline Mat4 perspective(f32 fov_y_radians, f32 aspect, f32 near_plane, f32 far_plane) {
    ORE_ASSERT(fov_y_radians > 0.0f && fov_y_radians < kPi);
    ORE_ASSERT(aspect > 0.0f);
    ORE_ASSERT(near_plane > 0.0f);
    ORE_ASSERT(far_plane > near_plane);

    const f32 tan_half_fov = std::tan(fov_y_radians * 0.5f);

    Mat4 result{0.0f};
    result[0][0] = 1.0f / (aspect * tan_half_fov);
    result[1][1] = -1.0f / tan_half_fov; // Vulkan Y-down flip
    result[2][2] = far_plane / (near_plane - far_plane);
    result[2][3] = -1.0f;
    result[3][2] = (far_plane * near_plane) / (near_plane - far_plane);
    return result;
}

/// Same as perspective() but with clip-space +Y pointing up (OpenGL convention).
///
/// Vulkan presents this form correctly when the viewport is created with a
/// negative height (VK_KHR_maintenance1, core in Vulkan 1.1). Useful when porting
/// OpenGL code or when a render target expects Y-up NDC. Depth is still 0..1.
[[nodiscard]] inline Mat4 perspective_flipped(f32 fov_y_radians, f32 aspect, f32 near_plane, f32 far_plane) {
    Mat4 result = perspective(fov_y_radians, aspect, near_plane, far_plane);
    result[1][1] = -result[1][1];
    return result;
}

/// Right-handed orthographic projection into Vulkan clip space (depth 0..1, Y down).
///
/// left/right/bottom/top are world-space bounds; p near_plane and p far_plane are
/// distances along the camera's -Z axis, so z = -near_plane maps to depth 0 and
/// z = -far_plane to depth 1. As in perspective(), Y is flipped, so the top bound
/// still means "up on screen". For a top-left origin 2D system pass
/// bottom = height, top = 0.
[[nodiscard]] inline Mat4 orthographic(f32 left, f32 right, f32 bottom, f32 top, f32 near_plane,
                                       f32 far_plane) {
    ORE_ASSERT(right != left);
    ORE_ASSERT(top != bottom);
    ORE_ASSERT(far_plane != near_plane);

    const f32 width = right - left;
    const f32 height = top - bottom;
    const f32 depth = far_plane - near_plane;

    Mat4 result{0.0f};
    result[0][0] = 2.0f / width;
    result[1][1] = -2.0f / height; // Vulkan Y-down flip
    result[2][2] = -1.0f / depth;  // depth 0..1
    result[3][0] = -(right + left) / width;
    result[3][1] = (top + bottom) / height;
    result[3][2] = -near_plane / depth;
    result[3][3] = 1.0f;
    return result;
}

/// Right-handed view matrix: the world -> view transform of a camera at p eye
/// looking towards p center. p up must not be parallel to the view direction and
/// does not need to be normalized. The camera sees +X right, +Y up and -Z forward.
[[nodiscard]] inline Mat4 look_at(const Vec3& eye, const Vec3& center, const Vec3& up) {
    const Vec3 forward = center - eye;
    const Vec3 side = glm::cross(forward, up);
    ORE_ASSERT(glm::dot(forward, forward) > 0.0f);
    ORE_ASSERT(glm::dot(side, side) > 0.0f);
    return glm::lookAt(eye, center, up);
}

/// Model matrix in T * R * S order: scale first, then rotate, then translate.
/// p rotation is expected to be a unit quaternion (see euler_angles()).
[[nodiscard]] inline Mat4 compose(const Vec3& position, const Quat& rotation, const Vec3& scale) {
    const Mat4 translation = glm::translate(Mat4{1.0f}, position);
    const Mat4 rotation_matrix = glm::mat4_cast(rotation);
    const Mat4 scale_matrix = glm::scale(Mat4{1.0f}, scale);
    return translation * rotation_matrix * scale_matrix;
}

/// YXZ Euler angles as a unit quaternion: yaw about +Y first, then pitch about
/// the rotated +X, then roll about the resulting +Z.
///
/// With a right-handed world, positive pitch looks up and positive yaw turns left
/// (counter-clockwise seen from above). Equivalent to glm::eulerAngleYXZ() without
/// pulling in the GLM experimental (GTX) headers.
[[nodiscard]] inline Quat euler_angles(f32 pitch_radians, f32 yaw_radians, f32 roll_radians) {
    const Quat yaw = glm::angleAxis(yaw_radians, Vec3{0.0f, 1.0f, 0.0f});
    const Quat pitch = glm::angleAxis(pitch_radians, Vec3{1.0f, 0.0f, 0.0f});
    const Quat roll = glm::angleAxis(roll_radians, Vec3{0.0f, 0.0f, 1.0f});
    return glm::normalize(yaw * pitch * roll);
}

/// Axis-aligned bounding box. min/max are inclusive on every axis.
struct Aabb {
    Vec3 min{0.0f};
    Vec3 max{0.0f};

    /// Box centred on p center with half size p extents; negative extents are
    /// treated as their absolute value.
    [[nodiscard]] static Aabb from_center_extents(const Vec3& center, const Vec3& extents) {
        const Vec3 half = glm::abs(extents);
        return Aabb{center - half, center + half};
    }

    [[nodiscard]] Vec3 center() const { return (min + max) * 0.5f; }
    [[nodiscard]] Vec3 extents() const { return (max - min) * 0.5f; }
    [[nodiscard]] Vec3 size() const { return max - min; }

    /// Grows the box so that p point is inside it.
    void expand(const Vec3& point) {
        min = glm::min(min, point);
        max = glm::max(max, point);
    }

    /// Inclusive containment test: points on the boundary count as inside.
    [[nodiscard]] bool contains(const Vec3& point) const {
        return glm::all(glm::greaterThanEqual(point, min)) && glm::all(glm::lessThanEqual(point, max));
    }

    /// Box of the 8 transformed corners. An affine p matrix simply moves the
    /// corners; a projective one (e.g. a projection matrix) is divided by w.
    [[nodiscard]] Aabb transformed(const Mat4& matrix) const {
        const auto corner = [this](int index) {
            return Vec3{(index & 1) != 0 ? max.x : min.x, (index & 2) != 0 ? max.y : min.y,
                        (index & 4) != 0 ? max.z : min.z};
        };
        const auto project = [&matrix](const Vec3& point) {
            Vec4 clip = matrix * Vec4{point, 1.0f};
            if (clip.w != 0.0f) {
                clip /= clip.w; // w == 0 keeps a point at infinity undivided
            }
            return Vec3{clip};
        };

        Aabb result;
        result.min = result.max = project(corner(0));
        for (int index = 1; index < 8; ++index) {
            result.expand(project(corner(index)));
        }
        return result;
    }

    /// Inclusive overlap test on all three axes; boxes that merely touch count as
    /// intersecting.
    [[nodiscard]] bool intersects(const Aabb& other) const {
        return glm::all(glm::lessThanEqual(min, other.max)) &&
               glm::all(glm::greaterThanEqual(max, other.min));
    }
};

} // namespace ore
