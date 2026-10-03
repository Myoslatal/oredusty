#include <ore/core/file.h>
#include <ore/core/image.h>

#include <support/test_support.h>

#include <cstdio>

using namespace ore;

ORE_TEST(image_create_and_pixels) {
    Image image = Image::create(8, 4, make_rgba(10, 20, 30, 255));
    ORE_CHECK_EQ(image.width, 8u);
    ORE_CHECK_EQ(image.height, 4u);
    ORE_CHECK_EQ(image.byte_size(), 8u * 4u * 4u);
    ORE_CHECK_EQ(image.pixel(3, 2), make_rgba(10, 20, 30, 255));
    image.set_pixel(3, 2, make_rgba(1, 2, 3, 4));
    ORE_CHECK_EQ(image.pixel(3, 2), make_rgba(1, 2, 3, 4));
    const Color color = Color::from_packed(image.pixel(3, 2));
    ORE_CHECK_EQ(color.r, 1);
    ORE_CHECK_EQ(color.g, 2);
    ORE_CHECK_EQ(color.b, 3);
    ORE_CHECK_EQ(color.a, 4);
}

ORE_TEST(image_flip_vertical) {
    Image image = Image::create(2, 2, 0);
    image.set_pixel(0, 0, 1);
    image.set_pixel(1, 1, 2);
    image.flip_vertical();
    ORE_CHECK_EQ(image.pixel(0, 1), 1u);
    ORE_CHECK_EQ(image.pixel(1, 0), 2u);
}

ORE_TEST(image_png_roundtrip) {
    Image source = make_checkerboard(37, 5, make_rgba(255, 0, 0, 255), make_rgba(0, 0, 255, 128));
    const auto encoded = source.encode_png();
    ORE_REQUIRE(encoded.has_value());
    ORE_CHECK(encoded->size() > 8u);
    // PNG signature.
    ORE_CHECK_EQ((*encoded)[0], 0x89);
    ORE_CHECK_EQ((*encoded)[1], 'P');
    ORE_CHECK_EQ((*encoded)[2], 'N');
    ORE_CHECK_EQ((*encoded)[3], 'G');

    const auto decoded = Image::decode_png(*encoded);
    ORE_REQUIRE(decoded.has_value());
    ORE_CHECK_EQ(decoded->width, source.width);
    ORE_CHECK_EQ(decoded->height, source.height);
    ORE_CHECK_EQ(decoded->pixels, source.pixels);
}

ORE_TEST(image_png_roundtrip_odd_sizes) {
    for (u32 size : {1u, 2u, 3u, 16u, 33u}) {
        Image source = make_color_gradient(size, size + 1);
        const auto encoded = source.encode_png();
        ORE_REQUIRE(encoded.has_value());
        const auto decoded = Image::decode_png(*encoded);
        ORE_REQUIRE(decoded.has_value());
        ORE_CHECK_EQ(decoded->pixels, source.pixels);
    }
}

ORE_TEST(image_png_file_roundtrip) {
    const std::string path = path_join(current_dir(), "ore_test_image.png");
    Image source = make_checkerboard(16, 4, make_rgba(255, 255, 255, 255), make_rgba(0, 0, 0, 255));
    ORE_REQUIRE(source.save_png(path));
    const auto loaded = Image::load_png(path);
    ORE_REQUIRE(loaded.has_value());
    ORE_CHECK_EQ(loaded->pixels, source.pixels);
    std::remove(path.c_str());
    ORE_CHECK_FALSE(path_exists(path));
}

ORE_TEST(image_png_rejects_garbage) {
    const std::vector<u8> garbage = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    ORE_CHECK_FALSE(Image::decode_png(garbage).has_value());
    std::vector<u8> truncated = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
    ORE_CHECK_FALSE(Image::decode_png(truncated).has_value());
}

ORE_TEST_MAIN
