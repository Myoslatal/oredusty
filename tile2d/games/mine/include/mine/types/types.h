// Mine - mine::types: the game's content logic, in C++.
//
// Content has two halves. The **definitions** are data: the designer writes them in .ecfg tables
// (item::, structure::, machine:: ...) and the registry hands out the ids a save stores
// (mine/registry.h). The **logic** is C++ and lives here: the types that say what a piece of content
// *is* and *does* once it is part of a mine.
//
// What belongs in this namespace:
//   * the types of the things a mine is made of - plots first (types/tile.h), and whatever the mine
//     turns out to need next (logistics, demands, layer rules);
//   * anything those types need in order to do their job, as long as it is about the mine and not
//     about the engine.
// What does not belong here:
//   * the definitions themselves. No resource, structure, machine or recipe is named in C++: what
//     exists is what the designer's data says exists (docs/GAME_DESIGN.md section 7);
//   * storage, loading and the interface - the map (mine/content_grid.h), the registry
//     (mine/registry.h), the sandbox and the screens (mine/app.h, mine/sandbox.h), and anything that
//     knows about windows, files or the GPU.
//
// The namespace is the contract between the two halves: data says *what*, these types say *how*.
#pragma once

#include <mine/types/entity_tile.h>
#include <mine/types/floor.h>
#include <mine/types/scene_tile.h>
#include <mine/types/single_ore.h>
#include <mine/types/tile.h>
#include <mine/types/tile_definition.h>
