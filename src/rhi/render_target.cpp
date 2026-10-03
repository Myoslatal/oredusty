#include <ore/rhi/render_target.h>

#include <ore/core/assert.h>
#include <ore/core/log.h>

namespace ore::rhi {

Scope<RenderTarget> RenderTarget::create(GpuAllocator& allocator, const RenderTargetDesc& desc) {
    if (desc.width == 0 || desc.height == 0) {
        ORE_ERROR("RenderTarget::create: invalid size {}x{}", desc.width, desc.height);
        return nullptr;
    }

    Scope<RenderTarget> target(new RenderTarget());
    target->desc_ = desc;

    TextureDesc color_desc;
    color_desc.width = desc.width;
    color_desc.height = desc.height;
    color_desc.format = desc.color_format;
    color_desc.samples = desc.samples;
    color_desc.debug_name = desc.debug_name.empty() ? "render_target.color" : desc.debug_name + ".color";
    color_desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (desc.sampled) color_desc.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    target->color_ = Texture::create(allocator, color_desc);
    if (target->color_ == nullptr) return nullptr;
    target->color_image_ = target->color_->image();
    target->color_view_ = target->color_->view();

    if (desc.depth_format != VK_FORMAT_UNDEFINED) {
        TextureDesc depth_desc;
        depth_desc.width = desc.width;
        depth_desc.height = desc.height;
        depth_desc.format = desc.depth_format;
        depth_desc.samples = desc.samples;
        depth_desc.debug_name = desc.debug_name.empty() ? "render_target.depth" : desc.debug_name + ".depth";
        depth_desc.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        target->depth_ = Texture::create(allocator, depth_desc);
        if (target->depth_ == nullptr) return nullptr;
    }
    return target;
}

Scope<RenderTarget> RenderTarget::create_external(GpuAllocator& allocator, const RenderTargetDesc& desc,
                                                 VkImage color_image, VkImageView color_view) {
    Scope<RenderTarget> target(new RenderTarget());
    target->desc_ = desc;
    target->color_image_ = color_image;
    target->color_view_ = color_view;

    if (desc.depth_format != VK_FORMAT_UNDEFINED) {
        TextureDesc depth_desc;
        depth_desc.width = desc.width;
        depth_desc.height = desc.height;
        depth_desc.format = desc.depth_format;
        depth_desc.samples = desc.samples;
        depth_desc.debug_name = desc.debug_name.empty() ? "swapchain.depth" : desc.debug_name + ".depth";
        depth_desc.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        target->depth_ = Texture::create(allocator, depth_desc);
        if (target->depth_ == nullptr) return nullptr;
    }
    return target;
}

RenderTarget::~RenderTarget() = default;

VkImage RenderTarget::color_image() const { return color_image_; }
VkImageView RenderTarget::color_view() const { return color_view_; }
VkImage RenderTarget::depth_image() const { return depth_ != nullptr ? depth_->image() : VK_NULL_HANDLE; }
VkImageView RenderTarget::depth_view() const { return depth_ != nullptr ? depth_->view() : VK_NULL_HANDLE; }

void RenderTarget::begin(CommandBuffer& cmd, const VkClearValue& color_clear, f32 depth_clear, u32 stencil_clear) {
    ORE_ASSERT(!rendering_);
    cmd.begin_label(desc_.debug_name.empty() ? "render target" : desc_.debug_name);

    // Move the colour image into an attachment layout this frame can render into.
    if (color_layout_ != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        cmd.transition_image(color_image_, color_layout_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             subresource_range(VK_IMAGE_ASPECT_COLOR_BIT, 1, 1));
        color_layout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (has_depth() && depth_layout_ != VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) {
        cmd.transition_image(depth_image(), depth_layout_, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                             subresource_range(VK_IMAGE_ASPECT_DEPTH_BIT, 1, 1));
        depth_layout_ = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    }

    ColorAttachmentInfo color_attachment;
    color_attachment.view = color_view_;
    color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.store_op = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.clear = color_clear;

    DepthAttachmentInfo depth_attachment;
    const DepthAttachmentInfo* depth_ptr = nullptr;
    if (has_depth()) {
        depth_attachment.view = depth_view();
        depth_attachment.layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_attachment.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_attachment.store_op = VK_ATTACHMENT_STORE_OP_STORE;
        depth_attachment.clear.depthStencil.depth = depth_clear;
        depth_attachment.clear.depthStencil.stencil = stencil_clear;
        depth_ptr = &depth_attachment;
    }

    RenderingInfo info;
    info.area = render_area();
    info.colors = ConstSpan<ColorAttachmentInfo>(&color_attachment, 1);
    info.depth = depth_ptr;
    cmd.begin_rendering(info);
    rendering_ = true;
}

void RenderTarget::begin_load(CommandBuffer& cmd) {
    ORE_ASSERT(!rendering_);
    if (color_layout_ != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        cmd.transition_image(color_image_, color_layout_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             subresource_range(VK_IMAGE_ASPECT_COLOR_BIT, 1, 1));
        color_layout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (has_depth() && depth_layout_ != VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) {
        cmd.transition_image(depth_image(), depth_layout_, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                             subresource_range(VK_IMAGE_ASPECT_DEPTH_BIT, 1, 1));
        depth_layout_ = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    }

    ColorAttachmentInfo color_attachment;
    color_attachment.view = color_view_;
    color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
    color_attachment.store_op = VK_ATTACHMENT_STORE_OP_STORE;

    DepthAttachmentInfo depth_attachment;
    const DepthAttachmentInfo* depth_ptr = nullptr;
    if (has_depth()) {
        depth_attachment.view = depth_view();
        depth_attachment.layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_attachment.load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth_attachment.store_op = VK_ATTACHMENT_STORE_OP_STORE;
        depth_ptr = &depth_attachment;
    }

    RenderingInfo info;
    info.area = render_area();
    info.colors = ConstSpan<ColorAttachmentInfo>(&color_attachment, 1);
    info.depth = depth_ptr;
    cmd.begin_rendering(info);
    rendering_ = true;
}

void RenderTarget::end(CommandBuffer& cmd) {
    ORE_ASSERT(rendering_);
    cmd.end_rendering();
    rendering_ = false;

    if (color_layout_ != desc_.final_color_layout) {
        cmd.transition_image(color_image_, color_layout_, desc_.final_color_layout,
                             subresource_range(VK_IMAGE_ASPECT_COLOR_BIT, 1, 1));
        color_layout_ = desc_.final_color_layout;
    }
    cmd.end_label();
}

} // namespace ore::rhi
