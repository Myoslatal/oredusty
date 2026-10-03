// Ore framework - perspective camera and framework-agnostic camera controllers.
//
// A camera is a world-space transform (position + unit quaternion orientation,
// no scale): it looks down its local -Z axis with local +X right and local +Y up
// (right-handed). The view matrix is the inverse of that transform and the
// projection follows the Vulkan conventions documented in <ore/math/math.h>.
//
// The controllers never touch a window API: the platform layer fills a
// CameraInputState and the controllers apply it to the camera.
#pragma once

#include <ore/math/math.h>

namespace ore {

/// Perspective camera.
///
/// fov_y_degrees is the full vertical field of view; aspect is width / height.
/// near_plane / far_plane are distances along the view axis (both positive).
struct PerspectiveCamera {
    f32 fov_y_degrees = 60.0f;
    f32 aspect = 16.0f / 9.0f;
    f32 near_plane = 0.1f;
    f32 far_plane = 100.0f;

    Vec3 position{0.0f, 0.0f, 0.0f};
    Quat orientation{1.0f, 0.0f, 0.0f, 0.0f}; // identity: looking down -Z, +Y up

    /// World -> view: the inverse of the camera transform.
    [[nodiscard]] Mat4 view() const;

    /// View -> clip, Vulkan conventions: depth 0..1, Y down.
    [[nodiscard]] Mat4 projection() const;

    [[nodiscard]] Mat4 view_projection() const;

    /// Unit basis vectors of the camera, in world space.
    [[nodiscard]] Vec3 forward() const;
    [[nodiscard]] Vec3 right() const;
    [[nodiscard]] Vec3 up() const;

    /// Sets aspect = width / height. Both must be positive.
    void set_aspect(f32 width, f32 height);
    void set_aspect_from_size(u32 width, u32 height);

    /// Unprojects p ndc (x, y in [-1, 1]) at p depth (0 = near plane, 1 = far
    /// plane, Vulkan convention) into world space. Use the camera centre as the
    /// ray origin for picking.
    [[nodiscard]] Vec3 world_from_screen(const Vec2& ndc, f32 depth) const;
};

/// Framework-agnostic input that drives a camera controller. The window layer
/// fills this in every frame.
struct CameraInputState {
    bool move_forward = false;
    bool move_backward = false;
    bool move_left = false;
    bool move_right = false;
    bool move_up = false;
    bool move_down = false;

    /// True while the look button / drag is held.
    bool look_active = false;
    /// Mouse movement since the previous frame, in pixels, screen y growing downwards.
    f32 look_delta_x = 0.0f;
    f32 look_delta_y = 0.0f;

    /// Wheel notches since the previous frame; positive scrolls up / away from the user.
    f32 scroll_delta = 0.0f;
    /// Scales this frame's movement (e.g. a sprint modifier). Negative values reverse it.
    f32 speed_multiplier = 1.0f;
    /// Radians of rotation per pixel of mouse movement.
    f32 sensitivity = 0.0015f;
};

/// WASD + mouse-look fly camera.
///
/// Movement follows the camera's own basis, so the camera flies where it looks,
/// and a diagonal move is not faster than a straight one. Mouse look applies yaw
/// about the local +Y and pitch about the local +X (no roll); the scroll wheel
/// scales move_speed multiplicatively. The referenced camera must outlive the
/// controller.
class FlyCameraController {
public:
    explicit FlyCameraController(PerspectiveCamera& camera, f32 move_speed = 4.0f);

    /// Applies p input to the camera. p delta_seconds scales movement only;
    /// mouse look is applied as-is (it is already a per-frame delta).
    void update(const CameraInputState& input, f32 delta_seconds);

    /// Rebinds the controller to another camera.
    void set_camera(PerspectiveCamera& camera);
    [[nodiscard]] PerspectiveCamera& camera() const;

    /// World units per second, before CameraInputState::speed_multiplier.
    [[nodiscard]] f32 move_speed() const;
    void set_move_speed(f32 speed);

private:
    PerspectiveCamera* camera_ = nullptr;
    f32 move_speed_ = 4.0f;
};

/// Orbits a camera around a target point.
///
/// The orbit state is a yaw (radians about world +Y), a pitch (radians above the
/// XZ plane) and a distance to the target. rotate(), zoom() and pan() change that
/// state; update() recomputes the camera transform from it. The referenced camera
/// must outlive the controller.
class OrbitCameraController {
public:
    /// Keeps the camera's current direction from p target (the angles are derived
    /// from its current position) and does not move the camera until update() is
    /// called. p distance is clamped to the distance limits.
    explicit OrbitCameraController(PerspectiveCamera& camera, const Vec3& target = Vec3{0.0f},
                                   f32 distance = 5.0f);

    void set_target(const Vec3& target);
    [[nodiscard]] const Vec3& target() const;

    void set_distance(f32 distance);
    [[nodiscard]] f32 distance() const;

    /// Adds to the orbit angles; pitch is clamped to the pitch limits. Positive
    /// yaw orbits counter-clockwise seen from above, positive pitch lifts the
    /// camera above the target.
    void rotate(f32 delta_yaw_radians, f32 delta_pitch_radians);

    /// Scales the distance by exp(-scroll_delta * k), clamped to
    /// [min_distance, max_distance]. Positive p scroll_delta (wheel up) zooms in.
    void zoom(f32 scroll_delta);

    /// Moves the target inside the camera plane. p delta_x and p delta_y are
    /// mouse deltas in pixels (screen y downwards) and the target follows the
    /// inverse of the drag, so the content tracks the cursor. The world-space
    /// step scales with the current distance.
    void pan(f32 delta_x, f32 delta_y);

    /// Recomputes the camera position and orientation from yaw/pitch/distance/target.
    /// With the default smoothing of 0 the camera snaps to that pose; otherwise it
    /// approaches it exponentially with the smoothing rate, and p delta_seconds is
    /// the elapsed time in seconds.
    void update(f32 delta_seconds);

    /// Sets the pitch (radians, within +-pi/2) and distance limits; the current
    /// pitch and distance are clamped to the new limits.
    void set_limits(f32 min_pitch, f32 max_pitch, f32 min_distance, f32 max_distance);

    /// 0 (the default) makes update() snap; a positive rate smooths it. The rate
    /// is the reciprocal of the approach time constant, in 1/seconds.
    void set_smoothing(f32 rate);
    [[nodiscard]] f32 smoothing() const;

private:
    PerspectiveCamera* camera_ = nullptr;
    Vec3 target_{0.0f};
    f32 distance_ = 5.0f;
    f32 yaw_ = 0.0f;
    f32 pitch_ = 0.0f;
    f32 min_pitch_ = -radians(85.0f);
    f32 max_pitch_ = radians(85.0f);
    f32 min_distance_ = 0.1f;
    f32 max_distance_ = 1000.0f;
    f32 smoothing_ = 0.0f;
};

} // namespace ore
