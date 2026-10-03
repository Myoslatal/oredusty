// End-to-end rendering tests: they render offscreen on whatever Vulkan device is available and
// assert on the pixels that come back. Skips (instead of failing) on machines without a device.
#include <ore/ore.h>

#include <support/test_support.h>

#include <cstdio>

using namespace ore;

namespace {

struct SolidPushConstants {
    Vec4 color{1.0f};
};

struct Fixture {
    Scope<rhi::GraphicsContext> context;
    Scope<Renderer> renderer;
    [[nodiscard]] bool valid() const { return context != nullptr && renderer != nullptr; }
};

[[nodiscard]] Fixture make_fixture(u32 size = 32) {
    Fixture fixture;
    rhi::ContextDesc context_desc;
    context_desc.application_name = "ore_test_gpu_render";
    context_desc.enable_validation = false;
    context_desc.headless = true;
    fixture.context = rhi::GraphicsContext::create(context_desc);
    if (fixture.context == nullptr) return fixture;

    Renderer::Desc renderer_desc;
    renderer_desc.context = fixture.context.get();
    renderer_desc.width = size;
    renderer_desc.height = size;
    renderer_desc.depth_format = VK_FORMAT_D32_SFLOAT;
    fixture.renderer = Renderer::create(renderer_desc);
    return fixture;
}

[[nodiscard]] bool nearly(u8 value, int expected, int tolerance = 2) {
    const int difference = static_cast<int>(value) - expected;
    return difference >= -tolerance && difference <= tolerance;
}

[[nodiscard]] std::string shader_dir() {
#if defined(ORE_SHADER_DIR)
    return ORE_SHADER_DIR;
#else
    return "shaders";
#endif
}

} // namespace

ORE_TEST(gpu_clear_and_read_back_every_pixel) {
    Fixture fixture = make_fixture(32);
    if (!fixture.valid()) ORE_SKIP("no Vulkan device available");

    VkClearValue clear{};
    clear.color = {{0.25f, 0.5f, 0.75f, 1.0f}};

    RenderFrame& frame = fixture.renderer->begin_frame(1.0f / 60.0f);
    ORE_CHECK(frame.cmd != nullptr);
    ORE_CHECK(frame.ring != nullptr);
    ORE_CHECK_EQ(frame.width, 32u);
    fixture.renderer->begin_pass(clear, 1.0f);
    fixture.renderer->end_pass();
    fixture.renderer->end_frame();
    fixture.renderer->wait_idle();

    const Image shot = fixture.context->read_render_target(fixture.renderer->target());
    ORE_REQUIRE(!shot.empty());
    ORE_CHECK_EQ(shot.width, 32u);
    ORE_CHECK_EQ(shot.height, 32u);

    const Color center = Color::from_packed(shot.pixel(16, 16));
    ORE_CHECK_MSG(nearly(center.r, 64), "red channel was {}", center.r);
    ORE_CHECK_MSG(nearly(center.g, 128), "green channel was {}", center.g);
    ORE_CHECK_MSG(nearly(center.b, 191), "blue channel was {}", center.b);
    ORE_CHECK_EQ(center.a, 255);

    // The whole attachment must carry the clear colour, not just the middle.
    u32 mismatches = 0;
    for (u32 y = 0; y < shot.height; ++y) {
        for (u32 x = 0; x < shot.width; ++x) {
            if (shot.pixel(x, y) != shot.pixel(16, 16)) ++mismatches;
        }
    }
    ORE_CHECK_EQ(mismatches, 0u);

    // The same image can be written out as a PNG.
    const std::string path = path_join(current_dir(), "ore_test_gpu_clear.png");
    ORE_CHECK(shot.save_png(path));
    ORE_CHECK(path_exists(path));
    std::remove(path.c_str());
}

