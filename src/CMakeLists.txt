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

# ----------------------------------------------------------- test_support -----
add_library(test_support STATIC ../tests/support/test_support.cpp)
add_library(ore::test_support ALIAS test_support)
ore_apply_common_settings(test_support)
target_include_directories(test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../tests")
target_link_libraries(test_support PUBLIC ore::ore)
