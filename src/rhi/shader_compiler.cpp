#include <ore/rhi/shader_compiler.h>

#include <ore/core/file.h>
#include <ore/core/log.h>

#include <algorithm>
#include <memory>

#if ORE_ENABLE_SHADERC
#include <shaderc/shaderc.hpp>
#endif

namespace ore::rhi {
namespace {

#if ORE_ENABLE_SHADERC
/// Resolves #include directives against a list of search directories.
class FileIncluder final : public shaderc::CompileOptions::IncluderInterface {
public:
    explicit FileIncluder(std::vector<std::string> directories) : directories_(std::move(directories)) {}

    shaderc_include_result* GetInclude(const char* requested_source, shaderc_include_type type,
                                       const char* requesting_source, size_t include_depth) override {
        std::vector<std::string> candidates;
        if (type == shaderc_include_type_relative && requesting_source != nullptr) {
            candidates.push_back(path_join(parent_path(requesting_source), requested_source));
        }
        for (const std::string& directory : directories_) {
            candidates.push_back(path_join(directory, requested_source));
        }
        candidates.emplace_back(requested_source);

        for (const std::string& candidate : candidates) {
            auto content = read_text_file(candidate);
            if (!content.has_value()) continue;
            auto* payload = new Payload{file_name(candidate), std::move(*content)};
            auto* result = new shaderc_include_result();
            result->source_name = payload->name.c_str();
            result->source_name_length = payload->name.size();
            result->content = payload->content.c_str();
            result->content_length = payload->content.size();
            result->user_data = payload;
            return result;
        }

        ORE_WARN("shader include '{}' not found (depth {})", requested_source, include_depth);
        auto* payload = new Payload{std::string("cannot resolve include: ") + requested_source, {}};
        auto* result = new shaderc_include_result();
        result->source_name = payload->name.c_str();
        result->source_name_length = payload->name.size();
        result->content = nullptr; // signals the failure to shaderc
        result->content_length = 0;
        result->user_data = payload;
        return result;
    }

    void ReleaseInclude(shaderc_include_result* data) override {
        delete static_cast<Payload*>(data->user_data);
        delete data;
    }

private:
    struct Payload {
        std::string name;
        std::string content;
    };

    std::vector<std::string> directories_;
};

[[nodiscard]] shaderc_shader_kind to_shaderc_kind(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex: return shaderc_glsl_vertex_shader;
        case ShaderStage::Fragment: return shaderc_glsl_fragment_shader;
        case ShaderStage::Compute: return shaderc_glsl_compute_shader;
        case ShaderStage::Geometry: return shaderc_glsl_geometry_shader;
        case ShaderStage::TessellationControl: return shaderc_glsl_tess_control_shader;
        case ShaderStage::TessellationEvaluation: return shaderc_glsl_tess_evaluation_shader;
    }
    return shaderc_glsl_vertex_shader;
}
#endif

} // namespace

bool runtime_compilation_available() {
#if ORE_ENABLE_SHADERC
    return true;
#else
    return false;
#endif
}

ShaderCompileResult compile_glsl(std::string_view source, const ShaderCompileOptions& options) {
    ShaderCompileResult result;
#if ORE_ENABLE_SHADERC
    shaderc::Compiler compiler;
    shaderc::CompileOptions compile_options;
    compile_options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    compile_options.SetTargetSpirv(shaderc_spirv_version_1_6);
    compile_options.SetSourceLanguage(shaderc_source_language_glsl);
    if (options.optimize) compile_options.SetOptimizationLevel(shaderc_optimization_level_performance);
    if (options.debug_info) compile_options.SetGenerateDebugInfo();
    compile_options.SetIncluder(std::make_unique<FileIncluder>(options.include_directories));

    const std::string name(options.source_name);
    const shaderc::SpvCompilationResult compilation = compiler.CompileGlslToSpv(
        source.data(), source.size(), to_shaderc_kind(options.stage), name.c_str(), "main", compile_options);
    if (compilation.GetCompilationStatus() != shaderc_compilation_status_success) {
        result.error = compilation.GetErrorMessage();
        return result;
    }
    result.spirv.assign(compilation.cbegin(), compilation.cend());
    result.success = !result.spirv.empty();
    return result;
#else
    (void)source;
    (void)options;
    result.error = "this build of Ore has no shaderc support (ORE_ENABLE_SHADERC=OFF)";
    return result;
#endif
}

ShaderCompileResult compile_glsl_file(std::string_view path, std::optional<ShaderStage> override_stage,
                                      ConstSpan<std::string> include_directories) {
    ShaderCompileResult result;
    const auto source = read_text_file(path);
    if (!source.has_value()) {
        result.error = std::string("cannot read '") + std::string(path) + "'";
        return result;
    }

    ShaderCompileOptions options;
    if (override_stage.has_value()) {
        options.stage = *override_stage;
    } else if (!shader_stage_from_path(path, options.stage)) {
        result.error = std::string("cannot infer the shader stage from '") + std::string(path) + "'";
        return result;
    }
    options.source_name = file_name(path);
    options.include_directories.push_back(parent_path(path));
    for (const std::string& directory : include_directories) options.include_directories.push_back(directory);
    return compile_glsl(*source, options);
}

void ShaderHotReloader::watch(std::string_view path) {
    const std::string owned(path);
    if (std::any_of(entries_.begin(), entries_.end(),
                    [&owned](const Entry& entry) { return entry.path == owned; })) {
        return;
    }
    entries_.push_back(Entry{owned, file_mtime_nanos(owned).value_or(0)});
}

void ShaderHotReloader::unwatch(std::string_view path) {
    const std::string owned(path);
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&owned](const Entry& entry) { return entry.path == owned; }),
                   entries_.end());
}

void ShaderHotReloader::clear() { entries_.clear(); }

std::vector<std::string> ShaderHotReloader::poll() {
    std::vector<std::string> changed;
    for (Entry& entry : entries_) {
        const u64 current = file_mtime_nanos(entry.path).value_or(0);
        if (current != entry.mtime) {
            entry.mtime = current;
            changed.push_back(entry.path);
        }
    }
    return changed;
}

} // namespace ore::rhi
