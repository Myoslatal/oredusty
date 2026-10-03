// Presentation-path tests. VK_EXT_headless_surface gives us a real swapchain without a window
// system, so the swapchain / acquire / submit / present code runs for real here. On drivers that
// do not offer the extension the whole file skips itself.
#include <ore/ore.h>

#include <support/test_support.h>

#include <cstdio>

using namespace ore;

namespace {

[[nodiscard]] bool headless_surface_supported() {
    const std::vector<std::string> extensions = rhi::Instance::available_extensions();
    return std::find(extensions.begin(), extensions.end(), VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME) != extensions.end();
}

struct SwapchainFixture {
    Scope<rhi::GraphicsContext> context;
    Scope<Renderer> renderer;
    [[nodiscard]] bool valid() const { return context != nullptr && renderer != nullptr; }
};

[[nodiscard]] SwapchainFixture make_swapchain_fixture(u32 width = 64, u32 height = 48) {
    SwapchainFixture fixture;
    rhi::ContextDesc context_desc;
    context_desc.application_name = "ore_test_gpu_swapchain";
    context_desc.enable_validation = false;
    context_desc.headless = false; // we do want a swapchain
    context_desc.instance_extensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
    context_desc.surface_factory = [](VkInstance instance) -> VkSurfaceKHR {
        auto* create = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateHeadlessSurfaceEXT"));
        if (create == nullptr) return VK_NULL_HANDLE;
        VkHeadlessSurfaceCreateInfoEXT info{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        return create(instance, &info, nullptr, &surface) == VK_SUCCESS ? surface : VK_NULL_HANDLE;
    };
    fixture.context = rhi::GraphicsContext::create(context_desc);
    if (fixture.context == nullptr) return fixture;

    Renderer::Desc renderer_desc;
    renderer_desc.context = fixture.context.get();
    renderer_desc.surface = fixture.context->surface();
    renderer_desc.width = width;
    renderer_desc.height = height;
    fixture.renderer = Renderer::create(renderer_desc);
    return fixture;
}

/// One full frame: clear the swapchain image and present it.
void render_frame(Renderer& renderer, const Vec4& color) {
    VkClearValue clear{};
    clear.color = {{color.r, color.g, color.b, color.a}};
    (void)renderer.begin_frame(1.0f / 60.0f);
    renderer.begin_pass(clear, 1.0f);
    renderer.end_pass();
    renderer.end_frame();
}

} // namespace

ORE_TEST(swapchain_query_without_surface_is_unusable) {
    const rhi::SwapchainSupport support = rhi::query_swapchain_support(VK_NULL_HANDLE, VK_NULL_HANDLE);
    ORE_CHECK_FALSE(support.usable());
    ORE_CHECK(support.formats.empty());
    ORE_CHECK(support.present_modes.empty());
}

ORE_TEST(swapchain_create_without_surface_fails_cleanly) {
    rhi::InstanceDesc instance_desc;
    instance_desc.application_name = "ore_test_swapchain_null";
    instance_desc.enable_validation = false;
    auto instance = rhi::Instance::create(instance_desc);
    ORE_REQUIRE(instance != nullptr);

    const auto devices = rhi::enumerate_physical_devices(*instance);
    if (devices.empty()) ORE_SKIP("no Vulkan device available");

    rhi::DeviceDesc device_desc;
    device_desc.physical_device = devices.front().handle;
    auto device = rhi::Device::create(*instance, device_desc);
    ORE_REQUIRE(device != nullptr);

    // No surface: creating a swapchain must fail with a diagnostic instead of crashing.
    rhi::SwapchainDesc swapchain_desc;
    swapchain_desc.width = 64;
    swapchain_desc.height = 64;
    ORE_CHECK(rhi::Swapchain::create(*device, VK_NULL_HANDLE, swapchain_desc) == nullptr);
}

ORE_TEST(gpu_headless_surface_swapchain_presents_frames) {
    if (!headless_surface_supported()) ORE_SKIP("VK_EXT_headless_surface is not available");
    SwapchainFixture fixture = make_swapchain_fixture(64, 48);
    if (!fixture.valid()) ORE_SKIP("no Vulkan device with a usable presentation path");

    ORE_CHECK_FALSE(fixture.renderer->headless());
    ORE_REQUIRE(fixture.renderer->swapchain() != nullptr);
    ORE_CHECK(fixture.renderer->swapchain()->image_count() >= 1u);
    ORE_CHECK_NE(fixture.renderer->color_format(), VK_FORMAT_UNDEFINED);

    // A surface may dictate the swapchain extent (VkSurfaceCapabilitiesKHR::currentExtent), so the
    // renderer adopts it instead of forcing the requested size.
    ORE_CHECK_EQ(fixture.renderer->width(), fixture.renderer->swapchain()->extent().width);
    ORE_CHECK_EQ(fixture.renderer->height(), fixture.renderer->swapchain()->extent().height);
    ORE_CHECK(fixture.renderer->width() >= 1u);
    ORE_CHECK(fixture.renderer->height() >= 1u);

    for (int i = 0; i < 3; ++i) {
        render_frame(*fixture.renderer, Vec4(0.1f, 0.2f, 0.3f, 1.0f));
        ORE_CHECK_FALSE(fixture.renderer->resize_pending());
    }
    fixture.renderer->wait_idle();
    ORE_CHECK_EQ(fixture.renderer->frame().number, 2u);
}

ORE_TEST(gpu_swapchain_screenshot_captures_the_presented_image) {
    if (!headless_surface_supported()) ORE_SKIP("VK_EXT_headless_surface is not available");
    SwapchainFixture fixture = make_swapchain_fixture(32, 32);
    if (!fixture.valid()) ORE_SKIP("no Vulkan device with a usable presentation path");

    const std::string path = path_join(current_dir(), "ore_test_swapchain_capture.png");
    std::remove(path.c_str());

    VkClearValue clear{};
    clear.color = {{0.0f, 1.0f, 0.0f, 1.0f}};
    fixture.renderer->request_screenshot(path);
    RenderFrame& frame = fixture.renderer->begin_frame(1.0f / 60.0f);
    (void)frame;
    fixture.renderer->begin_pass(clear, 1.0f);
    fixture.renderer->end_pass();
    fixture.renderer->end_frame();
    fixture.renderer->wait_idle();

    ORE_CHECK(path_exists(path));
    const auto captured = Image::load_png(path);
    ORE_REQUIRE(captured.has_value());
    ORE_CHECK_EQ(captured->width, fixture.renderer->width());
    ORE_CHECK_EQ(captured->height, fixture.renderer->height());
    const Color pixel = Color::from_packed(captured->pixel(16, 16));
    ORE_CHECK_MSG(pixel.g >= 250, "green channel was {}", pixel.g);
    std::remove(path.c_str());
}

ORE_TEST(gpu_swapchain_recreates_on_resize) {
    if (!headless_surface_supported()) ORE_SKIP("VK_EXT_headless_surface is not available");
    SwapchainFixture fixture = make_swapchain_fixture(48, 32);
    if (!fixture.valid()) ORE_SKIP("no Vulkan device with a usable presentation path");

    render_frame(*fixture.renderer, Vec4(0.2f, 0.2f, 0.2f, 1.0f));

    fixture.renderer->resize(96, 64);
    ORE_CHECK(fixture.renderer->resize_pending());

    const u32 submitted_width = fixture.renderer->width();
    const u32 submitted_height = fixture.renderer->height();
    render_frame(*fixture.renderer, Vec4(0.2f, 0.2f, 0.2f, 1.0f));
    ORE_CHECK_FALSE(fixture.renderer->resize_pending());
    ORE_REQUIRE(fixture.renderer->swapchain() != nullptr);
    // The surface has the final say on the extent; what matters is that the renderer agrees with
    // the swapchain it just recreated and that presenting keeps working.
    ORE_CHECK_EQ(fixture.renderer->width(), fixture.renderer->swapchain()->extent().width);
    ORE_CHECK_EQ(fixture.renderer->height(), fixture.renderer->swapchain()->extent().height);
    ORE_CHECK(fixture.renderer->width() >= 1u);
    ORE_INFO("resize: requested 96x64, previous {}x{}, swapchain {}x{}", submitted_width, submitted_height,
             fixture.renderer->width(), fixture.renderer->height());

    // And the new swapchain still presents.
    render_frame(*fixture.renderer, Vec4(0.3f, 0.3f, 0.3f, 1.0f));
    fixture.renderer->wait_idle();
    ORE_CHECK_FALSE(fixture.renderer->resize_pending());
}

ORE_TEST_MAIN
