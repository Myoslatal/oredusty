// Ore framework - the graphics context: instance + device + allocator + upload helpers.
//
// Everything a typical game needs for asset upload and readback lives here so the lower
// level modules stay free of "convenience" policy.
#pragma once

#include <ore/core/image.h>
#include <ore/core/types.h>
#include <ore/rhi/allocator.h>
#include <ore/rhi/buffer.h>
#include <ore/rhi/commands.h>
#include <ore/rhi/device.h>
#include <ore/rhi/instance.h>
#include <ore/rhi/texture.h>

#include <functional>
#include <string>
#include <string_view>
#include <string_view>
#include <vector>

namespace ore::rhi {

/// Converts tightly packed pixels of \p format (R8G8B8A8/B8G8R8A8 variants and
/// R16G16B16A16_SFLOAT) into an RGBA8 ore::Image. Returns an empty image for unsupported formats.
[[nodiscard]] Image image_from_format(const void* pixels, u32 width, u32 height, VkFormat format);

struct ContextDesc {
    std::string application_name = "Ore Application";
    bool enable_validation = ORE_ENABLE_VALIDATION != 0;
    /// Headless contexts do not require presentation support; they render offscreen only.
    bool headless = false;
    /// Surface to present to (ignored when headless). **The caller keeps ownership** and must
    /// destroy it with vkDestroySurfaceKHR after the context is gone.
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    /// Alternative to \p surface: called with the freshly created instance so the platform layer can
    /// create the surface (it needs a valid VkInstance). Required when \p headless is false and
    /// \p surface is VK_NULL_HANDLE. A surface created here is **owned and destroyed** by the
    /// context, because the caller never sees the instance it was created from.
    std::function<VkSurfaceKHR(VkInstance)> surface_factory;
    std::vector<std::string> instance_extensions;
    std::vector<std::string> device_extensions;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    u32 device_index = 0;
    bool prefer_discrete_gpu = true;
    bool allow_software_device = true;
    GpuAllocatorDesc allocator{};
};

class GraphicsContext {
public:
    [[nodiscard]] static Scope<GraphicsContext> create(const ContextDesc& desc);
    ~GraphicsContext();
    ORE_NON_MOVABLE(GraphicsContext);

    [[nodiscard]] Instance& instance() const { return *instance_; }
    [[nodiscard]] Device& device() const { return *device_; }
    [[nodiscard]] GpuAllocator& allocator() const { return *allocator_; }
    [[nodiscard]] const PhysicalDeviceInfo& device_info() const { return device_->info(); }
    [[nodiscard]] bool validation_enabled() const { return instance_->validation_enabled(); }
    [[nodiscard]] VkSurfaceKHR surface() const { return surface_; }
    [[nodiscard]] bool headless() const { return surface_ == VK_NULL_HANDLE; }

    void wait_idle();

    /// Command buffers for one-shot work (uploads, readbacks). Submits and waits synchronously.
    [[nodiscard]] ImmediateCommands& immediate() const { return *immediate_; }
    void immediate_submit(const std::function<void(CommandBuffer&)>& fn);

    /// Staging upload into a device local buffer.
    void upload_buffer(Buffer& destination, ConstSpan<u8> data, u64 offset = 0);
    /// Uploads an RGBA8 image into \p texture (which must have TRANSFER_DST usage) and leaves it
    /// in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
    void upload_texture(Texture& texture, const Image& image, bool generate_mipmaps = false);
    /// Records a full mip chain generation into \p cmd (blit based, requires TRANSFER_SRC/DST + SAMPLED).
    void generate_mipmaps(CommandBuffer& cmd, Texture& texture);

    [[nodiscard]] Scope<Texture> create_texture(const Image& image, bool generate_mipmaps = true,
                                               std::string_view debug_name = {});
    /// 1x1 texture, handy as a default binding.
    [[nodiscard]] Scope<Texture> create_solid_texture(u32 rgba = 0xFFFFFFFFu, std::string_view debug_name = {});
    [[nodiscard]] Scope<Buffer> create_buffer(const BufferDesc& desc) const;
    [[nodiscard]] Scope<Buffer> create_staging_buffer(u64 size, std::string_view debug_name = {}) const;

    /// Copies \p image back to host memory as RGBA8. \p current_layout is the layout the image is
    /// in right now; it is restored before returning.
    [[nodiscard]] Image read_image(VkImage image, u32 width, u32 height, VkFormat format,
                                   VkImageLayout current_layout);
    /// Reads back a render target's colour image (uses its tracked layout).
    [[nodiscard]] Image read_render_target(class RenderTarget& target);

    [[nodiscard]] GpuAllocator::Stats allocator_stats() const { return allocator_->stats(); }

private:
    GraphicsContext() = default;

    Scope<Instance> instance_;
    Scope<Device> device_;
    Scope<GpuAllocator> allocator_;
    Scope<ImmediateCommands> immediate_;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    bool owns_surface_ = false;   ///< true when surface_factory created it
    u64 upload_bytes_ = 0;
};

} // namespace ore::rhi
