#include <t2d/text/raster.h>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace t2d {
namespace {

/// Flattening tolerance in pixels: how far a curve may deviate from its polyline.
constexpr f32 kTolerance = 0.2f;

struct Edge {
    f32 x0 = 0.0f;
    f32 y0 = 0.0f;
    f32 x1 = 0.0f;
    f32 y1 = 0.0f;
    i32 direction = 1;
};

/// Number of line segments a curve needs so that its polyline stays within the tolerance.
[[nodiscard]] u32 segments_for(f32 deviation_px, f32 length_px) {
    if (deviation_px <= kTolerance) return 1;
    const f32 count = std::sqrt(deviation_px / kTolerance) * 1.5f;
    return static_cast<u32>(std::clamp(count, 1.0f, std::max(2.0f, length_px)));
}

struct Flattener {
    std::vector<Edge>& edges;
    f32 scale = 1.0f;
    bool flip_y = true;
    f32 origin_x = 0.0f;
    f32 origin_y = 0.0f;
    f32 current_x = 0.0f;
    f32 current_y = 0.0f;
    f32 start_x = 0.0f;
    f32 start_y = 0.0f;

    [[nodiscard]] f32 px(f32 x) const { return x * scale - origin_x; }
    [[nodiscard]] f32 py(f32 y) const { return flip_y ? -y * scale - origin_y : y * scale - origin_y; }

    void line_to(f32 x, f32 y) {
        const f32 x0 = px(current_x);
        const f32 y0 = py(current_y);
        const f32 x1 = px(x);
        const f32 y1 = py(y);
        current_x = x;
        current_y = y;
        if (y0 == y1) return; // horizontal edges carry no coverage
        Edge edge;
        if (y0 < y1) {
            edge = Edge{x0, y0, x1, y1, 1};
        } else {
            edge = Edge{x1, y1, x0, y0, -1};
        }
        edges.push_back(edge);
    }

    void quad_to(f32 cx, f32 cy, f32 x, f32 y) {
        const f32 x0 = px(current_x);
        const f32 y0 = py(current_y);
        const f32 cxp = px(cx);
        const f32 cyp = py(cy);
        const f32 x1 = px(x);
        const f32 y1 = py(y);
        const f32 deviation = std::fabs(x0 - 2.0f * cxp + x1) + std::fabs(y0 - 2.0f * cyp + y1);
        const f32 length = std::fabs(x0 - cxp) + std::fabs(y0 - cyp) + std::fabs(cxp - x1) + std::fabs(cyp - y1);
        const u32 segments = segments_for(deviation * 0.25f, length);
        f32 previous_x = current_x;
        f32 previous_y = current_y;
        for (u32 index = 1; index <= segments; ++index) {
            const f32 t = static_cast<f32>(index) / static_cast<f32>(segments);
            const f32 inverse = 1.0f - t;
            const f32 sx = inverse * inverse * previous_x + 2.0f * inverse * t * cx + t * t * x;
            const f32 sy = inverse * inverse * previous_y + 2.0f * inverse * t * cy + t * t * y;
            line_to(sx, sy);
        }
        current_x = x;
        current_y = y;
    }

