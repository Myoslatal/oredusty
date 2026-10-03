#include <ore/renderer/renderer.h>

#include <ore/core/assert.h>
#include <ore/core/file.h>
#include <ore/core/log.h>
#include <ore/core/time.h>

#include <algorithm>
#include <format>

namespace ore {
namespace {

void accumulate_sizes(std::vector<VkDescriptorPoolSize>& target, ConstSpan<VkDescriptorPoolSize> source) {
    for (const VkDescriptorPoolSize& entry : source) {
        const auto it = std::find_if(target.begin(), target.end(),
                                     [&entry](const VkDescriptorPoolSize& existing) {
                                         return existing.type == entry.type;
                                     });
        if (it != target.end()) {
            it->descriptorCount += entry.descriptorCount;
        } else {
            target.push_back(entry);
        }
    }
}

} // namespace

Scope<Renderer> Renderer::create(const Desc& desc) {
    if (desc.context == nullptr) {
        ORE_ERROR("Renderer::create: a GraphicsContext is required");
        return nullptr;
    }
    if (!desc.context->headless() && desc.surface == VK_NULL_HANDLE) {
        ORE_ERROR("Renderer::create: a surface is required for windowed rendering");
        return nullptr;
    }

    Scope<Renderer> renderer(new Renderer());
    renderer->context_ = desc.context;
    renderer->desc_ = desc;
    renderer->width_ = std::max(desc.width, 1u);
    renderer->height_ = std::max(desc.height, 1u);
    renderer->depth_format_ = desc.depth_format;

    renderer->frames_.resize(std::max(desc.frames_in_flight, 1u));
    renderer->ring_ = UploadRing::create(desc.context->allocator(), desc.upload_segment_size,
                                         static_cast<u32>(renderer->frames_.size()),
                                         desc.debug_name + ".ring");
    if (renderer->ring_ == nullptr) return nullptr;

    if (renderer->create_targets()) return nullptr;
    if (renderer->swapchain_ == nullptr && renderer->offscreen_target_ == nullptr) return nullptr;

    for (usize i = 0; i < renderer->frames_.size(); ++i) {
        FrameResources& resources = renderer->frames_[i];
        const u32 family = desc.context->device().queue_family(rhi::QueueKind::Graphics);
        resources.command_pool = rhi::CommandPool::create(desc.context->device(), family, false, false);
        resources.command_buffer = resources.command_pool->allocate(VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                                   "frame.command_buffer");
        // Signaled on creation: begin_frame() waits on this fence before the first submit ever
        // happens, so an unsignaled fence would deadlock the very first frame.
        resources.fence = rhi::Fence::create(desc.context->device(), true);
        resources.image_available = rhi::Semaphore::create(desc.context->device());
        resources.descriptor_pool = rhi::DescriptorPool::create(desc.context->device(),
                                                               std::vector<VkDescriptorPoolSize>{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 64},
                                                                                                 {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64},
                                                                                                 {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64}},
                                                               64);
        resources.descriptor_capacity_sets = 64;
    }
    return renderer;
}

Renderer::~Renderer() {
    if (context_ != nullptr) context_->device().wait_idle();
    render_finished_.clear();
    present_targets_.clear();
    offscreen_target_.reset();
    swapchain_.reset();
    frames_.clear();
    ring_.reset();
    readback_buffer_.reset();
}

