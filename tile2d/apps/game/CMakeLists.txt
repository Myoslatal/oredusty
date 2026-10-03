add_executable(tile2d_game main.cpp)
t2d_apply_settings(tile2d_game)
target_link_libraries(tile2d_game PRIVATE t2d::render)
ore_add_shaders(tile2d_game SHADERS
    "${CMAKE_SOURCE_DIR}/shaders/sprite.vert"
    "${CMAKE_SOURCE_DIR}/shaders/sprite.frag")
