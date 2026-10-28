# --------------------------------------------------------------- ore library --
add_library(ore STATIC
    core/log.cpp
    core/time.cpp
    core/file.cpp
    core/image.cpp
    core/cli.cpp
    core/block_allocator.cpp

    rhi/vulkan.cpp
    rhi/instance.cpp
    rhi/device.cpp
    rhi/allocator.cpp
    rhi/sync.cpp
    rhi/commands.cpp
    rhi/buffer.cpp
    rhi/texture.cpp
    rhi/shader.cpp
    rhi/shader_compiler.cpp
    rhi/descriptor.cpp
    rhi/pipeline.cpp
    rhi/render_target.cpp
    rhi/swapchain.cpp
    rhi/context.cpp

    platform/window.cpp
    platform/input.cpp

    debug/input_server.cpp

    math/camera.cpp

    renderer/mesh.cpp
    renderer/upload_ring.cpp
    renderer/scene.cpp
    renderer/renderer.cpp

    app/application.cpp
)

add_library(ore::ore ALIAS ore)

ore_apply_common_settings(ore)

target_include_directories(ore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
        $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(ore
    PUBLIC
        Vulkan::Vulkan
        glfw
        glm::glm
        Threads::Threads
    PUBLIC
        $<$<BOOL:${ORE_WITH_ZLIB}>:ZLIB::ZLIB>
        $<$<BOOL:${ORE_WITH_SHADERC}>:PkgConfig::SHADERC>)

target_compile_definitions(ore
    PUBLIC
        ORE_SOURCE_DIR="${ORE_SOURCE_DIR}"
        ORE_SOURCE_SHADER_DIR="${ORE_SOURCE_SHADER_DIR}")

# ---- the Wayland text-input client (see src/platform/wayland_text_input.h) ----
# The generated C is what wayland-scanner wrote from the protocol XML; it is compiled as C and its
# warnings are not ours to fix, so -w. Everything else in window.cpp sees ORE_HAS_WAYLAND_TEXT_INPUT
# and keeps a no-op when the client is not in the build.
if(ORE_HAS_WAYLAND_TEXT_INPUT)
    # wayland-scanner writes the protocol client from the system's wayland-protocols XML. The output has
    # to be declared in this directory, next to the target that compiles it.
    add_custom_command(
        OUTPUT "${ORE_WAYLAND_TEXT_INPUT_HEADER}"
        COMMAND "${ORE_WAYLAND_SCANNER_EXECUTABLE}" client-header
                "${ORE_WAYLAND_TEXT_INPUT_XML}" "${ORE_WAYLAND_TEXT_INPUT_HEADER}"
        DEPENDS "${ORE_WAYLAND_TEXT_INPUT_XML}"
        COMMENT "wayland-scanner: text-input-unstable-v3 client header"
        VERBATIM)
    add_custom_command(
        OUTPUT "${ORE_WAYLAND_TEXT_INPUT_SOURCE}"
        COMMAND "${ORE_WAYLAND_SCANNER_EXECUTABLE}" private-code
                "${ORE_WAYLAND_TEXT_INPUT_XML}" "${ORE_WAYLAND_TEXT_INPUT_SOURCE}"
        DEPENDS "${ORE_WAYLAND_TEXT_INPUT_XML}"
        COMMENT "wayland-scanner: text-input-unstable-v3 protocol code"
        VERBATIM)
    target_sources(ore PRIVATE
        platform/wayland_text_input.cpp
        "${ORE_WAYLAND_TEXT_INPUT_SOURCE}")
    target_include_directories(ore PRIVATE "${ORE_WAYLAND_TEXT_INPUT_DIR}")
    target_link_libraries(ore PRIVATE PkgConfig::WAYLAND_CLIENT)
    set_source_files_properties("${ORE_WAYLAND_TEXT_INPUT_SOURCE}" PROPERTIES
        LANGUAGE C
        COMPILE_OPTIONS "-w")
    set_source_files_properties(platform/wayland_text_input.cpp PROPERTIES
        OBJECT_DEPENDS "${ORE_WAYLAND_TEXT_INPUT_HEADER}")
endif()
target_compile_definitions(ore PRIVATE ORE_HAS_WAYLAND_TEXT_INPUT=${ORE_HAS_WAYLAND_TEXT_INPUT})

# ----------------------------------------------------------- test_support -----
add_library(test_support STATIC ../tests/support/test_support.cpp)
add_library(ore::test_support ALIAS test_support)
ore_apply_common_settings(test_support)
target_include_directories(test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../tests")
target_link_libraries(test_support PUBLIC ore::ore)
