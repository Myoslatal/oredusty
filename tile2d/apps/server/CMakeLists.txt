add_executable(tile2d_server main.cpp)
t2d_apply_settings(tile2d_server)
# Deliberately no renderer, no window, no Ore: the dedicated server is simulation + networking.
target_link_libraries(tile2d_server PRIVATE t2d::engine)