bool Renderer::create_targets() {
    rhi::GraphicsContext& context = *context_;
    const bool windowed = desc_.surface != VK_NULL_HANDLE;

    if (windowed) {
        rhi::SwapchainDesc swapchain_desc;
        swapchain_desc.width = width_;
        swapchain_desc.height = height_;
        swapchain_desc.vsync = desc_.vsync;
        swapchain_desc.preferred_format = desc_.color_format;
        swapchain_desc.preferred_image_count = desc_.swapchain_image_count;
        swapchain_desc.debug_name = desc_.debug_name;
        swapchain_ = rhi::Swapchain::create(context.device(), desc_.surface, swapchain_desc);
        if (swapchain_ == nullptr) return true;

        width_ = swapchain_->extent().width;
        height_ = swapchain_->extent().height;
        color_format_ = swapchain_->format();

        present_targets_.clear();
        present_targets_.reserve(swapchain_->image_count());
        for (u32 i = 0; i < swapchain_->image_count(); ++i) {
            rhi::RenderTargetDesc target_desc;
            target_desc.width = width_;
            target_desc.height = height_;
            target_desc.color_format = color_format_;
            target_desc.depth_format = depth_format_;
            target_desc.final_color_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            target_desc.debug_name = std::format("{}.swapchain[{}]", desc_.debug_name, i);
            auto target = rhi::RenderTarget::create_external(context.allocator(), target_desc,
                                                            swapchain_->image(i), swapchain_->view(i));
            if (target == nullptr) return true;
            present_targets_.push_back(std::move(target));
        }

        render_finished_.clear();
        render_finished_.reserve(swapchain_->image_count());
        for (u32 i = 0; i < swapchain_->image_count(); ++i) {
            render_finished_.push_back(rhi::Semaphore::create(context.device()));
        }
        return false;
    }

    rhi::RenderTargetDesc target_desc;
    target_desc.width = width_;
    target_desc.height = height_;
    target_desc.color_format = desc_.color_format == VK_FORMAT_UNDEFINED ? VK_FORMAT_R8G8B8A8_UNORM
                                                                        : desc_.color_format;
    target_desc.depth_format = depth_format_;
    target_desc.sampled = desc_.offscreen_sampled;
    target_desc.final_color_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    target_desc.debug_name = desc_.debug_name + ".offscreen";
    offscreen_target_ = rhi::RenderTarget::create(context.allocator(), target_desc);
    if (offscreen_target_ == nullptr) return true;
    color_format_ = target_desc.color_format;
    ORE_INFO("offscreen renderer: {}x{} {}", width_, height_, rhi::format_string(color_format_));
    return false;
}

void Renderer::destroy_targets() {
    present_targets_.clear();
    render_finished_.clear();
    offscreen_target_.reset();
    swapchain_.reset();
}

void Renderer::recreate_swapchain() {
    context_->wait_idle();
    destroy_targets();
    resize_pending_ = false;
    if (create_targets()) {
        ORE_ERROR("renderer: failed to recreate the presentation targets");
    }
    if (current_target_ != nullptr) {
        current_target_ = nullptr; // resolved again in begin_frame()
    }
}

