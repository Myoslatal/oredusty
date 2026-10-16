// Mine - what the designer's data says about a plot, and how a plot is made out of it.
//
// The definitions are data (docs/MODS.md): an entry under a kind's table names a piece of content and
// may carry fields. The engine reads a few of those fields and nothing else - "image", the picture the
// content is drawn with, and "random_reverse", whether the map may turn copies of it around when it is
// built, for every plot; plus the two numbers a mine is made of, for the kinds that have them: a
// **drill** says its "dig_power" (how fast it mines), an **ore** says its "hardness" (how long it takes
// to mine) and its "drop" (what it yields when it is mined), and a **warehouse** says its "capacity" (how
// many things it holds). Everything else in an entry belongs to the designer, and a field that belongs to
// another kind is one of those - not read, not an error.
//
// This is the definer: it reads an entry into a TileDefinition, and builds the plot a definition
// describes. Which class a definition becomes is decided by its kind - a floor is a Floor, a machine is
// an EntityTile (something that also runs), any other structure is scenery - and that mapping is
// engineering: it is about what the thing *is*, not about which content exists.
#pragma once

#include <mine/types/scene_tile.h>

#include <t2d/core/ecfg.h>
#include <t2d/core/types.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mine::types {

using t2d::f32;
using t2d::usize;

/// The fields the engine reads out of one content entry. Everything else in the entry is the
/// designer's, and the engine never looks at it.
struct TileDefinition {
    ContentKind kind = ContentKind::Item;   ///< the table the entry was declared in
    std::string name;                       ///< the entry's name, which is what the registry knows it by
    std::string image;                      ///< "image" as written; empty when the entry has none
    bool random_reverse = false;            ///< "random_reverse"; false when the entry does not say
    /// "dig_power" (a drill): how fast it mines, in hardness per second. 0 when the entry does not say,
    /// which is a drill that is reported as one that cannot mine rather than one given a rate.
    f32 dig_power = 0.0f;
    /// "hardness" (an ore): how long mining it takes, in power-seconds. 0 when the entry does not say.
    f32 hardness = 0.0f;
    /// "drop" (an ore): the name of the drop content it yields when it is mined. Empty when it does not
    /// say - the ore of a drill that has nothing to throw out.
    std::string drop;
    /// "capacity" (a warehouse): how many things it can hold. 0 when the entry does not say, which is a
    /// warehouse that can hold nothing rather than one given a size nobody wrote.
    usize capacity = 0;

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
/// out for it. Returns nullptr for a definition that is not a plot at all.
///
/// The result is a SceneTile because every plot is one - possibly scenery that also runs
/// (types/entity_tile.h) - so a caller that holds a layer's plots can refresh them without asking
/// each one what it is.
[[nodiscard]] std::unique_ptr<SceneTile> make_tile(const TileDefinition& definition, ContentId id, i32 layer,
                                                   GridPos anchor, i32 width = 1, i32 height = 1);

} // namespace mine::types
