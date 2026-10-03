#include <ore/rhi/texture.h>

#include <ore/core/assert.h>
#include <ore/core/log.h>
#include <ore/rhi/commands.h>
#include <ore/rhi/context.h>

#include <algorithm>
#include <cmath>

namespace ore::rhi {
namespace {

[[nodiscard]] bool format_supports_blit(Device& device, VkFormat format) {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(device.physical_device(), format, &properties);
    return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0u &&
           (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0u;
}

[[nodiscard]] u32 mip_level_count(u32 width, u32 height) {
    u32 levels = 1;
    u32 size = std::max(width, height);
    while (size > 1) {
        size /= 2;
        ++levels;
    }
    return levels;
}

} // namespace

SamplerDesc SamplerDesc::nearest() {
    SamplerDesc desc;
    desc.min_filter = VK_FILTER_NEAREST;
    desc.mag_filter = VK_FILTER_NEAREST;
    desc.mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    return desc;
}

SamplerDesc SamplerDesc::linear() { return SamplerDesc{}; }

SamplerDesc SamplerDesc::shadow_compare() {
    SamplerDesc desc;
    desc.compare_enable = true;
    desc.address_mode_u = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    desc.address_mode_v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    desc.address_mode_w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    desc.border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    return desc;
}

Scope<Texture> Texture::create(GpuAllocator& allocator, const TextureDesc& desc) {
    Scope<Texture> texture(new Texture());
    texture->allocator_ = &allocator;
    texture->desc_ = desc;
    if (desc.mip_levels == 0) texture->desc_.mip_levels = mip_level_count(desc.width, desc.height);

    VkImageCreateInfo create_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    create_info.imageType = desc.depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    create_info.format = desc.format;
    create_info.extent = desc.extent();
    create_info.mipLevels = texture->desc_.mip_levels;
    create_info.arrayLayers = desc.array_layers;
    create_info.samples = desc.samples;
    create_info.tiling = desc.tiling;
    create_info.usage = desc.usage;
    create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (desc.is_cube()) create_info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ORE_VK_CHECK(vkCreateImage(allocator.device().handle(), &create_info, nullptr, &texture->image_));

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(allocator.device().handle(), texture->image_, &requirements);
    const MemoryKind kind = desc.tiling == VK_IMAGE_TILING_LINEAR ? MemoryKind::Linear : MemoryKind::NonLinear;
    texture->allocation_ = allocator.allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, kind);
    if (!texture->allocation_.valid()) {
        ORE_ERROR("Texture::create: out of memory for {}x{} {} ({})", desc.width, desc.height,
                  format_string(desc.format), desc.debug_name);
        vkDestroyImage(allocator.device().handle(), texture->image_, nullptr);
        return nullptr;
    }
    ORE_VK_CHECK(vkBindImageMemory(allocator.device().handle(), texture->image_, texture->allocation_.memory,
                                   texture->allocation_.offset));

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = texture->image_;
    view_info.viewType = desc.is_cube() ? VK_IMAGE_VIEW_TYPE_CUBE
                                        : (desc.depth > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D);
    view_info.format = desc.format;
    view_info.subresourceRange = texture->full_range();
    ORE_VK_CHECK(vkCreateImageView(allocator.device().handle(), &view_info, nullptr, &texture->view_));

    if (!desc.debug_name.empty()) {
        allocator.device().set_debug_name(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<u64>(texture->image_),
                                          desc.debug_name);
    }
    return texture;
}

Scope<Texture> Texture::create_from_image(GraphicsContext& context, const Image& image, bool generate_mipmaps,
                                         std::string_view debug_name) {
    if (image.empty()) {
        ORE_ERROR("Texture::create_from_image: empty image");
        return nullptr;
    }

    Device& device = context.device();
    TextureDesc desc;
    desc.width = image.width;
    desc.height = image.height;
    desc.format = VK_FORMAT_R8G8B8A8_UNORM;
    desc.debug_name = std::string(debug_name);
    const bool mips = generate_mipmaps && format_supports_blit(device, desc.format);
    desc.mip_levels = mips ? mip_level_count(image.width, image.height) : 1;
    desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    Scope<Texture> texture = Texture::create(context.allocator(), desc);
    if (texture == nullptr) return nullptr;
    context.upload_texture(*texture, image, mips);
    return texture;
}

Texture::~Texture() {
    if (image_ != VK_NULL_HANDLE && allocator_ != nullptr) {
        VkDevice device = allocator_->device().handle();
        if (view_ != VK_NULL_HANDLE) vkDestroyImageView(device, view_, nullptr);
        vkDestroyImage(device, image_, nullptr);
        allocator_->free(allocation_);
    }
}

VkImageSubresourceRange Texture::full_range() const {
    const VkImageAspectFlags aspect =
        is_depth_format(desc_.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    return subresource_range(aspect, desc_.mip_levels, desc_.array_layers);
}

VkDescriptorImageInfo Texture::descriptor_info(VkSampler sampler) const {
    VkDescriptorImageInfo info{};
    info.sampler = sampler;
    info.imageView = view_;
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return info;
}

VkImageView Texture::create_view(u32 base_mip, u32 mip_count, u32 base_layer, u32 layer_count,
                                 VkImageViewType type) const {
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = image_;
    view_info.viewType = type;
    view_info.format = desc_.format;
    view_info.subresourceRange =
        subresource_range(full_range().aspectMask, mip_count, layer_count, base_mip, base_layer);
    VkImageView view = VK_NULL_HANDLE;
    ORE_VK_CHECK(vkCreateImageView(allocator_->device().handle(), &view_info, nullptr, &view));
    return view;
}

Scope<Sampler> Sampler::create(Device& device, const SamplerDesc& desc) {
    Scope<Sampler> sampler(new Sampler());
    sampler->device_ = &device;

    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = desc.mag_filter;
    info.minFilter = desc.min_filter;
    info.mipmapMode = desc.mipmap_mode;
    info.addressModeU = desc.address_mode_u;
    info.addressModeV = desc.address_mode_v;
    info.addressModeW = desc.address_mode_w;
    info.borderColor = desc.border_color;
    info.compareEnable = desc.compare_enable ? VK_TRUE : VK_FALSE;
    info.compareOp = desc.compare_op;
    info.anisotropyEnable =
        (desc.anisotropy_enable && device.features().sampler_anisotropy) ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy = info.anisotropyEnable == VK_TRUE ? desc.max_anisotropy : 1.0f;
    info.maxLod = VK_LOD_CLAMP_NONE;
    ORE_VK_CHECK(vkCreateSampler(device.handle(), &info, nullptr, &sampler->sampler_));
    if (!desc.debug_name.empty()) {
        device.set_debug_name(VK_OBJECT_TYPE_SAMPLER, reinterpret_cast<u64>(sampler->sampler_), desc.debug_name);
    }
    return sampler;
}

Sampler::~Sampler() {
    if (sampler_ != VK_NULL_HANDLE && device_ != nullptr) vkDestroySampler(device_->handle(), sampler_, nullptr);
}

} // namespace ore::rhi
