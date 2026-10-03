# Mine - the game on top of Tile2D. Only built when the renderer is available.
#
# mine_core is deliberately free of any graphics dependency (and of any game content): it holds the
# session description and the start screen model, so it is testable in milliseconds and reusable by
# a dedicated server later.

add_library(mine_core STATIC
    src/menu.cpp
    src/registry.cpp
)
add_library(mine::core ALIAS mine_core)
t2d_apply_settings(mine_core)
target_include_directories(mine_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(mine_core PUBLIC t2d::core)

add_library(mine_app STATIC
    src/app.cpp
)
add_library(mine::app ALIAS mine_app)
t2d_apply_settings(mine_app)
target_link_libraries(mine_app PUBLIC mine_core t2d::render)

add_executable(mine_game src/main.cpp)
t2d_apply_settings(mine_game)
target_link_libraries(mine_game PRIVATE mine_app)
ore_add_shaders(mine_game SHADERS
    "${CMAKE_SOURCE_DIR}/shaders/sprite.vert"
    "${CMAKE_SOURCE_DIR}/shaders/sprite.frag")

if(T2D_BUILD_TESTS)
    add_subdirectory(tests)
endif()
