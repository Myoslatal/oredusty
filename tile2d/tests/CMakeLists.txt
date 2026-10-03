# Unit and integration tests.
t2d_add_test(test_tilemap test_tilemap.cpp)
# The .ecfg configuration reader (format: example.ecfg in the repository root).
t2d_add_test(test_ecfg test_ecfg.cpp)
# Pure simulation and wire format: no threads, no sleeps, no wall clock.
t2d_add_test(test_snapshot test_snapshot.cpp)
t2d_add_test(test_kcp test_kcp.cpp)
# Protocol framing, byte streams and the shared memory channel, without threads or a GPU.
t2d_add_test(test_protocol test_protocol.cpp)
# The KCP suite prints diagnostics with printf; the %zu/%u mixups are cosmetic, so silence the
# format checker for that one file instead of slowing the protocol work down.
target_compile_options(test_kcp PRIVATE -Wno-format)
t2d_add_test(test_integration_local test_integration_local.cpp)

# Renderer maths that needs no GPU, so it runs even where no Vulkan device exists.
if(T2D_WITH_RENDERER)
    t2d_add_test(test_sprite_projection test_sprite_projection.cpp)
    target_link_libraries(test_sprite_projection PRIVATE t2d::render)

    # Built-in content: tileset, atlases and the shipped level must agree. CPU only.
    t2d_add_test(test_atlas test_atlas.cpp)
    target_link_libraries(test_atlas PRIVATE t2d::render)

    # Offscreen rendering with real pixel readback (skips when no Vulkan device exists).
    t2d_add_test(test_render_offscreen test_render_offscreen.cpp)
    target_link_libraries(test_render_offscreen PRIVATE t2d::render)
    target_compile_definitions(test_render_offscreen
        PRIVATE T2D_TEST_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders")
    ore_add_shaders(test_render_offscreen SHADERS
        "${CMAKE_SOURCE_DIR}/shaders/sprite.vert"
        "${CMAKE_SOURCE_DIR}/shaders/sprite.frag")

    if(T2D_ENABLE_THREAD_SANITIZER)
        # SwiftShader and the Vulkan loader run their own threads and trip ThreadSanitizer inside
        # code we neither build nor control; tests/tsan.supp keeps those reports out of the run so
        # the remaining warnings are about Tile2D's own threads.
        set_tests_properties(test_render_offscreen PROPERTIES
            ENVIRONMENT "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan.supp")
    endif()
endif()

# Multi-process end-to-end test: a dedicated server, two KCP bots, real UDP sockets.
add_test(NAME test_integration_multiprocess
         COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/integration/multiprocess.sh"
                 "${CMAKE_BINARY_DIR}")
set_tests_properties(test_integration_multiprocess PROPERTIES TIMEOUT 180)
