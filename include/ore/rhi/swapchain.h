// Ore framework - window swapchain management.
#pragma once

#include <ore/core/types.h>
#include <ore/rhi/device.h>
#include <ore/rhi/sync.h>

#include <string>
#include <vector>

namespace ore::rhi {

struct SwapchainDesc {
    u32 width = 0;
    u32 height = 0;
    bool vsync = true;
    /// Preferred format; VK_FORMAT_UNDEFINED picks the best surface format (preferring sRGB).
    VkFormat preferred_format = VK_FORMAT_UNDEFINED;
    u32 preferred_image_count = 3;
    std::string debug_name;
};

struct SwapchainSupport {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> present_modes;
    [[nodiscard]] bool usable() const { return !formats.empty() && !present_modes.empty(); }
};

[[nodiscard]] SwapchainSupport query_swapchain_support(VkPhysicalDevice device, VkSurfaceKHR surface);

class Swapchain {
public:
    [[nodiscard]] static Scope<Swapchain> create(Device& device, VkSurfaceKHR surface, const SwapchainDesc& desc);
    ~Swapchain();
    ORE_NON_MOVABLE(Swapchain);

    [[nodiscard]] VkSwapchainKHR handle() const { return swapchain_; }
    [[nodiscard]] VkFormat format() const { return format_; }
    [[nodiscard]] VkColorSpaceKHR color_space() const { return color_space_; }
    [[nodiscard]] VkExtent2D extent() const { return extent_; }
    [[nodiscard]] u32 image_count() const { return static_cast<u32>(images_.size()); }
    [[nodiscard]] VkImage image(u32 index) const { return images_[index]; }
    [[nodiscard]] VkImageView view(u32 index) const { return views_[index]; }
    [[nodiscard]] bool vsync() const { return vsync_; }

    /// Returns false (and sets out_of_date) when the swapchain needs to be recreated.
    /// On timeout the call returns false with out_of_date set to false.
    bool acquire_next_image(Semaphore& signal_semaphore, u32& image_index, bool& out_of_date,
                            u64 timeout_ns = ~0ull);
    /// Returns false when the swapchain is out of date and must be recreated.
    bool present(VkQueue queue, ConstSpan<VkSemaphore> wait_semaphores, u32 image_index, bool& out_of_date);

private:
    Swapchain() = default;

    Device* device_ = nullptr;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    bool vsync_ = true;
};

} // namespace ore::rhi
