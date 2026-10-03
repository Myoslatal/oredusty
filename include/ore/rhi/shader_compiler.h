// Ore framework - optional runtime GLSL -> SPIR-V compilation and shader hot reload.
//
// Available when the build found shaderc (ORE_ENABLE_SHADERC). Build-time compilation with
// glslc through ore_add_shaders() is the default path; this module is for tools, tests and
// interactive shader editing.
#pragma once

#include <ore/core/types.h>
#include <ore/rhi/shader.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ore::rhi {

struct ShaderCompileOptions {
    ShaderStage stage = ShaderStage::Vertex;
    std::string source_name = "shader.glsl";
    bool optimize = true;
    bool debug_info = ORE_DEBUG_BUILD != 0;
    std::vector<std::string> include_directories;
};

struct ShaderCompileResult {
    bool success = false;
    std::vector<u32> spirv;
    std::string error;

    [[nodiscard]] explicit operator bool() const { return success; }
};

/// True when the library was built with shaderc support.
[[nodiscard]] bool runtime_compilation_available();

/// Compiles GLSL source to SPIR-V. When shaderc is unavailable the result carries an error.
[[nodiscard]] ShaderCompileResult compile_glsl(std::string_view source, const ShaderCompileOptions& options);
/// Reads \p path and compiles it. The stage is derived from the extension unless \p override_stage is set.
[[nodiscard]] ShaderCompileResult compile_glsl_file(std::string_view path,
                                                   std::optional<ShaderStage> override_stage = std::nullopt,
                                                   ConstSpan<std::string> include_directories = {});

/// Polls shader files and reports the ones whose modification time changed.
class ShaderHotReloader {
public:
    void watch(std::string_view path);
    void unwatch(std::string_view path);
    void clear();
    [[nodiscard]] usize watched_count() const { return entries_.size(); }
    /// Returns the paths that changed since the last call (and refreshes their timestamps).
    [[nodiscard]] std::vector<std::string> poll();

private:
    struct Entry {
        std::string path;
        u64 mtime = 0;
    };
    std::vector<Entry> entries_;
};

} // namespace ore::rhi
