# The code table toolchain: a host tool, no graphics and no game. It drives the system compiler and
# packs what it emitted into a table the runtime can merge.
add_executable(codetab codetab/main.cpp)
t2d_apply_settings(codetab)
target_link_libraries(codetab PRIVATE t2d::core)