    void cubic_to(f32 c1x, f32 c1y, f32 c2x, f32 c2y, f32 x, f32 y) {
        const f32 x0 = px(current_x);
        const f32 y0 = py(current_y);
        const f32 p1x = px(c1x);
        const f32 p1y = py(c1y);
        const f32 p2x = px(c2x);
        const f32 p2y = py(c2y);
        const f32 x1 = px(x);
        const f32 y1 = py(y);
        const f32 d1 = std::fabs(x0 - 2.0f * p1x + p2x) + std::fabs(y0 - 2.0f * p1y + p2y);
        const f32 d2 = std::fabs(p1x - 2.0f * p2x + x1) + std::fabs(p1y - 2.0f * p2y + y1);
        const f32 length = std::fabs(x0 - p1x) + std::fabs(y0 - p1y) + std::fabs(p1x - p2x) + std::fabs(p1y - p2y) +
                           std::fabs(p2x - x1) + std::fabs(p2y - y1);
        const u32 segments = segments_for(std::max(d1, d2) * 0.75f, length);
        f32 previous_x = current_x;
        f32 previous_y = current_y;
        for (u32 index = 1; index <= segments; ++index) {
            const f32 t = static_cast<f32>(index) / static_cast<f32>(segments);
            const f32 inverse = 1.0f - t;
            const f32 a = inverse * inverse * inverse;
            const f32 b = 3.0f * inverse * inverse * t;
            const f32 c = 3.0f * inverse * t * t;
            const f32 d = t * t * t;
            const f32 sx = a * previous_x + b * c1x + c * c2x + d * x;
            const f32 sy = a * previous_y + b * c1y + c * c2y + d * y;
            line_to(sx, sy);
        }
        current_x = x;
        current_y = y;
    }
};

/// Exact area of the pixel column [column, column + 1] covered by the linear edge piece
/// (xa, ya) -> (xb, yb), where the edge is guaranteed to run downwards (ya < yb).
///
/// The edge is x(y) = xa + slope * (y - ya). For the column, the covered fraction is the integral of
/// clamp(x(y) - column, 0, 1) dy over the piece, which is piecewise quadratic and has a closed form:
/// split the y range where x(y) crosses the column's left and right borders.
[[nodiscard]] f32 column_area(f32 xa, f32 ya, f32 xb, f32 yb, i32 column) {
    const f32 left = static_cast<f32>(column);
    const f32 ua = xa - left; // where the edge sits relative to the column's left border, at the top
    const f32 ub = xb - left;
    const f32 height = yb - ya;
    if (ua >= 1.0f && ub >= 1.0f) return height; // the whole piece is right of the column
    if (ua <= 0.0f && ub <= 0.0f) return 0.0f;   // entirely left of it
    const f32 span = ub - ua;
    if (span == 0.0f) return std::clamp(ua, 0.0f, 1.0f) * height; // a vertical edge inside the column

    // u(y) is linear, so the integral of clamp(u, 0, 1) is exact once the piece is split where it
    // crosses the column's borders: each part then lies entirely inside one regime.
    const auto u_at = [&](f32 y) { return ua + span * (y - ya) / height; };
    const f32 cross_left = ya + (0.0f - ua) / span * height;
    const f32 cross_right = ya + (1.0f - ua) / span * height;
    f32 breaks[4] = {ya, yb, std::clamp(cross_left, ya, yb), std::clamp(cross_right, ya, yb)};
    std::sort(std::begin(breaks), std::end(breaks));

    f32 area = 0.0f;
    for (usize index = 0; index + 1 < 4; ++index) {
        const f32 y_start = breaks[index];
        const f32 y_end = breaks[index + 1];
        if (y_end <= y_start) continue;
        const f32 middle = std::clamp((u_at(y_start) + u_at(y_end)) * 0.5f, 0.0f, 1.0f);
        area += middle * (y_end - y_start);
    }
    return area;
}

} // namespace

u64 CoverageBitmap::ink() const {
    u64 total = 0;
    for (const u8 value : pixels) total += value;
    return total;
}

