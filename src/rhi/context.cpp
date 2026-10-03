#include <ore/rhi/context.h>

#include <ore/core/assert.h>
#include <ore/core/log.h>
#include <ore/rhi/render_target.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace ore::rhi {
namespace {

[[nodiscard]] f32 half_to_float(u16 half) {
    const u32 sign = (half >> 15) & 0x1u;
    const u32 exponent = (half >> 10) & 0x1Fu;
    const u32 mantissa = half & 0x3FFu;
    f32 value = 0.0f;
    if (exponent == 0) {
        value = std::ldexp(static_cast<f32>(mantissa), -24);
    } else if (exponent != 31) {
        value = std::ldexp(static_cast<f32>(mantissa + 1024u), static_cast<int>(exponent) - 25);
    } else {
        value = 1.0f;
    }
    return sign != 0u ? -value : value;
}

} // namespace

Image image_from_format(const void* pixels, u32 width, u32 height, VkFormat format) {
    Image result = Image::create(width, height);
    if (pixels == nullptr || result.empty()) return result;

    const auto* source = static_cast<const u8*>(pixels);
    u32* destination = reinterpret_cast<u32*>(result.pixels.data());
    const u32 pixel_count = width * height;

    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            for (u32 i = 0; i < pixel_count; ++i) {
                destination[i] = make_rgba(source[i * 4 + 0], source[i * 4 + 1], source[i * 4 + 2],
                                           source[i * 4 + 3]);
            }
            break;
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            for (u32 i = 0; i < pixel_count; ++i) {
                destination[i] = make_rgba(source[i * 4 + 2], source[i * 4 + 1], source[i * 4 + 0],
                                           source[i * 4 + 3]);
            }
            break;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            for (u32 i = 0; i < pixel_count; ++i) {
                const auto* halves = reinterpret_cast<const u16*>(source + i * 8);
                const auto to_unorm = [](f32 value) {
                    return static_cast<u8>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
                };
                destination[i] = make_rgba(to_unorm(half_to_float(halves[0])), to_unorm(half_to_float(halves[1])),
                                           to_unorm(half_to_float(halves[2])), to_unorm(half_to_float(halves[3])));
            }
            break;
        default:
            ORE_ERROR("image_from_format: unsupported format {}", format_string(format));
            return Image::create(0, 0);
    }
    return result;
}

Scope<GraphicsContext> GraphicsContext::create(const ContextDesc& desc) {
    Scope<GraphicsContext> context(new GraphicsContext());
    context->surface_ = desc.headless ? VK_NULL_HANDLE : desc.surface;

    if (!desc.headless && desc.surface == VK_NULL_HANDLE && !desc.surface_factory) {
        ORE_ERROR("GraphicsContext::create: a surface or a surface_factory is required unless the "
                  "context is headless");
        return nullptr;
    }

    InstanceDesc instance_desc;
    instance_desc.application_name = desc.application_name;
    instance_desc.enable_validation = desc.enable_validation;
    instance_desc.extensions = desc.instance_extensions;
    context->instance_ = Instance::create(instance_desc);
    if (context->instance_ == nullptr) return nullptr;

    // The surface (when built by the platform layer) needs the instance, and the device choice
    // needs the surface, so it is created here rather than by the caller.
    if (!desc.headless && context->surface_ == VK_NULL_HANDLE) {
        context->surface_ = desc.surface_factory(context->instance_->handle());
        if (context->surface_ == VK_NULL_HANDLE) {
            ORE_ERROR("GraphicsContext::create: the surface factory returned no surface");
            return nullptr;
        }
        context->owns_surface_ = true;
    }

    DeviceDesc device_desc;
    device_desc.surface = context->surface_;
    device_desc.require_presentation = context->surface_ != VK_NULL_HANDLE;
    device_desc.physical_device = desc.physical_device;
    device_desc.device_index = desc.device_index;
    device_desc.prefer_discrete_gpu = desc.prefer_discrete_gpu;
    device_desc.allow_software_device = desc.allow_software_device;
    device_desc.extensions = desc.device_extensions;
    context->device_ = Device::create(*context->instance_, device_desc);
    if (context->device_ == nullptr) return nullptr;

    GpuAllocatorDesc allocator_desc = desc.allocator;
    context->allocator_ = make_scope<GpuAllocator>(*context->device_, allocator_desc);
    context->immediate_ = ImmediateCommands::create(*context->device_, QueueKind::Graphics, 1);
    if (context->immediate_ == nullptr) return nullptr;

    if (desc.enable_validation && !context->instance_->validation_enabled()) {
        ORE_WARN("validation was requested but VK_LAYER_KHRONOS_validation is unavailable");
    }
    return context;
}