ORE_TEST(gpu_draw_fullscreen_triangle_with_push_constants) {
    Fixture fixture = make_fixture(32);
    if (!fixture.valid()) ORE_SKIP("no Vulkan device available");
    rhi::GraphicsContext& context = *fixture.context;

    auto vertex_shader = rhi::ShaderModule::load(context.device(), path_join(shader_dir(), "solid.vert.spv"));
    auto fragment_shader = rhi::ShaderModule::load(context.device(), path_join(shader_dir(), "solid.frag.spv"));
    ORE_REQUIRE(vertex_shader != nullptr);
    ORE_REQUIRE(fragment_shader != nullptr);

    const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                    sizeof(SolidPushConstants)};
    auto layout = rhi::PipelineLayout::create(context.device(), {},
                                              ConstSpan<VkPushConstantRange>(&range, 1), "test.solid.layout");

    rhi::GraphicsPipelineDesc pipeline_desc;
    pipeline_desc.vertex_shader = vertex_shader.get();
    pipeline_desc.fragment_shader = fragment_shader.get();
    pipeline_desc.layout = layout.get();
    pipeline_desc.color_formats = {fixture.renderer->color_format()};
    pipeline_desc.depth_format = VK_FORMAT_UNDEFINED; // the pass has a depth attachment, so it must match
    pipeline_desc.depth_format = fixture.renderer->depth_format();
    pipeline_desc.depth_test = false;
    pipeline_desc.depth_write = false;
    pipeline_desc.cull_mode = VK_CULL_MODE_NONE;
    pipeline_desc.debug_name = "test.solid.pipeline";
    auto pipeline = rhi::GraphicsPipeline::create(context.device(), pipeline_desc);
    ORE_REQUIRE(pipeline != nullptr);

    const auto render_solid = [&](const Vec4& color) {
        VkClearValue clear{};
        clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        RenderFrame& frame = fixture.renderer->begin_frame(1.0f / 60.0f);
        fixture.renderer->begin_pass(clear, 1.0f);
        frame.cmd->bind_pipeline(*pipeline);
        frame.cmd->set_viewport(frame.width, frame.height);
        frame.cmd->set_scissor_full(frame.width, frame.height);
        SolidPushConstants constants;
        constants.color = color;
        frame.cmd->push_constants(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, constants);
        frame.cmd->draw(3);
        fixture.renderer->end_pass();
        fixture.renderer->end_frame();
        fixture.renderer->wait_idle();
        return fixture.context->read_render_target(fixture.renderer->target());
    };

    const Image green = render_solid(Vec4(0.0f, 1.0f, 0.0f, 1.0f));
    ORE_REQUIRE(!green.empty());
    const Color pixel = Color::from_packed(green.pixel(16, 16));
    ORE_CHECK_MSG(nearly(pixel.g, 255, 1), "green channel was {}", pixel.g);
    ORE_CHECK_MSG(nearly(pixel.r, 0, 1), "red channel was {}", pixel.r);
    ORE_CHECK_EQ(pixel.a, 255);

    // A second frame with a different push constant must produce a different image.
    const Image red = render_solid(Vec4(1.0f, 0.0f, 0.0f, 1.0f));
    ORE_REQUIRE(!red.empty());
    const Color second = Color::from_packed(red.pixel(16, 16));
    ORE_CHECK_MSG(nearly(second.r, 255, 1), "red channel was {}", second.r);
    ORE_CHECK_MSG(nearly(second.g, 0, 1), "green channel was {}", second.g);
    ORE_CHECK_NE(red.pixel(16, 16), green.pixel(16, 16));
}

