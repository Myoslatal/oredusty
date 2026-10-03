// Ore framework - GPU images, image views and samplers.
#pragma once

#include <ore/core/image.h>
#include <ore/core/types.h>
#include <ore/rhi/allocator.h>
#include <ore/rhi/device.h>

#include <string>
#include <string_view>

namespace ore::rhi {

struct TextureDesc {
    u32 width = 1;
    u32 height = 1;
    u32 depth = 1;
    u32 mip_levels = 1;
    u32 array_layers = 1;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
    std::string debug_name;

    [[nodiscard]] VkExtent3D extent() const { return VkExtent3D{width, height, depth}; }
    [[nodiscard]] bool is_cube() const { return array_layers == 6; }
};

class GraphicsContext;

class Texture {
public:
    [[nodiscard]] static Scope<Texture> create(GpuAllocator& allocator, const TextureDesc& desc);
    /// Convenience: creates the image and uploads \p image through \p context (blocking).
    /// Prefer GraphicsContext::create_texture() in game code.
    [[nodiscard]] static Scope<Texture> create_from_image(GraphicsContext& context, const Image& image,
                                                          bool generate_mipmaps = true,
                                                          std::string_view debug_name = {});
    ~Texture();
    ORE_NON_MOVABLE(Texture);

    [[nodiscard]] VkImage image() const { return image_; }
    [[nodiscard]] VkImageView view() const { return view_; }
    [[nodiscard]] const TextureDesc& desc() const { return desc_; }
    [[nodiscard]] VkFormat format() const { return desc_.format; }
    [[nodiscard]] VkExtent3D extent() const { return desc_.extent(); }
    [[nodiscard]] u32 mip_levels() const { return desc_.mip_levels; }
    [[nodiscard]] u32 array_layers() const { return desc_.array_layers; }
    /// Full subresource range of the default view.
    [[nodiscard]] VkImageSubresourceRange full_range() const;
    [[nodiscard]] VkDescriptorImageInfo descriptor_info(VkSampler sampler) const;
    [[nodiscard]] const Allocation& allocation() const { return allocation_; }

    /// Creates an additional view (single mip / single layer) owned by the caller.
    [[nodiscard]] VkImageView create_view(u32 base_mip, u32 mip_count, u32 base_layer, u32 layer_count,
                                          VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D) const;

private:
    Texture() = default;

    GpuAllocator* allocator_ = nullptr;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    Allocation allocation_{};
    TextureDesc desc_{};
};

struct SamplerDesc {
    VkFilter min_filter = VK_FILTER_LINEAR;
    VkFilter mag_filter = VK_FILTER_LINEAR;
    VkSamplerMipmapMode mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    VkSamplerAddressMode address_mode_u = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode address_mode_v = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode address_mode_w = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkBorderColor border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    bool anisotropy_enable = false;
    f32 max_anisotropy = 8.0f;
    bool compare_enable = false;
    VkCompareOp compare_op = VK_COMPARE_OP_LESS_OR_EQUAL;
    std::string debug_name;

    [[nodiscard]] static SamplerDesc nearest();
    [[nodiscard]] static SamplerDesc linear();
    [[nodiscard]] static SamplerDesc shadow_compare();
};

class Sampler {
public:
    [[nodiscard]] static Scope<Sampler> create(Device& device, const SamplerDesc& desc = {});
    ~Sampler();
    ORE_NON_MOVABLE(Sampler);

    [[nodiscard]] VkSampler handle() const { return sampler_; }

private:
    Sampler() = default;
    Device* device_ = nullptr;
    VkSampler sampler_ = VK_NULL_HANDLE;
};

} // namespace ore::rhi
