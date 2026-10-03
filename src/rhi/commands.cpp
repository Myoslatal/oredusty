#include <ore/rhi/commands.h>

#include <ore/core/assert.h>
#include <ore/rhi/buffer.h>
#include <ore/rhi/descriptor.h>
#include <ore/rhi/pipeline.h>

#include <algorithm>

namespace ore::rhi {

void default_barrier_stages(VkImageLayout old_layout, VkImageLayout new_layout, VkPipelineStageFlags2& src_stage,
                            VkAccessFlags2& src_access, VkPipelineStageFlags2& dst_stage, VkAccessFlags2& dst_access) {
    // Pragmatic mapping that covers every transition the framework itself performs.
    src_stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    src_access = 0;
    dst_stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    dst_access = 0;

    switch (old_layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
        case VK_IMAGE_LAYOUT_PREINITIALIZED:
            src_stage = VK_PIPELINE_STAGE_2_NONE;
            src_access = 0;
            break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            src_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            src_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            src_stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            src_access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            src_stage = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT;
            src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            src_stage = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT;
            src_access = VK_ACCESS_2_TRANSFER_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_GENERAL:
            src_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            src_access = VK_ACCESS_2_SHADER_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
            src_stage = VK_PIPELINE_STAGE_2_NONE;
            src_access = 0;
            break;
        default:
            break;
    }

    switch (new_layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            dst_stage = VK_PIPELINE_STAGE_2_NONE;
            dst_access = 0;
            break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            dst_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            dst_access = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            dst_stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            dst_access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            dst_stage = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT;
            dst_access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            dst_stage = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT;
            dst_access = VK_ACCESS_2_TRANSFER_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            dst_access = VK_ACCESS_2_SHADER_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_GENERAL:
            dst_stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            dst_access = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
            dst_stage = VK_PIPELINE_STAGE_2_NONE;
            dst_access = 0;
            break;
        default:
            break;
    }
}

// ------------------------------------------------------------------ pool -----

Scope<CommandPool> CommandPool::create(Device& device, u32 queue_family, bool transient,
                                        bool allow_individual_reset) {
    Scope<CommandPool> pool(new CommandPool());
    pool->device_ = &device;
    pool->queue_family_ = queue_family;

    VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    if (transient) info.flags |= VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (allow_individual_reset) info.flags |= VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    info.queueFamilyIndex = queue_family;
    ORE_VK_CHECK(vkCreateCommandPool(device.handle(), &info, nullptr, &pool->pool_));
    return pool;
}

CommandPool::~CommandPool() {
    if (pool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_->handle(), pool_, nullptr);
}

CommandBuffer CommandPool::allocate(VkCommandBufferLevel level, std::string_view debug_name) {
    VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    info.commandPool = pool_;
    info.level = level;
    info.commandBufferCount = 1;
    VkCommandBuffer handle = VK_NULL_HANDLE;
    ORE_VK_CHECK(vkAllocateCommandBuffers(device_->handle(), &info, &handle));
    if (!debug_name.empty()) {
        device_->set_debug_name(VK_OBJECT_TYPE_COMMAND_BUFFER, reinterpret_cast<u64>(handle), debug_name);
    }
    return CommandBuffer(*device_, pool_, handle);
}

void CommandPool::reset() { ORE_VK_CHECK(vkResetCommandPool(device_->handle(), pool_, 0)); }

// ---------------------------------------------------------- command buffer ---

CommandBuffer::CommandBuffer(Device& device, VkCommandPool pool, VkCommandBuffer handle)
    : device_(&device), pool_(pool), command_buffer_(handle) {}

CommandBuffer::CommandBuffer(CommandBuffer&& other) noexcept
    : device_(other.device_), pool_(other.pool_), command_buffer_(other.command_buffer_),
      recording_(other.recording_) {
    other.device_ = nullptr;
    other.pool_ = VK_NULL_HANDLE;
    other.command_buffer_ = VK_NULL_HANDLE;
    other.recording_ = false;
}

CommandBuffer& CommandBuffer::operator=(CommandBuffer&& other) noexcept {
    if (this == &other) return *this;
    if (command_buffer_ != VK_NULL_HANDLE && device_ != nullptr) {
        vkFreeCommandBuffers(device_->handle(), pool_, 1, &command_buffer_);
    }
    device_ = other.device_;
    pool_ = other.pool_;
    command_buffer_ = other.command_buffer_;
    recording_ = other.recording_;
    other.device_ = nullptr;
    other.pool_ = VK_NULL_HANDLE;
    other.command_buffer_ = VK_NULL_HANDLE;
    other.recording_ = false;
    return *this;
}

CommandBuffer::~CommandBuffer() {
    if (command_buffer_ != VK_NULL_HANDLE && device_ != nullptr) {
        vkFreeCommandBuffers(device_->handle(), pool_, 1, &command_buffer_);
    }
}

void CommandBuffer::begin(VkCommandBufferUsageFlags flags) {
    ORE_ASSERT(!recording_);
    VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    info.flags = flags;
    ORE_VK_CHECK(vkBeginCommandBuffer(command_buffer_, &info));
    recording_ = true;
}

void CommandBuffer::end() {
    ORE_ASSERT(recording_);
    ORE_VK_CHECK(vkEndCommandBuffer(command_buffer_));
    recording_ = false;
}

void CommandBuffer::reset(VkCommandBufferResetFlags flags) {
    ORE_VK_CHECK(vkResetCommandBuffer(command_buffer_, flags));
    recording_ = false;
}

void CommandBuffer::begin_label(std::string_view name, std::array<f32, 4> color) {
    if (device_ == nullptr || !device_->debug_utils().valid()) return;
    VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    const std::string owned(name);
    label.pLabelName = owned.c_str();
    std::copy(color.begin(), color.end(), label.color);
    device_->debug_utils().cmd_begin_label(command_buffer_, &label);
}

void CommandBuffer::end_label() {
    if (device_ == nullptr || !device_->debug_utils().valid()) return;
    device_->debug_utils().cmd_end_label(command_buffer_);
}

void CommandBuffer::insert_label(std::string_view name, std::array<f32, 4> color) {
    if (device_ == nullptr || !device_->debug_utils().valid()) return;
    VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    const std::string owned(name);
    label.pLabelName = owned.c_str();
    std::copy(color.begin(), color.end(), label.color);
    device_->debug_utils().cmd_insert_label(command_buffer_, &label);
}

void CommandBuffer::begin_rendering(const RenderingInfo& info) {
    std::vector<VkRenderingAttachmentInfo> color_attachments;
    color_attachments.reserve(info.colors.size());
    for (const ColorAttachmentInfo& color : info.colors) {
        VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = color.view;
        attachment.imageLayout = color.layout;
        attachment.loadOp = color.load_op;
        attachment.storeOp = color.store_op;
        attachment.clearValue = color.clear;
        attachment.resolveMode = color.resolve_mode;
        attachment.resolveImageView = color.resolve_view;
        attachment.resolveImageLayout = color.layout;
        color_attachments.push_back(attachment);
    }

    VkRenderingAttachmentInfo depth_attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (info.depth != nullptr) {
        depth_attachment.imageView = info.depth->view;
        depth_attachment.imageLayout = info.depth->layout;
        depth_attachment.loadOp = info.depth->load_op;
        depth_attachment.storeOp = info.depth->store_op;
        depth_attachment.clearValue = info.depth->clear;
        depth_attachment.resolveMode = VK_RESOLVE_MODE_NONE;
    }

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = info.area;
    rendering.layerCount = info.layer_count;
    rendering.colorAttachmentCount = static_cast<u32>(color_attachments.size());
    rendering.pColorAttachments = color_attachments.empty() ? nullptr : color_attachments.data();
    rendering.pDepthAttachment = info.depth != nullptr ? &depth_attachment : nullptr;
    vkCmdBeginRendering(command_buffer_, &rendering);
}

void CommandBuffer::begin_rendering(VkRect2D area, ConstSpan<ColorAttachmentInfo> colors,
                                    const DepthAttachmentInfo* depth) {
    RenderingInfo info;
    info.area = area;
    info.colors = colors;
    info.depth = depth;
    begin_rendering(info);
}

void CommandBuffer::end_rendering() { vkCmdEndRendering(command_buffer_); }

void CommandBuffer::bind_pipeline(const GraphicsPipeline& pipeline) {
    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
    current_layout_ = pipeline.layout();
    bind_point_ = VK_PIPELINE_BIND_POINT_GRAPHICS;
}

void CommandBuffer::bind_pipeline(const ComputePipeline& pipeline) {
    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.handle());
    current_layout_ = pipeline.layout();
    bind_point_ = VK_PIPELINE_BIND_POINT_COMPUTE;
}

