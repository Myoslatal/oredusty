#include <ore/rhi/device.h>

#include <ore/core/assert.h>
#include <ore/core/log.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <set>

namespace ore::rhi {
namespace {

[[nodiscard]] std::string device_type_name(VkPhysicalDeviceType type) {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "software";
        default: return "other";
    }
}

[[nodiscard]] int score_device(const PhysicalDeviceInfo& info, bool prefer_discrete, bool allow_software) {
    if (!info.features.meets_requirements()) return -1;
    if (info.type == VK_PHYSICAL_DEVICE_TYPE_CPU && !allow_software) return -1;
    int score = 0;
    switch (info.type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: score = prefer_discrete ? 1000 : 500; break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = prefer_discrete ? 400 : 600; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: score = 200; break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: score = allow_software ? 100 : -1; break;
        default: score = 50; break;
    }
    if (score < 0) return score;
    // Prefer devices with more device local memory, capped so it never dominates the type ordering.
    score += static_cast<int>(std::min<u64>(info.device_local_memory / (256ull * 1024 * 1024), 100));
    if (info.features.buffer_device_address) score += 5;
    return score;
}

} // namespace

const QueueFamilyInfo* PhysicalDeviceInfo::find_family(u32 family_index) const {
    for (const QueueFamilyInfo& family : queue_families) {
        if (family.index == family_index) return &family;
    }
    return nullptr;
}

std::vector<PhysicalDeviceInfo> enumerate_physical_devices(const Instance& instance, VkSurfaceKHR surface) {
    u32 count = 0;
    if (vkEnumeratePhysicalDevices(instance.handle(), &count, nullptr) != VK_SUCCESS || count == 0) return {};

    std::vector<VkPhysicalDevice> devices(count);
    if (vkEnumeratePhysicalDevices(instance.handle(), &count, devices.data()) != VK_SUCCESS) return {};

    std::vector<PhysicalDeviceInfo> out;
    out.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        PhysicalDeviceInfo info;
        info.handle = devices[i];
        info.index = i;

        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        VkPhysicalDeviceVulkan12Properties properties12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
        properties.pNext = &properties12;
        vkGetPhysicalDeviceProperties2(devices[i], &properties);
        info.name = properties.properties.deviceName;
        info.api_version = properties.properties.apiVersion;
        info.type = properties.properties.deviceType;
        info.vendor_id = properties.properties.vendorID;
        info.device_id = properties.properties.deviceID;
        info.driver_version = properties.properties.driverVersion;
        info.driver_name = properties12.driverName;

        VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features12.pNext = &features13;
        features2.pNext = &features12;
        vkGetPhysicalDeviceFeatures2(devices[i], &features2);

        info.features.dynamic_rendering = features13.dynamicRendering == VK_TRUE;
        info.features.synchronization2 = features13.synchronization2 == VK_TRUE;
        info.features.maintenance4 = features13.maintenance4 == VK_TRUE;
        info.features.timeline_semaphore = features12.timelineSemaphore == VK_TRUE;
        info.features.descriptor_indexing = features12.descriptorIndexing == VK_TRUE;
        info.features.buffer_device_address = features12.bufferDeviceAddress == VK_TRUE;
        info.features.sampler_filter_minmax = features12.samplerFilterMinmax == VK_TRUE;
        info.features.sampler_anisotropy = features2.features.samplerAnisotropy == VK_TRUE;
        info.features.fill_mode_non_solid = features2.features.fillModeNonSolid == VK_TRUE;
        info.features.wide_lines = features2.features.wideLines == VK_TRUE;
        info.features.depth_clamp = features2.features.depthClamp == VK_TRUE;
        info.features.independent_blend = features2.features.independentBlend == VK_TRUE;
        info.features.shader_float64 = features2.features.shaderFloat64 == VK_TRUE;

        VkPhysicalDeviceMemoryProperties memory_properties{};
        vkGetPhysicalDeviceMemoryProperties(devices[i], &memory_properties);
        for (u32 heap = 0; heap < memory_properties.memoryHeapCount; ++heap) {
            if ((memory_properties.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0u) {
                info.device_local_memory += memory_properties.memoryHeaps[heap].size;
            } else {
                info.host_visible_memory += memory_properties.memoryHeaps[heap].size;
            }
        }

        u32 family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &family_count, families.data());
        for (u32 f = 0; f < family_count; ++f) {
            QueueFamilyInfo family;
            family.index = f;
            family.flags = families[f].queueFlags;
            family.queue_count = families[f].queueCount;
            if (surface != VK_NULL_HANDLE) {
                VkBool32 supported = VK_FALSE;
                if (vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], f, surface, &supported) == VK_SUCCESS) {
                    family.supports_present = supported == VK_TRUE;
                }
            } else {
                family.supports_present = false;
            }
            info.queue_families.push_back(family);
        }

        // Choose one family per role, preferring a single family that can do everything.
        const bool need_present = surface != VK_NULL_HANDLE;
        for (const QueueFamilyInfo& family : info.queue_families) {
            if (!family.supports_graphics()) continue;
            if (need_present && !family.supports_present) {
                if (info.graphics_family == kInvalidIndex) info.graphics_family = family.index;
                continue;
            }
            info.graphics_family = family.index;
            break;
        }
        if (info.graphics_family == kInvalidIndex && !info.queue_families.empty()) info.graphics_family = 0;

        info.compute_family = info.graphics_family;
        const QueueFamilyInfo* graphics = info.find_family(info.graphics_family);
        if (graphics == nullptr || !graphics->supports_compute()) {
            for (const QueueFamilyInfo& family : info.queue_families) {
                if (family.supports_compute()) {
                    info.compute_family = family.index;
                    break;
                }
            }
        }

        info.transfer_family = info.graphics_family;
        if (graphics == nullptr || !graphics->supports_transfer()) {
            for (const QueueFamilyInfo& family : info.queue_families) {
                if (family.supports_transfer()) {
                    info.transfer_family = family.index;
                    break;
                }
            }
        }

        if (need_present) {
            for (const QueueFamilyInfo& family : info.queue_families) {
                if (family.supports_present) {
                    if (family.index == info.graphics_family) {
                        info.present_family = family.index;
                        break;
                    }
                    if (info.present_family == kInvalidIndex) info.present_family = family.index;
                }
            }
        }

        out.push_back(std::move(info));
    }
    return out;
}

