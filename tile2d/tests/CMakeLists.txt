# Unit tests. Everything here is pure CPU work: no window, no threads, no sleeps, milliseconds.
t2d_add_test(test_tilemap test_tilemap.cpp)
# The camera: coordinates, zooming, fitting a map of hundreds of cells and bounding the view.
t2d_add_test(test_camera2d test_camera2d.cpp)
# The .ecfg configuration reader (format: example.ecfg in the repository root).
t2d_add_test(test_ecfg test_ecfg.cpp)

# Where the running program is: the path the platform reports, and the directory rule that turns it
# into "what is next to the game".
t2d_add_test(test_executable test_executable.cpp)

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

# The dynamic loader: a MODULE library built from tests/fixtures, loaded at run time.
add_library(t2d_test_module MODULE fixtures/module_fixture.cpp)
t2d_apply_settings(t2d_test_module)
t2d_add_test(test_module test_module.cpp)
target_compile_definitions(test_module PRIVATE T2D_TEST_MODULE_PATH="$<TARGET_FILE:t2d_test_module>")
# The path is baked in at configure time, so the library has to exist before the test runs.
add_dependencies(test_module t2d_test_module)


# --- code tables ------------------------------------------------------------------------------------
# The fixtures are compiled by the build and *not* linked into the test: a table holds exactly what the
# compiler emitted, so the test has to read a real object. The flags are the ones the toolchain uses,
# and -fsemantic-interposition is the one that matters: without it the call the tests replace would
# have been inlined away before the table ever saw it.
set(T2D_TABLE_FIXTURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/tables")
set(T2D_TABLE_FIXTURE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/data/tables")
set(T2D_TABLE_FIXTURE_FLAGS
    -std=c++20 -O2 -fPIC -fsemantic-interposition -ffunction-sections -fdata-sections
    -fno-stack-protector -fno-exceptions
    -I${T2D_TABLE_FIXTURE_SOURCES})
set(T2D_TABLE_FIXTURE_OBJECTS "")
foreach(fixture base caller mod missing machine factory mod_machine)
    add_custom_command(
        OUTPUT "${T2D_TABLE_FIXTURE_DIR}/${fixture}.o"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${T2D_TABLE_FIXTURE_DIR}"
        COMMAND ${CMAKE_CXX_COMPILER} ${T2D_TABLE_FIXTURE_FLAGS}
                -c "${T2D_TABLE_FIXTURE_SOURCES}/${fixture}.cpp"
                -o "${T2D_TABLE_FIXTURE_DIR}/${fixture}.o"
        DEPENDS "${T2D_TABLE_FIXTURE_SOURCES}/${fixture}.cpp" "${T2D_TABLE_FIXTURE_SOURCES}/machine.h"
        COMMENT "Compiling the code table fixture ${fixture}.cpp")
    list(APPEND T2D_TABLE_FIXTURE_OBJECTS "${T2D_TABLE_FIXTURE_DIR}/${fixture}.o")
endforeach()
add_custom_target(t2d_table_fixtures DEPENDS ${T2D_TABLE_FIXTURE_OBJECTS})

# The object reader, the table format, and the merge that lets one table replace another's symbols.
t2d_add_test(test_code_table test_code_table.cpp)
target_compile_definitions(test_code_table PRIVATE
    T2D_TEST_TABLE_DIR="${T2D_TABLE_FIXTURE_DIR}"
    T2D_TEST_TABLE_SOURCES="${T2D_TABLE_FIXTURE_SOURCES}"
    T2D_TEST_TOOLCHAIN="$<TARGET_FILE:t2dtab>"
    T2D_TEST_COMPILER="${CMAKE_CXX_COMPILER}")
add_dependencies(test_code_table t2d_table_fixtures t2dtab)

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
