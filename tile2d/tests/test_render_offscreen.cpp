// End to end rendering tests for the 2D renderer: they draw offscreen on whatever Vulkan device is
// available and assert on the pixels that come back, so "the renderer reported draw calls but the
// screen stayed empty" can never happen again. Skips (instead of failing) where there is no device.
#include <t2d/render/atlas.h>
#include <t2d/render/sprite_batch.h>
#include <t2d/render/tilemap_renderer.h>
#include <t2d/sim/tileset.h>

#include <ore/ore.h>

#include <support/test_support.h>

using namespace t2d;

namespace {

struct Fixture {
    Scope<ore::rhi::GraphicsContext> context;
    Scope<ore::Renderer> renderer;
    [[nodiscard]] bool valid() const { return context != nullptr && renderer != nullptr; }
};

[[nodiscard]] Fixture make_fixture(u32 size = 64) {
    Fixture fixture;
    ore::rhi::ContextDesc context_desc;
    context_desc.application_name = "t2d_test_render_offscreen";
    context_desc.enable_validation = false;
    context_desc.headless = true;
    fixture.context = ore::rhi::GraphicsContext::create(context_desc);
    if (fixture.context == nullptr) return fixture;

    ore::Renderer::Desc renderer_desc;
    renderer_desc.context = fixture.context.get();
    renderer_desc.width = size;
    renderer_desc.height = size;
    renderer_desc.depth_format = VK_FORMAT_UNDEFINED; // pure 2D, exactly like the game
    fixture.renderer = ore::Renderer::create(renderer_desc);
    return fixture;
}

[[nodiscard]] Scope<SpriteBatch> make_batch(const Fixture& fixture) {
    SpriteBatch::Options options;
    options.color_format = fixture.renderer->color_format();
    options.depth_format = fixture.renderer->depth_format();
    options.max_quads = 64;
    options.vertex_shader_path = std::string(T2D_TEST_SHADER_DIR) + "/sprite.vert.spv";
    options.fragment_shader_path = std::string(T2D_TEST_SHADER_DIR) + "/sprite.frag.spv";
    return SpriteBatch::create(*fixture.context, options);
}

[[nodiscard]] Scope<ore::rhi::Sampler> make_sampler(const Fixture& fixture) {
    ore::rhi::SamplerDesc desc;
    desc.min_filter = VK_FILTER_NEAREST;
    desc.mag_filter = VK_FILTER_NEAREST;
    desc.mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    return ore::rhi::Sampler::create(fixture.context->device(), desc);
}

/// Renders one full target quad covering the whole view and returns the middle pixel.
template <class Draw>
[[nodiscard]] ore::Color render_and_read_center(Fixture& fixture, SpriteBatch& batch,
                                                ore::rhi::Texture& atlas, ore::rhi::Sampler& sampler,
                                                Draw&& draw) {
    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    ore::RenderFrame& frame = fixture.renderer->begin_frame(1.0f / 60.0f);
    fixture.renderer->begin_pass(clear, 1.0f);
    batch.begin(frame, Vec2{32.0f, 32.0f}, Vec2{64.0f, 64.0f}, atlas, sampler.handle());
    draw(batch);
    batch.end();
    fixture.renderer->end_pass();
    fixture.renderer->end_frame();
    fixture.renderer->wait_idle();

    const ore::Image shot = fixture.context->read_render_target(fixture.renderer->target());
    if (shot.empty()) return {};
    return ore::Color::from_packed(shot.pixel(shot.width / 2, shot.height / 2));
}

[[nodiscard]] u32 count_lit_pixels(const ore::Image& image, u8 threshold) {
    u32 count = 0;
    for (u32 y = 0; y < image.height; ++y) {
        for (u32 x = 0; x < image.width; ++x) {
            const ore::Color pixel = ore::Color::from_packed(image.pixel(x, y));
            if (pixel.r > threshold || pixel.g > threshold || pixel.b > threshold) ++count;
        }
    }
    return count;
}

} // namespace