Scope<Device> Device::create(const Instance& instance, const DeviceDesc& desc) {
    const auto devices = enumerate_physical_devices(instance, desc.surface);
    if (devices.empty()) {
        ORE_ERROR("no Vulkan capable physical device found");
        return nullptr;
    }

    const PhysicalDeviceInfo* chosen = nullptr;
    int best_score = -1;
    for (const PhysicalDeviceInfo& info : devices) {
        if (desc.physical_device != VK_NULL_HANDLE && info.handle != desc.physical_device) continue;
        if (desc.physical_device == VK_NULL_HANDLE && info.index != desc.device_index &&
            !(desc.device_index == 0 && false)) {
            // device_index selects explicitly; index 0 is also the default fallback below.
        }
        if (desc.require_presentation && !info.supports_presentation()) continue;
        const int score = score_device(info, desc.prefer_discrete_gpu, desc.allow_software_device);
        if (score > best_score) {
            best_score = score;
            chosen = &info;
        }
    }

    if (chosen == nullptr && desc.physical_device == VK_NULL_HANDLE) {
        // Explicit index fallback, even when the device looks unsupported: report a clear error below.
        for (const PhysicalDeviceInfo& info : devices) {
            if (info.index == desc.device_index) {
                chosen = &info;
                break;
            }
        }
    }
    if (chosen == nullptr) {
        ORE_ERROR("no suitable Vulkan device (device_index={}, require_presentation={})", desc.device_index,
                  desc.require_presentation);
        return nullptr;
    }
    if (!chosen->features.meets_requirements()) {
        ORE_ERROR("device '{}' cannot run the Ore renderer: it needs Vulkan 1.3 with dynamic rendering, "
                  "synchronization2 and timeline semaphores",
                  chosen->name);
        return nullptr;
    }

    Scope<Device> device(new Device());
    device->physical_ = chosen->handle;
    device->info_ = *chosen;
    vkGetPhysicalDeviceProperties(device->physical_, &device->properties_);
    vkGetPhysicalDeviceMemoryProperties(device->physical_, &device->memory_properties_);

    // ---------------------------------------------------------------- queues ---
    struct FamilyRequest {
        u32 family = kInvalidIndex;
        u32 count = 0;
    };
    std::vector<FamilyRequest> requests;
    const auto request_family = [&requests](u32 family) {
        if (family == kInvalidIndex) return;
        for (FamilyRequest& request : requests) {
            if (request.family == family) return;
        }
        requests.push_back(FamilyRequest{family, 1});
    };
    request_family(chosen->graphics_family);
    request_family(chosen->compute_family);
    request_family(chosen->transfer_family);
    if (desc.require_presentation) request_family(chosen->present_family);
    if (requests.empty()) {
        ORE_ERROR("device '{}' exposes no usable queue family", chosen->name);
        return nullptr;
    }

    const float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    queue_infos.reserve(requests.size());
    for (const FamilyRequest& request : requests) {
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = request.family;
        queue_info.queueCount = request.count;
        queue_info.pQueuePriorities = &priority;
        queue_infos.push_back(queue_info);
    }

    // -------------------------------------------------------------- features ---
    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    features13.maintenance4 = chosen->features.maintenance4 ? VK_TRUE : VK_FALSE;

    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.pNext = &features13;
    features12.timelineSemaphore = VK_TRUE;
    features12.descriptorIndexing = chosen->features.descriptor_indexing ? VK_TRUE : VK_FALSE;
    features12.bufferDeviceAddress = chosen->features.buffer_device_address ? VK_TRUE : VK_FALSE;
    features12.samplerFilterMinmax = chosen->features.sampler_filter_minmax ? VK_TRUE : VK_FALSE;
    if (features12.descriptorIndexing == VK_TRUE) {
        features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        features12.descriptorBindingPartiallyBound = VK_TRUE;
        features12.runtimeDescriptorArray = VK_TRUE;
    }

    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features2.pNext = &features12;
    features2.features.samplerAnisotropy = chosen->features.sampler_anisotropy ? VK_TRUE : VK_FALSE;
    features2.features.fillModeNonSolid = chosen->features.fill_mode_non_solid ? VK_TRUE : VK_FALSE;
    features2.features.wideLines = chosen->features.wide_lines ? VK_TRUE : VK_FALSE;
    features2.features.depthClamp = chosen->features.depth_clamp ? VK_TRUE : VK_FALSE;
    features2.features.independentBlend = chosen->features.independent_blend ? VK_TRUE : VK_FALSE;
    features2.features.shaderFloat64 = chosen->features.shader_float64 ? VK_TRUE : VK_FALSE;
    features2.features.samplerAnisotropy = chosen->features.sampler_anisotropy ? VK_TRUE : VK_FALSE;

    // ------------------------------------------------------------ extensions ---
    u32 extension_count = 0;
    vkEnumerateDeviceExtensionProperties(device->physical_, nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> available_extensions(extension_count);
    vkEnumerateDeviceExtensionProperties(device->physical_, nullptr, &extension_count, available_extensions.data());

    std::vector<const char*> extensions;
    if (desc.require_presentation || desc.surface != VK_NULL_HANDLE) {
        if (!has_extension(available_extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            ORE_ERROR("device '{}' does not support {}", chosen->name, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            return nullptr;
        }
        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        device->enabled_extensions_.emplace_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    if (has_extension(available_extensions, "VK_KHR_portability_subset")) {
        extensions.push_back("VK_KHR_portability_subset");
        device->enabled_extensions_.emplace_back("VK_KHR_portability_subset");
    }
    for (const std::string& extension : desc.extensions) {
        if (!has_extension(available_extensions, extension)) {
            ORE_WARN("requested device extension '{}' is not available", extension);
            continue;
        }
        extensions.push_back(extension.c_str());
        device->enabled_extensions_.push_back(extension);
    }

    VkDeviceCreateInfo create_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    create_info.pNext = &features2;
    create_info.queueCreateInfoCount = static_cast<u32>(queue_infos.size());
    create_info.pQueueCreateInfos = queue_infos.data();
    create_info.enabledExtensionCount = static_cast<u32>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();

    ORE_VK_CHECK(vkCreateDevice(device->physical_, &create_info, nullptr, &device->device_));

    const auto fetch_queue = [&device](u32 family, QueueKind kind) {
        if (family == kInvalidIndex) {
            device->queues_[static_cast<usize>(kind)] = VK_NULL_HANDLE;
            device->families_[static_cast<usize>(kind)] = kInvalidIndex;
            return;
        }
        VkQueue queue = VK_NULL_HANDLE;
        vkGetDeviceQueue(device->device_, family, 0, &queue);
        device->queues_[static_cast<usize>(kind)] = queue;
        device->families_[static_cast<usize>(kind)] = family;
    };
    fetch_queue(chosen->graphics_family, QueueKind::Graphics);
    fetch_queue(chosen->compute_family != kInvalidIndex ? chosen->compute_family : chosen->graphics_family,
                QueueKind::Compute);
    fetch_queue(chosen->transfer_family != kInvalidIndex ? chosen->transfer_family : chosen->graphics_family,
                QueueKind::Transfer);
    fetch_queue(desc.require_presentation ? chosen->present_family : kInvalidIndex, QueueKind::Present);

    // Debug utils, when the instance enabled VK_EXT_debug_utils.
    if (instance.extension_enabled(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        device->debug_utils_.set_object_name = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            vkGetDeviceProcAddr(device->device_, "vkSetDebugUtilsObjectNameEXT"));
        device->debug_utils_.cmd_begin_label = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
            vkGetDeviceProcAddr(device->device_, "vkCmdBeginDebugUtilsLabelEXT"));
        device->debug_utils_.cmd_end_label = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
            vkGetDeviceProcAddr(device->device_, "vkCmdEndDebugUtilsLabelEXT"));
        device->debug_utils_.cmd_insert_label = reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(
            vkGetDeviceProcAddr(device->device_, "vkCmdInsertDebugUtilsLabelEXT"));
    }

    ORE_INFO("Vulkan device: {} ({}, api {}) driver {}", device->info_.name, device_type_name(device->info_.type),
             api_version_string(device->info_.api_version), device->info_.driver_name);
    ORE_DEBUG("  queues: graphics={} compute={} transfer={} present={}", device->queue_family(QueueKind::Graphics),
              device->queue_family(QueueKind::Compute), device->queue_family(QueueKind::Transfer),
              device->queue_family(QueueKind::Present));
    return device;
}

Device::~Device() {
    if (device_ != VK_NULL_HANDLE) vkDestroyDevice(device_, nullptr);
}

u32 Device::queue_family(QueueKind kind) const { return families_[static_cast<usize>(kind)]; }

void Device::wait_idle() const {
    if (device_ != VK_NULL_HANDLE) ORE_VK_CHECK(vkDeviceWaitIdle(device_));
}

bool Device::extension_enabled(std::string_view name) const {
    return std::find(enabled_extensions_.begin(), enabled_extensions_.end(), name) != enabled_extensions_.end();
}

VkFormat Device::find_supported_format(ConstSpan<VkFormat> candidates, VkImageTiling tiling,
                                       VkFormatFeatureFlags features) const {
    for (VkFormat format : candidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physical_, format, &properties);
        const VkFormatFeatureFlags supported =
            tiling == VK_IMAGE_TILING_LINEAR ? properties.linearTilingFeatures : properties.optimalTilingFeatures;
        if ((supported & features) == features) return format;
    }
    return VK_FORMAT_UNDEFINED;
}

std::optional<u32> Device::find_memory_type(u32 type_bits, VkMemoryPropertyFlags properties) const {
    for (u32 i = 0; i < memory_properties_.memoryTypeCount; ++i) {
        const bool type_matches = (type_bits & (1u << i)) != 0u;
        const bool properties_match =
            (memory_properties_.memoryTypes[i].propertyFlags & properties) == properties;
        if (type_matches && properties_match) return i;
    }
    return std::nullopt;
}

void Device::set_debug_name(VkObjectType type, u64 handle, std::string_view name) const {
    if (!debug_utils_.valid() || handle == 0) return;
    VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    info.objectType = type;
    info.objectHandle = handle;
    const std::string owned(name);
    info.pObjectName = owned.c_str();
    debug_utils_.set_object_name(device_, &info);
}

PFN_vkVoidFunction Device::proc_address(const char* name) const { return vkGetDeviceProcAddr(device_, name); }

} // namespace ore::rhi
