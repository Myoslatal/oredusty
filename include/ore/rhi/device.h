// Ore framework - physical device selection and the logical device with its queues.
#pragma once

#include <ore/core/types.h>
#include <ore/rhi/instance.h>
#include <ore/rhi/vulkan.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ore::rhi {

/// Optional features the renderer would like to have. Missing optional features degrade
/// gracefully; the three core ones (dynamic rendering, synchronization2, timeline semaphores)
/// are mandatory because the whole frame model is built on them.
struct PhysicalFeatures {
    bool dynamic_rendering = false;
    bool synchronization2 = false;
    bool timeline_semaphore = false;
    bool descriptor_indexing = false;
    bool buffer_device_address = false;
    bool sampler_anisotropy = false;
    bool fill_mode_non_solid = false;
    bool wide_lines = false;
    bool depth_clamp = false;
    bool independent_blend = false;
    bool shader_float64 = false;
    bool sampler_filter_minmax = false;
    bool maintenance4 = false;

    /// True when every feature the framework requires at runtime is present.
    [[nodiscard]] bool meets_requirements() const {
        return dynamic_rendering && synchronization2 && timeline_semaphore;
    }
};

struct QueueFamilyInfo {
    u32 index = 0;
    VkQueueFlags flags = 0;
    u32 queue_count = 0;
    bool supports_present = false;
    [[nodiscard]] bool supports_graphics() const { return (flags & VK_QUEUE_GRAPHICS_BIT) != 0u; }
    [[nodiscard]] bool supports_compute() const { return (flags & VK_QUEUE_COMPUTE_BIT) != 0u; }
    [[nodiscard]] bool supports_transfer() const { return (flags & VK_QUEUE_TRANSFER_BIT) != 0u; }
};

struct PhysicalDeviceInfo {
    VkPhysicalDevice handle = VK_NULL_HANDLE;
    u32 index = 0;
    std::string name;
    u32 api_version = 0;
    u32 driver_version = 0;
    std::string driver_name;
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    u32 vendor_id = 0;
    u32 device_id = 0;
    u64 device_local_memory = 0;
    u64 host_visible_memory = 0;
    PhysicalFeatures features;
    std::vector<QueueFamilyInfo> queue_families;
    /// Queue family indices chosen for each role (kInvalidIndex when unavailable).
    u32 graphics_family = kInvalidIndex;
    u32 compute_family = kInvalidIndex;
    u32 transfer_family = kInvalidIndex;
    u32 present_family = kInvalidIndex;

    [[nodiscard]] bool supports_presentation() const { return present_family != kInvalidIndex; }
    [[nodiscard]] bool is_discrete() const { return type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU; }
    [[nodiscard]] const QueueFamilyInfo* find_family(u32 family_index) const;
};

/// Scores and lists every device. Pass a surface to also resolve presentation support.
[[nodiscard]] std::vector<PhysicalDeviceInfo> enumerate_physical_devices(const Instance& instance,
                                                                        VkSurfaceKHR surface = VK_NULL_HANDLE);

struct DeviceDesc {
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    /// Used when physical_device is VK_NULL_HANDLE: index into enumerate_physical_devices().
    u32 device_index = 0;
    /// Selecting a device picks the highest scoring candidate that satisfies this.
    bool require_presentation = false;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    bool prefer_discrete_gpu = true;
    /// Extra device extensions to enable in addition to VK_KHR_swapchain.
    std::vector<std::string> extensions;
    /// Lets the caller override the automatic device choice.
    bool allow_software_device = true;
};

/// Queue roles. Graphics/Compute/Transfer/Present may alias the same family.
enum class QueueKind : u32 { Graphics = 0, Compute, Transfer, Present, Count };

class Device {
public:
    [[nodiscard]] static Scope<Device> create(const Instance& instance, const DeviceDesc& desc);
    ~Device();

    ORE_NON_MOVABLE(Device);

    [[nodiscard]] VkDevice handle() const { return device_; }
    [[nodiscard]] VkPhysicalDevice physical_device() const { return physical_; }
    [[nodiscard]] const PhysicalDeviceInfo& info() const { return info_; }
    [[nodiscard]] const PhysicalFeatures& features() const { return info_.features; }
    [[nodiscard]] const VkPhysicalDeviceProperties& properties() const { return properties_; }
    [[nodiscard]] const VkPhysicalDeviceMemoryProperties& memory_properties() const { return memory_properties_; }
    [[nodiscard]] const DebugUtilsFunctions& debug_utils() const { return debug_utils_; }

    [[nodiscard]] VkQueue queue(QueueKind kind) const { return queues_[static_cast<usize>(kind)]; }
    [[nodiscard]] bool has_queue(QueueKind kind) const { return queues_[static_cast<usize>(kind)] != VK_NULL_HANDLE; }
    [[nodiscard]] u32 queue_family(QueueKind kind) const;
    [[nodiscard]] VkQueue graphics_queue() const { return queue(QueueKind::Graphics); }
    [[nodiscard]] VkQueue compute_queue() const { return queue(QueueKind::Compute); }
    [[nodiscard]] VkQueue transfer_queue() const { return queue(QueueKind::Transfer); }
    [[nodiscard]] VkQueue present_queue() const { return queue(QueueKind::Present); }

    void wait_idle() const;

    [[nodiscard]] bool extension_enabled(std::string_view name) const;
    [[nodiscard]] const std::vector<std::string>& enabled_extensions() const { return enabled_extensions_; }

    /// Picks the first supported format out of \p candidates.
    [[nodiscard]] VkFormat find_supported_format(ConstSpan<VkFormat> candidates, VkImageTiling tiling,
                                                 VkFormatFeatureFlags features) const;
    /// Resolves a memory type index from a requirements bit mask plus desired properties.
    [[nodiscard]] std::optional<u32> find_memory_type(u32 type_bits, VkMemoryPropertyFlags properties) const;

    /// Attaches a human readable name to any Vulkan object (no-op without VK_EXT_debug_utils).
    void set_debug_name(VkObjectType type, u64 handle, std::string_view name) const;

    /// vkGetDeviceProcAddr cache for anything the core wrappers do not cover.
    [[nodiscard]] PFN_vkVoidFunction proc_address(const char* name) const;

private:
    Device() = default;

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    PhysicalDeviceInfo info_{};
    VkPhysicalDeviceProperties properties_{};
    VkPhysicalDeviceMemoryProperties memory_properties_{};
    std::array<VkQueue, static_cast<usize>(QueueKind::Count)> queues_{};
    std::array<u32, static_cast<usize>(QueueKind::Count)> families_{};
    std::vector<std::string> enabled_extensions_;
    DebugUtilsFunctions debug_utils_{};
};

} // namespace ore::rhi
