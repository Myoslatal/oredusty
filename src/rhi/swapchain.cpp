#include <ore/rhi/swapchain.h>

#include <ore/core/assert.h>
#include <ore/core/log.h>
#include <ore/rhi/commands.h>

#include <algorithm>

namespace ore::rhi {
namespace {

[[nodiscard]] VkSurfaceFormatKHR choose_format(ConstSpan<VkSurfaceFormatKHR> formats, VkFormat preferred) {
    if (preferred != VK_FORMAT_UNDEFINED) {
        for (const VkSurfaceFormatKHR& format : formats) {
            if (format.format == preferred) return format;
        }
    }
    for (const VkSurfaceFormatKHR& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    for (const VkSurfaceFormatKHR& format : formats) {
        if (format.format == VK_FORMAT_R8G8B8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    for (const VkSurfaceFormatKHR& format : formats) {
        if (!is_srgb_format(format.format)) return format;
    }
    return formats.front();
}

[[nodiscard]] VkPresentModeKHR choose_present_mode(ConstSpan<VkPresentModeKHR> modes, bool vsync) {
    if (vsync) return VK_PRESENT_MODE_FIFO_KHR; // always supported
    for (VkPresentModeKHR mode : modes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) return mode;
    }
    for (VkPresentModeKHR mode : modes) {
        if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR) return mode;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

} // namespace

SwapchainSupport query_swapchain_support(VkPhysicalDevice device, VkSurfaceKHR surface) {
    SwapchainSupport support;
    if (surface == VK_NULL_HANDLE) return support;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &support.capabilities) != VK_SUCCESS) return support;

    u32 format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, nullptr);
    if (format_count > 0) {
        support.formats.resize(format_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, support.formats.data());
    }

    u32 mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &mode_count, nullptr);
    if (mode_count > 0) {
        support.present_modes.resize(mode_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &mode_count, support.present_modes.data());
    }
    return support;
}

Scope<Swapchain> Swapchain::create(Device& device, VkSurfaceKHR surface, const SwapchainDesc& desc) {
    const SwapchainSupport support = query_swapchain_support(device.physical_device(), surface);
    if (!support.usable()) {
        ORE_ERROR("swapchain: the surface reports no usable formats or present modes "
                  "(is the window system surface valid?)");
        return nullptr;
    }

    Scope<Swapchain> swapchain(new Swapchain());
    swapchain->device_ = &device;
    swapchain->vsync_ = desc.vsync;

    const VkSurfaceFormatKHR surface_format = choose_format(support.formats, desc.preferred_format);
    swapchain->format_ = surface_format.format;
    swapchain->color_space_ = surface_format.colorSpace;

    const VkSurfaceCapabilitiesKHR& capabilities = support.capabilities;
    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == 0xFFFF'FFFFu) {
        extent.width = std::clamp(desc.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(desc.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }
    swapchain->extent_ = extent;

    u32 image_count = std::max(desc.preferred_image_count, capabilities.minImageCount);
    if (capabilities.maxImageCount > 0) image_count = std::min(image_count, capabilities.maxImageCount);

    VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    const VkCompositeAlphaFlagBitsKHR candidates[] = {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                                      VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                                      VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                                                      VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
    for (VkCompositeAlphaFlagBitsKHR candidate : candidates) {
        if ((capabilities.supportedCompositeAlpha & candidate) != 0u) {
            composite_alpha = candidate;
            break;
        }
    }

    VkSwapchainCreateInfoKHR create_info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    create_info.surface = surface;
    create_info.minImageCount = image_count;
    create_info.imageFormat = swapchain->format_;
    create_info.imageColorSpace = swapchain->color_space_;
    create_info.imageExtent = extent;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create_info.preTransform = capabilities.currentTransform;
    create_info.compositeAlpha = composite_alpha;
    create_info.presentMode = choose_present_mode(support.present_modes, desc.vsync);
    create_info.clipped = VK_TRUE;
    create_info.oldSwapchain = VK_NULL_HANDLE;

    ORE_VK_CHECK(vkCreateSwapchainKHR(device.handle(), &create_info, nullptr, &swapchain->swapchain_));

    u32 actual_count = 0;
    ORE_VK_CHECK(vkGetSwapchainImagesKHR(device.handle(), swapchain->swapchain_, &actual_count, nullptr));
    swapchain->images_.resize(actual_count);
    ORE_VK_CHECK(vkGetSwapchainImagesKHR(device.handle(), swapchain->swapchain_, &actual_count,
                                         swapchain->images_.data()));

    swapchain->views_.resize(actual_count, VK_NULL_HANDLE);
    for (u32 i = 0; i < actual_count; ++i) {
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = swapchain->images_[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = swapchain->format_;
        view_info.subresourceRange = subresource_range(VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
        ORE_VK_CHECK(vkCreateImageView(device.handle(), &view_info, nullptr, &swapchain->views_[i]));
        device.set_debug_name(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<u64>(swapchain->images_[i]),
                              std::format("swapchain.image[{}]", i));
    }

    ORE_INFO("swapchain: {} image(s), {}x{}, format {}, vsync {}", actual_count, extent.width, extent.height,
             format_string(swapchain->format_), desc.vsync ? "on" : "off");
    return swapchain;
}

Swapchain::~Swapchain() {
    if (swapchain_ != VK_NULL_HANDLE && device_ != nullptr) {
        for (VkImageView view : views_) {
            if (view != VK_NULL_HANDLE) vkDestroyImageView(device_->handle(), view, nullptr);
        }
        vkDestroySwapchainKHR(device_->handle(), swapchain_, nullptr);
    }
}

bool Swapchain::acquire_next_image(Semaphore& signal_semaphore, u32& image_index, bool& out_of_date, u64 timeout_ns) {
    out_of_date = false;
    const VkResult result = vkAcquireNextImageKHR(device_->handle(), swapchain_, timeout_ns,
                                                 signal_semaphore.handle(), VK_NULL_HANDLE, &image_index);
    switch (result) {
        case VK_SUCCESS:
        case VK_SUBOPTIMAL_KHR:
            if (result == VK_SUBOPTIMAL_KHR) out_of_date = true;
            return true;
        case VK_ERROR_OUT_OF_DATE_KHR:
            out_of_date = true;
            return false;
        case VK_TIMEOUT:
        case VK_NOT_READY:
            return false;
        default:
            ORE_VK_CHECK(result);
            return false;
    }
}

bool Swapchain::present(VkQueue queue, ConstSpan<VkSemaphore> wait_semaphores, u32 image_index, bool& out_of_date) {
    out_of_date = false;
    VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    info.waitSemaphoreCount = static_cast<u32>(wait_semaphores.size());
    info.pWaitSemaphores = wait_semaphores.empty() ? nullptr : wait_semaphores.data();
    info.swapchainCount = 1;
    info.pSwapchains = &swapchain_;
    info.pImageIndices = &image_index;

    const VkResult result = vkQueuePresentKHR(queue, &info);
    switch (result) {
        case VK_SUCCESS: return true;
        case VK_SUBOPTIMAL_KHR:
            out_of_date = true;
            return true;
        case VK_ERROR_OUT_OF_DATE_KHR:
            out_of_date = true;
            return false;
        default:
            ORE_VK_CHECK(result);
            return false;
    }
}

} // namespace ore::rhi