void CommandBuffer::bind_descriptor_set(u32 set_index, const DescriptorSet& set, ConstSpan<u32> dynamic_offsets) {
    ORE_ASSERT_MSG(current_layout_ != VK_NULL_HANDLE, "bind a pipeline before binding descriptor sets");
    const VkDescriptorSet handle = set.handle();
    vkCmdBindDescriptorSets(command_buffer_, bind_point_, current_layout_, set_index, 1, &handle,
                            static_cast<u32>(dynamic_offsets.size()), dynamic_offsets.data());
}

void CommandBuffer::push_constants(const PipelineLayout& layout, VkShaderStageFlags stages, const void* data,
                                   u32 size, u32 offset) {
    vkCmdPushConstants(command_buffer_, layout.handle(), stages, offset, size, data);
}

void CommandBuffer::push_constants(VkShaderStageFlags stages, const void* data, u32 size, u32 offset) {
    ORE_ASSERT_MSG(current_layout_ != VK_NULL_HANDLE, "bind a pipeline before pushing constants");
    vkCmdPushConstants(command_buffer_, current_layout_, stages, offset, size, data);
}

void CommandBuffer::set_viewport(f32 x, f32 y, f32 width, f32 height, f32 min_depth, f32 max_depth) {
    VkViewport viewport{};
    viewport.x = x;
    viewport.y = y;
    viewport.width = width;
    viewport.height = height;
    viewport.minDepth = min_depth;
    viewport.maxDepth = max_depth;
    vkCmdSetViewport(command_buffer_, 0, 1, &viewport);
}