GraphicsContext::~GraphicsContext() {
    if (device_ != nullptr) {
        device_->wait_idle();
        immediate_.reset();
        allocator_.reset();
        device_.reset();
    }
    // Surfaces created through surface_factory belong to the context; the instance must outlive them.
    if (owns_surface_ && surface_ != VK_NULL_HANDLE && instance_ != nullptr) {
        vkDestroySurfaceKHR(instance_->handle(), surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    instance_.reset();
}

void GraphicsContext::wait_idle() { device_->wait_idle(); }

void GraphicsContext::immediate_submit(const std::function<void(CommandBuffer&)>& fn) { immediate_->run(fn); }

Scope<Buffer> GraphicsContext::create_buffer(const BufferDesc& desc) const {
    return Buffer::create(*allocator_, desc);
}

Scope<Buffer> GraphicsContext::create_staging_buffer(u64 size, std::string_view debug_name) const {
    BufferDesc desc;
    desc.size = size;
    desc.usage = BufferUsage::TransferSrc | BufferUsage::TransferDst;
    desc.host_visible = true;
    desc.debug_name = std::string(debug_name);
    return Buffer::create(*allocator_, desc);
}

void GraphicsContext::upload_buffer(Buffer& destination, ConstSpan<u8> data, u64 offset) {
    if (data.empty()) return;
    if (destination.host_visible()) {
        destination.write(data, offset);
        return;
    }

    auto staging = create_staging_buffer(data.size(), "upload staging");
    if (staging == nullptr) return;
    staging->write(data, 0);

    immediate_->run([&](CommandBuffer& cmd) {
        cmd.begin_label("upload buffer");
        cmd.copy_buffer(*staging, destination, data.size(), 0, offset);
        cmd.end_label();
    });
    upload_bytes_ += data.size();
}

void GraphicsContext::generate_mipmaps(CommandBuffer& cmd, Texture& texture) {
    const u32 mip_levels = texture.mip_levels();
    if (mip_levels <= 1) return;

    const VkImage image = texture.image();
    const VkImageSubresourceRange full = subresource_range(VK_IMAGE_ASPECT_COLOR_BIT, mip_levels, 1);
    i32 source_width = static_cast<i32>(texture.extent().width);
    i32 source_height = static_cast<i32>(texture.extent().height);

    cmd.begin_label("generate mipmaps");
    // The upload left the whole image in SHADER_READ_ONLY; move it to TRANSFER_SRC for the blits.
    cmd.transition_image(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         subresource_range(VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 0, 1));
    (void)full;

    for (u32 level = 1; level < mip_levels; ++level) {
        const i32 target_width = std::max(source_width / 2, 1);
        const i32 target_height = std::max(source_height / 2, 1);

        cmd.transition_image(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             subresource_range(VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, level, 1));

        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {source_width, source_height, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {target_width, target_height, 1};
        cmd.blit_image(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       ConstSpan<VkImageBlit>(&blit, 1));

        cmd.transition_image(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             subresource_range(VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, level, 1));
        source_width = target_width;
        source_height = target_height;
    }

    cmd.transition_image(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         full);
    cmd.end_label();
}

void GraphicsContext::update_texture_region(Texture& texture, const Image& image, u32 x, u32 y) {
    if (image.empty()) return;
    const VkExtent3D extent = texture.extent();
    if (x >= extent.width || y >= extent.height) return;
    if (image.width > extent.width - x || image.height > extent.height - y) {
        ORE_ERROR("update_texture_region: {}x{} at ({}, {}) does not fit in a {}x{} texture", image.width,
                  image.height, x, y, extent.width, extent.height);
        return;
    }
    const u64 byte_size = image.byte_size();
    auto staging = create_staging_buffer(byte_size, "texture region staging");
    if (staging == nullptr) return;
    staging->write(ConstSpan<u8>(image.pixels.data(), image.pixels.size()), 0);

    immediate_->run([&](CommandBuffer& cmd) {
        cmd.begin_label("update texture region");
        // A texture that went through create_texture/upload_texture is in SHADER_READ_ONLY_OPTIMAL, so
        // the region is moved to TRANSFER_DST and back instead of from UNDEFINED - the page keeps its
        // earlier glyphs.
        cmd.transition_image(texture.image(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, texture.full_range());
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {static_cast<i32>(x), static_cast<i32>(y), 0};
        region.imageExtent = {image.width, image.height, 1};
        cmd.copy_buffer_to_image(staging->handle(), texture.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 ConstSpan<VkBufferImageCopy>(&region, 1));
        cmd.transition_image(texture.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, texture.full_range());
        cmd.end_label();
    });
    upload_bytes_ += byte_size;
}

void GraphicsContext::upload_texture(Texture& texture, const Image& image, bool generate_mipmaps_requested) {
    if (image.empty()) return;
    const u64 byte_size = image.byte_size();

    auto staging = create_staging_buffer(byte_size, "texture staging");
    if (staging == nullptr) return;
    staging->write(ConstSpan<u8>(image.pixels.data(), image.pixels.size()), 0);

    const bool mips = generate_mipmaps_requested && texture.mip_levels() > 1;
    immediate_->run([&](CommandBuffer& cmd) {
        cmd.begin_label("upload texture");
        cmd.transition_image(texture.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             texture.full_range());
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {0, 0, 0};
        region.imageExtent = texture.extent();
        cmd.copy_buffer_to_image(staging->handle(), texture.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 ConstSpan<VkBufferImageCopy>(&region, 1));
        cmd.transition_image(texture.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, texture.full_range());
        if (mips) generate_mipmaps(cmd, texture);
        cmd.end_label();
    });
    upload_bytes_ += byte_size;
}

Scope<Texture> GraphicsContext::create_texture(const Image& image, bool generate_mipmaps_requested,
                                              std::string_view debug_name) {
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    TextureDesc desc;
    desc.width = image.width;
    desc.height = image.height;
    desc.format = format;
    desc.debug_name = std::string(debug_name);

    bool mips = generate_mipmaps_requested;
    if (mips) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(device_->physical_device(), format, &properties);
        const bool blit_supported = (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0u &&
                                    (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0u;
        if (!blit_supported) mips = false;
    }
    desc.mip_levels = mips ? static_cast<u32>(std::floor(std::log2(std::max(image.width, image.height)))) + 1u : 1u;
    desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    Scope<Texture> texture = Texture::create(*allocator_, desc);
    if (texture == nullptr) return nullptr;
    upload_texture(*texture, image, mips);
    return texture;
}

Scope<Texture> GraphicsContext::create_solid_texture(u32 rgba, std::string_view debug_name) {
    Image image = Image::create(1, 1, rgba);
    return create_texture(image, false, debug_name);
}

Image GraphicsContext::read_image(VkImage image, u32 width, u32 height, VkFormat format,
                                  VkImageLayout current_layout) {
    Image result = Image::create(width, height);

    u32 bytes_per_pixel = 4;
    if (format == VK_FORMAT_R16G16B16A16_SFLOAT) bytes_per_pixel = 8;
    const u64 byte_size = static_cast<u64>(width) * height * bytes_per_pixel;

    auto readback = create_staging_buffer(byte_size, "readback");
    if (readback == nullptr) return result;

    immediate_->run([&](CommandBuffer& cmd) {
        cmd.begin_label("read image");
        cmd.transition_image(image, current_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             subresource_range(VK_IMAGE_ASPECT_COLOR_BIT, 1, 1));
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {width, height, 1};
        cmd.copy_image_to_buffer(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(),
                                 ConstSpan<VkBufferImageCopy>(&region, 1));
        cmd.transition_image(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, current_layout,
                             subresource_range(VK_IMAGE_ASPECT_COLOR_BIT, 1, 1));
        cmd.end_label();
    });

    readback->invalidate(0, byte_size);
    if (const void* mapped = readback->mapped_data(); mapped != nullptr) {
        return image_from_format(mapped, width, height, format);
    }
    return result;
}

Image GraphicsContext::read_render_target(RenderTarget& target) {
    return read_image(target.color_image(), target.width(), target.height(), target.color_format(),
                      target.color_layout());
}

} // namespace ore::rhi
