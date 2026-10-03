// Tile2D - the 2D renderer: one batched quad pipeline for tiles, sprites, overlays and text.
//
// Rendering sits on top of the Ore rendering framework (window, Vulkan context, frame loop, upload
// ring). Everything the game draws in a frame becomes a single vertex buffer slice and one draw call
// per texture, which is what a tilemap needs to stay cheap.
#pragma once

#include <ore/math/math.h>
#include <ore/renderer/renderer.h>
#include <ore/rhi/buffer.h>
#include <ore/rhi/descriptor.h>
#include <ore/rhi/pipeline.h>
#include <ore/rhi/texture.h>

#include <t2d/core/math2d.h>
#include <t2d/core/types.h>

#include <string>
#include <string_view>

namespace t2d {

/// Vertex layout of the batch: world position, atlas uv, RGBA8 tint. 20 bytes, packed.
struct SpriteVertex {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 u = 0.0f;
    f32 v = 0.0f;
    u8 r = 255, g = 255, b = 255, a = 255;

    void set_color(u32 rgba) {
        r = static_cast<u8>(rgba & 0xFFu);
        g = static_cast<u8>((rgba >> 8) & 0xFFu);
        b = static_cast<u8>((rgba >> 16) & 0xFFu);
        a = static_cast<u8>((rgba >> 24) & 0xFFu);
    }
};

class SpriteBatch {
public:
    struct Options {
        u32 max_quads = 12288;
        VkFormat color_format = VK_FORMAT_UNDEFINED;
        VkFormat depth_format = VK_FORMAT_UNDEFINED;
        std::string vertex_shader_path;
        std::string fragment_shader_path;
        /// Nearest keeps pixel art crisp and avoids bleeding between atlas cells.
        bool nearest_filter = true;
    };

    [[nodiscard]] static Scope<SpriteBatch> create(ore::rhi::GraphicsContext& context, const Options& options);
    ~SpriteBatch();
    T2D_NON_MOVABLE(SpriteBatch);

    /// Starts a frame: allocates the vertex slice from the frame ring and binds state.
    void begin(ore::RenderFrame& frame, const Vec2& camera_center, const Vec2& camera_size,
               ore::rhi::Texture& atlas, VkSampler sampler);
    /// Convenience overload for callers that already have a projection matrix.
    void begin(ore::RenderFrame& frame, const ore::Mat4& view_projection, ore::rhi::Texture& atlas,
               VkSampler sampler);

    void draw_quad(const Aabb2& rect, const Aabb2& uv_rect, u32 color);
    /// Draws a filled rect using a single opaque texel of the atlas.
    void draw_rect(const Aabb2& rect, u32 color);
    void draw_rect_outline(const Aabb2& rect, f32 thickness, u32 color);
    void draw_text(f32 x, f32 y, f32 scale, u32 color, std::string_view text);

    /// Uploads nothing (the ring is already host visible) and issues one draw call.
    void end();

    [[nodiscard]] u32 quads() const { return quad_count_; }
    [[nodiscard]] u32 draw_calls() const { return draw_calls_; }
    [[nodiscard]] u32 dropped_quads() const { return dropped_quads_; }
    [[nodiscard]] u32 max_quads() const { return max_quads_; }
    [[nodiscard]] u32 vertices_uploaded() const { return quad_count_ * 4u; }

private:
    SpriteBatch() = default;

    Scope<ore::rhi::Buffer> index_buffer_;
    Scope<ore::rhi::ShaderModule> vertex_shader_;
    Scope<ore::rhi::ShaderModule> fragment_shader_;
    Scope<ore::rhi::DescriptorSetLayout> descriptor_layout_;
    Scope<ore::rhi::PipelineLayout> pipeline_layout_;
    Scope<ore::rhi::GraphicsPipeline> pipeline_;
    Scope<ore::rhi::DescriptorPool> descriptor_pool_;
    ore::rhi::DescriptorSet descriptor_set_;

    SpriteVertex* vertices_ = nullptr;   ///< mapped slice of the frame ring
    u64 vertex_offset_ = 0;              ///< byte offset of the slice inside the ring buffer
    ore::rhi::Buffer* vertex_buffer_ = nullptr;
    u32 max_quads_ = 0;
    u32 quad_count_ = 0;
    u32 dropped_quads_ = 0;
    u32 draw_calls_ = 0;
    ore::rhi::CommandBuffer* cmd_ = nullptr;
    f32 white_u_ = 0.0f;   ///< uv of an opaque white texel (last font cell)
    f32 white_v_ = 0.0f;
};

} // namespace t2d
