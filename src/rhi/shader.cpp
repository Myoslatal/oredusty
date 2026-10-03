#include <ore/rhi/shader.h>

#include <ore/core/assert.h>
#include <ore/core/file.h>
#include <ore/core/log.h>

#include <algorithm>
#include <unordered_map>

namespace ore::rhi {
namespace {

[[nodiscard]] std::string lowercase(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

} // namespace

VkShaderStageFlagBits to_vk_stage(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex: return VK_SHADER_STAGE_VERTEX_BIT;
        case ShaderStage::Fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
        case ShaderStage::Compute: return VK_SHADER_STAGE_COMPUTE_BIT;
        case ShaderStage::Geometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
        case ShaderStage::TessellationControl: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        case ShaderStage::TessellationEvaluation: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    }
    return VK_SHADER_STAGE_VERTEX_BIT;
}

const char* shader_stage_name(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex: return "vertex";
        case ShaderStage::Fragment: return "fragment";
        case ShaderStage::Compute: return "compute";
        case ShaderStage::Geometry: return "geometry";
        case ShaderStage::TessellationControl: return "tessellation control";
        case ShaderStage::TessellationEvaluation: return "tessellation evaluation";
    }
    return "unknown";
}

bool shader_stage_from_path(std::string_view path, ShaderStage& stage) {
    const std::string lower = lowercase(path);
    const auto ends_with = [&lower](std::string_view suffix) {
        return lower.size() >= suffix.size() && lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (ends_with(".vert") || ends_with(".vert.spv")) stage = ShaderStage::Vertex;
    else if (ends_with(".frag") || ends_with(".frag.spv")) stage = ShaderStage::Fragment;
    else if (ends_with(".comp") || ends_with(".comp.spv")) stage = ShaderStage::Compute;
    else if (ends_with(".geom") || ends_with(".geom.spv")) stage = ShaderStage::Geometry;
    else if (ends_with(".tesc") || ends_with(".tesc.spv")) stage = ShaderStage::TessellationControl;
    else if (ends_with(".tese") || ends_with(".tese.spv")) stage = ShaderStage::TessellationEvaluation;
    else return false;
    return true;
}

namespace shader_registry {
namespace {

[[nodiscard]] std::unordered_map<std::string, ShaderBlob>& table() {
    static std::unordered_map<std::string, ShaderBlob> blobs;
    return blobs;
}

} // namespace

void add_blobs(ConstSpan<ShaderBlob> blobs) {
    for (const ShaderBlob& blob : blobs) {
        if (blob.data == nullptr || blob.byte_size == 0) continue;
        table()[std::string(blob.name)] = blob;
    }
}

const ShaderBlob* find(std::string_view name) {
    const auto& blobs = table();
    const auto it = blobs.find(std::string(name));
    return it == blobs.end() ? nullptr : &it->second;
}

usize blob_count() { return table().size(); }

void clear() { table().clear(); }

} // namespace shader_registry

Scope<ShaderModule> ShaderModule::create(Device& device, ConstSpan<u32> spirv, std::string_view debug_name) {
    if (spirv.empty()) {
        ORE_ERROR("ShaderModule::create: empty SPIR-V ('{}')", debug_name);
        return nullptr;
    }
    if (spirv[0] != 0x07230203u) {
        ORE_ERROR("ShaderModule::create: '{}' is not SPIR-V (magic = 0x{:08x})", debug_name, spirv[0]);
        return nullptr;
    }

    Scope<ShaderModule> module(new ShaderModule());
    module->device_ = &device;
    module->name_ = std::string(debug_name);

    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = spirv.size() * sizeof(u32);
    info.pCode = spirv.data();
    ORE_VK_CHECK(vkCreateShaderModule(device.handle(), &info, nullptr, &module->module_));
    if (!debug_name.empty()) {
        device.set_debug_name(VK_OBJECT_TYPE_SHADER_MODULE, reinterpret_cast<u64>(module->module_), debug_name);
    }
    return module;
}

Scope<ShaderModule> ShaderModule::load(Device& device, std::string_view spv_path) {
    if (const auto bytes = read_binary_file(spv_path); bytes.has_value() && bytes->size() >= 4) {
        const usize word_count = bytes->size() / sizeof(u32);
        ConstSpan<u32> words(reinterpret_cast<const u32*>(bytes->data()), word_count);
        return create(device, words, file_name(spv_path));
    }

    const std::string name = file_name(spv_path);
    if (const ShaderBlob* blob = shader_registry::find(name); blob != nullptr) {
        ConstSpan<u32> words(blob->data, blob->byte_size / sizeof(u32));
        return create(device, words, name);
    }

    ORE_ERROR("ShaderModule::load: '{}' not found on disk and not embedded", spv_path);
    return nullptr;
}

ShaderModule::~ShaderModule() {
    if (module_ != VK_NULL_HANDLE && device_ != nullptr) vkDestroyShaderModule(device_->handle(), module_, nullptr);
}

} // namespace ore::rhi
