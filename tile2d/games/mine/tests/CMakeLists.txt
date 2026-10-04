# Pure logic: no window, no GPU, no thread.
t2d_add_test(test_mine_menu test_mine_menu.cpp)
target_link_libraries(test_mine_menu PRIVATE mine_core)

# The content registry and the per-save name -> id tables that keep saves readable.
t2d_add_test(test_registry test_registry.cpp)
target_link_libraries(test_registry PRIVATE mine_core)

# .ecfg content file -> registry -> save table -> loading it back.
t2d_add_test(test_content_loader test_content_loader.cpp)
target_link_libraries(test_content_loader PRIVATE mine_core)

# The map grid: packed four byte cells, layers allocated on first write, O(1) fill counters.
t2d_add_test(test_content_grid test_content_grid.cpp)
target_link_libraries(test_content_grid PRIVATE mine_core)

# The single layer sandbox: grid, palette, camera, reload by name, layout save/load.
t2d_add_test(test_sandbox test_sandbox.cpp)
target_link_libraries(test_sandbox PRIVATE mine_core)

# --- content packs and mod packages ---------------------------------------------------------------
# The packs are plain .ecfg files in the source tree: no build step, which is the point of a pack.
t2d_add_test(test_content_pack test_content_pack.cpp)
target_link_libraries(test_content_pack PRIVATE mine_core)
target_compile_definitions(test_content_pack PRIVATE
    T2D_TEST_PACKS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/packs")

# The example packages are assembled in the build tree exactly the way a real install would lay them
# out: a directory per mod, its manifest and data copied next to the library built from its source.
set(MINE_TEST_MODS_DIR "${CMAKE_CURRENT_BINARY_DIR}/mods")

add_library(example_native MODULE mods/example_native/native.cpp)
t2d_apply_settings(example_native)
target_link_libraries(example_native PRIVATE mine_mod_sdk)
set_target_properties(example_native PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${MINE_TEST_MODS_DIR}/example_native"
    OUTPUT_NAME "example_native")

# Two deliberately broken variants, for the checks the host has to make before it calls anything.
add_library(example_native_bad_api MODULE mods/example_native/native.cpp)
t2d_apply_settings(example_native_bad_api)
target_link_libraries(example_native_bad_api PRIVATE mine_mod_sdk)
target_compile_definitions(example_native_bad_api PRIVATE EXAMPLE_MOD_API_VERSION=99)
set_target_properties(example_native_bad_api PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/variants"
    OUTPUT_NAME "example_native_bad_api")

add_library(example_native_fails MODULE mods/example_native/native.cpp)
t2d_apply_settings(example_native_fails)
target_link_libraries(example_native_fails PRIVATE mine_mod_sdk)
target_compile_definitions(example_native_fails PRIVATE EXAMPLE_MOD_FAIL=1)
set_target_properties(example_native_fails PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/variants"
    OUTPUT_NAME "example_native_fails")

file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/mods/example_data" DESTINATION "${MINE_TEST_MODS_DIR}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/mods/example_native/mod.ecfg"
     DESTINATION "${MINE_TEST_MODS_DIR}/example_native")

t2d_add_test(test_mod_package test_mod_package.cpp)
target_link_libraries(test_mod_package PRIVATE mine_core)
target_compile_definitions(test_mod_package PRIVATE
    T2D_TEST_MODS_DIR="${MINE_TEST_MODS_DIR}"
    T2D_TEST_MOD_GOOD="$<TARGET_FILE:example_native>"
    T2D_TEST_MOD_BAD_API="$<TARGET_FILE:example_native_bad_api>"
    T2D_TEST_MOD_FAILS="$<TARGET_FILE:example_native_fails>")
add_dependencies(test_mod_package example_native example_native_bad_api example_native_fails)

# The pipeline test loads all three sources, so it needs the assembled mods directory as well.
target_compile_definitions(test_content_pack PRIVATE
    T2D_TEST_MODS_DIR="${MINE_TEST_MODS_DIR}")
add_dependencies(test_content_pack example_native example_native_bad_api example_native_fails)

# The content-logic types: a plot's identity and footprint, scenery that is refreshed on demand, and
# functional plots that run on a cadence.
t2d_add_test(test_mine_types test_mine_types.cpp)
target_link_libraries(test_mine_types PRIVATE mine_core)

# The list of what the registry was filled from: the same data the game's content screen draws.
t2d_add_test(test_content_list test_content_list.cpp)
target_link_libraries(test_content_list PRIVATE mine_core)
target_compile_definitions(test_content_list PRIVATE
    T2D_TEST_PACKS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/packs"
    T2D_TEST_MODS_DIR="${MINE_TEST_MODS_DIR}")
add_dependencies(test_content_list example_native example_native_bad_api example_native_fails)
