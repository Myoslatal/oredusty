// Mine - what the designer's data says about an item, and how an item is made out of it.
//
// An item is declared in an `item::` table (docs/MODS.md), and everything below its name is the
// designer's - with the one exception the engine has to be able to read: "image", the picture that
// everything showing an item shows. There is no `random_reverse` here: that field is about how a *plot*
// draws itself on the map (types/tile_definition.h), and an item is not on the map, so an entry that
// writes one is writing a designer field the engine does not read - the same as any other field it does
// not know.
//
// This is the small half of the pair the plot side has (types/tile_definition.h): a plot has a class per
// kind because plots *do* different things (a machine runs, a floor is stood on), while one class covers
// every item (types/item.h) - what makes one item different from another is the content behind it, never
// the type. So there is no per-kind table of classes to build here, and no footprint to fill in.
#pragma once

#include <mine/types/item.h>

#include <t2d/core/ecfg.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mine::types {

/// The fields the engine reads out of one item entry. Everything else in the entry is the designer's,
/// and the engine never looks at it.
struct ItemDefinition {
    ContentKind kind = ContentKind::Item;  ///< the table the entry was declared in
    std::string name;                      ///< the entry's name, which is what the registry knows it by
    std::string image;                     ///< "image" as written; empty when the entry has none
};

/// Reads one entry of the item table. \p kind is the table it was declared in and has to be the item
/// table: the table an entry is written in is what it *is* (docs/TYPES.md section 7), so an entry read
/// out of another table would be an item no file declares. Returns nothing and fills \p error when the
/// entry cannot be read - a field the engine reads and cannot read is refused rather than guessed at.
[[nodiscard]] std::optional<ItemDefinition> read_item_definition(ContentKind kind, std::string_view name,
                                                                 const t2d::EcfgValue& entry,
                                                                 std::string* error = nullptr);

/// Every item definition \p document declares, in file order. Tables that are not the item table are
/// skipped: another kind's entries are that kind's definer's business (types/tile_definition.h).
[[nodiscard]] std::vector<ItemDefinition> item_definitions(const t2d::EcfgDocument& document,
                                                           std::vector<std::string>* errors = nullptr);

/// The item a definition describes, with the id the registry handed out for it. This is the one place
/// that knows which field of a definition the class takes, the way make_tile is for plots.
[[nodiscard]] Item make_item(const ItemDefinition& definition, ContentId id);

} // namespace mine::types
