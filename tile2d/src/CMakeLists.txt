# ------------------------------------------------------------------ t2d_core --
add_library(t2d_core STATIC
    core/camera2d.cpp
    core/code_table.cpp
    core/ecfg.cpp
    core/executable.cpp
    core/log.cpp
    core/module.cpp
    core/object_file.cpp
    core/rng.cpp
    core/time.cpp
)
add_library(t2d::core ALIAS t2d_core)
t2d_apply_settings(t2d_core)
target_include_directories(t2d_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/../include"
        "${PROJECT_BINARY_DIR}/generated")
# ${CMAKE_DL_LIBS} is where dlopen lives on this platform (empty on glibc 2.34+ and on Windows).
target_link_libraries(t2d_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
target_compile_definitions(t2d_core PUBLIC T2D_SOURCE_DIR="${T2D_SOURCE_DIR}")

# ------------------------------------------------------------------- t2d_sim --
# The tile map and the tileset: storage, collision queries, serialisation, ASCII authoring. What a
# tile means is the game's data; the framework only knows how to store and query it.
add_library(t2d_sim STATIC
    sim/tilemap.cpp
    sim/tileset.cpp
)
add_library(t2d::sim ALIAS t2d_sim)
t2d_apply_settings(t2d_sim)
target_link_libraries(t2d_sim PUBLIC t2d::core)

# ------------------------------------------------------------------ t2d_text --
# The font engine (TrueType and CFF outlines, rasterising, layout) and the localisation tables. No
# graphics: a dedicated server can measure and lay out text without a GPU.
add_library(t2d_text STATIC
    text/cff.cpp
    text/font.cpp
    text/locale.cpp
    text/raster.cpp
    text/text_layout.cpp
    text/utf8.cpp
)
add_library(t2d::text ALIAS t2d_text)
t2d_apply_settings(t2d_text)
target_link_libraries(t2d_text PUBLIC t2d::core)

# ------------------------------------------------------------------- t2d_net --
# The semi-separated channel: shared memory rings for a local client, KCP over UDP for remote ones,
# the framing and the session handshake every link shares. It carries bytes and sessions, not a game.
add_library(t2d_net STATIC
    net/kcp.cpp
    net/socket.cpp
    net/protocol.cpp
    net/shm_link.cpp
    net/udp_link.cpp
)
add_library(t2d::net ALIAS t2d_net)
t2d_apply_settings(t2d_net)
target_link_libraries(t2d_net PUBLIC t2d::core)

# --------------------------------------------------------------- test_support --
add_library(t2d_test_support STATIC ../tests/support/test_support.cpp)
add_library(t2d::test_support ALIAS t2d_test_support)
t2d_apply_settings(t2d_test_support)
target_include_directories(t2d_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../tests")
target_link_libraries(t2d_test_support PUBLIC t2d::core)

# ----------------------------------------------------------------- t2d_render --
# The 2D renderer. Only built when the Ore framework is available: the simulation, the networking and
# every graphics-free test build without it.
if(T2D_WITH_RENDERER)
    add_library(t2d_render STATIC
        render/atlas.cpp
        render/glyph_atlas.cpp
        render/image_atlas.cpp
        render/sprite_batch.cpp
        render/text_renderer.cpp
        render/tilemap_renderer.cpp
    )
    add_library(t2d::render ALIAS t2d_render)
    t2d_apply_settings(t2d_render)
    target_link_libraries(t2d_render PUBLIC t2d::net t2d::text ore::ore)
endif()