T2D_TEST(an_opaque_quad_actually_reaches_the_framebuffer) {
    Fixture fixture = make_fixture(64);
    if (!fixture.valid()) T2D_SKIP("no Vulkan device available");
    Scope<SpriteBatch> batch = make_batch(fixture);
    T2D_REQUIRE(batch != nullptr);
    Scope<ore::rhi::Texture> atlas = fixture.context->create_solid_texture(0xFFFFFFFFu, "test.white");
    T2D_REQUIRE(atlas != nullptr);
    Scope<ore::rhi::Sampler> sampler = make_sampler(fixture);
    T2D_REQUIRE(sampler != nullptr);

    const ore::Color center = render_and_read_center(
        fixture, *batch, *atlas, *sampler, [](SpriteBatch& target) {
            target.draw_quad(Aabb2{Vec2{0.0f, 0.0f}, Vec2{64.0f, 64.0f}}, Aabb2{Vec2{0.0f, 0.0f}, Vec2{1.0f, 1.0f}},
                             0xFF0000FFu); // red, opaque (packed AABBGGRR)
        });
    T2D_CHECK_MSG(center.r > 200, "the red channel was {}", center.r);
    T2D_CHECK_MSG(center.g < 40, "the green channel was {}", center.g);
    T2D_CHECK_MSG(center.b < 40, "the blue channel was {}", center.b);
    T2D_CHECK_EQ(center.a, 255);
    T2D_CHECK_EQ(batch->draw_calls(), 1u);
    T2D_CHECK_EQ(batch->quads(), 1u);
    T2D_CHECK_EQ(batch->dropped_quads(), 0u);
}

T2D_TEST(the_font_atlas_white_texel_draws_an_opaque_rectangle) {
    Fixture fixture = make_fixture(64);
    if (!fixture.valid()) T2D_SKIP("no Vulkan device available");
    Scope<SpriteBatch> batch = make_batch(fixture);
    T2D_REQUIRE(batch != nullptr);
    Scope<ore::rhi::Texture> font = fixture.context->create_texture(make_font_atlas(), false, "test.font");
    T2D_REQUIRE(font != nullptr);
    Scope<ore::rhi::Sampler> sampler = make_sampler(fixture);
    T2D_REQUIRE(sampler != nullptr);

    // draw_rect samples the empty cell the font atlas leaves free, so it must come out solid.
    const ore::Color center = render_and_read_center(
        fixture, *batch, *font, *sampler,
        [](SpriteBatch& target) { target.draw_rect(Aabb2{Vec2{0.0f, 0.0f}, Vec2{64.0f, 64.0f}}, 0xFF00FF00u); });
    T2D_CHECK_MSG(center.g > 200, "the green channel was {}", center.g);
    T2D_CHECK_MSG(center.r < 40, "the red channel was {}", center.r);
    T2D_CHECK_EQ(center.a, 255);
}

T2D_TEST(a_coloured_atlas_cell_arrives_with_its_colour) {
    Fixture fixture = make_fixture(64);
    if (!fixture.valid()) T2D_SKIP("no Vulkan device available");
    Scope<SpriteBatch> batch = make_batch(fixture);
    T2D_REQUIRE(batch != nullptr);
    // Exactly what the game does: the tileset atlas is created with a mip chain.
    Scope<ore::rhi::Texture> tiles = fixture.context->create_texture(make_tileset_atlas(), true, "test.tiles");
    T2D_REQUIRE(tiles != nullptr);
    Scope<ore::rhi::Sampler> sampler = make_sampler(fixture);
    T2D_REQUIRE(sampler != nullptr);

    // The dirt tile: a filled brown cell. Sampling it must return brown, not white and not the
    // clear colour - a texture that arrives without its RGB would still show the right silhouette.
    const TileDef dirt = Tileset::default_platformer().atlas_of(2);
    const f32 cell = 1.0f / static_cast<f32>(kTileAtlasColumns);
    const Aabb2 uv{Vec2{static_cast<f32>(dirt.atlas_x) * cell, static_cast<f32>(dirt.atlas_y) * cell},
                   Vec2{static_cast<f32>(dirt.atlas_x + 1) * cell, static_cast<f32>(dirt.atlas_y + 1) * cell}};
    const ore::Color center = render_and_read_center(
        fixture, *batch, *tiles, *sampler, [uv](SpriteBatch& target) {
            target.draw_quad(Aabb2{Vec2{0.0f, 0.0f}, Vec2{64.0f, 64.0f}}, uv, 0xFFFFFFFFu);
        });
    T2D_CHECK_MSG(center.r > 60, "the red channel was {}", center.r);
    T2D_CHECK_MSG(center.r > center.g && center.g > center.b, "the dirt tile was not brown: ({}, {}, {})",
                  center.r, center.g, center.b);
    T2D_CHECK_EQ(center.a, 255);
}

