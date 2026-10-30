// Mine - a content entry that can be **researched**: what it costs, what has to come first, which tree
// it belongs to, and the first mine layer it may be researched on (docs/GAME_DESIGN.md section 1.50).
//
// A research node is not a new kind of content: it is a sub-object of an entry that is already one - the
// stone warehouse is a building and a node of the introduction tree. What the engine reads lives here;
// where the node sits in the tree, and whether it is visible or researchable yet, is the tree's business
// (mine/research.h).
#pragma once

#include <mine/types/content_stack.h>

#include <t2d/core/types.h>

#include <string>
#include <vector>

namespace mine::types {

struct Research {
    /// What researching it costs, as the data spells it: an item **name** and a count (the same pairs
    /// building:: materials uses). Resolved against the registry when it is paid for.
    std::vector<ContentStack> cost;
    /// The node names that have to be researched first. A root has none; every other node has at least
    /// one (mine/research.cpp refuses a node that says otherwise).
    std::vector<std::string> parents;
    /// True for a tree's starting node - the one that is visible before anything is researched.
    bool is_root = false;
    /// Which tree the node belongs to ("intro" today): the category the tree screen lists it under.
    std::string tree;
    /// The first mine layer the node may be researched on: 0 means any layer, 1 means from layer one up.
    t2d::i32 min_layer = 0;
};

} // namespace mine::types
