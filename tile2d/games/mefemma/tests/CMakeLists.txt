# Mefemma's tests: pure logic, no window and no GPU. They link mefemma_core, which has no graphics
# dependency, and run in milliseconds.

# The command list a scene fills: what a sprites, rectangles and text turn into, in order.
t2d_add_test(test_draw_list test_draw_list.cpp)
target_link_libraries(test_draw_list PRIVATE mefemma_core)

# The scene stack: deferred push/pop/replace, the top-only update, the all-scenes draw, pause/resume.
t2d_add_test(test_scene_stack test_scene_stack.cpp)
target_link_libraries(test_scene_stack PRIVATE mefemma_core)

# The settings file: defaults, every field the shell reads, and a field that is wrong being reported.
t2d_add_test(test_settings test_settings.cpp)
target_link_libraries(test_settings PRIVATE mefemma_core)

# The frame's input: edges, pointer deltas, wheel and typed text.
t2d_add_test(test_input test_input.cpp)
target_link_libraries(test_input PRIVATE mefemma_core)

# Where the game's files are: beside the executable first, the source tree only in a build tree.
t2d_add_test(test_data_paths test_data_paths.cpp)
target_link_libraries(test_data_paths PRIVATE mefemma_core)

# The first-run save path: redirect.ecfg beside the executable, its path rules, and the UTF-8 path field
# model the prompt will use. No window, no GPU and no scene.
t2d_add_test(test_first_run test_first_run.cpp)
target_link_libraries(test_first_run PRIVATE mefemma_core)

# The two example scenes, driven by a fake GameServices: the commands they draw and what Enter, Escape and
# the keyboard do to the stack and the camera. No window and no GPU - that is the point of a scene.
t2d_add_test(test_scenes test_scenes.cpp)
target_link_libraries(test_scenes PRIVATE mefemma_app)

# The open-world test scene: two test tiles in two test chunks, streamed, edited, saved through the
# ChunkStore, reopened and drawn - all against a temp store path, with no window and no GPU.
t2d_add_test(test_open_world_scene test_open_world_scene.cpp)
target_link_libraries(test_open_world_scene PRIVATE mefemma_app)
