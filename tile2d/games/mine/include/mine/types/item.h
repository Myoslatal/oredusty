// Mine - items: content that is not on the map.
//
// An **item** is a kind of thing the mine moves around rather than a thing that sits in a cell: what a
// machine makes, what a recipe takes and gives, what a demand asks for, what the player carries
// (docs/GAME_DESIGN.md section 1.22). It has no plot, no footprint and no layer - "which cell is this
// item in" is a question about a container, and a container is the designer's data, not this class.
//
// What it holds is what a plot holds: the kind and the id the registry handed out, never a name - the
// name belongs to the registry (mine/registry.h), which is what keeps a save readable when the data
// changes under it. It also keeps the one field the engine reads for it, the picture the entry named,
// because everything that shows an item - an inventory slot, a recipe, a demand - shows that picture.
//
// It is deliberately **not** a Tile and does not derive from one. A plot is something that occupies
// cells (types/tile.h); an item is content that plots move around. A shared base class would say the
// two are the same kind of thing, and they are not: a plot has a layer and a footprint, an item has
// neither, and a cell holds a plot but never an item.
#pragma once

#include <mine/registry.h>

#include <string>

namespace mine::types {

/// One piece of item content, as the game knows it. The default-constructed one is the empty slot: no
/// content behind it and no picture.
class Item {
public:
    Item() = default;

    /// \p id is the id the registry handed out for this item's content (kNoContent when there is no
    /// content behind it), \p image the picture the entry named, exactly as written.
    Item(ContentId id, std::string image);

    /// The kind of content this is. An item is always an item; the accessor is here because a reference
    /// to content - in a recipe, in a container - is a (kind, id) pair, and this is that pair.
    [[nodiscard]] ContentKind kind() const { return ContentKind::Item; }
    [[nodiscard]] ContentId id() const { return id_; }
    /// The picture the entry named ("art/ore.png"), or empty when it named none. Resolving it against
    /// whatever the content came from is the loader's job (mine/content_loader.h).
    [[nodiscard]] const std::string& image() const { return image_; }

    /// The name the registry has for this item, or "#<id>" when the content is gone - the same text a
    /// plot shows (types/tile.cpp), so a report reads the same whichever half of the content it is about.
    [[nodiscard]] std::string name(const ContentRegistry& registry) const;
    /// There is content behind it and that content is here. A reload can take content away; the item
    /// keeps its id and says so, instead of quietly becoming a different item (docs/MODS.md section 4).
    [[nodiscard]] bool valid(const ContentRegistry& registry) const;

private:
    ContentId id_ = kNoContent;
    std::string image_;
};

} // namespace mine::types