void CommandBuffer::set_viewport(f32 width, f32 height) { set_viewport(0.0f, 0.0f, width, height); }

void CommandBuffer::set_viewport_flipped(f32 width, f32 height) { set_viewport(0.0f, height, width, -height); }

void CommandBuffer::set_scissor(i32 x, i32 y, u32 width, u32 height) {
    VkRect2D rect{};
    rect.offset = {x, y};
    rect.extent = {width, height};
    vkCmdSetScissor(command_buffer_, 0, 1, &rect);
}

void CommandBuffer::set_scissor(VkRect2D rect) { vkCmdSetScissor(command_buffer_, 0, 1, &rect); }

void CommandBuffer::set_scissor_full(f32 width, f32 height) {
    set_scissor(0, 0, static_cast<u32>(width), static_cast<u32>(height));
}

void CommandBuffer::bind_vertex_buffer(u32 binding, const Buffer& buffer, u64 offset) {
    const VkBuffer handle = buffer.handle();
    vkCmdBindVertexBuffers(command_buffer_, binding, 1, &handle, &offset);
}

void CommandBuffer::bind_vertex_buffers(u32 first_binding, ConstSpan<VkBuffer> buffers, ConstSpan<u64> offsets) {
    ORE_ASSERT(buffers.size() == offsets.size());
    vkCmdBindVertexBuffers(command_buffer_, first_binding, static_cast<u32>(buffers.size()), buffers.data(),
                           offsets.data());
}

