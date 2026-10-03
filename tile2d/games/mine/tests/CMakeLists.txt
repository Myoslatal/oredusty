# Pure logic: no window, no GPU, no thread.
t2d_add_test(test_mine_menu test_mine_menu.cpp)
target_link_libraries(test_mine_menu PRIVATE mine_core)

# The content registry and the per-save name -> id tables that keep saves readable.
t2d_add_test(test_registry test_registry.cpp)
target_link_libraries(test_registry PRIVATE mine_core)

# .ecfg content file -> registry -> save table -> loading it back.
t2d_add_test(test_content_loader test_content_loader.cpp)
target_link_libraries(test_content_loader PRIVATE mine_core)
