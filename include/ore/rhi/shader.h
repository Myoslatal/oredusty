// Ore framework - SPIR-V shader modules and the embedded shader registry.
#pragma once

#include <ore/core/types.h>
#include <ore/rhi/device.h>

#include <string>
#include <string_view>

namespace ore::rhi {

enum class ShaderStage : u8 { Vertex = 0, Fragment, Compute, Geometry, TessellationControl, TessellationEvaluation };

[[nodiscard]] VkShaderStageFlagBits to_vk_stage(ShaderStage stage);
[[nodiscard]] const char* shader_stage_name(ShaderStage stage);
/// Maps "foo.vert.spv" / "foo.frag" to the matching stage. Returns false for unknown extensions.
[[nodiscard]] bool shader_stage_from_path(std::string_view path, ShaderStage& stage);

/// One SPIR-V binary. Generated code from cmake/EmbedSpirv.cmake produces an array of these.
struct ShaderBlob {
    std::string_view name;
    const u32* data = nullptr;
    usize byte_size = 0;
};

/// Lets a build embed SPIR-V into the executable instead of shipping .spv files.
namespace shader_registry {
void add_blobs(ConstSpan<ShaderBlob> blobs);
[[nodiscard]] const ShaderBlob* find(std::string_view name);
[[nodiscard]] usize blob_count();
void clear();
} // namespace shader_registry

class ShaderModule {
public:
    [[nodiscard]] static Scope<ShaderModule> create(Device& device, ConstSpan<u32> spirv,
                                                   std::string_view debug_name = {});
    /// Loads a .spv file; falls back to the embedded registry when the file is missing.
    [[nodiscard]] static Scope<ShaderModule> load(Device& device, std::string_view spv_path);
    ~ShaderModule();
    ORE_NON_MOVABLE(ShaderModule);

    [[nodiscard]] VkShaderModule handle() const { return module_; }
    [[nodiscard]] const std::string& name() const { return name_; }

private:
    ShaderModule() = default;
    Device* device_ = nullptr;
    VkShaderModule module_ = VK_NULL_HANDLE;
    std::string name_;
};

} // namespace ore::rhi