T2D_TEST(font_glyphs_are_actually_visible) {
    Fixture fixture = make_fixture(64);
    if (!fixture.valid()) T2D_SKIP("no Vulkan device available");
    Scope<SpriteBatch> batch = make_batch(fixture);
    T2D_REQUIRE(batch != nullptr);
    Scope<ore::rhi::Texture> font = fixture.context->create_texture(make_font_atlas(), false, "test.font");
    T2D_REQUIRE(font != nullptr);
    Scope<ore::rhi::Sampler> sampler = make_sampler(fixture);
    T2D_REQUIRE(sampler != nullptr);

    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    ore::RenderFrame& frame = fixture.renderer->begin_frame(1.0f / 60.0f);
    fixture.renderer->begin_pass(clear, 1.0f);
    batch->begin(frame, Vec2{32.0f, 32.0f}, Vec2{64.0f, 64.0f}, *font, sampler->handle());
    batch->draw_text(0.0f, 0.0f, 4.0f, 0xFFFFFFFFu, "A");
    batch->end();
    fixture.renderer->end_pass();
    fixture.renderer->end_frame();
    fixture.renderer->wait_idle();

    const ore::Image shot = fixture.context->read_render_target(fixture.renderer->target());
    T2D_REQUIRE(!shot.empty());
    // One 5x7 glyph at scale 4 covers up to 20x28 pixels; a blank atlas would light none of them.
    const u32 lit = count_lit_pixels(shot, 128);
    T2D_CHECK_MSG(lit > 60u, "only {} pixels were lit by draw_text(\"A\")", lit);
}

T2D_TEST(a_frame_with_many_quads_uses_only_the_ring_space_it_needs) {
    Fixture fixture = make_fixture(128);
    if (!fixture.valid()) T2D_SKIP("no Vulkan device available");
    Scope<SpriteBatch> batch = make_batch(fixture);
    T2D_REQUIRE(batch != nullptr);
    Scope<ore::rhi::Texture> atlas = fixture.context->create_solid_texture(0xFFFFFFFFu, "test.white");
    T2D_REQUIRE(atlas != nullptr);
    Scope<ore::rhi::Sampler> sampler = make_sampler(fixture);
    T2D_REQUIRE(sampler != nullptr);

    const u64 before = fixture.renderer->ring().stats().peak_usage;
    const ore::Color center = render_and_read_center(
        fixture, *batch, *atlas, *sampler, [](SpriteBatch& target) {
            for (u32 i = 0; i < 8; ++i) {
                target.draw_quad(Aabb2{Vec2{0.0f, 0.0f}, Vec2{128.0f, 128.0f}},
                                 Aabb2{Vec2{0.0f, 0.0f}, Vec2{1.0f, 1.0f}}, 0xFF0000FFu);
            }
        });
    T2D_CHECK_EQ(batch->quads(), 8u);
    T2D_CHECK_EQ(center.r, 255);
    // Eight quads are 640 bytes: the batch must not reserve its whole capacity from the frame ring.
    const u64 after = fixture.renderer->ring().stats().peak_usage;
    T2D_CHECK_MSG(after - before < 4096u, "the frame ring grew by {} bytes for 8 quads", after - before);
}

