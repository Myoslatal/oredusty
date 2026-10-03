// Ore framework - command pools, command buffers and one-shot immediate submission.
//
// Rendering uses dynamic rendering (Vulkan 1.3 core), so a "render pass" is just
// CommandBuffer::begin_rendering() / end_rendering() with explicit attachments.
#pragma once

#include <ore/core/types.h>
#include <ore/rhi/device.h>
#include <ore/rhi/sync.h>

#include <array>
#include <functional>
#include <string_view>

namespace ore::rhi {

class Buffer;
class Texture;
class CommandBuffer;
class DescriptorSet;
class PipelineLayout;
class GraphicsPipeline;
class ComputePipeline;

/// Colour attachment description for dynamic rendering.
struct ColorAttachmentInfo {
    VkImageView view = VK_NULL_HANDLE;
    VkImageView resolve_view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentLoadOp load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
    VkAttachmentStoreOp store_op = VK_ATTACHMENT_STORE_OP_STORE;
    VkResolveModeFlagBits resolve_mode = VK_RESOLVE_MODE_NONE;
    VkClearValue clear{};
};

struct DepthAttachmentInfo {
    VkImageView view = VK_NULL_HANDLE;
    VkImageView resolve_view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    VkAttachmentLoadOp load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
    VkAttachmentStoreOp store_op = VK_ATTACHMENT_STORE_OP_STORE;
    VkAttachmentLoadOp stencil_load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    VkAttachmentStoreOp stencil_store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    VkClearValue clear{};
};

struct RenderingInfo {
    VkRect2D area{};
    ConstSpan<ColorAttachmentInfo> colors{};
    const DepthAttachmentInfo* depth = nullptr;
    u32 layer_count = 1;
};

struct ImageBarrier {
    VkImage image = VK_NULL_HANDLE;
    VkImageLayout old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout new_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageSubresourceRange range{};
    VkPipelineStageFlags2 src_stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkAccessFlags2 src_access = VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkPipelineStageFlags2 dst_stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkAccessFlags2 dst_access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
};

/// Fills \p src_stage/src_access/dst_stage/dst_access with sensible defaults for a layout pair.
void default_barrier_stages(VkImageLayout old_layout, VkImageLayout new_layout, VkPipelineStageFlags2& src_stage,
                            VkAccessFlags2& src_access, VkPipelineStageFlags2& dst_stage, VkAccessFlags2& dst_access);

[[nodiscard]] constexpr VkImageSubresourceRange subresource_range(VkImageAspectFlags aspect, u32 mip_levels = 1,
                                                                 u32 layers = 1, u32 base_mip = 0,
                                                                 u32 base_layer = 0) {
    VkImageSubresourceRange range{};
    range.aspectMask = aspect;
    range.baseMipLevel = base_mip;
    range.levelCount = mip_levels;
    range.baseArrayLayer = base_layer;
    range.layerCount = layers;
    return range;
}

class CommandPool {
public:
    [[nodiscard]] static Scope<CommandPool> create(Device& device, u32 queue_family, bool transient = false,
                                                   bool allow_individual_reset = false);
    ~CommandPool();
    ORE_NON_MOVABLE(CommandPool);

    [[nodiscard]] CommandBuffer allocate(VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                         std::string_view debug_name = {});
    /// Resets the pool, which implicitly resets every command buffer allocated from it.
    void reset();
    [[nodiscard]] VkCommandPool handle() const { return pool_; }
    [[nodiscard]] u32 queue_family() const { return queue_family_; }
    [[nodiscard]] Device& device() const { return *device_; }

private:
    CommandPool() = default;
    Device* device_ = nullptr;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    u32 queue_family_ = kInvalidIndex;
};

class CommandBuffer {
public:
    CommandBuffer() = default;
    CommandBuffer(Device& device, VkCommandPool pool, VkCommandBuffer handle);
    ~CommandBuffer();
    ORE_NON_COPYABLE(CommandBuffer);
    CommandBuffer(CommandBuffer&& other) noexcept;
    CommandBuffer& operator=(CommandBuffer&& other) noexcept;

    [[nodiscard]] VkCommandBuffer handle() const { return command_buffer_; }
    [[nodiscard]] bool valid() const { return command_buffer_ != VK_NULL_HANDLE; }
    [[nodiscard]] bool recording() const { return recording_; }

    void begin(VkCommandBufferUsageFlags flags = 0);
    void end();
    void reset(VkCommandBufferResetFlags flags = 0);

    // Debug labels (no-ops without VK_EXT_debug_utils).
    void begin_label(std::string_view name, std::array<f32, 4> color = {0.4f, 0.7f, 1.0f, 1.0f});
    void end_label();
    void insert_label(std::string_view name, std::array<f32, 4> color = {0.4f, 0.7f, 1.0f, 1.0f});

    // --- dynamic rendering -------------------------------------------------
    void begin_rendering(const RenderingInfo& info);
    void begin_rendering(VkRect2D area, ConstSpan<ColorAttachmentInfo> colors,
                         const DepthAttachmentInfo* depth = nullptr);
    void end_rendering();

