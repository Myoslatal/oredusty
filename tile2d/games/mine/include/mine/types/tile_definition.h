// Mine - what the designer's data says about a plot, and how a plot is made out of it.
//
// The definitions are data (docs/MODS.md): an entry under a kind's table names a piece of content and
// may carry fields. The engine reads a few of those fields and nothing else - "image", the picture the
// content is drawn with, "random_reverse", whether the map may turn copies of it around when it is
// built, and "size", how many cells the plot covers - for every plot; plus the numbers a mine is made
// of, for the kinds that have them: a **drill** says its "dig_power" (how fast it mines), an **ore**
// says its "hardness" (how long it takes to mine) and its "drop" (what it yields when it is mined), a
// plot that carries a container says its "capacity" (how many things it holds), and a plot that asks
// something of the player says its "tasks" (the chain it works through, docs/GAME_DESIGN.md 1.52).
// Everything else in an entry belongs to the designer, and a field that belongs to another kind is one
// of those - not read, not an error.
//
// This is the definer: it reads an entry into a TileDefinition, and builds the plot a definition
// describes. The kind picks a tile type, and that type's components decide what the plot can do - tick,
// hold things, take clicks or drags, mine, or work through tasks. None of that is a C++ subclass.
#pragma once

#include <mine/types/building.h>
#include <mine/types/research.h>
#include <mine/types/task.h>
#include <mine/types/tile.h>

#include <t2d/core/ecfg.h>
#include <t2d/core/types.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mine {

class ContentDefinitions;

namespace types {

using t2d::f32;
using t2d::usize;

/// The fields the engine reads out of one content entry. Everything else in the entry is the
/// designer's, and the engine never looks at it.
struct TileDefinition {
    ContentKind kind = ContentKind::Item; ///< the table the entry was declared in
    std::string name;                     ///< the entry's name, which is what the registry knows it by
    std::string image;                    ///< "image" as written; empty when the entry has none
    /// "tile_type": the registered tile type this content is made of, by name. Empty means the
    /// built-in mapping by ContentKind (floor, ore, warehouse, drill, generic structure/machine).
    std::string tile_type;
    bool random_reverse = false; ///< "random_reverse"; false when the entry does not say
    /// "size::" - how many cells the plot covers: width and height, 1..4096 each, 1x1 when the entry
    /// does not say. A **definition** attribute like random_reverse: every plot of the entry is this
    /// size, and a placement that names a size of its own overrides it - a save records the footprint
    /// each plot actually had (world.h, PlacedContent).
    i32 width = 1;
    i32 height = 1;
    /// "dig_power" (a drill): how fast it mines, in hardness per second. 0 when the entry does not say,
    /// which is a drill that is reported as one that cannot mine rather than one given a rate.
    f32 dig_power = 0.0f;
    /// "top_image" (a drill): the picture of the part that turns, drawn over "image" - which is the base
    /// it turns on. Empty when the entry does not say, which is a drill drawn as one picture.
    std::string top_image;
    /// "spin" (a drill): how long one full turn of that part takes, in seconds. 0 when the entry does not
    /// say, which is a drill whose top does not turn.
    f32 spin = 0.0f;
    /// "spin_origin" (a drill): the point of the top's **picture** it turns about, in the picture's own
    /// coordinates - (0, 0) is its bottom left corner and (1, 1) its top right. The middle when the entry
    /// does not say, which is a part drawn to turn where it is drawn (docs/GAME_DESIGN.md section 1.38).
    t2d::Vec2 spin_origin{0.5f, 0.5f};
    /// "hardness" (an ore): how long mining it takes, in power-seconds. 0 when the entry does not say.
    f32 hardness = 0.0f;
    /// "drop" (an ore): the name of the drop content it yields when it is mined. Empty when it does not
    /// say - the ore of a drill that has nothing to throw out.
    std::string drop;
    /// "capacity" - how many things the plot's container holds (the stone warehouse, the task block:
    /// docs/GAME_DESIGN.md section 1.51). Read for every plot kind; a plot whose tile type carries no
    /// container simply has nowhere to put it. 0 when the entry does not say, which is a container that
    /// can hold nothing rather than one given a size nobody wrote.
    usize capacity = 0;
    /// "building::" - what this plot costs to build, how long it takes and what it is for
    /// (types/building.h). A **definition** attribute: every plot of this entry shares it, and a layer
    /// placement cannot change it (docs/GAME_DESIGN.md section 1.35). Empty for content that is not a
    /// building - a floor, an ore vein.
    Building building;
    /// "research::" - the technology tree node this entry is, when it declares one (types/research.h).
    /// A definition attribute like building: every plot of the entry shares the node, and a placement
    /// cannot write it (docs/GAME_DESIGN.md section 1.50). Not set for content that is not researchable.
    std::optional<Research> research;
    /// "tasks::" - the chain of tasks this plot works through, in execution order (types/task.h). A
    /// **definition** attribute: every plot of the entry carries the same chain, and each plot keeps its
    /// own progress. Empty for content that asks nothing of the player (docs/GAME_DESIGN.md section
    /// 1.52).
    std::vector<TaskSpec> tasks;

    /// True for the kinds whose entries describe something that occupies cells. An item, a recipe, a
    /// layer or a channel describes something else, and is not a plot.
    [[nodiscard]] bool is_tile() const;
};

/// True for the kinds whose entries describe a plot (docs/GAME_DESIGN.md section 1.12).
[[nodiscard]] bool kind_is_a_tile(ContentKind kind);

/// Reads one entry. \p kind is the table it was declared in, \p name its key. Returns nothing and fills
/// \p error when a field the engine reads is there but is not what it has to be - a field the engine
/// cannot read is refused rather than guessed at.
[[nodiscard]] std::optional<TileDefinition> read_tile_definition(ContentKind kind, std::string_view name,
                                                                 const t2d::EcfgValue& entry,
                                                                 std::string* error = nullptr);

/// Every plot definition \p document declares, in file order. Tables that are not tile kinds are
/// skipped.
[[nodiscard]] std::vector<TileDefinition> tile_definitions(const t2d::EcfgDocument& document,
                                                           std::vector<std::string>* errors = nullptr);

/// Builds the plot a definition describes, at \p anchor on \p layer, with the id the registry handed
/// out for it. Returns nullptr for a definition that is not a plot at all. The definition list provides
/// the runtime shared records and archetype (ContentDefinitions::runtime), so the plot keeps them alive.
/// \p width and \p height are the footprint the caller wants: 0 (the default) means "the size the
/// definition declares", and a value above zero overrides it - a save records the footprint a plot
/// actually had, and a test may build a plot of any size.
[[nodiscard]] std::unique_ptr<Tile> make_tile(const ContentDefinitions& definitions,
                                              const TileDefinition& definition,
                                              const ContentRegistry& registry, ContentId id, i32 layer,
                                              GridPos anchor, i32 width = 0, i32 height = 0);

} // namespace types
} // namespace mine
