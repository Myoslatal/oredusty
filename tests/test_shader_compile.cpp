// Runtime GLSL -> SPIR-V compilation (shaderc) and shader hot reload tracking.
#include <ore/core/file.h>
#include <ore/rhi/shader_compiler.h>

#include <support/test_support.h>

#include <cstdio>
#include <string>

using namespace ore;

namespace {

[[nodiscard]] std::string shader_dir() { return ORE_SOURCE_SHADER_DIR; }

} // namespace

ORE_TEST(shader_compiler_availability) {
    if (!rhi::runtime_compilation_available()) {
        ORE_SKIP("this build has no shaderc support (ORE_ENABLE_SHADERC=OFF)");
    }
    ORE_CHECK(rhi::runtime_compilation_available());
}

ORE_TEST(shader_compiler_compiles_repo_shaders) {
    if (!rhi::runtime_compilation_available()) ORE_SKIP("no shaderc");

    const char* const names[] = {"triangle.vert", "triangle.frag", "mesh.vert", "mesh.frag", "grid.vert", "grid.frag"};
    for (const char* name : names) {
        const std::string path = path_join(shader_dir(), name);
        if (!path_exists(path)) ORE_SKIP("shaders are not available next to the source tree");
        const rhi::ShaderCompileResult result = rhi::compile_glsl_file(path);
        ORE_CHECK_MSG(result.success, "failed to compile {}: {}", name, result.error);
        ORE_REQUIRE(result.success);
        ORE_CHECK(result.spirv.size() > 5u);
        ORE_CHECK_EQ(result.spirv.front(), 0x07230203u); // SPIR-V magic
    }
}

ORE_TEST(shader_compiler_reports_errors) {
    if (!rhi::runtime_compilation_available()) ORE_SKIP("no shaderc");

    rhi::ShaderCompileOptions options;
    options.stage = rhi::ShaderStage::Fragment;
    options.source_name = "broken.frag";
    const rhi::ShaderCompileResult result =
        rhi::compile_glsl("#version 450\nlayout(location=0) out vec4 c;\nvoid main(){ c = undefined_symbol; }\n",
                          options);
    ORE_CHECK_FALSE(result.success);
    ORE_CHECK_FALSE(result.error.empty());
    ORE_CHECK(result.spirv.empty());
}

ORE_TEST(shader_compiler_resolves_includes) {
    if (!rhi::runtime_compilation_available()) ORE_SKIP("no shaderc");

    const std::string include_path = path_join(current_dir(), "ore_test_include.glsl");
    const std::string main_path = path_join(current_dir(), "ore_test_main.glsl");
    ORE_REQUIRE(write_text_file(include_path, "#define SCALE 2.0\n"));
    ORE_REQUIRE(write_text_file(main_path,
                                "#version 450\n#include \"ore_test_include.glsl\"\n"
                                "layout(location=0) out vec4 c;\nvoid main(){ c = vec4(SCALE); }\n"));

    rhi::ShaderCompileOptions options;
    options.stage = rhi::ShaderStage::Fragment;
    options.source_name = main_path;
    options.include_directories.push_back(current_dir());
    const auto source = read_text_file(main_path);
    ORE_REQUIRE(source.has_value());
    const rhi::ShaderCompileResult result = rhi::compile_glsl(*source, options);
    ORE_CHECK_MSG(result.success, "include resolution failed: {}", result.error);

    std::remove(include_path.c_str());
    std::remove(main_path.c_str());
}

ORE_TEST(shader_stage_detection) {
    rhi::ShaderStage stage = rhi::ShaderStage::Vertex;
    ORE_CHECK(rhi::shader_stage_from_path("cube.vert.spv", stage));
    ORE_CHECK(stage == rhi::ShaderStage::Vertex);
    ORE_CHECK(rhi::shader_stage_from_path("cube.frag", stage));
    ORE_CHECK(stage == rhi::ShaderStage::Fragment);
    ORE_CHECK(rhi::shader_stage_from_path("post.comp.spv", stage));
    ORE_CHECK(stage == rhi::ShaderStage::Compute);
    ORE_CHECK_FALSE(rhi::shader_stage_from_path("readme.md", stage));
}

ORE_TEST(hot_reloader_detects_changes) {
    const std::string path = path_join(current_dir(), "ore_test_watched.glsl");
    ORE_REQUIRE(write_text_file(path, "#version 450\n"));

    rhi::ShaderHotReloader reloader;
    reloader.watch(path);
    reloader.watch(path); // duplicate registrations are ignored
    ORE_CHECK_EQ(reloader.watched_count(), 1u);
    ORE_CHECK(reloader.poll().empty()); // nothing changed yet

    // Touch the file with new content; mtime granularity is nanoseconds on Linux.
    ORE_REQUIRE(write_text_file(path, "#version 450\n// changed\n"));
    const std::vector<std::string> changed = reloader.poll();
    ORE_REQUIRE(changed.size() == 1u);
    ORE_CHECK_EQ(changed[0], path);
    ORE_CHECK(reloader.poll().empty());

    reloader.unwatch(path);
    ORE_CHECK_EQ(reloader.watched_count(), 0u);
    std::remove(path.c_str());
}

ORE_TEST(shader_registry_stores_embedded_blobs) {
    const u32 spirv[] = {0x07230203u, 0x00010000u, 0x00000000u, 0x00000001u};
    const rhi::ShaderBlob blobs[] = {{"embedded.test.spv", spirv, sizeof(spirv)}};
    rhi::shader_registry::add_blobs(blobs);
    const rhi::ShaderBlob* found = rhi::shader_registry::find("embedded.test.spv");
    ORE_REQUIRE(found != nullptr);
    ORE_CHECK_EQ(found->byte_size, sizeof(spirv));
    ORE_CHECK_EQ(found->data[0], 0x07230203u);
    ORE_CHECK(rhi::shader_registry::find("missing.spv") == nullptr);
    rhi::shader_registry::clear();
    ORE_CHECK(rhi::shader_registry::find("embedded.test.spv") == nullptr);
}

ORE_TEST_MAIN