    // --- pipeline state ----------------------------------------------------
    void bind_pipeline(const GraphicsPipeline& pipeline);
    void bind_pipeline(const ComputePipeline& pipeline);
    /// Uses the pipeline layout of the most recently bound pipeline.
    void bind_descriptor_set(u32 set_index, const DescriptorSet& set, ConstSpan<u32> dynamic_offsets = {});
    void push_constants(const PipelineLayout& layout, VkShaderStageFlags stages, const void* data, u32 size,
                        u32 offset = 0);
    /// Uses the pipeline layout of the most recently bound pipeline.
    void push_constants(VkShaderStageFlags stages, const void* data, u32 size, u32 offset = 0);
    template <class T>
    void push_constants(const PipelineLayout& layout, VkShaderStageFlags stages, const T& value, u32 offset = 0) {
        push_constants(layout, stages, &value, static_cast<u32>(sizeof(T)), offset);
    }
    template <class T>
    void push_constants(VkShaderStageFlags stages, const T& value, u32 offset = 0) {
        push_constants(stages, &value, static_cast<u32>(sizeof(T)), offset);
    }

    void set_viewport(f32 x, f32 y, f32 width, f32 height, f32 min_depth = 0.0f, f32 max_depth = 1.0f);
    /// Full-target viewport using Vulkan's Y-down clip space.
    void set_viewport(f32 width, f32 height);
    /// Full-target viewport with a negative height, for shaders written for Y-up clip space.
    void set_viewport_flipped(f32 width, f32 height);
    void set_scissor(i32 x, i32 y, u32 width, u32 height);
    void set_scissor(VkRect2D rect);
    void set_scissor_full(f32 width, f32 height);

    // --- geometry ----------------------------------------------------------
    void bind_vertex_buffer(u32 binding, const Buffer& buffer, u64 offset = 0);
    void bind_vertex_buffers(u32 first_binding, ConstSpan<VkBuffer> buffers, ConstSpan<u64> offsets);
    void bind_index_buffer(const Buffer& buffer, VkIndexType type = VK_INDEX_TYPE_UINT32, u64 offset = 0);
    void draw(u32 vertex_count, u32 instance_count = 1, u32 first_vertex = 0, u32 first_instance = 0);
    void draw_indexed(u32 index_count, u32 instance_count = 1, u32 first_index = 0, i32 vertex_offset = 0,
                      u32 first_instance = 0);
    void dispatch(u32 group_count_x, u32 group_count_y = 1, u32 group_count_z = 1);

    // --- transfers ---------------------------------------------------------
    void copy_buffer(VkBuffer src, VkBuffer dst, ConstSpan<VkBufferCopy> regions);
    void copy_buffer(const Buffer& src, const Buffer& dst, u64 size, u64 src_offset = 0, u64 dst_offset = 0);
    void copy_buffer_to_image(VkBuffer src, VkImage dst, VkImageLayout dst_layout,
                              ConstSpan<VkBufferImageCopy> regions);
    void copy_image_to_buffer(VkImage src, VkImageLayout src_layout, VkBuffer dst,
                              ConstSpan<VkBufferImageCopy> regions);
    void blit_image(VkImage src, VkImageLayout src_layout, VkImage dst, VkImageLayout dst_layout,
                    ConstSpan<VkImageBlit> regions, VkFilter filter = VK_FILTER_LINEAR);
    void fill_buffer(const Buffer& dst, u32 value, u64 offset = 0, u64 size = VK_WHOLE_SIZE);

    // --- synchronisation (Vulkan 1.3 synchronization2) ---------------------
    void image_barrier(const ImageBarrier& barrier);
    void transition_image(VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                          VkImageSubresourceRange range);
    void memory_barrier(VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                        VkAccessFlags2 dst_access);

private:
    Device* device_ = nullptr;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    VkPipelineLayout current_layout_ = VK_NULL_HANDLE;
    VkPipelineBindPoint bind_point_ = VK_PIPELINE_BIND_POINT_GRAPHICS;
    bool recording_ = false;
};

/// Records a single command buffer, submits it and blocks until the GPU is done.
/// Ideal for uploads, one-off computes and debug readbacks.
class ImmediateCommands {
public:
    [[nodiscard]] static Scope<ImmediateCommands> create(Device& device,
                                                         QueueKind kind = QueueKind::Graphics,
                                                         u32 command_buffer_count = 1);
    ~ImmediateCommands();
    ORE_NON_MOVABLE(ImmediateCommands);

    /// begin() + \p fn + submit() + wait().
    void run(const std::function<void(CommandBuffer&)>& fn);

    /// Records into the next command buffer; call submit() afterwards.
    [[nodiscard]] CommandBuffer& begin();
    /// Submits the current command buffer and returns the timeline value that signals its completion.
    u64 submit(ConstSpan<VkSemaphore> wait_semaphores = {}, ConstSpan<VkPipelineStageFlags> wait_stages = {},
               ConstSpan<VkSemaphore> signal_semaphores = {});
    /// Host-waits for \p value (0 waits for the most recent submission).
    void wait(u64 value = 0);

    [[nodiscard]] VkQueue queue() const { return queue_; }
    [[nodiscard]] u32 queue_family() const { return queue_family_; }
    [[nodiscard]] TimelineSemaphore& timeline() const { return *timeline_; }
    [[nodiscard]] Device& device() const { return *device_; }

private:
    ImmediateCommands() = default;

    Device* device_ = nullptr;
    VkQueue queue_ = VK_NULL_HANDLE;
    u32 queue_family_ = kInvalidIndex;
    Scope<CommandPool> pool_;
    std::vector<CommandBuffer> buffers_;
    Scope<TimelineSemaphore> timeline_;
    u32 cursor_ = 0;
    u64 last_value_ = 0;
};

} // namespace ore::rhi
