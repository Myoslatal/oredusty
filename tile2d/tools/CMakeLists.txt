# The code table toolchain: a host tool, no graphics and no game. It drives the system compiler and
# packs what it emitted into a table the runtime can merge.
add_executable(t2dtab t2dtab/main.cpp)
t2d_apply_settings(t2dtab)
target_link_libraries(t2dtab PRIVATE t2d::core)
