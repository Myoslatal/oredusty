// Pure logic of the RHI layer that does not need a device: flush range alignment, barrier stage
// derivation, enum mappings and subresource ranges. These rules are easy to get subtly wrong and
// impossible to notice without validation layers, so they are pinned down here.
#include <ore/rhi/buffer.h>
#include <ore/rhi/commands.h>
#include <ore/rhi/shader.h>

#include <support/test_support.h>

using namespace ore;
using namespace ore::rhi;

ORE_TEST(flush_range_aligns_to_the_atom_size) {
    // A 100 byte write at offset 40 with a 64 byte atom: the range must cover [0, 192).
    const FlushRange range = align_flush_range(40, 100, 4096, 64);
    ORE_CHECK_EQ(range.offset, 0u);
    ORE_CHECK_EQ(range.size, 192u);

    // Already aligned writes stay untouched.
    const FlushRange aligned = align_flush_range(128, 256, 4096, 64);
    ORE_CHECK_EQ(aligned.offset, 128u);
    ORE_CHECK_EQ(aligned.size, 256u);

    // The tail is clamped to the allocation size instead of running past it.
    const FlushRange tail = align_flush_range(4000, 1000, 4096, 64);
    ORE_CHECK_EQ(tail.offset, 3968u);
    ORE_CHECK_EQ(tail.size, 128u);

    // Whole-range flushes are passed through as VK_WHOLE_SIZE.
    const FlushRange whole = align_flush_range(0, VK_WHOLE_SIZE, 4096, 64);
    ORE_CHECK_EQ(whole.offset, 0u);
    ORE_CHECK_EQ(whole.size, VK_WHOLE_SIZE);

    // Degenerate inputs must not produce a bogus range.
    ORE_CHECK_EQ(align_flush_range(10, 5, 4096, 0).size, 5u);
    ORE_CHECK_EQ(align_flush_range(0, 0, 4096, 64).size, 0u);
    const FlushRange beyond = align_flush_range(8192, 16, 4096, 64);
    ORE_CHECK_EQ(beyond.offset, 4096u);
    ORE_CHECK_EQ(beyond.size, 0u);
}

ORE_TEST(flush_range_covers_the_request) {
    // Whatever the alignment, the original byte range has to stay inside the flushed range.
    for (u64 offset : {0u, 1u, 63u, 64u, 65u, 4095u}) {
        for (u64 size : {1u, 7u, 64u, 100u, 4096u}) {
            const FlushRange range = align_flush_range(offset, size, 8192, 256);
            ORE_CHECK_MSG(range.offset <= offset, "offset {} not covered by {}", offset, range.offset);
            ORE_CHECK_MSG(range.offset + range.size >= offset + size, "range [{}, {}) does not cover [{}, {})",
                          range.offset, range.offset + range.size, offset, offset + size);
            ORE_CHECK_EQ(range.offset % 256u, 0u);
            ORE_CHECK_EQ(range.size % 256u, 0u);
        }
    }
}

ORE_TEST(barrier_defaults_cover_the_transitions_the_renderer_uses) {
    const auto stages = [](VkImageLayout from, VkImageLayout to) {
        VkPipelineStageFlags2 src_stage = 0;
        VkAccessFlags2 src_access = 0;
        VkPipelineStageFlags2 dst_stage = 0;
        VkAccessFlags2 dst_access = 0;
        default_barrier_stages(from, to, src_stage, src_access, dst_stage, dst_access);
        return std::array<VkPipelineStageFlags2, 2>{src_stage, dst_stage};
    };

    // UNDEFINED -> colour attachment: nothing to wait for, but the write must be made visible.
    const auto to_color = stages(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ORE_CHECK_EQ(to_color[0], VK_PIPELINE_STAGE_2_NONE);
    ORE_CHECK((to_color[1] & VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) != 0u);

    // Colour attachment -> present: the queue must finish writing before presentation.
    const auto to_present = stages(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    ORE_CHECK((to_present[0] & VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) != 0u);
    ORE_CHECK_EQ(to_present[1], VK_PIPELINE_STAGE_2_NONE);

    // Transfer -> shader read: the copy must complete before the texture is sampled.
    const auto to_sample = stages(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ORE_CHECK((to_sample[0] & VK_PIPELINE_STAGE_2_COPY_BIT) != 0u);
    ORE_CHECK((to_sample[1] & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT) != 0u);

    // Depth attachment round trip.
    const auto to_depth = stages(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    ORE_CHECK((to_depth[1] & VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT) != 0u);
}

ORE_TEST(subresource_range_helper) {
    const VkImageSubresourceRange range = subresource_range(VK_IMAGE_ASPECT_COLOR_BIT, 4, 2, 1, 3);
    ORE_CHECK_EQ(range.aspectMask, VK_IMAGE_ASPECT_COLOR_BIT);
    ORE_CHECK_EQ(range.levelCount, 4u);
    ORE_CHECK_EQ(range.layerCount, 2u);
    ORE_CHECK_EQ(range.baseMipLevel, 1u);
    ORE_CHECK_EQ(range.baseArrayLayer, 3u);
}

ORE_TEST(buffer_usage_and_shader_stage_mappings) {
    const VkBufferUsageFlags vertex = to_vk_usage(BufferUsage::Vertex | BufferUsage::TransferDst);
    ORE_CHECK((vertex & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) != 0u);
    ORE_CHECK((vertex & VK_BUFFER_USAGE_TRANSFER_DST_BIT) != 0u);
    ORE_CHECK((vertex & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0u);
    // An empty usage still has to produce something bindable.
    ORE_CHECK((to_vk_usage(BufferUsage::None) & VK_BUFFER_USAGE_TRANSFER_DST_BIT) != 0u);
    ORE_CHECK(has_flag(BufferUsage::Uniform | BufferUsage::Storage, BufferUsage::Storage));
    ORE_CHECK_FALSE(has_flag(BufferUsage::Uniform, BufferUsage::Storage));

    ORE_CHECK_EQ(to_vk_stage(ShaderStage::Vertex), VK_SHADER_STAGE_VERTEX_BIT);
    ORE_CHECK_EQ(to_vk_stage(ShaderStage::Fragment), VK_SHADER_STAGE_FRAGMENT_BIT);
    ORE_CHECK_EQ(to_vk_stage(ShaderStage::Compute), VK_SHADER_STAGE_COMPUTE_BIT);
}

ORE_TEST(format_helpers_classify_depth_and_srgb) {
    ORE_CHECK(is_depth_format(VK_FORMAT_D32_SFLOAT));
    ORE_CHECK(is_depth_format(VK_FORMAT_D24_UNORM_S8_UINT));
    ORE_CHECK_FALSE(is_depth_format(VK_FORMAT_R8G8B8A8_UNORM));
    ORE_CHECK(is_srgb_format(VK_FORMAT_B8G8R8A8_SRGB));
    ORE_CHECK_FALSE(is_srgb_format(VK_FORMAT_B8G8R8A8_UNORM));
    ORE_CHECK_EQ(result_string(VK_ERROR_OUT_OF_DATE_KHR), std::string("VK_ERROR_OUT_OF_DATE_KHR"));
    ORE_CHECK_EQ(api_version_string(VK_API_VERSION_1_3), std::string("1.3.0"));
    ORE_CHECK(format_string(VK_FORMAT_R8G8B8A8_UNORM).find("R8G8B8A8") != std::string::npos);
}

ORE_TEST_MAIN
