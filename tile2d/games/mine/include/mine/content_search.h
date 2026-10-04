// Mine - where the game looks for content nobody told it about.
//
// A designer drops a pack next to the game and runs it. The rule that makes that work is this file: the
// game looks in the **packs directory beside its executable** - "packs" in the directory the game was
// started from (t2d/core/executable.h), not in whatever working directory the player happened to be in
// - and loads everything it finds there: content packs (a *.ecfg file, content_pack.h) and mod
// packages (a directory with a mod.ecfg in it, mod_package.h) alike.
//
// The working directory's own "packs" is a second default, because that is the workspace a designer
// develops in: "cd tile2d && ./build/debug/games/mine/mine_game" still picks up "tile2d/packs". A
// directory both rules name is looked at once.
//
// Neither default is a promise. Nobody asked for them, so a missing one is silent - unlike an explicit
// --packs or --mods, which is reported when it is not there. What was used is on the content list
// screen (content_list.h), one line per source, with its path.
#pragma once

#include <string>
#include <vector>

namespace mine {

/// The name of the directory the rule is about. It is one word, and it is part of the interface a
/// designer sees, so it lives here rather than being spelled out in three places.
inline constexpr const char* kPacksDirectoryName = "packs";

/// The name of the directory the game's own content pack lives in, beside the executable.
inline constexpr const char* kContentDirectoryName = "content";

/// The "packs" directory of the game at \p executable_path - the path of the executable itself, as
/// t2d::executable_path() reports it. Empty when that path names no directory of its own (a bare file
/// name), in which case there is nothing "beside" it to look in.
[[nodiscard]] std::string packs_beside(const std::string& executable_path);

/// The game's own content pack: the "content" directory beside the executable, found the same way the
/// packs directory is - the game's own content is a pack like any other (content_pack.h), it just
/// registers first. Empty when the path names no directory.
[[nodiscard]] std::string content_beside(const std::string& executable_path);

/// The directories a run looks in for content packs and mod packages when the command line does not
/// say, in load order: "packs" beside the executable, then "packs" in \p working_directory. Only the
/// ones that exist are in the list, and a directory both rules name is in it once.
[[nodiscard]] std::vector<std::string> default_content_directories(const std::string& executable_path,
                                                                  const std::string& working_directory);

} // namespace mine
