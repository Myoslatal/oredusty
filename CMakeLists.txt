cmake_minimum_required(VERSION 3.24)

project(Ore
    VERSION 0.1.0
    DESCRIPTION "Ore - a compact C++20 + Vulkan 1.3 game development framework"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CMakeDependentOption)

# ---------------------------------------------------------------- options -----
option(ORE_BUILD_EXAMPLES "Build the example applications" ON)
option(ORE_BUILD_TESTS "Build the unit / integration tests" ON)
option(ORE_ENABLE_VALIDATION "Enable Vulkan validation layers by default (Debug)" ON)
option(ORE_ENABLE_SHADERC "Enable runtime GLSL -> SPIR-V compilation (needs shaderc)" ON)
option(ORE_ENABLE_HOT_RELOAD "Poll shader files for changes and reload them at runtime" ON)
option(ORE_EMBED_SHADERS "Embed SPIR-V binaries into the executables" OFF)
option(ORE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(ORE_ENABLE_SANITIZERS "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(ORE_INSTALL "Generate install rules" ON)
set(ORE_SHADER_TARGET_ENV "vulkan1.3" CACHE STRING "SPIR-V target environment passed to glslc")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# ------------------------------------------------------------ dependencies ----
find_package(Vulkan REQUIRED)
find_package(glfw3 3.3 REQUIRED)
find_package(glm 0.9.9 REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB QUIET)
find_package(PkgConfig QUIET)

set(ORE_WITH_ZLIB OFF)
if(ZLIB_FOUND)
    set(ORE_WITH_ZLIB ON)
endif()

set(ORE_WITH_SHADERC OFF)
if(ORE_ENABLE_SHADERC AND PkgConfig_FOUND)
    # Prefer the shared libshaderc: the static shaderc_combined archive does not carry its
    # glslang/SPIRV-Tools dependencies in its pkg-config file.
    pkg_check_modules(SHADERC QUIET IMPORTED_TARGET shaderc)
    if(TARGET PkgConfig::SHADERC)
        set(ORE_WITH_SHADERC ON)
    else()
        message(STATUS "Ore: shaderc not found - runtime shader compilation disabled")
    endif()
endif()

set(ORE_PLATFORM_LINUX 0)
set(ORE_PLATFORM_WINDOWS 0)
set(ORE_PLATFORM_MACOS 0)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(ORE_PLATFORM_LINUX 1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(ORE_PLATFORM_WINDOWS 1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(ORE_PLATFORM_MACOS 1)
else()
    message(WARNING "Ore: untested platform '${CMAKE_SYSTEM_NAME}'")
endif()

set(ORE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(ORE_SOURCE_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")

# config.h.in is rendered with the *effective* capabilities, not the requested ones.
if(ORE_WITH_ZLIB)
    set(ORE_ENABLE_ZLIB 1)
else()
    set(ORE_ENABLE_ZLIB 0)
endif()
if(ORE_WITH_SHADERC)
    set(ORE_ENABLE_SHADERC 1)
else()
    set(ORE_ENABLE_SHADERC 0)
endif()

configure_file(include/ore/config.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/ore/config.h" @ONLY)

include(cmake/OreHelpers.cmake)
include(cmake/OreShaders.cmake)

# ----------------------------------------------------------------- targets ----
add_subdirectory(src)

if(ORE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(ORE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ----------------------------------------------------------------- install ----
if(ORE_INSTALL)
    include(CMakePackageConfigHelpers)
    install(TARGETS ore EXPORT oreTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(DIRECTORY include/ore DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        PATTERN "config.h.in" EXCLUDE)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/ore/config.h" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ore)

    install(EXPORT oreTargets
        FILE oreTargets.cmake
        NAMESPACE ore::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ore)

    configure_package_config_file(cmake/oreConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/oreConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ore)
    write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/oreConfigVersion.cmake"
        VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/oreConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/oreConfigVersion.cmake"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/OreShaders.cmake"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ore)
endif()

message(STATUS "")
message(STATUS "Ore ${PROJECT_VERSION}  (${CMAKE_BUILD_TYPE})")
message(STATUS "  Vulkan      : ${Vulkan_VERSION} (${Vulkan_LIBRARIES})")
message(STATUS "  glfw        : ${glfw3_VERSION}")
message(STATUS "  glm         : ${glm_VERSION}")
message(STATUS "  glslc       : ${ORE_GLSLC_EXECUTABLE}")
message(STATUS "  validation  : ${ORE_ENABLE_VALIDATION}")
message(STATUS "  shaderc     : ${ORE_WITH_SHADERC}")
message(STATUS "  zlib        : ${ORE_WITH_ZLIB}")
message(STATUS "  examples    : ${ORE_BUILD_EXAMPLES}")
message(STATUS "  tests       : ${ORE_BUILD_TESTS}")
message(STATUS "")