void CommandBuffer::bind_index_buffer(const Buffer& buffer, VkIndexType type, u64 offset) {
    vkCmdBindIndexBuffer(command_buffer_, buffer.handle(), offset, type);
}

void CommandBuffer::draw(u32 vertex_count, u32 instance_count, u32 first_vertex, u32 first_instance) {
    vkCmdDraw(command_buffer_, vertex_count, instance_count, first_vertex, first_instance);
}

void CommandBuffer::draw_indexed(u32 index_count, u32 instance_count, u32 first_index, i32 vertex_offset,
                                 u32 first_instance) {
    vkCmdDrawIndexed(command_buffer_, index_count, instance_count, first_index, vertex_offset, first_instance);
}

void CommandBuffer::dispatch(u32 group_count_x, u32 group_count_y, u32 group_count_z) {
    vkCmdDispatch(command_buffer_, group_count_x, group_count_y, group_count_z);
}

void CommandBuffer::copy_buffer(VkBuffer src, VkBuffer dst, ConstSpan<VkBufferCopy> regions) {
    vkCmdCopyBuffer(command_buffer_, src, dst, static_cast<u32>(regions.size()), regions.data());
}

void CommandBuffer::copy_buffer(const Buffer& src, const Buffer& dst, u64 size, u64 src_offset, u64 dst_offset) {
    VkBufferCopy region{};
    region.srcOffset = src_offset;
    region.dstOffset = dst_offset;
    region.size = size;
    vkCmdCopyBuffer(command_buffer_, src.handle(), dst.handle(), 1, &region);
}

void CommandBuffer::copy_buffer_to_image(VkBuffer src, VkImage dst, VkImageLayout dst_layout,
                                         ConstSpan<VkBufferImageCopy> regions) {
    vkCmdCopyBufferToImage(command_buffer_, src, dst, dst_layout, static_cast<u32>(regions.size()), regions.data());
}

void CommandBuffer::copy_image_to_buffer(VkImage src, VkImageLayout src_layout, VkBuffer dst,
                                         ConstSpan<VkBufferImageCopy> regions) {
    vkCmdCopyImageToBuffer(command_buffer_, src, src_layout, dst, static_cast<u32>(regions.size()), regions.data());
}

void CommandBuffer::blit_image(VkImage src, VkImageLayout src_layout, VkImage dst, VkImageLayout dst_layout,
                               ConstSpan<VkImageBlit> regions, VkFilter filter) {
    vkCmdBlitImage(command_buffer_, src, src_layout, dst, dst_layout, static_cast<u32>(regions.size()), regions.data(),
                   filter);
}

void CommandBuffer::fill_buffer(const Buffer& dst, u32 value, u64 offset, u64 size) {
    const u64 range = size == VK_WHOLE_SIZE ? dst.size() - offset : size;
    vkCmdFillBuffer(command_buffer_, dst.handle(), offset, range, value);
}

void CommandBuffer::image_barrier(const ImageBarrier& barrier) {
    VkImageMemoryBarrier2 memory_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    memory_barrier.srcStageMask = barrier.src_stage;
    memory_barrier.srcAccessMask = barrier.src_access;
    memory_barrier.dstStageMask = barrier.dst_stage;
    memory_barrier.dstAccessMask = barrier.dst_access;
    memory_barrier.oldLayout = barrier.old_layout;
    memory_barrier.newLayout = barrier.new_layout;
    memory_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    memory_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    memory_barrier.image = barrier.image;
    memory_barrier.subresourceRange = barrier.range;

    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &memory_barrier;
    vkCmdPipelineBarrier2(command_buffer_, &dependency);
}

void CommandBuffer::transition_image(VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                                     VkImageSubresourceRange range) {
    if (old_layout == new_layout) return;
    ImageBarrier barrier;
    barrier.image = image;
    barrier.old_layout = old_layout;
    barrier.new_layout = new_layout;
    barrier.range = range;
    default_barrier_stages(old_layout, new_layout, barrier.src_stage, barrier.src_access, barrier.dst_stage,
                           barrier.dst_access);
    image_barrier(barrier);
}

