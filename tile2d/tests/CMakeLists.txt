# Unit tests. Everything here is pure CPU work: no window, no threads, no sleeps, milliseconds.
t2d_add_test(test_tilemap test_tilemap.cpp)
# The .ecfg configuration reader (format: example.ecfg in the repository root).
t2d_add_test(test_ecfg test_ecfg.cpp)

# The font engine: containers, cmaps, metrics and TrueType outlines (CFF has its own suite).
t2d_add_test(test_font test_font.cpp)
target_link_libraries(test_font PRIVATE t2d::text)

# UTF-8, the language tables and the interface strings that ship with the shell.
t2d_add_test(test_text test_text.cpp)
target_link_libraries(test_text PRIVATE t2d::text)

# CFF Type 2 outlines: synthetic charstrings for every operator, and every glyph of the CID keyed
# Noto CJK collections that ship with the machine.
t2d_add_test(test_cff test_cff.cpp)
target_link_libraries(test_cff PRIVATE t2d::text)

# The transports: KCP reliability over a lossy link, framing, the session handshake, the map chunk
# transfer and the shared memory rings. No game rides them yet - that is the point of the split.
t2d_add_test(test_kcp test_kcp.cpp)
# The KCP suite prints diagnostics with printf; the %zu/%u mixups are cosmetic, so silence the
# format checker for that one file instead of slowing the protocol work down.
target_compile_options(test_kcp PRIVATE -Wno-format)
t2d_add_test(test_protocol test_protocol.cpp)

# Renderer maths that needs no GPU, so it runs even where no Vulkan device exists.
if(T2D_WITH_RENDERER)
    t2d_add_test(test_sprite_projection test_sprite_projection.cpp)
    target_link_libraries(test_sprite_projection PRIVATE t2d::render)

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
