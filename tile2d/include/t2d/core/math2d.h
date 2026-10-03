// Tile2D - 2D math. Deliberately dependency free: the simulation must produce identical results
// on the server, the client and the bot, so everything here is plain IEEE-754 arithmetic.
#pragma once

#include <t2d/core/types.h>

#include <cmath>

namespace t2d {

struct Vec2 {
    f32 x = 0.0f;
    f32 y = 0.0f;

    [[nodiscard]] static constexpr Vec2 splat(f32 value) { return Vec2{value, value}; }
    friend constexpr Vec2 operator+(Vec2 a, Vec2 b) { return Vec2{a.x + b.x, a.y + b.y}; }
    friend constexpr Vec2 operator-(Vec2 a, Vec2 b) { return Vec2{a.x - b.x, a.y - b.y}; }
    friend constexpr Vec2 operator*(Vec2 a, f32 scalar) { return Vec2{a.x * scalar, a.y * scalar}; }
    friend constexpr Vec2 operator*(f32 scalar, Vec2 a) { return Vec2{a.x * scalar, a.y * scalar}; }
    friend constexpr Vec2 operator/(Vec2 a, f32 scalar) { return Vec2{a.x / scalar, a.y / scalar}; }
    friend constexpr Vec2 operator-(Vec2 a) { return Vec2{-a.x, -a.y}; }
    constexpr Vec2& operator+=(Vec2 other) { x += other.x; y += other.y; return *this; }
    constexpr Vec2& operator-=(Vec2 other) { x -= other.x; y -= other.y; return *this; }
    constexpr Vec2& operator*=(f32 scalar) { x *= scalar; y *= scalar; return *this; }
    friend constexpr bool operator==(const Vec2&, const Vec2&) = default;
};

[[nodiscard]] constexpr f32 dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
[[nodiscard]] constexpr f32 length_squared(Vec2 v) { return dot(v, v); }
[[nodiscard]] inline f32 length(Vec2 v) { return std::sqrt(length_squared(v)); }
[[nodiscard]] inline Vec2 normalize(Vec2 v) {
    const f32 len = length(v);
    return len > 1e-6f ? v / len : Vec2{};
}
[[nodiscard]] constexpr f32 cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }

[[nodiscard]] constexpr f32 clamp(f32 value, f32 low, f32 high) {
    return value < low ? low : (value > high ? high : value);
}
[[nodiscard]] constexpr i32 clamp_i32(i32 value, i32 low, i32 high) {
    return value < low ? low : (value > high ? high : value);
}
[[nodiscard]] constexpr f32 lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }
[[nodiscard]] constexpr Vec2 lerp(Vec2 a, Vec2 b, f32 t) { return a + (b - a) * t; }
[[nodiscard]] constexpr f32 sign_of(f32 value) { return value > 0.0f ? 1.0f : (value < 0.0f ? -1.0f : 0.0f); }
[[nodiscard]] constexpr f32 abs_f32(f32 value) { return value < 0.0f ? -value : value; }
[[nodiscard]] inline f32 approach(f32 value, f32 target, f32 max_delta) {
    const f32 difference = target - value;
    if (abs_f32(difference) <= max_delta) return target;
    return value + sign_of(difference) * max_delta;
}
[[nodiscard]] constexpr i32 floor_to_i32(f32 value) {
    const i32 truncated = static_cast<i32>(value);
    return (value < static_cast<f32>(truncated)) ? truncated - 1 : truncated;
}
[[nodiscard]] constexpr i32 ceil_to_i32(f32 value) {
    const i32 truncated = static_cast<i32>(value);
    return (value > static_cast<f32>(truncated)) ? truncated + 1 : truncated;
}

/// Axis aligned box, defined by its top-left corner plus size (screen style, +Y points down).
struct Aabb2 {
    Vec2 min{};
    Vec2 max{};

    [[nodiscard]] static constexpr Aabb2 from_center(Vec2 center, Vec2 half_extents) {
        return Aabb2{center - half_extents, center + half_extents};
    }
    [[nodiscard]] static constexpr Aabb2 from_top_left(Vec2 top_left, Vec2 size) {
        return Aabb2{top_left, top_left + size};
    }
    [[nodiscard]] constexpr Vec2 size() const { return max - min; }
    [[nodiscard]] constexpr Vec2 center() const { return (min + max) * 0.5f; }
    [[nodiscard]] constexpr f32 width() const { return max.x - min.x; }
    [[nodiscard]] constexpr f32 height() const { return max.y - min.y; }

    /// Touching edges do not count as an overlap (important for tile collision).
    [[nodiscard]] constexpr bool overlaps(const Aabb2& other) const {
        return min.x < other.max.x && max.x > other.min.x && min.y < other.max.y && max.y > other.min.y;
    }
    [[nodiscard]] constexpr bool contains(Vec2 point) const {
        return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y;
    }
    [[nodiscard]] constexpr Aabb2 translated(Vec2 offset) const { return Aabb2{min + offset, max + offset}; }
    [[nodiscard]] constexpr Aabb2 expanded(f32 amount) const {
        return Aabb2{Vec2{min.x - amount, min.y - amount}, Vec2{max.x + amount, max.y + amount}};
    }
    friend constexpr bool operator==(const Aabb2&, const Aabb2&) = default;
};

/// Integer rectangle in tile coordinates (half open: [x, x + w) x [y, y + h)).
struct TileRect {
    i32 x = 0;
    i32 y = 0;
    i32 width = 0;
    i32 height = 0;

    [[nodiscard]] constexpr i32 right() const { return x + width; }
    [[nodiscard]] constexpr i32 bottom() const { return y + height; }
    [[nodiscard]] constexpr bool empty() const { return width <= 0 || height <= 0; }
    [[nodiscard]] constexpr bool contains(i32 px, i32 py) const {
        return px >= x && px < right() && py >= y && py < bottom();
    }
    friend constexpr bool operator==(const TileRect&, const TileRect&) = default;
};

} // namespace t2d
