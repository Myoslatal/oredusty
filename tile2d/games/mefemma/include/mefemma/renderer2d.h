// Mefemma - the 2D renderer: a DrawList in, batches out.
//
// One SpriteBatch, one TextRenderer, and a rule that is the whole of the renderer: **the submission order is
// the drawing order**. Commands that need the same texture share a batch; a command that needs another one
// flushes and starts a new batch. Rectangles and text share the glyph atlas (a rectangle samples its white
// texel), pictures have a texture of their own.
//
// A batch binds one texture at a time, so a frame that alternates pictures with rectangles issues a batch
// per alternation. Sorting the commands by texture would save batches and change what is drawn on top of
// what - a renderer that quietly reorders a frame is worse than one that draws it as asked.
#pragma once

#include <mefemma/assets.h>
#include <mefemma/draw_list.h>

#include <t2d/render/glyph_atlas.h>
#include <t2d/render/sprite_batch.h>
#include <t2d/render/text_renderer.h>

#include <ore/renderer/renderer.h>
#include <ore/rhi/context.h>
#include <ore/rhi/texture.h>

#include <string>

namespace mefemma {

using t2d::Scope;
using t2d::u16;
using t2d::u32;

struct Renderer2DOptions {
    /// The frame's quad budget. A DrawList bigger than this loses its last quads, and dropped_quads() says
    /// so; a game that draws more raises it (and the frame's upload ring with it).
    u32 max_quads = 16384;
    /// Formats must match the renderer's: a pipeline built for another colour format draws nothing (or
    /// draws into the wrong target), so the shell passes renderer().color_format()/depth_format() here.
    VkFormat color_format = VK_FORMAT_UNDEFINED;
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    /// Resolved by the application through ore::Application::shader_path().
    std::string vertex_shader;
    std::string fragment_shader;
};

class Renderer2D {
public:
    struct Stats {
        u32 draw_calls = 0;
        u32 quads = 0;
        u32 dropped_quads = 0;
        u32 missing_textures = 0;
        u32 missing_fonts = 0;
    };

    [[nodiscard]] static Scope<Renderer2D> create(ore::rhi::GraphicsContext& context, const Renderer2DOptions& options);
    ~Renderer2D();
    T2D_NON_MOVABLE(Renderer2D);

    /// The fonts text is drawn with. Pointers are copied, so the fonts must outlive the renderer's use of
    /// them - which they do while they belong to the Assets the frame also draws from.
    void set_fonts(const t2d::FontSet& fonts) { fonts_ = fonts; }

    /// The box \p text fills at \p size_px. What a scene measures before placing a line; the renderer draws
    /// with the same text engine, so the two agree.
    [[nodiscard]] t2d::TextMetrics measure(std::string_view text, u16 size_px) const;

    /// Draws \p list into \p frame. The view is screen pixels with the origin at the top left, which is what
    /// a DrawList is authored in. Pictures are resolved through \p assets by name and cached there.
    void draw(ore::RenderFrame& frame, const DrawList& list, Assets& assets);

    [[nodiscard]] const Stats& stats() const { return stats_; }

private:
    Renderer2D() = default;

    enum class Batch : t2d::u8 { None, Glyph, Picture };
    void begin_glyph_batch(ore::RenderFrame& frame, const ore::Mat4& view_projection);
    void begin_picture_batch(ore::RenderFrame& frame, const ore::Mat4& view_projection, ore::rhi::Texture& texture,
                             const std::string& name);
    void flush();
    void note_batch_cost();

    Scope<t2d::SpriteBatch> batch_;
    Scope<t2d::TextRenderer> text_;
    Scope<ore::rhi::Sampler> text_sampler_;
    Scope<ore::rhi::Sampler> picture_sampler_;
    t2d::FontSet fonts_{};

    Batch batch_kind_ = Batch::None;
    std::string batch_picture_;
    Stats stats_{};
    /// The batch's over-budget counter when it was last reported: a frame that begins more batches than the
    /// sprite batch has descriptor sets wraps round and its earliest draws sample the wrong texture. It is
    /// said out loud once per new occurrence, not once per frame.
    u32 reported_batches_over_ = 0;
};

} // namespace mefemma
