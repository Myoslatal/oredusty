// Ore framework - render targets for dynamic rendering (offscreen images or swapchain images).
//
// The target tracks the current image layouts, so begin()/end() insert exactly the barriers
// that are needed and the caller never has to think about transitions.
#pragma once

#include <ore/core/types.h>
#include <ore/rhi/allocator.h>
#include <ore/rhi/commands.h>
#include <ore/rhi/device.h>
#include <ore/rhi/texture.h>

#include <string>

namespace ore::rhi {

struct RenderTargetDesc {
    u32 width = 0;
    u32 height = 0;
    VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    /// Adds SAMPLED usage so the resolve/colour image can be read in a shader.
    bool sampled = false;
    /// Adds TRANSFER_SRC usage so the image can be copied out (screenshots, tests).
    bool transfer_src = true;
    /// Layout the colour image is left in after end(). Use PRESENT_SRC_KHR for swapchain images.
    VkImageLayout final_color_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    std::string debug_name;
};

class RenderTarget {
public:
    /// Creates and owns the colour (and depth) images.
    [[nodiscard]] static Scope<RenderTarget> create(GpuAllocator& allocator, const RenderTargetDesc& desc);
    /// Wraps externally owned colour images (swapchain images). Only the depth image is allocated here.
    [[nodiscard]] static Scope<RenderTarget> create_external(GpuAllocator& allocator, const RenderTargetDesc& desc,
                                                            VkImage color_image, VkImageView color_view);
    ~RenderTarget();
    ORE_NON_MOVABLE(RenderTarget);

    [[nodiscard]] u32 width() const { return desc_.width; }
    [[nodiscard]] u32 height() const { return desc_.height; }
    [[nodiscard]] VkFormat color_format() const { return desc_.color_format; }
    [[nodiscard]] VkFormat depth_format() const { return desc_.depth_format; }
    [[nodiscard]] bool has_depth() const { return depth_format() != VK_FORMAT_UNDEFINED; }
    [[nodiscard]] VkRect2D render_area() const { return VkRect2D{{0, 0}, {desc_.width, desc_.height}}; }

    [[nodiscard]] VkImage color_image() const;
    [[nodiscard]] VkImageView color_view() const;
    [[nodiscard]] VkImage depth_image() const;
    [[nodiscard]] VkImageView depth_view() const;
    [[nodiscard]] VkImageLayout color_layout() const { return color_layout_; }
    [[nodiscard]] VkImageLayout depth_layout() const { return depth_layout_; }
    [[nodiscard]] const Texture* color_texture() const { return color_.get(); }
    /// True once begin() was called and end() has not run yet.
    [[nodiscard]] bool rendering() const { return rendering_; }

    /// Clears the colour attachment(s) and depth, then starts dynamic rendering.
    void begin(CommandBuffer& cmd, const VkClearValue& color_clear = {}, f32 depth_clear = 1.0f,
               u32 stencil_clear = 0);
    /// Preserves the previous contents (load op LOAD).
    void begin_load(CommandBuffer& cmd);
    /// Ends dynamic rendering and moves the images to their final layouts.
    void end(CommandBuffer& cmd);

private:
    RenderTarget() = default;

    RenderTargetDesc desc_{};
    Scope<Texture> color_;        ///< owned colour image (null for external/swapchain targets)
    Scope<Texture> depth_;        ///< owned depth image
    VkImage color_image_ = VK_NULL_HANDLE;      ///< external or owned
    VkImageView color_view_ = VK_NULL_HANDLE;
    VkImageLayout color_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout depth_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    bool rendering_ = false;
};

} // namespace ore::rhi
