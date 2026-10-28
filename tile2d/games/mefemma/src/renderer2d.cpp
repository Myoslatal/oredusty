#include <mefemma/renderer2d.h>

#include <t2d/core/log.h>

namespace mefemma {

Scope<Renderer2D> Renderer2D::create(ore::rhi::GraphicsContext& context, const Renderer2DOptions& options) {
    Scope<Renderer2D> renderer(new Renderer2D());

    t2d::SpriteBatch::Options batch_options;
    batch_options.max_quads = options.max_quads;
    batch_options.color_format = options.color_format;
    batch_options.depth_format = options.depth_format;
    batch_options.vertex_shader_path = options.vertex_shader;
    batch_options.fragment_shader_path = options.fragment_shader;
    renderer->batch_ = t2d::SpriteBatch::create(context, batch_options);
    if (renderer->batch_ == nullptr) {
        T2D_ERROR("renderer2d: the sprite batch could not be created");
        return nullptr;
    }

    t2d::GlyphAtlas::Options atlas_options;
    atlas_options.page_size = 2048;
    atlas_options.max_pages = 1;
    renderer->text_ = t2d::TextRenderer::create(context, atlas_options);
    if (renderer->text_ == nullptr) {
        T2D_ERROR("renderer2d: the text renderer could not be created");
        return nullptr;
    }
    // Rectangles sample the glyph atlas's white texel; every rectangle drawn before this was re-pointed
    // would be filtered by whatever texel happened to be there (usually a glyph's edge).
    renderer->batch_->set_white_texel(renderer->text_->atlas().white_uv());

    ore::rhi::SamplerDesc text_desc;
    text_desc.min_filter = VK_FILTER_LINEAR;   // glyphs are anti-aliased: linear keeps them smooth
    text_desc.mag_filter = VK_FILTER_LINEAR;
    text_desc.mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    renderer->text_sampler_ = ore::rhi::Sampler::create(context.device(), text_desc);
    if (renderer->text_sampler_ == nullptr) return nullptr;

    ore::rhi::SamplerDesc picture_desc;
    picture_desc.min_filter = VK_FILTER_NEAREST;
    picture_desc.mag_filter = VK_FILTER_NEAREST;
    picture_desc.mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    renderer->picture_sampler_ = ore::rhi::Sampler::create(context.device(), picture_desc);
    if (renderer->picture_sampler_ == nullptr) return nullptr;

    ore::rhi::SamplerDesc smooth_desc;
    smooth_desc.min_filter = VK_FILTER_LINEAR;
    smooth_desc.mag_filter = VK_FILTER_LINEAR;
    smooth_desc.mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    renderer->smooth_sampler_ = ore::rhi::Sampler::create(context.device(), smooth_desc);
    if (renderer->smooth_sampler_ == nullptr) return nullptr;
    return renderer;
}

Renderer2D::~Renderer2D() = default;

t2d::TextMetrics Renderer2D::measure(std::string_view text, u16 size_px) const {
    if (text_ == nullptr || fonts_.latin == nullptr) return t2d::TextMetrics{};
    t2d::TextStyle style;
    style.size_px = size_px;
    return text_->measure(text, fonts_, style);
}

void Renderer2D::begin_glyph_batch(ore::RenderFrame& frame, const ore::Mat4& view_projection) {
    if (batch_kind_ == Batch::Glyph) return;
    flush();
    batch_->begin(frame, view_projection, *text_->texture(), text_sampler_->handle());
    batch_kind_ = Batch::Glyph;
}

void Renderer2D::begin_picture_batch(ore::RenderFrame& frame, const ore::Mat4& view_projection,
                                     ore::rhi::Texture& texture, const std::string& name, TextureFilter filter) {
    if (batch_kind_ == Batch::Picture && batch_picture_ == name && batch_filter_ == filter) return;
    flush();
    const VkSampler sampler =
        filter == TextureFilter::Linear ? smooth_sampler_->handle() : picture_sampler_->handle();
    batch_->begin(frame, view_projection, texture, sampler);
    batch_kind_ = Batch::Picture;
    batch_picture_ = name;
    batch_filter_ = filter;
}

void Renderer2D::note_batch_cost() {
    stats_.draw_calls += batch_->draw_calls();
    stats_.quads += batch_->quads();
    stats_.dropped_quads += batch_->dropped_quads();
}

void Renderer2D::flush() {
    if (batch_kind_ == Batch::None) return;
    batch_->end();
    note_batch_cost();
    batch_kind_ = Batch::None;
    batch_picture_.clear();
    batch_filter_ = TextureFilter::Nearest;
}

void Renderer2D::draw(ore::RenderFrame& frame, const DrawList& list, Assets& assets) {
    stats_ = Stats{};
    if (text_ == nullptr || batch_ == nullptr || frame.width == 0 || frame.height == 0) return;

    const t2d::Vec2 view{static_cast<t2d::f32>(frame.width), static_cast<t2d::f32>(frame.height)};
    const ore::Mat4 view_projection = t2d::sprite_view_projection(t2d::Vec2{view.x * 0.5f, view.y * 0.5f}, view);

    for (const DrawCommand& command : list.commands()) {
        switch (command.kind) {
            case DrawCommand::Kind::Rect: {
                begin_glyph_batch(frame, view_projection);
                batch_->draw_rect(command.rect, command.color);
                break;
            }
            case DrawCommand::Kind::Text: {
                if (fonts_.latin == nullptr) {
                    ++stats_.missing_fonts;
                    break;
                }
                begin_glyph_batch(frame, view_projection);
                t2d::TextStyle style;
                style.size_px = command.size_px;
                style.color = command.color;
                (void)text_->draw(*batch_, command.text, fonts_, style, command.pen, command.max_width);
                break;
            }
            case DrawCommand::Kind::Sprite: {
                ore::rhi::Texture* texture = assets.texture(command.texture);
                if (texture == nullptr) {
                    // A picture that is not there is drawn as a marked box, not skipped: a scene that lost
                    // its art has to look broken, because "nothing appeared" is not a report anyone can
                    // act on.
                    ++stats_.missing_textures;
                    begin_glyph_batch(frame, view_projection);
                    batch_->draw_rect(command.rect, rgba(180, 40, 150, 120));
                    batch_->draw_rect_outline(command.rect, 2.0f, rgba(240, 80, 200));
                    break;
                }
                begin_picture_batch(frame, view_projection, *texture, command.texture, command.filter);
                batch_->draw_quad(command.rect, command.uv, command.color);
                break;
            }
        }
    }
    flush();
    if (batch_->batches_over() > reported_batches_over_) {
        T2D_WARN("renderer2d: {} batch(es) past the sprite batch's descriptor sets - the earliest draws of "
                 "the frame sample another texture (draw fewer distinct pictures, or raise the batch's set "
                 "count)",
                 batch_->batches_over() - reported_batches_over_);
        reported_batches_over_ = batch_->batches_over();
    }
}

} // namespace mefemma
