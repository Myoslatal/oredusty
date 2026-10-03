// Ore framework - graphics and compute pipelines built for dynamic rendering.
#pragma once

#include <ore/core/types.h>
#include <ore/rhi/descriptor.h>
#include <ore/rhi/device.h>
#include <ore/rhi/shader.h>

#include <string>
#include <vector>

namespace ore::rhi {

struct VertexAttribute {
    u32 location = 0;
    u32 binding = 0;
    VkFormat format = VK_FORMAT_R32G32B32_SFLOAT;
    u32 offset = 0;
};

struct VertexBinding {
    u32 binding = 0;
    u32 stride = 0;
    VkVertexInputRate input_rate = VK_VERTEX_INPUT_RATE_VERTEX;
};

struct VertexLayout {
    std::vector<VertexBinding> bindings;
    std::vector<VertexAttribute> attributes;
    [[nodiscard]] bool empty() const { return bindings.empty() && attributes.empty(); }
};

class PipelineLayout {
public:
    [[nodiscard]] static Scope<PipelineLayout> create(Device& device, ConstSpan<const DescriptorSetLayout*> set_layouts,
                                                     ConstSpan<VkPushConstantRange> push_constant_ranges = {},
                                                     std::string_view debug_name = {});
    ~PipelineLayout();
    ORE_NON_MOVABLE(PipelineLayout);

    [[nodiscard]] VkPipelineLayout handle() const { return layout_; }

private:
    PipelineLayout() = default;
    Device* device_ = nullptr;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
};

enum class BlendMode : u8 {
    None,              ///< opaque
    Alpha,             ///< src alpha / one minus src alpha
    Additive,          ///< src alpha / one
    PremultipliedAlpha ///< one / one minus src alpha
};

struct GraphicsPipelineDesc {
    const ShaderModule* vertex_shader = nullptr;
    const ShaderModule* fragment_shader = nullptr;
    VertexLayout vertex_layout{};
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkCullModeFlags cull_mode = VK_CULL_MODE_BACK_BIT;
    VkFrontFace front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPolygonMode polygon_mode = VK_POLYGON_MODE_FILL;
    bool depth_test = true;
    bool depth_write = true;
    VkCompareOp depth_compare = VK_COMPARE_OP_LESS_OR_EQUAL;
    bool depth_clamp = false;
    bool depth_bias_enable = false;
    f32 depth_bias_constant = 0.0f;
    f32 depth_bias_slope = 0.0f;
    BlendMode blend = BlendMode::None;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    std::vector<VkFormat> color_formats{VK_FORMAT_B8G8R8A8_UNORM};
    VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
    const PipelineLayout* layout = nullptr;
    std::string debug_name;
};

class GraphicsPipeline {
public:
    [[nodiscard]] static Scope<GraphicsPipeline> create(Device& device, const GraphicsPipelineDesc& desc);
    ~GraphicsPipeline();
    ORE_NON_MOVABLE(GraphicsPipeline);

    [[nodiscard]] VkPipeline handle() const { return pipeline_; }
    [[nodiscard]] VkPipelineLayout layout() const { return layout_; }
    [[nodiscard]] const GraphicsPipelineDesc& desc() const { return desc_; }

private:
    GraphicsPipeline() = default;
    Device* device_ = nullptr;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    GraphicsPipelineDesc desc_{};
};

struct ComputePipelineDesc {
    const ShaderModule* shader = nullptr;
    const PipelineLayout* layout = nullptr;
    std::string debug_name;
};

class ComputePipeline {
public:
    [[nodiscard]] static Scope<ComputePipeline> create(Device& device, const ComputePipelineDesc& desc);
    ~ComputePipeline();
    ORE_NON_MOVABLE(ComputePipeline);

    [[nodiscard]] VkPipeline handle() const { return pipeline_; }
    [[nodiscard]] VkPipelineLayout layout() const { return layout_; }

private:
    ComputePipeline() = default;
    Device* device_ = nullptr;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
};

/// Blend factors for a mode, so callers can inspect what the pipeline was created with.
void blend_factors(BlendMode mode, VkBlendFactor& src_color, VkBlendFactor& dst_color, VkBlendFactor& src_alpha,
                   VkBlendFactor& dst_alpha);

} // namespace ore::rhi
