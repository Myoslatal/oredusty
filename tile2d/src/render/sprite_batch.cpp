#include <t2d/render/sprite_batch.h>

#include <t2d/core/log.h>
#include <t2d/render/atlas.h>

#include <ore/math/math.h>

#include <cstring>

namespace t2d {
namespace {

constexpr usize kVertexStride = sizeof(SpriteVertex);

} // namespace

ore::Mat4 sprite_view_projection(const Vec2& center, const Vec2& size) {
    const f32 half_width = size.x * 0.5f;
    const f32 half_height = size.y * 0.5f;
    // World +Y points down and the framebuffer's top edge is -1 in clip space, so the camera's
    // "top" is its smaller y. near = -1 / far = 1 maps world depth 0 to the middle of [0, 1].
    return ore::orthographic(center.x - half_width, center.x + half_width, center.y + half_height,
                             center.y - half_height, -1.0f, 1.0f);
}

Scope<SpriteBatch> SpriteBatch::create(ore::rhi::GraphicsContext& context, const Options& options) {
    Scope<SpriteBatch> batch(new SpriteBatch());
    batch->max_quads_ = options.max_quads == 0 ? 1024u : options.max_quads;
    batch->staging_.resize(static_cast<usize>(batch->max_quads_) * 4u);

    batch->vertex_shader_ = ore::rhi::ShaderModule::load(context.device(), options.vertex_shader_path);
    batch->fragment_shader_ = ore::rhi::ShaderModule::load(context.device(), options.fragment_shader_path);
    if (batch->vertex_shader_ == nullptr || batch->fragment_shader_ == nullptr) {
        T2D_ERROR("sprite batch: shaders are missing ({} / {})", options.vertex_shader_path,
                  options.fragment_shader_path);
        return nullptr;
    }

    // Static index buffer: quad i uses vertices 4i .. 4i+3, two triangles.
    const u32 quad_count = batch->max_quads_;
    ore::rhi::BufferDesc index_desc;
    index_desc.size = static_cast<u64>(quad_count) * 6u * sizeof(u32);
    index_desc.usage = ore::rhi::BufferUsage::Index | ore::rhi::BufferUsage::TransferDst;
    index_desc.debug_name = "t2d.sprite.indices";
    batch->index_buffer_ = ore::rhi::Buffer::create(context.allocator(), index_desc);
    if (batch->index_buffer_ == nullptr) return nullptr;
    {
        std::vector<u32> indices(static_cast<usize>(quad_count) * 6u);
        for (u32 quad = 0; quad < quad_count; ++quad) {
            const u32 base = quad * 4u;
            const usize offset = static_cast<usize>(quad) * 6u;
            indices[offset + 0] = base + 0;
            indices[offset + 1] = base + 1;
            indices[offset + 2] = base + 2;
            indices[offset + 3] = base + 0;
            indices[offset + 4] = base + 2;
            indices[offset + 5] = base + 3;
        }
        context.upload_buffer(*batch->index_buffer_,
                              ore::ConstSpan<ore::u8>(reinterpret_cast<const ore::u8*>(indices.data()),
                                                       indices.size() * sizeof(u32)));
    }

    const ore::rhi::DescriptorBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                              VK_SHADER_STAGE_FRAGMENT_BIT, 0};
    batch->descriptor_layout_ = ore::rhi::DescriptorSetLayout::create(context.device(),
                                                                     ore::ConstSpan<ore::rhi::DescriptorBinding>(&binding, 1),
                                                                     "t2d.sprite.set");
    const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ore::Mat4)};
    const ore::rhi::DescriptorSetLayout* set_layouts[] = {batch->descriptor_layout_.get()};
    batch->pipeline_layout_ = ore::rhi::PipelineLayout::create(
        context.device(), ore::ConstSpan<const ore::rhi::DescriptorSetLayout*>(set_layouts, 1),
        ore::ConstSpan<VkPushConstantRange>(&range, 1), "t2d.sprite.layout");
    if (batch->descriptor_layout_ == nullptr || batch->pipeline_layout_ == nullptr) return nullptr;

    ore::rhi::GraphicsPipelineDesc pipeline_desc;
    pipeline_desc.vertex_shader = batch->vertex_shader_.get();
    pipeline_desc.fragment_shader = batch->fragment_shader_.get();
    pipeline_desc.layout = batch->pipeline_layout_.get();
    pipeline_desc.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    pipeline_desc.cull_mode = VK_CULL_MODE_NONE;   // quads are drawn in both winding orders
    pipeline_desc.depth_test = false;
    pipeline_desc.depth_write = false;
    pipeline_desc.blend = ore::rhi::BlendMode::Alpha;
    pipeline_desc.color_formats = {options.color_format};
    // Must match the render pass: VK_FORMAT_UNDEFINED for a pure 2D target, the depth format when
    // the application asked for one.
    pipeline_desc.depth_format = options.depth_format;
    pipeline_desc.debug_name = "t2d.sprite.pipeline";

    ore::rhi::VertexLayout layout;
    layout.bindings.push_back({0, static_cast<u32>(kVertexStride), VK_VERTEX_INPUT_RATE_VERTEX});
    layout.attributes.push_back({0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(SpriteVertex, x)});
    layout.attributes.push_back({1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(SpriteVertex, u)});
    layout.attributes.push_back({2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(SpriteVertex, r)});
    pipeline_desc.vertex_layout = layout;

    batch->pipeline_ = ore::rhi::GraphicsPipeline::create(context.device(), pipeline_desc);
    if (batch->pipeline_ == nullptr) return nullptr;

    // Enough sets that a frame's batches never collide and a set comes back around only after
    // several frames have been retired by the GPU.
    constexpr u32 kDescriptorSetCount = 8;
    const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kDescriptorSetCount};
    batch->descriptor_pool_ = ore::rhi::DescriptorPool::create(
        context.device(), ore::ConstSpan<VkDescriptorPoolSize>(&pool_size, 1), kDescriptorSetCount);
    if (batch->descriptor_pool_ == nullptr) return nullptr;
    batch->descriptor_sets_.reserve(kDescriptorSetCount);
    for (u32 index = 0; index < kDescriptorSetCount; ++index) {
        batch->descriptor_sets_.emplace_back(
            context.device(), batch->descriptor_pool_->allocate(*batch->descriptor_layout_, "t2d.sprite.set"),
            *batch->descriptor_layout_);
    }

    // The font atlas reserves its last cell as opaque white: sample its centre for solid rectangles.
    const u32 white_cell = kFontAtlasColumns * kFontAtlasRows - 1;
    const f32 half_cell = static_cast<f32>(kFontCellSize) * 0.5f;
    batch->white_u_ =
        (static_cast<f32>((white_cell % kFontAtlasColumns) * kFontCellSize) + half_cell) / static_cast<f32>(kFontAtlasWidth);
    batch->white_v_ =
        (static_cast<f32>((white_cell / kFontAtlasColumns) * kFontCellSize) + half_cell) / static_cast<f32>(kFontAtlasHeight);
    T2D_DEBUG("sprite batch: up to {} quads per frame ({} KiB of vertex data)", batch->max_quads_,
              static_cast<f64>(batch->max_quads_) * 4.0 * static_cast<f64>(kVertexStride) / 1024.0);
    return batch;
}