void CommandBuffer::memory_barrier(VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                                   VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(command_buffer_, &dependency);
}

// -------------------------------------------------------- immediate commands --

Scope<ImmediateCommands> ImmediateCommands::create(Device& device, QueueKind kind, u32 command_buffer_count) {
    Scope<ImmediateCommands> immediate(new ImmediateCommands());
    immediate->device_ = &device;
    immediate->queue_ = device.queue(kind);
    immediate->queue_family_ = device.queue_family(kind);
    if (immediate->queue_ == VK_NULL_HANDLE) {
        ORE_ERROR("ImmediateCommands: the device exposes no queue for that role");
        return nullptr;
    }
    immediate->pool_ = CommandPool::create(device, immediate->queue_family_, true, false);
    immediate->buffers_.reserve(command_buffer_count);
    for (u32 i = 0; i < std::max(1u, command_buffer_count); ++i) {
        immediate->buffers_.push_back(immediate->pool_->allocate(VK_COMMAND_BUFFER_LEVEL_PRIMARY, "immediate"));
    }
    immediate->timeline_ = TimelineSemaphore::create(device);
    return immediate;
}

ImmediateCommands::~ImmediateCommands() = default;

CommandBuffer& ImmediateCommands::begin() {
    CommandBuffer& buffer = buffers_[cursor_ % buffers_.size()];
    buffer.reset();
    buffer.begin(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    return buffer;
}

u64 ImmediateCommands::submit(ConstSpan<VkSemaphore> wait_semaphores, ConstSpan<VkPipelineStageFlags> wait_stages,
                              ConstSpan<VkSemaphore> signal_semaphores) {
    CommandBuffer& buffer = buffers_[cursor_ % buffers_.size()];
    ORE_ASSERT(buffer.recording());
    buffer.end();

    const u64 value = timeline_->value() + 1;

    std::vector<VkSemaphoreSubmitInfo> waits;
    waits.reserve(wait_semaphores.size());
    for (usize i = 0; i < wait_semaphores.size(); ++i) {
        VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        wait.semaphore = wait_semaphores[i];
        wait.stageMask = i < wait_stages.size() ? static_cast<VkPipelineStageFlags2>(wait_stages[i])
                                                : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        waits.push_back(wait);
    }

    // The internal timeline semaphore always signals; requested binary semaphores are signalled too.
    std::vector<VkSemaphoreSubmitInfo> signals;
    signals.reserve(signal_semaphores.size() + 1);
    for (VkSemaphore semaphore : signal_semaphores) {
        VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signal.semaphore = semaphore;
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        signals.push_back(signal);
    }
    VkSemaphoreSubmitInfo timeline_signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    timeline_signal.semaphore = timeline_->handle();
    timeline_signal.value = value;
    timeline_signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signals.push_back(timeline_signal);

    const VkCommandBuffer handle = buffer.handle();
    VkCommandBufferSubmitInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    command_info.commandBuffer = handle;

    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = static_cast<u32>(waits.size());
    submit.pWaitSemaphoreInfos = waits.empty() ? nullptr : waits.data();
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &command_info;
    submit.signalSemaphoreInfoCount = static_cast<u32>(signals.size());
    submit.pSignalSemaphoreInfos = signals.data();
    ORE_VK_CHECK(vkQueueSubmit2(queue_, 1, &submit, VK_NULL_HANDLE));

    last_value_ = value;
    ++cursor_;
    return value;
}

void ImmediateCommands::wait(u64 value) {
    const u64 target = value == 0 ? last_value_ : value;
    if (target == 0) return;
    timeline_->wait_host(target);
}

void ImmediateCommands::run(const std::function<void(CommandBuffer&)>& fn) {
    CommandBuffer& buffer = begin();
    fn(buffer);
    wait(submit());
}

} // namespace ore::rhi
