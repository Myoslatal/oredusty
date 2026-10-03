// Ore framework - perspective camera and camera controllers.
#include <ore/math/camera.h>

#include <algorithm>
#include <cmath>

namespace ore {
namespace {

/// A full turn; used to keep the orbit yaw in a small range.
constexpr f32 kTwoPi = 2.0f * kPi;
/// Fly speed limits; the scroll wheel stays inside them.
constexpr f32 kMinMoveSpeed = 0.01f;
constexpr f32 kMaxMoveSpeed = 1000.0f;
/// One wheel notch scales the fly speed by exp(0.1), about 10%.
constexpr f32 kScrollSpeedScale = 0.1f;
/// One wheel notch scales the orbit distance by exp(-0.15), about 14%.
constexpr f32 kZoomPerNotch = 0.15f;
/// Panning moves the target by distance * 0.001 world units per pixel.
constexpr f32 kPanUnitsPerPixel = 0.001f;

/// Wraps p angle into (-pi, pi] so that long sessions keep full float precision.
[[nodiscard]] f32 wrap_angle(f32 angle) {
    f32 wrapped = std::fmod(angle + kPi, kTwoPi);
    if (wrapped < 0.0f) {
        wrapped += kTwoPi;
    }
    return wrapped - kPi;
}

/// Target -> camera offset: yaw = pitch = 0 puts the camera on +Z, and positive
/// yaw rotates the camera counter-clockwise seen from above (+Z towards +X).
[[nodiscard]] Vec3 orbit_offset(f32 yaw, f32 pitch, f32 distance) {
    const f32 cos_pitch = std::cos(pitch);
    return Vec3{std::sin(yaw) * cos_pitch, std::sin(pitch), std::cos(yaw) * cos_pitch} * distance;
}

} // namespace

// ------------------------------------------------------------ PerspectiveCamera --
Mat4 PerspectiveCamera::view() const {
    // Inverse of the camera transform (translation * rotation): rotate into view
    // space, then translate by -position. The conjugate is the inverse rotation
    // for a unit quaternion.
    const Mat4 rotation = glm::mat4_cast(glm::conjugate(orientation));
    return glm::translate(rotation, -position);
}

Mat4 PerspectiveCamera::projection() const {
    return perspective(radians(fov_y_degrees), aspect, near_plane, far_plane);
}

Mat4 PerspectiveCamera::view_projection() const {
    return projection() * view();
}

Vec3 PerspectiveCamera::forward() const {
    return orientation * Vec3{0.0f, 0.0f, -1.0f};
}

Vec3 PerspectiveCamera::right() const {
    return orientation * Vec3{1.0f, 0.0f, 0.0f};
}

Vec3 PerspectiveCamera::up() const {
    return orientation * Vec3{0.0f, 1.0f, 0.0f};
}

void PerspectiveCamera::set_aspect(f32 width, f32 height) {
    ORE_ASSERT(width > 0.0f);
    ORE_ASSERT(height > 0.0f);
    aspect = width / height;
}

void PerspectiveCamera::set_aspect_from_size(u32 width, u32 height) {
    ORE_ASSERT(width > 0u);
    ORE_ASSERT(height > 0u);
    set_aspect(static_cast<f32>(width), static_cast<f32>(height));
}

Vec3 PerspectiveCamera::world_from_screen(const Vec2& ndc, f32 depth) const {
    Vec4 world = glm::inverse(view_projection()) * Vec4{ndc.x, ndc.y, depth, 1.0f};
    // Undo the perspective divide. w is 0 only for points at infinity (depth == 1
    // with an infinite far plane).
    world /= world.w;
    return Vec3{world};
}

// --------------------------------------------------------- FlyCameraController --
FlyCameraController::FlyCameraController(PerspectiveCamera& camera_ref, f32 move_speed_value)
    : camera_(&camera_ref), move_speed_(move_speed_value) {
    ORE_ASSERT(move_speed_value >= 0.0f);
}

void FlyCameraController::update(const CameraInputState& input, f32 delta_seconds) {
    ORE_ASSERT(camera_ != nullptr);

    // The wheel scales the flight speed; multiplicative, so it feels the same at
    // any speed.
    if (input.scroll_delta != 0.0f) {
        const f32 scaled = move_speed_ * std::exp(input.scroll_delta * kScrollSpeedScale);
        move_speed_ = std::clamp(scaled, kMinMoveSpeed, kMaxMoveSpeed);
    }

    // Mouse look. Screen x grows to the right, so turning right is a negative
    // rotation about the local +Y; screen y grows downwards, so moving the mouse
    // down looks down (a negative rotation about the local +X).
    if (input.look_active && (input.look_delta_x != 0.0f || input.look_delta_y != 0.0f)) {
        const f32 yaw_delta = -input.look_delta_x * input.sensitivity;
        const f32 pitch_delta = -input.look_delta_y * input.sensitivity;
        // YXZ in camera space: yaw axis is the local +Y, pitch axis the local +X,
        // so the camera never accumulates roll.
        camera_->orientation =
            glm::normalize(camera_->orientation * euler_angles(pitch_delta, yaw_delta, 0.0f));
    }

    Vec3 direction{0.0f};
    if (input.move_forward) direction += camera_->forward();
    if (input.move_backward) direction -= camera_->forward();
    if (input.move_right) direction += camera_->right();
    if (input.move_left) direction -= camera_->right();
    if (input.move_up) direction += camera_->up();
    if (input.move_down) direction -= camera_->up();

    const f32 length_squared = glm::dot(direction, direction);
    if (length_squared > 0.0f && delta_seconds > 0.0f) {
        direction /= std::sqrt(length_squared); // a diagonal move is not faster
        camera_->position += direction * (move_speed_ * input.speed_multiplier * delta_seconds);
    }
}

void FlyCameraController::set_camera(PerspectiveCamera& camera_ref) {
    camera_ = &camera_ref;
}

PerspectiveCamera& FlyCameraController::camera() const {
    return *camera_;
}

f32 FlyCameraController::move_speed() const {
    return move_speed_;
}

void FlyCameraController::set_move_speed(f32 speed) {
    ORE_ASSERT(speed >= 0.0f);
    move_speed_ = speed;
}

// -------------------------------------------------------- OrbitCameraController --
OrbitCameraController::OrbitCameraController(PerspectiveCamera& camera_ref, const Vec3& target,
                                             f32 distance_value)
    : camera_(&camera_ref), target_(target) {
    ORE_ASSERT(distance_value > 0.0f);
    distance_ = std::clamp(distance_value, min_distance_, max_distance_);

    // Keep the camera where it is: remember the angles of its current offset from
    // the target so the first update() only changes the distance.
    const Vec3 offset = camera_ref.position - target_;
    const f32 length = glm::length(offset);
    if (length > 0.0f) {
        pitch_ = std::clamp(std::asin(std::clamp(offset.y / length, -1.0f, 1.0f)), min_pitch_, max_pitch_);
        yaw_ = std::atan2(offset.x, offset.z);
    }
}

void OrbitCameraController::set_target(const Vec3& target_value) {
    target_ = target_value;
}

const Vec3& OrbitCameraController::target() const {
    return target_;
}

void OrbitCameraController::set_distance(f32 distance_value) {
    ORE_ASSERT(distance_value > 0.0f);
    distance_ = std::clamp(distance_value, min_distance_, max_distance_);
}

f32 OrbitCameraController::distance() const {
    return distance_;
}

void OrbitCameraController::rotate(f32 delta_yaw_radians, f32 delta_pitch_radians) {
    yaw_ = wrap_angle(yaw_ + delta_yaw_radians);
    pitch_ = std::clamp(pitch_ + delta_pitch_radians, min_pitch_, max_pitch_);
}

void OrbitCameraController::zoom(f32 scroll_delta) {
    if (scroll_delta == 0.0f) {
        return;
    }
    // Multiplicative so that a notch feels the same at any distance; the clamp
    // also keeps the distance positive.
    distance_ = std::clamp(distance_ * std::exp(-scroll_delta * kZoomPerNotch), min_distance_, max_distance_);
}

void OrbitCameraController::pan(f32 delta_x, f32 delta_y) {
    ORE_ASSERT(camera_ != nullptr);
    // The target follows the inverse of the drag so the content tracks the cursor:
    // dragging right moves the camera left, dragging down moves it up.
    const f32 units_per_pixel = distance_ * kPanUnitsPerPixel;
    target_ -= camera_->right() * (delta_x * units_per_pixel);
    target_ += camera_->up() * (delta_y * units_per_pixel);
}

void OrbitCameraController::update(f32 delta_seconds) {
    ORE_ASSERT(camera_ != nullptr);

    const Vec3 desired_position = target_ + orbit_offset(yaw_, pitch_, distance_);
    // euler_angles(-pitch, yaw, 0) points the camera's -Z axis from its position
    // back at the target.
    const Quat desired_orientation = euler_angles(-pitch_, yaw_, 0.0f);

    f32 blend = 1.0f; // snap
    if (smoothing_ > 0.0f) {
        ORE_ASSERT(delta_seconds >= 0.0f);
        // Frame-rate independent exponential approach.
        blend = 1.0f - std::exp(-smoothing_ * delta_seconds);
    }

    camera_->position = glm::mix(camera_->position, desired_position, blend);
    camera_->orientation = glm::normalize(glm::mix(camera_->orientation, desired_orientation, blend));
}

void OrbitCameraController::set_limits(f32 min_pitch, f32 max_pitch, f32 min_distance, f32 max_distance) {
    // The poles would make the orbit basis degenerate (the view direction and the
    // up axis would be parallel).
    ORE_ASSERT(min_pitch > -kPi * 0.5f);
    ORE_ASSERT(max_pitch < kPi * 0.5f);
    ORE_ASSERT(min_pitch <= max_pitch);
    ORE_ASSERT(min_distance > 0.0f);
    ORE_ASSERT(min_distance <= max_distance);

    min_pitch_ = min_pitch;
    max_pitch_ = max_pitch;
    min_distance_ = min_distance;
    max_distance_ = max_distance;
    pitch_ = std::clamp(pitch_, min_pitch_, max_pitch_);
    distance_ = std::clamp(distance_, min_distance_, max_distance_);
}

void OrbitCameraController::set_smoothing(f32 rate) {
    ORE_ASSERT(rate >= 0.0f);
    smoothing_ = rate;
}

f32 OrbitCameraController::smoothing() const {
    return smoothing_;
}

} // namespace ore