T2D_TEST(a_rectangle_sampled_from_a_single_white_texel_is_flat_and_opaque) {
    // The glyph atlas reserves exactly one white pixel and the game samples it through a *linear*
    // filter (glyphs are anti-aliased). If draw_rect() asks for a uv *span* instead of a point, the
    // corners of the quad sample the transparent neighbours of that pixel and the rectangle comes out
    // as a gradient fading towards one corner - which is what every panel, every highlight row and
    // every sandbox cell looked like while the span was 0.001 of a 2048 page.
    Fixture fixture = make_fixture(64);
    if (!fixture.valid()) T2D_SKIP("no Vulkan device available");
    Scope<SpriteBatch> batch = make_batch(fixture);
    T2D_REQUIRE(batch != nullptr);

    constexpr u32 kPage = 512; // 0.001 of a 512 page is half a texel: enough to leave the white pixel
    ore::Image image = ore::Image::create(kPage, kPage, ore::make_rgba(255, 255, 255, 0));
    image.set_pixel(0, 0, ore::make_rgba(255, 255, 255, 255));
    Scope<ore::rhi::Texture> atlas = fixture.context->create_texture(image, false, "test.one.white.texel");
    T2D_REQUIRE(atlas != nullptr);
    ore::rhi::SamplerDesc desc;
    desc.min_filter = VK_FILTER_LINEAR;
    desc.mag_filter = VK_FILTER_LINEAR;
    desc.mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    Scope<ore::rhi::Sampler> sampler = ore::rhi::Sampler::create(fixture.context->device(), desc);
    T2D_REQUIRE(sampler != nullptr);

    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    ore::RenderFrame& frame = fixture.renderer->begin_frame(1.0f / 60.0f);
    fixture.renderer->begin_pass(clear, 1.0f);
    batch->begin(frame, Vec2{32.0f, 32.0f}, Vec2{64.0f, 64.0f}, *atlas, sampler->handle());
    batch->set_white_texel(Vec2{0.5f / static_cast<f32>(kPage), 0.5f / static_cast<f32>(kPage)});
    batch->draw_rect(Aabb2{Vec2{0.0f, 0.0f}, Vec2{64.0f, 64.0f}}, 0xFF00FF00u); // opaque green
    batch->end();
    fixture.renderer->end_pass();
    fixture.renderer->end_frame();
    fixture.renderer->wait_idle();

    const ore::Image shot = fixture.context->read_render_target(fixture.renderer->target());
    T2D_REQUIRE(!shot.empty());
    const u32 corners[5][2] = {{1u, 1u}, {62u, 1u}, {1u, 62u}, {62u, 62u}, {32u, 32u}};
    for (const auto& point : corners) {
        const ore::Color pixel = ore::Color::from_packed(shot.pixel(point[0], point[1]));
        T2D_CHECK_MSG(pixel.g > 200 && pixel.a == 255,
                      "the pixel at {},{} came out ({}, {}, {}, {}) instead of opaque green", point[0], point[1],
                      pixel.r, pixel.g, pixel.b, pixel.a);
    }
}