RenderFrame& Renderer::begin_frame(f32 delta_seconds) {
    if (resize_pending_) recreate_swapchain();

    FrameResources& resources = frames_[current_slot_];
    const f64 wait_start = Clock::now_seconds();
    resources.fence->wait();
    stats_.gpu_wait_ms = static_cast<f32>((Clock::now_seconds() - wait_start) * 1000.0);
    resources.fence->reset();
    resources.command_pool->reset();

    // Descriptor pool: grow when the previous frame needed more than the current capacity.
    if (needs_pool_rebuild_ || last_frame_sets_ > resources.descriptor_capacity_sets) {
        std::vector<VkDescriptorPoolSize> sizes = last_frame_sizes_;
        if (sizes.empty()) sizes = resources.descriptor_capacity_sizes;
        for (VkDescriptorPoolSize& size : sizes) size.descriptorCount = std::max(size.descriptorCount * 2, 64u);
        const u32 sets = std::max(last_frame_sets_ * 2, 64u);
        resources.descriptor_pool = rhi::DescriptorPool::create(context_->device(), sizes, sets);
        resources.descriptor_capacity_sets = sets;
        resources.descriptor_capacity_sizes = sizes;
        needs_pool_rebuild_ = false;
    } else {
        resources.descriptor_pool->reset();
    }
    resources.used_sets = 0;
    resources.used_sizes.clear();

    ring_->begin_frame(current_slot_);

    if (swapchain_ != nullptr) {
        bool out_of_date = false;
        if (!swapchain_->acquire_next_image(*resources.image_available, current_image_index_, out_of_date)) {
            if (out_of_date) {
                recreate_swapchain();
                return begin_frame(delta_seconds);
            }
            ORE_WARN("renderer: image acquisition timed out");
        } else if (out_of_date) {
            resize_pending_ = true;
        }
        current_target_ = present_targets_[current_image_index_].get();
    } else {
        current_target_ = offscreen_target_.get();
    }

    resources.command_buffer.begin(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    frame_.slot = current_slot_;
    frame_.number = frame_number_;
    frame_.delta_seconds = delta_seconds;
    frame_.cmd = &resources.command_buffer;
    frame_.ring = ring_.get();
    frame_.target = current_target_;
    frame_.width = width_;
    frame_.height = height_;
    frame_.counters = {};
    screenshot_recorded_ = false;
    return frame_;
}

void Renderer::begin_pass(const VkClearValue& color_clear, f32 depth_clear, u32 stencil_clear) {
    ORE_ASSERT(current_target_ != nullptr);
    current_target_->begin(*frame_.cmd, color_clear, depth_clear, stencil_clear);
    // Viewport and scissor are dynamic state: leaving them unset makes every draw disappear, which
    // is a silent failure that costs hours. A pass starts covering its whole target; a caller that
    // wants a different rectangle sets it again after this call.
    set_default_viewport(*current_target_);
}

void Renderer::begin_pass_load() {
    ORE_ASSERT(current_target_ != nullptr);
    current_target_->begin_load(*frame_.cmd);
    set_default_viewport(*current_target_);
}

void Renderer::begin_pass(rhi::RenderTarget& target, const VkClearValue& color_clear, f32 depth_clear) {
    target.begin(*frame_.cmd, color_clear, depth_clear);
    set_default_viewport(target);
}

void Renderer::set_default_viewport(const rhi::RenderTarget& target) {
    frame_.cmd->set_viewport(static_cast<f32>(target.width()), static_cast<f32>(target.height()));
    frame_.cmd->set_scissor_full(static_cast<f32>(target.width()), static_cast<f32>(target.height()));
}

void Renderer::begin_pass_load(rhi::RenderTarget& target) { target.begin_load(*frame_.cmd); }

void Renderer::end_pass() {
    ORE_ASSERT(current_target_ != nullptr);
    current_target_->end(*frame_.cmd);
}

void Renderer::end_pass(rhi::RenderTarget& target) { target.end(*frame_.cmd); }

rhi::DescriptorSet Renderer::allocate_descriptor_set(const rhi::DescriptorSetLayout& layout,
                                                     std::string_view debug_name) {
    FrameResources& resources = frames_[current_slot_];
    const VkDescriptorSet set = resources.descriptor_pool->allocate(layout, debug_name);
    ++resources.used_sets;
    accumulate_sizes(resources.used_sizes, layout.pool_sizes(1));
    if (set == VK_NULL_HANDLE) {
        ORE_ERROR("renderer: descriptor pool exhausted ({} sets used); the pool grows automatically next frame",
                  resources.used_sets);
        needs_pool_rebuild_ = true;
    }
    return rhi::DescriptorSet(context_->device(), set, layout);
}

void Renderer::end_frame() {
    FrameResources& resources = frames_[current_slot_];
    rhi::CommandBuffer& cmd = resources.command_buffer;

    // Optional screen capture: copy the target colour image into the readback buffer first.
    if (!screenshot_path_.empty() && current_target_ != nullptr) {
        const u64 required = static_cast<u64>(width_) * height_ * 4u;
        if (readback_capacity_ < required) {
            rhi::BufferDesc desc;
            desc.size = required;
            desc.usage = rhi::BufferUsage::TransferDst;
            desc.host_visible = true;
            desc.debug_name = "renderer.readback";
            readback_buffer_ = rhi::Buffer::create(context_->allocator(), desc);
            readback_capacity_ = readback_buffer_ != nullptr ? required : 0;
            readback_width_ = 0;
            readback_height_ = 0;
        }
        if (readback_buffer_ != nullptr) {
            const VkImageLayout previous = current_target_->color_layout();
            const VkImageSubresourceRange range = rhi::subresource_range(VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
            cmd.transition_image(current_target_->color_image(), previous, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 range);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {width_, height_, 1};
            cmd.copy_image_to_buffer(current_target_->color_image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     readback_buffer_->handle(), ConstSpan<VkBufferImageCopy>(&region, 1));
            cmd.transition_image(current_target_->color_image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, previous,
                                 range);
            readback_width_ = width_;
            readback_height_ = height_;
            screenshot_recorded_ = true;
        }
    }

    cmd.end();

    std::vector<VkSemaphore> wait_semaphores;
    std::vector<VkPipelineStageFlags> wait_stages;
    if (swapchain_ != nullptr) {
        wait_semaphores.push_back(resources.image_available->handle());
        wait_stages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }

    VkSemaphoreSubmitInfo wait_info[1]{};
    std::vector<VkSemaphoreSubmitInfo> waits;
    if (!wait_semaphores.empty()) {
        wait_info[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait_info[0].semaphore = wait_semaphores.front();
        wait_info[0].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        waits.push_back(wait_info[0]);
    }

    std::vector<VkSemaphoreSubmitInfo> signals;
    if (swapchain_ != nullptr) {
        VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signal.semaphore = render_finished_[current_image_index_]->handle();
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        signals.push_back(signal);
    }

    const VkCommandBuffer command_handle = cmd.handle();
    VkCommandBufferSubmitInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    command_info.commandBuffer = command_handle;

    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = static_cast<u32>(waits.size());
    submit.pWaitSemaphoreInfos = waits.empty() ? nullptr : waits.data();
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &command_info;
    submit.signalSemaphoreInfoCount = static_cast<u32>(signals.size());
    submit.pSignalSemaphoreInfos = signals.empty() ? nullptr : signals.data();
    const VkQueue queue = context_->device().graphics_queue();
    ORE_VK_CHECK(vkQueueSubmit2(queue, 1, &submit, resources.fence->handle()));

    if (swapchain_ != nullptr) {
        bool out_of_date = false;
        std::vector<VkSemaphore> present_waits{render_finished_[current_image_index_]->handle()};
        if (!swapchain_->present(queue, present_waits, current_image_index_, out_of_date) || out_of_date) {
            resize_pending_ = true;
        }
    }

    stats_.last_frame = frame_.counters;
    stats_.frames = frame_number_;
    ++frame_number_;
    current_slot_ = (current_slot_ + 1) % static_cast<u32>(frames_.size());

    if (screenshot_recorded_) {
        // Wait for this frame's fence before the host reads the buffer.
        save_screenshot();
    }
}

void Renderer::request_screenshot(std::string path) { screenshot_path_ = std::move(path); }

bool Renderer::save_screenshot() {
    if (screenshot_path_.empty()) return false;
    if (readback_buffer_ == nullptr || readback_width_ == 0) {
        ORE_WARN("renderer: no frame has been recorded yet, cannot write '{}'", screenshot_path_);
        screenshot_path_.clear();
        return false;
    }

    wait_idle();
    const void* mapped = readback_buffer_->mapped_data();
    if (mapped == nullptr) {
        ORE_ERROR("renderer: readback buffer is not mapped");
        screenshot_path_.clear();
        return false;
    }
    rhi::Buffer& buffer = *readback_buffer_;
    buffer.invalidate(0, static_cast<u64>(readback_width_) * readback_height_ * 4u);
    Image image = rhi::image_from_format(mapped, readback_width_, readback_height_, color_format_);
    const bool saved = image.save_png(screenshot_path_);
    if (saved) {
        ORE_INFO("saved screenshot '{}' ({}x{})", screenshot_path_, image.width, image.height);
    } else {
        ORE_ERROR("renderer: failed to write '{}'", screenshot_path_);
    }
    screenshot_path_.clear();
    screenshot_recorded_ = false;
    return saved;
}

void Renderer::resize(u32 width, u32 height, bool vsync) {
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    if (width == width_ && height == height_ && vsync == desc_.vsync && !resize_pending_) return;
    pending_width_ = width;
    pending_height_ = height;
    pending_vsync_ = vsync;
    desc_.vsync = vsync;
    width_ = width;
    height_ = height;
    resize_pending_ = true;
}

void Renderer::wait_idle() { context_->wait_idle(); }

void Renderer::report_frame_stats() { stats_.cpu_frame_ms = stats_.cpu_frame_ms; }

std::string Renderer::dump_stats() const {
    return std::format("renderer: {} frame(s), last frame: {} draw call(s), {} triangle(s), gpu wait {:.2f} ms",
                       stats_.frames, stats_.last_frame.draw_calls, stats_.last_frame.triangles,
                       static_cast<f64>(stats_.gpu_wait_ms));
}

} // namespace ore
