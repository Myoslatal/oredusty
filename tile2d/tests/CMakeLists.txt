# Unit and integration tests.
t2d_add_test(test_tilemap test_tilemap.cpp)
t2d_add_test(test_kcp test_kcp.cpp)
# The KCP suite prints diagnostics with printf; the %zu/%u mixups are cosmetic, so silence the
# format checker for that one file instead of slowing the protocol work down.
target_compile_options(test_kcp PRIVATE -Wno-format)
t2d_add_test(test_integration_local test_integration_local.cpp)

# Multi-process end-to-end test: a dedicated server, two KCP bots, real UDP sockets.
add_test(NAME test_integration_multiprocess
         COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/integration/multiprocess.sh"
                 "${CMAKE_BINARY_DIR}")
set_tests_properties(test_integration_multiprocess PROPERTIES TIMEOUT 180)
