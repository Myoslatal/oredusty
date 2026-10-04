// Tile2D - where the running program is, and what is next to it.
//
// A game that is copied somewhere has to be able to find the things that travel with it: content, art,
// packs, a configuration file. The working directory is not that place - it is wherever the player
// happened to be standing - so the framework answers the question the program actually has: which file
// was started, and which directory is it in.
//
// This is deliberately small: a path, its directory, and nothing about what lives there. Deciding what
// to load from beside a program is the game's business (games/mine/content_search.h).
#pragma once

#include <string>
#include <string_view>

namespace t2d {

/// The absolute path of the running executable, or an empty string when the platform will not say
/// (a system that hides it, a process started in a way that loses it). A caller that needs a
/// directory must handle the empty answer rather than assume one.
///
/// The path is the platform's own answer - /proc/self/exe on Linux, the module handle on Windows - so
/// it survives a rename, a symlink and a changed working directory.
[[nodiscard]] std::string executable_path();

/// The directory \p path lives in: "" when it names no directory at all, "/" for a file in the root,
/// and a trailing separator is not part of the answer. Pure string work, so the rule can be tested
/// without a filesystem, and so a caller can use it on a path it has not touched yet.
[[nodiscard]] std::string parent_directory_of(std::string_view path);

/// The directory the running executable is in, or an empty string when executable_path() could not
/// answer.
[[nodiscard]] std::string executable_directory();

} // namespace t2d