SpriteBatch::~SpriteBatch() = default;

void SpriteBatch::begin(ore::RenderFrame& frame, const Vec2& camera_center, const Vec2& camera_size,
                        ore::rhi::Texture& atlas, VkSampler sampler) {
    begin(frame, sprite_view_projection(camera_center, camera_size), atlas, sampler);
}

void SpriteBatch::begin(ore::RenderFrame& frame, const ore::Mat4& view_projection, ore::rhi::Texture& atlas,
                        VkSampler sampler) {
    cmd_ = frame.cmd;
    ring_ = frame.ring;
    quad_count_ = 0;
    dropped_quads_ = 0;
    draw_calls_ = 0;
    vertices_ = staging_.data();

    // A fresh set per batch: the write below is immediate, so sharing one set between the batches
    // of a frame would leave every draw sampling the last texture written.
    ore::rhi::DescriptorSet& descriptor_set = descriptor_sets_[next_descriptor_set_];
    next_descriptor_set_ = (next_descriptor_set_ + 1) % descriptor_sets_.size();
    descriptor_set.write_texture(0, atlas, sampler);
    cmd_->bind_pipeline(*pipeline_);
    // The batch always draws in screen space, so it owns the dynamic viewport and scissor state
    // (an unset viewport silently clips every quad away).
    cmd_->set_viewport(static_cast<f32>(frame.width), static_cast<f32>(frame.height));
    cmd_->set_scissor_full(static_cast<f32>(frame.width), static_cast<f32>(frame.height));
    cmd_->bind_descriptor_set(0, descriptor_set);
    cmd_->push_constants(VK_SHADER_STAGE_VERTEX_BIT, view_projection);
    cmd_->bind_index_buffer(*index_buffer_, VK_INDEX_TYPE_UINT32);
}

