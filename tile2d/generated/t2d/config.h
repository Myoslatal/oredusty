// Tile2D - build configuration. Generated from include/t2d/config.h.in.
#pragma once

#define T2D_VERSION_MAJOR 0
#define T2D_VERSION_MINOR 1
#define T2D_VERSION_PATCH 0
#define T2D_VERSION_STRING "0.1.0"

#define T2D_WITH_RENDERER 1

#if defined(NDEBUG)
#define T2D_DEBUG_BUILD 0
#else
#define T2D_DEBUG_BUILD 1
#endif

#if defined(_WIN32)
#define T2D_PLATFORM_WINDOWS 1
#else
#define T2D_PLATFORM_WINDOWS 0
#endif
#if defined(__linux__)
#define T2D_PLATFORM_LINUX 1
#else
#define T2D_PLATFORM_LINUX 0
#endif
