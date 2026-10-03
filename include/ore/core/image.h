// Ore framework - in-memory RGBA8 images with a dependency-light PNG codec.
//
// The encoder uses zlib when available and falls back to stored (uncompressed) deflate
// blocks otherwise. The decoder requires zlib and handles the common non-interlaced
// 8-bit PNG colour types (grey, RGB, palette, grey+alpha, RGBA).
#pragma once

#include <ore/core/types.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ore {

/// Tightly packed 8-bit RGBA image, row major, top-left origin.
struct Image {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> pixels;

    [[nodiscard]] static Image create(u32 width, u32 height, u32 rgba_value = 0x00000000u);

    [[nodiscard]] bool empty() const { return width == 0 || height == 0; }
    [[nodiscard]] usize byte_size() const { return static_cast<usize>(width) * height * 4u; }
    [[nodiscard]] u8* data() { return pixels.data(); }
    [[nodiscard]] const u8* data() const { return pixels.data(); }
    [[nodiscard]] u32* row(u32 y) { return reinterpret_cast<u32*>(pixels.data() + static_cast<usize>(y) * width * 4u); }
    [[nodiscard]] const u32* row(u32 y) const {
        return reinterpret_cast<const u32*>(pixels.data() + static_cast<usize>(y) * width * 4u);
    }
    [[nodiscard]] u32 pixel(u32 x, u32 y) const { return row(y)[x]; }
    void set_pixel(u32 x, u32 y, u32 rgba_value) { row(y)[x] = rgba_value; }

    void fill(u32 rgba_value);
    void fill_rect(u32 x, u32 y, u32 w, u32 h, u32 rgba_value);
    void flip_vertical();

    [[nodiscard]] std::optional<std::vector<u8>> encode_png() const;
    bool save_png(std::string_view path) const;
    [[nodiscard]] static std::optional<Image> decode_png(ConstSpan<u8> bytes);
    [[nodiscard]] static std::optional<Image> load_png(std::string_view path);
};

/// Packs 8-bit channels into the 0xAABBGGRR layout used by Image::pixel().
[[nodiscard]] constexpr u32 make_rgba(u8 r, u8 g, u8 b, u8 a = 255) {
    return static_cast<u32>(r) | (static_cast<u32>(g) << 8) | (static_cast<u32>(b) << 16) | (static_cast<u32>(a) << 24);
}

/// Colour of a pixel as seen by the examples and tests: {r, g, b, a}.
struct Color {
    u8 r = 0, g = 0, b = 0, a = 255;
    [[nodiscard]] constexpr u32 packed() const { return make_rgba(r, g, b, a); }
    [[nodiscard]] static constexpr Color from_packed(u32 value) {
        return Color{static_cast<u8>(value & 0xFFu), static_cast<u8>((value >> 8) & 0xFFu),
                     static_cast<u8>((value >> 16) & 0xFFu), static_cast<u8>((value >> 24) & 0xFFu)};
    }
};

/// Procedural images, handy for tests, placeholders and lighting-free demos.
[[nodiscard]] Image make_checkerboard(u32 size, u32 cells, u32 color_a, u32 color_b);
[[nodiscard]] Image make_color_gradient(u32 width, u32 height, u32 top_left = make_rgba(20, 30, 60),
                                        u32 bottom_right = make_rgba(230, 140, 60));

} // namespace ore
