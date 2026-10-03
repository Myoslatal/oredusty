#include <ore/rhi/pipeline.h>

#include <ore/core/assert.h>
#include <ore/core/log.h>

namespace ore::rhi {

Scope<PipelineLayout> PipelineLayout::create(Device& device, ConstSpan<const DescriptorSetLayout*> set_layouts,
                                             ConstSpan<VkPushConstantRange> push_constant_ranges,
                                             std::string_view debug_name) {
    Scope<PipelineLayout> layout(new PipelineLayout());
    layout->device_ = &device;

    std::vector<VkDescriptorSetLayout> handles;
    handles.reserve(set_layouts.size());
    for (const DescriptorSetLayout* set_layout : set_layouts) {
        if (set_layout != nullptr) handles.push_back(set_layout->handle());
    }

    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = static_cast<u32>(handles.size());
    info.pSetLayouts = handles.empty() ? nullptr : handles.data();
    info.pushConstantRangeCount = static_cast<u32>(push_constant_ranges.size());
    info.pPushConstantRanges = push_constant_ranges.empty() ? nullptr : push_constant_ranges.data();
    ORE_VK_CHECK(vkCreatePipelineLayout(device.handle(), &info, nullptr, &layout->layout_));
    if (!debug_name.empty()) {
        device.set_debug_name(VK_OBJECT_TYPE_PIPELINE_LAYOUT, reinterpret_cast<u64>(layout->layout_), debug_name);
    }
    return layout;
}

PipelineLayout::~PipelineLayout() {
    if (layout_ != VK_NULL_HANDLE && device_ != nullptr) vkDestroyPipelineLayout(device_->handle(), layout_, nullptr);
}

void blend_factors(BlendMode mode, VkBlendFactor& src_color, VkBlendFactor& dst_color, VkBlendFactor& src_alpha,
                   VkBlendFactor& dst_alpha) {
    switch (mode) {
        case BlendMode::None:
            src_color = VK_BLEND_FACTOR_ONE;
            dst_color = VK_BLEND_FACTOR_ZERO;
            src_alpha = VK_BLEND_FACTOR_ONE;
            dst_alpha = VK_BLEND_FACTOR_ZERO;
            break;
        case BlendMode::Alpha:
            src_color = VK_BLEND_FACTOR_SRC_ALPHA;
            dst_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            src_alpha = VK_BLEND_FACTOR_ONE;
            dst_alpha = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case BlendMode::Additive:
            src_color = VK_BLEND_FACTOR_SRC_ALPHA;
            dst_color = VK_BLEND_FACTOR_ONE;
            src_alpha = VK_BLEND_FACTOR_ONE;
            dst_alpha = VK_BLEND_FACTOR_ONE;
            break;
        case BlendMode::PremultipliedAlpha:
            src_color = VK_BLEND_FACTOR_ONE;
            dst_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            src_alpha = VK_BLEND_FACTOR_ONE;
            dst_alpha = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
    }
}

Scope<GraphicsPipeline> GraphicsPipeline::create(Device& device, const GraphicsPipelineDesc& desc) {
    if (desc.vertex_shader == nullptr || desc.layout == nullptr) {
        ORE_ERROR("GraphicsPipeline::create: vertex shader and pipeline layout are required ({})", desc.debug_name);
        return nullptr;
    }

    Scope<GraphicsPipeline> pipeline(new GraphicsPipeline());
    pipeline->device_ = &device;
    pipeline->layout_ = desc.layout->handle();
    pipeline->desc_ = desc;

    std::vector<VkPipelineShaderStageCreateInfo> stages;
    VkPipelineShaderStageCreateInfo vertex_stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    vertex_stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertex_stage.module = desc.vertex_shader->handle();
    vertex_stage.pName = "main";
    stages.push_back(vertex_stage);
    if (desc.fragment_shader != nullptr) {
        VkPipelineShaderStageCreateInfo fragment_stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        fragment_stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragment_stage.module = desc.fragment_shader->handle();
        fragment_stage.pName = "main";
        stages.push_back(fragment_stage);
    }

    std::vector<VkVertexInputBindingDescription> bindings;
    bindings.reserve(desc.vertex_layout.bindings.size());
    for (const VertexBinding& binding : desc.vertex_layout.bindings) {
        VkVertexInputBindingDescription description{};
        description.binding = binding.binding;
        description.stride = binding.stride;
        description.inputRate = binding.input_rate;
        bindings.push_back(description);
    }
    std::vector<VkVertexInputAttributeDescription> attributes;
    attributes.reserve(desc.vertex_layout.attributes.size());
    for (const VertexAttribute& attribute : desc.vertex_layout.attributes) {
        VkVertexInputAttributeDescription description{};
        description.location = attribute.location;
        description.binding = attribute.binding;
        description.format = attribute.format;
        description.offset = attribute.offset;
        attributes.push_back(description);
    }

    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = static_cast<u32>(bindings.size());
    vertex_input.pVertexBindingDescriptions = bindings.empty() ? nullptr : bindings.data();
    vertex_input.vertexAttributeDescriptionCount = static_cast<u32>(attributes.size());
    vertex_input.pVertexAttributeDescriptions = attributes.empty() ? nullptr : attributes.data();

    VkPipelineInputAssemblyStateCreateInfo input_assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = desc.topology;
    input_assembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.depthClampEnable = desc.depth_clamp ? VK_TRUE : VK_FALSE;
    rasterization.rasterizerDiscardEnable = VK_FALSE;
    rasterization.polygonMode = desc.polygon_mode;
    rasterization.cullMode = desc.cull_mode;
    rasterization.frontFace = desc.front_face;
    rasterization.depthBiasEnable = desc.depth_bias_enable ? VK_TRUE : VK_FALSE;
    rasterization.depthBiasConstantFactor = desc.depth_bias_constant;
    rasterization.depthBiasSlopeFactor = desc.depth_bias_slope;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = desc.samples;
    multisample.sampleShadingEnable = VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth_stencil.depthTestEnable = desc.depth_test ? VK_TRUE : VK_FALSE;
    depth_stencil.depthWriteEnable = desc.depth_write ? VK_TRUE : VK_FALSE;
    depth_stencil.depthCompareOp = desc.depth_compare;
    depth_stencil.depthBoundsTestEnable = VK_FALSE;
    depth_stencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    VkBlendFactor src_color = VK_BLEND_FACTOR_ONE;
    VkBlendFactor dst_color = VK_BLEND_FACTOR_ZERO;
    VkBlendFactor src_alpha = VK_BLEND_FACTOR_ONE;
    VkBlendFactor dst_alpha = VK_BLEND_FACTOR_ZERO;
    blend_factors(desc.blend, src_color, dst_color, src_alpha, dst_alpha);
    blend_attachment.blendEnable = desc.blend == BlendMode::None ? VK_FALSE : VK_TRUE;
    blend_attachment.srcColorBlendFactor = src_color;
    blend_attachment.dstColorBlendFactor = dst_color;
    blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.srcAlphaBlendFactor = src_alpha;
    blend_attachment.dstAlphaBlendFactor = dst_alpha;
    blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    std::vector<VkPipelineColorBlendAttachmentState> blend_attachments(desc.color_formats.size(), blend_attachment);

    VkPipelineColorBlendStateCreateInfo color_blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    color_blend.logicOpEnable = VK_FALSE;
    color_blend.attachmentCount = static_cast<u32>(blend_attachments.size());
    color_blend.pAttachments = blend_attachments.empty() ? nullptr : blend_attachments.data();

    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dynamic_states;

    // Dynamic rendering: the pipeline declares the attachment formats instead of a render pass.
    VkPipelineRenderingCreateInfo rendering_info{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering_info.colorAttachmentCount = static_cast<u32>(desc.color_formats.size());
    rendering_info.pColorAttachmentFormats = desc.color_formats.empty() ? nullptr : desc.color_formats.data();
    rendering_info.depthAttachmentFormat = desc.depth_format;

    VkGraphicsPipelineCreateInfo create_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    create_info.pNext = &rendering_info;
    create_info.stageCount = static_cast<u32>(stages.size());
    create_info.pStages = stages.data();
    create_info.pVertexInputState = &vertex_input;
    create_info.pInputAssemblyState = &input_assembly;
    create_info.pViewportState = &viewport_state;
    create_info.pRasterizationState = &rasterization;
    create_info.pMultisampleState = &multisample;
    create_info.pDepthStencilState = &depth_stencil;
    create_info.pColorBlendState = &color_blend;
    create_info.pDynamicState = &dynamic_state;
    create_info.layout = desc.layout->handle();
    create_info.renderPass = VK_NULL_HANDLE;

    ORE_VK_CHECK(vkCreateGraphicsPipelines(device.handle(), VK_NULL_HANDLE, 1, &create_info, nullptr,
                                           &pipeline->pipeline_));
    if (!desc.debug_name.empty()) {
        device.set_debug_name(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<u64>(pipeline->pipeline_), desc.debug_name);
    }
    return pipeline;
}

GraphicsPipeline::~GraphicsPipeline() {
    if (pipeline_ != VK_NULL_HANDLE && device_ != nullptr) {
        vkDestroyPipeline(device_->handle(), pipeline_, nullptr);
    }
}

Scope<ComputePipeline> ComputePipeline::create(Device& device, const ComputePipelineDesc& desc) {
    if (desc.shader == nullptr || desc.layout == nullptr) {
        ORE_ERROR("ComputePipeline::create: shader and pipeline layout are required ({})", desc.debug_name);
        return nullptr;
    }

    Scope<ComputePipeline> pipeline(new ComputePipeline());
    pipeline->device_ = &device;
    pipeline->layout_ = desc.layout->handle();

    VkComputePipelineCreateInfo create_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    create_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    create_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    create_info.stage.module = desc.shader->handle();
    create_info.stage.pName = "main";
    create_info.layout = desc.layout->handle();

    ORE_VK_CHECK(vkCreateComputePipelines(device.handle(), VK_NULL_HANDLE, 1, &create_info, nullptr,
                                          &pipeline->pipeline_));
    if (!desc.debug_name.empty()) {
        device.set_debug_name(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<u64>(pipeline->pipeline_), desc.debug_name);
    }
    return pipeline;
}

ComputePipeline::~ComputePipeline() {
    if (pipeline_ != VK_NULL_HANDLE && device_ != nullptr) {
        vkDestroyPipeline(device_->handle(), pipeline_, nullptr);
    }
}

} // namespace ore::rhi