ORE_TEST(gpu_host_visible_staging_is_always_mappable) {
    // A context without a renderer: no upload ring exists, so the *first* allocation is a
    // device-local buffer. On many drivers (integrated GPUs) the device-local and host-visible
    // memory types overlap, and a naive allocator then hands that unmapped block to the staging
    // buffer below - which used to abort in Buffer::write("upload staging").
    rhi::ContextDesc context_desc;
    context_desc.application_name = "ore_test_gpu_render";
    context_desc.enable_validation = false;
    context_desc.headless = true;
    auto context = rhi::GraphicsContext::create(context_desc);
    if (context == nullptr) ORE_SKIP("no Vulkan device available");

    rhi::BufferDesc device_desc;
    device_desc.size = 4096;
    device_desc.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::TransferDst;
    device_desc.host_visible = false;
    device_desc.debug_name = "test.device_local";
    auto device_buffer = rhi::Buffer::create(context->allocator(), device_desc);
    ORE_REQUIRE(device_buffer != nullptr);

    rhi::BufferDesc staging_desc;
    staging_desc.size = 4096;
    staging_desc.usage = rhi::BufferUsage::TransferSrc;
    staging_desc.host_visible = true;
    staging_desc.debug_name = "test.staging";
    auto staging = rhi::Buffer::create(context->allocator(), staging_desc);
    ORE_REQUIRE(staging != nullptr);
    ORE_CHECK_MSG(staging->mapped(), "host visible staging buffer is not mapped (block reuse bug)");

    const std::vector<u8> payload(4096, 0xAB);
    staging->write(payload);            // would abort when the block is not mapped
    staging->flush();
    ORE_CHECK(staging->mapped_data() != nullptr);

    // And the whole upload path has to work with that ordering too.
    context->upload_buffer(*device_buffer, payload);
    ORE_CHECK_EQ(context->allocator().stats().live_allocations >= 2u, true);
}

ORE_TEST(gpu_buffer_upload_round_trip) {
    Fixture fixture = make_fixture(16);
    if (!fixture.valid()) ORE_SKIP("no Vulkan device available");
    rhi::GraphicsContext& context = *fixture.context;

    const std::vector<u32> source = {1u, 2u, 3u, 4u, 0xDEADBEEFu, 6u, 7u, 8u};
    rhi::BufferDesc buffer_desc;
    buffer_desc.size = source.size() * sizeof(u32);
    buffer_desc.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDst | rhi::BufferUsage::TransferSrc;
    buffer_desc.host_visible = false;
    buffer_desc.debug_name = "test.storage";
    auto device_buffer = rhi::Buffer::create(context.allocator(), buffer_desc);
    ORE_REQUIRE(device_buffer != nullptr);
    // Device local, so the application must upload through staging. (Software rasterisers such as
    // SwiftShader expose a single memory type that is both host visible and device local, so the
    // allocation may still be mapped under the hood - what matters is the intended usage.)
    ORE_CHECK_FALSE(device_buffer->host_visible());

    context.upload_buffer(*device_buffer, ConstSpan<u8>(reinterpret_cast<const u8*>(source.data()),
                                                       source.size() * sizeof(u32)));

    auto readback = context.create_staging_buffer(buffer_desc.size, "test.readback");
    ORE_REQUIRE(readback != nullptr);
    context.immediate_submit([&](rhi::CommandBuffer& cmd) {
        cmd.copy_buffer(*device_buffer, *readback, buffer_desc.size);
    });
    readback->invalidate();

    ORE_REQUIRE(readback->mapped());
    const auto* words = static_cast<const u32*>(readback->mapped_data());
    for (usize i = 0; i < source.size(); ++i) {
        ORE_CHECK_MSG(words[i] == source[i], "word {} was {} instead of {}", i, words[i], source[i]);
    }
}