void SpriteBatch::set_white_texel(const Vec2& uv) {
    white_u_ = uv.x;
    white_v_ = uv.y;
}

void SpriteBatch::draw_quad(const Aabb2& rect, const Aabb2& uv_rect, u32 color) {
    if (vertices_ == nullptr) return;
    if (quad_count_ >= max_quads_) {
        ++dropped_quads_;
        return;
    }
    SpriteVertex* vertex = vertices_ + static_cast<usize>(quad_count_) * 4u;
    const f32 u0 = uv_rect.min.x;
    const f32 v0 = uv_rect.min.y;
    const f32 u1 = uv_rect.max.x;
    const f32 v1 = uv_rect.max.y;

    vertex[0] = SpriteVertex{rect.min.x, rect.min.y, u0, v0};
    vertex[1] = SpriteVertex{rect.max.x, rect.min.y, u1, v0};
    vertex[2] = SpriteVertex{rect.max.x, rect.max.y, u1, v1};
    vertex[3] = SpriteVertex{rect.min.x, rect.max.y, u0, v1};
    for (int i = 0; i < 4; ++i) vertex[i].set_color(color);
    ++quad_count_;
}

void SpriteBatch::draw_rect(const Aabb2& rect, u32 color) {
    draw_quad(rect, Aabb2{Vec2{white_u_, white_v_}, Vec2{white_u_ + 0.001f, white_v_ + 0.001f}}, color);
}

void SpriteBatch::draw_rect_outline(const Aabb2& rect, f32 thickness, u32 color) {
    draw_rect(Aabb2{rect.min, Vec2{rect.max.x, rect.min.y + thickness}}, color);
    draw_rect(Aabb2{Vec2{rect.min.x, rect.max.y - thickness}, rect.max}, color);
    draw_rect(Aabb2{rect.min, Vec2{rect.min.x + thickness, rect.max.y}}, color);
    draw_rect(Aabb2{Vec2{rect.max.x - thickness, rect.min.y}, rect.max}, color);
}

void SpriteBatch::draw_text(f32 x, f32 y, f32 scale, u32 color, std::string_view text) {
    if (vertices_ == nullptr) return;
    const f32 advance = (static_cast<f32>(kFontGlyphWidth) + 1.0f) * scale;
    f32 cursor = x;
    for (char character : text) {
        if (character == '\n') {
            cursor = x;
            y += (static_cast<f32>(kFontGlyphHeight) + 3.0f) * scale;
            continue;
        }
        u32 cell_x = 0;
        u32 cell_y = 0;
        if (font_glyph_rect(character, cell_x, cell_y)) {
            const f32 inv_width = 1.0f / static_cast<f32>(kFontAtlasWidth);
            const f32 inv_height = 1.0f / static_cast<f32>(kFontAtlasHeight);
            const Aabb2 uv{Vec2{static_cast<f32>(cell_x) * inv_width, static_cast<f32>(cell_y) * inv_height},
                           Vec2{static_cast<f32>(cell_x + kFontCellSize) * inv_width,
                                static_cast<f32>(cell_y + kFontCellSize) * inv_height}};
            const Aabb2 rect{Vec2{cursor, y},
                             Vec2{cursor + static_cast<f32>(kFontGlyphWidth) * scale,
                                  y + static_cast<f32>(kFontGlyphHeight) * scale}};
            draw_quad(rect, uv, color);
        }
        cursor += advance;
    }
}

void SpriteBatch::end() {
    if (cmd_ == nullptr || ring_ == nullptr || quad_count_ == 0) return;

    const u64 bytes = static_cast<u64>(quad_count_) * 4u * kVertexStride;
    const auto slice = ring_->allocate(bytes, 16);
    if (!slice.valid()) {
        T2D_ERROR("sprite batch: the frame ring could not provide {} KiB for {} quads", bytes / 1024,
                  quad_count_);
        dropped_quads_ += quad_count_;
        quad_count_ = 0;
        return;
    }
    std::memcpy(slice.mapped, vertices_, static_cast<usize>(bytes));
    cmd_->bind_vertex_buffer(0, ring_->buffer(), slice.offset);
    cmd_->draw_indexed(quad_count_ * 6u);
    ++draw_calls_;
}

} // namespace t2d