GlyphBitmap rasterize_glyph(const GlyphPath& path, f32 scale, u32 padding, bool flip_y) {
    GlyphBitmap result;
    if (path.empty() || scale <= 0.0f) return result;

    FontPoint min{};
    FontPoint max{};
    if (!path.bounds(min, max)) return result;
    const f32 min_x = min.x * scale;
    const f32 max_x = max.x * scale;
    const f32 min_y = flip_y ? -max.y * scale : min.y * scale;
    const f32 max_y = flip_y ? -min.y * scale : max.y * scale;

    const i32 left = static_cast<i32>(std::floor(min_x)) - static_cast<i32>(padding);
    const i32 top = static_cast<i32>(std::floor(min_y)) - static_cast<i32>(padding);
    const i32 right = static_cast<i32>(std::ceil(max_x)) + static_cast<i32>(padding);
    const i32 bottom = static_cast<i32>(std::ceil(max_y)) + static_cast<i32>(padding);
    const u32 width = static_cast<u32>(std::max(right - left, 0));
    const u32 height = static_cast<u32>(std::max(bottom - top, 0));
    if (width == 0 || height == 0 || width > 4096 || height > 4096) return result;

    // Flatten the outline into edges in bitmap pixel space.
    std::vector<Edge> edges;
    edges.reserve(path.commands().size() * 4);
    Flattener flattener{edges, scale, flip_y, static_cast<f32>(left), static_cast<f32>(top)};
    for (const PathCommand& command : path.commands()) {
        switch (command.verb) {
            case PathVerb::Move:
                flattener.current_x = command.points[0].x;
                flattener.current_y = command.points[0].y;
                flattener.start_x = command.points[0].x;
                flattener.start_y = command.points[0].y;
                break;
            case PathVerb::Line:
                flattener.line_to(command.points[0].x, command.points[0].y);
                break;
            case PathVerb::Quad:
                flattener.quad_to(command.points[0].x, command.points[0].y, command.points[1].x,
                                  command.points[1].y);
                break;
            case PathVerb::Cubic:
                flattener.cubic_to(command.points[0].x, command.points[0].y, command.points[1].x,
                                   command.points[1].y, command.points[2].x, command.points[2].y);
                break;
            case PathVerb::Close:
                flattener.line_to(flattener.start_x, flattener.start_y);
                flattener.current_x = flattener.start_x;
                flattener.current_y = flattener.start_y;
                break;
        }
    }
    if (edges.empty()) return result;

    // Coverage accumulates in two parts per edge and row:
    //   * columns strictly left of the edge's leftmost x are covered by the full height of the piece,
    //     which a difference array adds in constant time (a vertical edge on the right border covers
    //     every column to its left, so this is most of the shape, not an edge case);
    //   * the columns the edge actually passes through get their exact partial area.
    std::vector<f32> coverage(static_cast<usize>(width) * height, 0.0f);
    std::vector<f32> full_delta(static_cast<usize>(width + 1) * height, 0.0f);
    for (const Edge& edge : edges) {
        const f32 slope = (edge.x1 - edge.x0) / (edge.y1 - edge.y0);
        const i32 first_row = std::max<i32>(static_cast<i32>(std::floor(edge.y0)), 0);
        const i32 last_row = std::min<i32>(static_cast<i32>(std::ceil(edge.y1)), static_cast<i32>(height));
        for (i32 row = first_row; row < last_row; ++row) {
            const f32 row_top = static_cast<f32>(row);
            const f32 row_bottom = row_top + 1.0f;
            const f32 ya = std::max(edge.y0, row_top);
            const f32 yb = std::min(edge.y1, row_bottom);
            if (yb <= ya) continue;
            const f32 xa = edge.x0 + slope * (ya - edge.y0);
            const f32 xb = edge.x0 + slope * (yb - edge.y0);
            const f32 direction = static_cast<f32>(edge.direction);
            const f32 piece_height = yb - ya;

            const i32 last_full =
                std::min<i32>(static_cast<i32>(std::floor(std::min(xa, xb) - 1.0f)), static_cast<i32>(width) - 1);
            if (last_full >= 0) {
                const f32 value = direction * piece_height;
                full_delta[static_cast<usize>(row) * (width + 1)] += value;
                full_delta[static_cast<usize>(row) * (width + 1) + static_cast<usize>(last_full) + 1] -= value;
            }

            const i32 first_column = std::max<i32>(static_cast<i32>(std::floor(std::min(xa, xb))), 0);
            const i32 last_column =
                std::min<i32>(static_cast<i32>(std::floor(std::max(xa, xb))), static_cast<i32>(width) - 1);
            for (i32 column = first_column; column <= last_column; ++column) {
                const f32 area = column_area(xa, ya, xb, yb, column);
                if (area == 0.0f) continue;
                coverage[static_cast<usize>(row) * width + static_cast<usize>(column)] += direction * area;
            }
        }
    }
    for (u32 row = 0; row < height; ++row) {
        f32 running = 0.0f;
        for (u32 column = 0; column < width; ++column) {
            running += full_delta[static_cast<usize>(row) * (width + 1) + column];
            coverage[static_cast<usize>(row) * width + column] += running;
        }
    }

    result.bitmap.width = width;
    result.bitmap.height = height;
    result.bitmap.pixels.assign(static_cast<usize>(width) * height, 0u);
    // The accumulation is the signed area of the pixel covered by the shape. Its sign follows the
    // contour direction, and font formats disagree about that (TrueType outlines run clockwise,
    // CFF ones counter-clockwise), so the magnitude is what matters: |winding| != 0 is exactly the
    // non-zero fill rule, and a hole wound the other way still cancels its outer contour.
    u64 ink = 0;
    for (usize index = 0; index < coverage.size(); ++index) {
        const f32 value = std::clamp(std::fabs(coverage[index]), 0.0f, 1.0f);
        const u8 byte = static_cast<u8>(std::lround(value * 255.0f));
        result.bitmap.pixels[index] = byte;
        ink += byte;
    }
    result.left = left;
    result.top = top;
    result.has_ink = ink > 0;
    return result;
}

} // namespace t2d
