add_executable(tile2d_bot main.cpp)
t2d_apply_settings(tile2d_bot)
# A headless client: it runs the client half of the framework, but draws nothing.
target_link_libraries(tile2d_bot PRIVATE t2d::engine)
