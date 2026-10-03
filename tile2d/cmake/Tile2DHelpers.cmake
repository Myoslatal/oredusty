# Tile2D - shared CMake helpers.

function(t2d_apply_settings target)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON)

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
        if(T2D_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
            -Wformat=2 -Wundef -Wcast-align -Wdouble-promotion -Wnull-dereference
            -Wno-unused-parameter -Wno-missing-field-initializers)
        if(T2D_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()

    if(T2D_ENABLE_SANITIZERS)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
    if(T2D_ENABLE_THREAD_SANITIZER)
        target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer -g)
        target_link_options(${target} PRIVATE -fsanitize=thread)
    endif()
endfunction()

function(t2d_add_test name)
    add_executable(${name} ${ARGN})
    t2d_apply_settings(${name})
    target_link_libraries(${name} PRIVATE t2d::core t2d::sim t2d::net t2d::engine t2d::test_support)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 300)
endfunction()