namespace {

/// Draws \p map through the tilemap renderer and reads one pixel back.
[[nodiscard]] ore::Color render_map_pixel(Fixture& fixture, SpriteBatch& batch, const TileMap& map,
                                          ore::rhi::Texture& atlas, ore::rhi::Sampler& sampler,
                                          TileMap::LayerMask mask, u32 pixel_x, u32 pixel_y) {
    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    ore::RenderFrame& frame = fixture.renderer->begin_frame(1.0f / 60.0f);
    fixture.renderer->begin_pass(clear, 1.0f);
    batch.begin(frame, Vec2{32.0f, 32.0f}, Vec2{64.0f, 64.0f}, atlas, sampler.handle());
    TilemapRenderer renderer;
    renderer.draw_map(batch, map, Tileset::default_platformer(), Aabb2{Vec2{0.0f, 0.0f}, Vec2{64.0f, 64.0f}}, mask);
    batch.end();
    fixture.renderer->end_pass();
    fixture.renderer->end_frame();
    fixture.renderer->wait_idle();
    const ore::Image shot = fixture.context->read_render_target(fixture.renderer->target());
    if (shot.empty()) return {};
    return ore::Color::from_packed(shot.pixel(pixel_x, pixel_y));
}

[[nodiscard]] bool same_color(const ore::Color& a, const ore::Color& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

} // namespace

T2D_TEST(the_higher_map_layer_is_drawn_over_the_lower_one) {
    Fixture fixture = make_fixture(64);
    if (!fixture.valid()) T2D_SKIP("no Vulkan device available");
    Scope<SpriteBatch> batch = make_batch(fixture);
    T2D_REQUIRE(batch != nullptr);
    Scope<ore::rhi::Texture> tiles = fixture.context->create_texture(make_tileset_atlas(), true, "test.tiles");
    T2D_REQUIRE(tiles != nullptr);
    Scope<ore::rhi::Sampler> sampler = make_sampler(fixture);
    T2D_REQUIRE(sampler != nullptr);

    // The same cell on two map layers: stone on the ground layer, dirt on the structures layer. Cell
    // (1,1) covers world 16..32, so its centre lands on pixel (24,24) of a 64x64 target.
    TileMap both(4, 4, 16.0f, false, 2);
    both.set(0, 1, 1, 1);
    both.set(1, 1, 1, 2);
    TileMap stone_only(4, 4, 16.0f, false, 1);
    stone_only.set(0, 1, 1, 1);
    TileMap dirt_only(4, 4, 16.0f, false, 1);
    dirt_only.set(0, 1, 1, 2);

    const ore::Color stone =
        render_map_pixel(fixture, *batch, stone_only, *tiles, *sampler, TileMap::kAllLayers, 24, 24);
    const ore::Color dirt =
        render_map_pixel(fixture, *batch, dirt_only, *tiles, *sampler, TileMap::kAllLayers, 24, 24);
    // Two different tiles, or nothing below proves anything.
    T2D_CHECK_FALSE(same_color(stone, dirt));

    const ore::Color layered =
        render_map_pixel(fixture, *batch, both, *tiles, *sampler, TileMap::kAllLayers, 24, 24);
    const ore::Color lower =
        render_map_pixel(fixture, *batch, both, *tiles, *sampler, TileMap::layer_mask(0), 24, 24);
    const ore::Color upper =
        render_map_pixel(fixture, *batch, both, *tiles, *sampler, TileMap::layer_mask(1), 24, 24);

    // Both layers drawn: the higher one covers the lower one. One layer at a time: exactly that tile.
    T2D_CHECK_MSG(same_color(layered, dirt), "the layered cell was ({}, {}, {}), the top tile ({}, {}, {})",
                  layered.r, layered.g, layered.b, dirt.r, dirt.g, dirt.b);
    T2D_CHECK_MSG(same_color(lower, stone), "the ground layer cell was ({}, {}, {})", lower.r, lower.g, lower.b);
    T2D_CHECK_MSG(same_color(upper, dirt), "the structure layer cell was ({}, {}, {})", upper.r, upper.g, upper.b);

    // An empty cell stays clear, and a mask that names no layer of this map draws nothing at all.
    const ore::Color empty =
        render_map_pixel(fixture, *batch, both, *tiles, *sampler, TileMap::kAllLayers, 56, 56);
    const ore::Color masked_out =
        render_map_pixel(fixture, *batch, both, *tiles, *sampler, TileMap::layer_mask(5), 24, 24);
    T2D_CHECK_EQ(empty.r + empty.g + empty.b, 0);
    T2D_CHECK_EQ(masked_out.r + masked_out.g + masked_out.b, 0);
}

T2D_TEST_MAIN
