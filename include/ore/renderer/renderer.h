// Ore framework - the frame renderer.
//
// Owns the presentation target (swapchain or offscreen), the frames-in-flight resources and the
// per-frame upload ring, and drives the classic "wait / acquire / record / submit / present" loop.
#pragma once

#include <ore/core/types.h>
#include <ore/renderer/upload_ring.h>
#include <ore/rhi/commands.h>
#include <ore/rhi/context.h>
#include <ore/rhi/descriptor.h>
#include <ore/rhi/render_target.h>
#include <ore/rhi/swapchain.h>

#include <string>
#include <vector>

namespace ore {

/// Per-frame counters that applications may fill in for statistics.
struct FrameCounters {
    u32 draw_calls = 0;
    u64 vertices = 0;
    u64 triangles = 0;
};

struct RenderFrame {
    u32 slot = 0;                       ///< frames-in-flight slot
    u64 number = 0;                     ///< monotonic frame index
    f32 delta_seconds = 0.0f;
    rhi::CommandBuffer* cmd = nullptr;  ///< recording command buffer for this frame
    UploadRing* ring = nullptr;         ///< dynamic data ring for this frame
    rhi::RenderTarget* target = nullptr;
    u32 width = 0;
    u32 height = 0;
    FrameCounters counters{};

    [[nodiscard]] f32 aspect_ratio() const {
        return height > 0 ? static_cast<f32>(width) / static_cast<f32>(height) : 1.0f;
    }
};

struct RenderStats {
    u64 frames = 0;
    f32 cpu_frame_ms = 0.0f;
    f32 gpu_wait_ms = 0.0f;
    FrameCounters last_frame{};
};

class Renderer {
public:
    struct Desc {
        rhi::GraphicsContext* context = nullptr;
        /// Presentation surface; leave VK_NULL_HANDLE for a headless/offscreen renderer.
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        u32 width = 1280;
        u32 height = 720;
        bool vsync = true;
        /// VK_FORMAT_UNDEFINED selects the surface format (or R8G8B8A8_UNORM offscreen).
        VkFormat color_format = VK_FORMAT_UNDEFINED;
        VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
        u32 frames_in_flight = 2;
        u32 swapchain_image_count = 3;
        /// Lets the offscreen colour image be sampled (post processing, tests).
        bool offscreen_sampled = false;
        u64 upload_segment_size = 1ull << 20;
        std::string debug_name = "renderer";
    };

    [[nodiscard]] static Scope<Renderer> create(const Desc& desc);
    ~Renderer();
    ORE_NON_MOVABLE(Renderer);

    // --- frame loop --------------------------------------------------------
    /// Waits for the slot, acquires the next image and starts recording. The reference stays
    /// valid until the next begin_frame() call.
    RenderFrame& begin_frame(f32 delta_seconds = 0.0f);
    /// Ends recording, submits and presents.
    void end_frame();

    // --- passes ------------------------------------------------------------
    /// Begins dynamic rendering on the current target with the given clear values.
    void begin_pass(const VkClearValue& color_clear, f32 depth_clear = 1.0f, u32 stencil_clear = 0);
    /// Begins dynamic rendering on the current target, preserving its contents.
    void begin_pass_load();
    /// Begins a pass on an explicit target (shadow maps, reflection probes, post processing).
    void begin_pass(rhi::RenderTarget& target, const VkClearValue& color_clear, f32 depth_clear = 1.0f);
    void begin_pass_load(rhi::RenderTarget& target);
    /// Ends the pass started by begin_pass().
    void end_pass();
    /// Ends a pass on an explicit target.
    void end_pass(rhi::RenderTarget& target);

    // --- per-frame resources ----------------------------------------------
    /// Allocates a descriptor set from this frame's pool (2 frames in flight => 2 sets).
    [[nodiscard]] rhi::DescriptorSet allocate_descriptor_set(const rhi::DescriptorSetLayout& layout,
                                                            std::string_view debug_name = {});
    [[nodiscard]] UploadRing& ring() const { return *ring_; }

    // --- screenshots -------------------------------------------------------
    /// Records a copy of the current frame's colour image; call save_screenshot() after the frame.
    void request_screenshot(std::string path);
    [[nodiscard]] bool has_pending_screenshot() const { return !screenshot_path_.empty(); }
    /// Waits for the GPU, writes the PNG and clears the request.
    bool save_screenshot();

    // --- target management -------------------------------------------------
    void resize(u32 width, u32 height, bool vsync = true);
    [[nodiscard]] bool resize_pending() const { return resize_pending_; }
    void wait_idle();

    [[nodiscard]] bool headless() const { return swapchain_ == nullptr; }
    [[nodiscard]] u32 width() const { return width_; }
    [[nodiscard]] u32 height() const { return height_; }
    [[nodiscard]] f32 aspect_ratio() const {
        return height_ > 0 ? static_cast<f32>(width_) / static_cast<f32>(height_) : 1.0f;
    }
    [[nodiscard]] VkFormat color_format() const { return color_format_; }
    [[nodiscard]] VkFormat depth_format() const { return depth_format_; }
    [[nodiscard]] rhi::GraphicsContext& context() const { return *context_; }
    [[nodiscard]] rhi::RenderTarget& target() const { return *current_target_; }
    [[nodiscard]] rhi::Swapchain* swapchain() const { return swapchain_.get(); }
    [[nodiscard]] const RenderFrame& frame() const { return frame_; }
    [[nodiscard]] const RenderStats& stats() const { return stats_; }
    [[nodiscard]] std::string dump_stats() const;

private:
    Renderer() = default;

    struct FrameResources {
        Scope<rhi::CommandPool> command_pool;
        rhi::CommandBuffer command_buffer;
        Scope<rhi::Fence> fence;
        Scope<rhi::Semaphore> image_available;
        Scope<rhi::DescriptorPool> descriptor_pool;
        u32 descriptor_capacity_sets = 0;
        std::vector<VkDescriptorPoolSize> descriptor_capacity_sizes;
        u32 used_sets = 0;
        std::vector<VkDescriptorPoolSize> used_sizes;
    };

    [[nodiscard]] bool create_targets();
    void destroy_targets();
    void recreate_swapchain();
    void report_frame_stats();

    rhi::GraphicsContext* context_ = nullptr;
    Desc desc_{};
    u32 width_ = 0;
    u32 height_ = 0;
    VkFormat color_format_ = VK_FORMAT_UNDEFINED;
    VkFormat depth_format_ = VK_FORMAT_UNDEFINED;

    std::vector<FrameResources> frames_;
    u32 current_slot_ = 0;
    u64 frame_number_ = 0;

    Scope<rhi::Swapchain> swapchain_;
    std::vector<Scope<rhi::RenderTarget>> present_targets_;
    Scope<rhi::RenderTarget> offscreen_target_;
    std::vector<Scope<rhi::Semaphore>> render_finished_;
    rhi::RenderTarget* current_target_ = nullptr;
    u32 current_image_index_ = 0;

    Scope<UploadRing> ring_;
    RenderFrame frame_{};
    RenderStats stats_{};

    bool resize_pending_ = false;
    u32 pending_width_ = 0;
    u32 pending_height_ = 0;
    bool pending_vsync_ = true;

    std::string screenshot_path_;
    bool screenshot_recorded_ = false;
    Scope<rhi::Buffer> readback_buffer_;
    u64 readback_capacity_ = 0;
    u32 readback_width_ = 0;
    u32 readback_height_ = 0;
    bool needs_pool_rebuild_ = false;
    std::vector<VkDescriptorPoolSize> last_frame_sizes_;
    u32 last_frame_sets_ = 0;
};

} // namespace ore
