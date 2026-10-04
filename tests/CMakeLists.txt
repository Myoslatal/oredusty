# Unit tests. Each test is a standalone executable registered with CTest.
ore_add_test(test_core_block_allocator test_core_block_allocator.cpp)
ore_add_test(test_core_image test_core_image.cpp)
ore_add_test(test_core_cli test_core_cli.cpp)
ore_add_test(test_core_time test_core_time.cpp)
ore_add_test(test_input_map test_input_map.cpp)
# The debug input server: commands over a loopback socket, applied to an InputState.
ore_add_test(test_debug_input test_debug_input.cpp)
ore_add_test(test_shader_compile test_shader_compile.cpp)
ore_add_test(test_scene test_scene.cpp)
ore_add_test(test_camera test_camera.cpp)
ore_add_test(test_rhi_helpers test_rhi_helpers.cpp)

# GPU tests run against whatever Vulkan device the machine exposes; they self-skip when no
# device is available (CI containers, for example). They render offscreen and assert on pixels.
ore_add_test(test_gpu_smoke test_gpu_smoke.cpp)
ore_add_test(test_gpu_render test_gpu_render.cpp)
ore_add_shaders(test_gpu_render SHADERS shaders/solid.vert shaders/solid.frag)

# Presentation path: a real swapchain through VK_EXT_headless_surface (skips when unavailable).
ore_add_test(test_gpu_swapchain test_gpu_swapchain.cpp)
