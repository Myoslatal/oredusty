// Mine - a count of one kind of content, named the way the data names it.
//
// It is what a **definition** says a building costs, and what a **layer placement** says a container
// starts with (docs/GAME_DESIGN.md sections 1.33, 1.34). The name is resolved against the registry where
// the thing is built, never in the file: a mod can add an item long after a content file was written.
#pragma once

#include <t2d/core/types.h>

#include <string>

namespace mine {

struct ContentStack {
    std::string name;
    t2d::i64 count = 0;
};

} // namespace mine
