# Ore framework - shared CMake helpers.

# Applies the framework's compiler settings, warning set and sanitizer configuration.
function(ore_apply_common_settings target)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON)

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
        if(ORE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
            -Wformat=2 -Wundef -Wcast-align -Wdouble-promotion -Wnull-dereference
            -Wno-unused-parameter
            # Vulkan's C structs are routinely initialised with just sType set.
            -Wno-missing-field-initializers)
        if(ORE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()

    if(ORE_ENABLE_SANITIZERS)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

# Convenience: build an executable that links the framework and gets sane defaults.
function(ore_add_example name)
    add_executable(${name} ${ARGN})
    ore_apply_common_settings(${name})
    target_link_libraries(${name} PRIVATE ore::ore)
endfunction()

# Convenience: register a unit test with CTest.
function(ore_add_test name)
    add_executable(${name} ${ARGN})
    ore_apply_common_settings(${name})
    target_link_libraries(${name} PRIVATE ore::ore ore::test_support)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 300)
endfunction()