ORE_TEST(gpu_texture_upload_round_trip) {
    Fixture fixture = make_fixture(16);
    if (!fixture.valid()) ORE_SKIP("no Vulkan device available");
    rhi::GraphicsContext& context = *fixture.context;

    Image source = Image::create(8, 4);
    for (u32 y = 0; y < source.height; ++y) {
        for (u32 x = 0; x < source.width; ++x) {
            source.set_pixel(x, y, make_rgba(static_cast<u8>(x * 32), static_cast<u8>(y * 64), 200, 255));
        }
    }

    auto texture = context.create_texture(source, false, "test.texture");
    ORE_REQUIRE(texture != nullptr);
    ORE_CHECK_EQ(texture->extent().width, 8u);
    ORE_CHECK_EQ(texture->extent().height, 4u);
    ORE_CHECK_EQ(texture->mip_levels(), 1u);

    const Image readback = context.read_image(texture->image(), 8, 4, texture->format(),
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ORE_REQUIRE(!readback.empty());
    ORE_CHECK_EQ(readback.pixels, source.pixels);
}

ORE_TEST(gpu_mip_chain_is_generated) {
    Fixture fixture = make_fixture(16);
    if (!fixture.valid()) ORE_SKIP("no Vulkan device available");
    rhi::GraphicsContext& context = *fixture.context;

    const Image source = make_checkerboard(64, 8, make_rgba(255, 0, 0), make_rgba(0, 0, 255));
    auto texture = context.create_texture(source, true, "test.mips");
    ORE_REQUIRE(texture != nullptr);
    ORE_CHECK_EQ(texture->mip_levels(), 7u); // 64 -> 1

    // Mip 0 must survive the upload unchanged; the tail of the chain must exist.
    const Image readback = context.read_image(texture->image(), 64, 64, texture->format(),
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ORE_REQUIRE(!readback.empty());
    ORE_CHECK_EQ(readback.pixel(0, 0), source.pixel(0, 0));
    ORE_CHECK_EQ(readback.pixel(63, 63), source.pixel(63, 63));
}

ORE_TEST(gpu_offscreen_resize_recreates_the_target) {
    Fixture fixture = make_fixture(32);
    if (!fixture.valid()) ORE_SKIP("no Vulkan device available");

    VkClearValue clear{};
    clear.color = {{1.0f, 0.0f, 0.0f, 1.0f}};
    {
        (void)fixture.renderer->begin_frame(0.0f);
        fixture.renderer->begin_pass(clear, 1.0f);
        fixture.renderer->end_pass();
        fixture.renderer->end_frame();
    }

    fixture.renderer->resize(64, 48);
    ORE_CHECK(fixture.renderer->resize_pending());
    ORE_CHECK_EQ(fixture.renderer->width(), 64u);
    ORE_CHECK_EQ(fixture.renderer->height(), 48u);

    RenderFrame& frame = fixture.renderer->begin_frame(0.0f);
    ORE_CHECK_EQ(frame.width, 64u);
    ORE_CHECK_EQ(frame.height, 48u);
    fixture.renderer->begin_pass(clear, 1.0f);
    fixture.renderer->end_pass();
    fixture.renderer->end_frame();
    fixture.renderer->wait_idle();
    ORE_CHECK_FALSE(fixture.renderer->resize_pending());

    const Image shot = fixture.context->read_render_target(fixture.renderer->target());
    ORE_REQUIRE(!shot.empty());
    ORE_CHECK_EQ(shot.width, 64u);
    ORE_CHECK_EQ(shot.height, 48u);
    ORE_CHECK_EQ(Color::from_packed(shot.pixel(32, 24)).r, 255);
}

ORE_TEST(gpu_allocator_reports_usage) {
    Fixture fixture = make_fixture(16);
    if (!fixture.valid()) ORE_SKIP("no Vulkan device available");

    const rhi::GpuAllocator::Stats before = fixture.context->allocator().stats();
    ORE_CHECK(before.block_count > 0u);

    {
        rhi::BufferDesc desc;
        desc.size = 1u << 20;
        desc.usage = rhi::BufferUsage::Storage;
        desc.host_visible = false;
        desc.debug_name = "test.big";
        auto buffer = rhi::Buffer::create(fixture.context->allocator(), desc);
        ORE_REQUIRE(buffer != nullptr);
        const rhi::GpuAllocator::Stats during = fixture.context->allocator().stats();
        ORE_CHECK(during.used_bytes >= before.used_bytes + desc.size);
        ORE_CHECK(during.live_allocations > before.live_allocations);
    }

    const rhi::GpuAllocator::Stats after = fixture.context->allocator().stats();
    ORE_CHECK_EQ(after.live_allocations, before.live_allocations);
    ORE_CHECK_FALSE(fixture.context->allocator().dump_stats().empty());
}

ORE_TEST_MAIN
