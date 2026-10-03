cmake_minimum_required(VERSION 3.24)

project(Tile2D
    VERSION 0.1.0
    DESCRIPTION "Tile2D - a 2D tilemap game framework with a semi-separated client/server design"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)

# ---------------------------------------------------------------- options -----
option(T2D_BUILD_MINE "Build the mine game (needs the Ore rendering framework)" ON)
option(T2D_BUILD_TESTS "Build the unit and integration tests" ON)
option(T2D_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(T2D_ENABLE_SANITIZERS "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(T2D_ENABLE_THREAD_SANITIZER "Build with ThreadSanitizer" OFF)
set(T2D_ORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH
    "Ore checkout to build the renderer against (only needed when T2D_BUILD_MINE=ON)")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

find_package(Threads REQUIRED)

# The rendering framework is only needed by the game. The tile map, the text engine, the networking
# and the graphics-free tests build and run without it - that is the whole point of the split.
set(T2D_WITH_RENDERER OFF)
if(T2D_BUILD_MINE)
    find_package(ore 0.1 QUIET)
    if(NOT ore_FOUND AND EXISTS "${T2D_ORE_DIR}/CMakeLists.txt")
        message(STATUS "Tile2D: Ore package not installed, building it from ${T2D_ORE_DIR}")
        set(ORE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(ORE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(ORE_INSTALL OFF CACHE BOOL "" FORCE)
        add_subdirectory("${T2D_ORE_DIR}" "${CMAKE_BINARY_DIR}/ore" EXCLUDE_FROM_ALL)
        set(T2D_WITH_RENDERER ON)
    elseif(ore_FOUND)
        set(T2D_WITH_RENDERER ON)
    else()
        message(WARNING "Tile2D: Ore was not found; building without the renderer "
                        "(set T2D_ORE_DIR or install ore, or configure with -DT2D_BUILD_MINE=OFF)")
        set(T2D_BUILD_MINE OFF)
    endif()
endif()

include(cmake/Tile2DHelpers.cmake)

set(T2D_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
configure_file(include/t2d/config.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/t2d/config.h" @ONLY)

add_subdirectory(src)

if(T2D_BUILD_TESTS)
    enable_testing()
endif()

if(T2D_BUILD_MINE AND T2D_WITH_RENDERER)
    add_subdirectory(games/mine)
endif()

if(T2D_BUILD_TESTS)
    add_subdirectory(tests)
endif()

message(STATUS "")
message(STATUS "Tile2D ${PROJECT_VERSION}  (${CMAKE_BUILD_TYPE})")
message(STATUS "  mine game     : ${T2D_BUILD_MINE} (renderer: ${T2D_WITH_RENDERER})")
message(STATUS "  tests         : ${T2D_BUILD_TESTS}")
message(STATUS "  sanitizers    : asan=${T2D_ENABLE_SANITIZERS} tsan=${T2D_ENABLE_THREAD_SANITIZER}")
message(STATUS "")
