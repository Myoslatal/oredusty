# ------------------------------------------------------------------ t2d_core --
add_library(t2d_core STATIC
    core/ecfg.cpp
    core/log.cpp
    core/rng.cpp
    core/time.cpp
)
add_library(t2d::core ALIAS t2d_core)
t2d_apply_settings(t2d_core)
target_include_directories(t2d_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/../include"
        "${PROJECT_BINARY_DIR}/generated")
target_link_libraries(t2d_core PUBLIC Threads::Threads)
target_compile_definitions(t2d_core PUBLIC T2D_SOURCE_DIR="${T2D_SOURCE_DIR}")

# ------------------------------------------------------------------- t2d_sim --
add_library(t2d_sim STATIC
    sim/tilemap.cpp
    sim/tileset.cpp
    sim/world.cpp
    sim/snapshot.cpp
)
add_library(t2d::sim ALIAS t2d_sim)
t2d_apply_settings(t2d_sim)
target_link_libraries(t2d_sim PUBLIC t2d::core)

# ------------------------------------------------------------------- t2d_net --
add_library(t2d_net STATIC
    net/kcp.cpp
    net/socket.cpp
    net/protocol.cpp
    net/shm_link.cpp
    net/udp_link.cpp
)
add_library(t2d::net ALIAS t2d_net)
t2d_apply_settings(t2d_net)
target_link_libraries(t2d_net PUBLIC t2d::core t2d::sim)

# ---------------------------------------------------------------- t2d_engine --
# Server host + local client. No rendering, no window: the dedicated server and the tests link
# exactly this and nothing else.
add_library(t2d_engine STATIC
    server/server_host.cpp
    client/local_client.cpp
    client/predictor.cpp
    client/snapshot_buffer.cpp
)
add_library(t2d::engine ALIAS t2d_engine)
t2d_apply_settings(t2d_engine)
target_link_libraries(t2d_engine PUBLIC t2d::core t2d::sim t2d::net)

# --------------------------------------------------------------- test_support --
add_library(t2d_test_support STATIC ../tests/support/test_support.cpp)
add_library(t2d::test_support ALIAS t2d_test_support)
t2d_apply_settings(t2d_test_support)
target_include_directories(t2d_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../tests")
target_link_libraries(t2d_test_support PUBLIC t2d::core)

# ----------------------------------------------------------------- t2d_render --
# The 2D renderer and the playable client. Only built when the Ore framework is available: the
# server, the simulation, the networking and every test build without it.
if(T2D_WITH_RENDERER)
    add_library(t2d_render STATIC
        render/atlas.cpp
        render/sprite_batch.cpp
        render/tilemap_renderer.cpp
        render/game_app.cpp
    )
    add_library(t2d::render ALIAS t2d_render)
    t2d_apply_settings(t2d_render)
    target_link_libraries(t2d_render PUBLIC t2d::engine ore::ore)
endif()
