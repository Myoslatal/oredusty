# Example applications. Shaders are compiled into <build>/examples/shaders and located at
# runtime through the ORE_SHADER_DIR compile definition that ore_add_shaders() sets.
set(ORE_EXAMPLE_SHADERS
    "${CMAKE_SOURCE_DIR}/shaders/triangle.vert"
    "${CMAKE_SOURCE_DIR}/shaders/triangle.frag"
    "${CMAKE_SOURCE_DIR}/shaders/mesh.vert"
    "${CMAKE_SOURCE_DIR}/shaders/mesh.frag"
    "${CMAKE_SOURCE_DIR}/shaders/grid.vert"
    "${CMAKE_SOURCE_DIR}/shaders/grid.frag")

# ---------------------------------------------------------------- 01 triangle --
ore_add_example(ore_example_01_triangle 01_triangle/main.cpp)
ore_add_shaders(ore_example_01_triangle SHADERS ${ORE_EXAMPLE_SHADERS})

# ------------------------------------------------------------------- 02 cube ---
ore_add_example(ore_example_02_cube 02_cube/main.cpp)
ore_add_shaders(ore_example_02_cube SHADERS ${ORE_EXAMPLE_SHADERS})

# ------------------------------------------------------------------ 03 scene ---
ore_add_example(ore_example_03_scene 03_scene/main.cpp)
ore_add_shaders(ore_example_03_scene SHADERS ${ORE_EXAMPLE_SHADERS})
