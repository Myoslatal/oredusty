# The start screen is pure logic, so it is tested without a window, a GPU or a thread.
t2d_add_test(test_mine_menu test_mine_menu.cpp)
target_link_libraries(test_mine_menu PRIVATE mine_core)
