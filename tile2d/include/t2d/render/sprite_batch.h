// Tile2D - the 2D renderer: one batched quad pipeline for tiles, sprites, overlays and text.
//
// Rendering sits on top of the Ore rendering framework (window, Vulkan context, frame loop, upload
// ring). Everything the game draws in a frame becomes a single vertex buffer slice and one draw call
// per texture, which is what a tilemap needs to stay cheap.
#pragma once

#include <ore/math/math.h>
#include <ore/renderer/renderer.h>
#include <ore/renderer/upload_ring.h>
#include <ore/rhi/buffer.h>
#include <ore/rhi/descriptor.h>
#include <ore/rhi/pipeline.h>
#include <ore/rhi/texture.h>

#include <t2d/core/math2d.h>
#include <t2d/core/types.h>

#include <string>
#include <string_view>
#include <vector>

namespace t2d {

/// Orthographic view-projection for the top-left origin 2D world the game uses: world (x, y) with
/// the camera centered on p center and p size world units across the screen.
///
/// The world lives at z = 0, so the depth range must contain zero: with the default camera looking
/// down -Z along near = 0.1 .. far = 100 every quad would sit behind the near plane, Vulkan would
/// clip the entire frame away and the renderer would report draw calls while the screen stayed
/// empty. Exposed as a free function so the mapping can be tested without a GPU.
[[nodiscard]] ore::Mat4 sprite_view_projection(const Vec2& center, const Vec2& size);

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
    /// Points draw_rect() at a texel of the currently bound texture that is opaque. The default is the
    /// font atlas's reserved white cell; a glyph atlas has its own.
    void set_white_texel(const Vec2& uv);
    /// Draws a filled rect using a single opaque texel of the atlas.
    void draw_rect(const Aabb2& rect, u32 color);
    void draw_rect_outline(const Aabb2& rect, f32 thickness, u32 color);
    void draw_text(f32 x, f32 y, f32 scale, u32 color, std::string_view text);

    /// Copies the staged vertices into the frame ring and issues one draw call. Only the quads
    /// that were actually drawn are uploaded, so a light frame does not reserve a heavy one's worth
    /// of the ring.
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
    /// One descriptor set per batch begun, handed out round robin. A frame records several batches
    /// (tiles, then text) and descriptor writes take effect immediately, so a single shared set
    /// would make every draw of the frame sample the texture written last - the tiles would come out
    /// wearing the font atlas.
    std::vector<ore::rhi::DescriptorSet> descriptor_sets_;
    usize next_descriptor_set_ = 0;

    /// Quads are staged in this CPU buffer and copied into the frame ring by end() with their exact
    /// size. Writing straight into a worst case slice of the ring wastes most of it (two batches per
    /// frame would ask for twice the ring segment and the second one would draw nothing).
    SpriteVertex* vertices_ = nullptr;
    ore::UploadRing* ring_ = nullptr;
    std::vector<SpriteVertex> staging_;
    u32 max_quads_ = 0;
    u32 quad_count_ = 0;
    u32 dropped_quads_ = 0;
    u32 draw_calls_ = 0;
    ore::rhi::CommandBuffer* cmd_ = nullptr;
    f32 white_u_ = 0.0f;   ///< uv of an opaque white texel (last font cell)
    f32 white_v_ = 0.0f;
};

} // namespace t2d
