// GPU smoke test: brings up a Vulkan 1.3 device and checks the features the renderer needs.
// Skips itself (exit code 0, reported as SKIP) when the machine exposes no Vulkan device,
// so the suite stays green inside containers.
#include <ore/rhi/instance.h>
#include <ore/rhi/device.h>

#include <support/test_support.h>

using namespace ore;

ORE_TEST(gpu_instance_and_device) {
    rhi::InstanceDesc desc;
    desc.application_name = "ore_test_gpu_smoke";
    desc.enable_validation = false;
    const auto instance = rhi::Instance::create(desc);
    ORE_REQUIRE(instance != nullptr);
    ORE_CHECK(instance->handle() != VK_NULL_HANDLE);

    const auto devices = rhi::enumerate_physical_devices(*instance);
    if (devices.empty()) {
        ORE_SKIP("no Vulkan physical device available on this machine");
    }
    ORE_CHECK(!devices.empty());
    const rhi::PhysicalDeviceInfo& info = devices.front();
    ORE_CHECK(!info.name.empty());
    ORE_CHECK(info.api_version >= VK_API_VERSION_1_3);

    rhi::DeviceDesc device_desc;
    device_desc.physical_device = info.handle;
    const auto device = rhi::Device::create(*instance, device_desc);
    ORE_REQUIRE(device != nullptr);
    ORE_CHECK(device->handle() != VK_NULL_HANDLE);
    ORE_CHECK(device->graphics_queue() != VK_NULL_HANDLE);
    ORE_INFO("device: {} | driver: {}", info.name, info.driver_name);
}

ORE_TEST_MAIN
