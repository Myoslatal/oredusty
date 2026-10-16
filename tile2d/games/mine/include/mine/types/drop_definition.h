// Mine - what the designer's data says about a drop, and how a drop is made out of it.
//
// A drop is declared in a `drop::` table (docs/MODS.md), and everything below its name is the
// designer's - with the one exception the engine has to be able to read: "image", the picture of the
// thing lying on the ground. There is no `random_reverse` here: that field is about how a *plot* draws
// itself on the map and is rolled while the map is built (types/tile_definition.h), and a drop is not a
// plot - it is put down later, and it is dragged around afterwards. An entry that writes one is writing
// a designer field the engine does not read, like any other field it does not know.
//
// The definer is the same small shape the item side has (types/item_definition.h): one class covers every
// drop (types/drop.h), so there is no per-kind table of classes to build here - what makes one drop
// different from another is the content behind it.
#pragma once

#include <mine/types/drop.h>

#include <t2d/core/ecfg.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mine::types {

/// The fields the engine reads out of one drop entry. Everything else in the entry is the designer's,
/// and the engine never looks at it.
struct DropDefinition {
    ContentKind kind = ContentKind::Drop;  ///< the table the entry was declared in
    std::string name;                      ///< the entry's name, which is what the registry knows it by
    std::string image;                     ///< "image" as written; empty when the entry has none
};

/// Reads one entry of the drop table. p kind is the table it was declared in and has to be the drop
/// table: the table an entry is written in is what it *is* (docs/TYPES.md section 10), so an entry read
/// out of another table would be a drop no file declares. Returns nothing and fills p error when the
/// entry cannot be read - a field the engine reads and cannot read is refused rather than guessed at.
[[nodiscard]] std::optional<DropDefinition> read_drop_definition(ContentKind kind, std::string_view name,
                                                                 const t2d::EcfgValue& entry,
                                                                 std::string* error = nullptr);

/// Every drop definition p document declares, in file order. Tables that are not the drop table are
/// skipped: another kind's entries are that kind's definer's business (types/tile_definition.h).
[[nodiscard]] std::vector<DropDefinition> drop_definitions(const t2d::EcfgDocument& document,
                                                           std::vector<std::string>* errors = nullptr);

/// The drop a definition describes, at p position, with the id the registry handed out for it. This is
/// the one place that knows which field of a definition the class takes, the way make_tile is for plots.
[[nodiscard]] std::unique_ptr<Drop> make_drop(const DropDefinition& definition, ContentId id,
                                              t2d::Vec2 position);

} // namespace mine::types
