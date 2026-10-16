// Mine - what the interface calls a piece of content.
//
// Content names are the designer's **ids**: "stone", "chunk", "entrance". An id is not a label - it is
// what a save stores and what the data refers to - so the interface shows a *localized* name when it has
// one, and the id when it does not (docs/TEXT.md, docs/GAME_DESIGN.md section 1.28).
//
// Where the localized name lives: the interface string table, under
//
//     content.name.<kind>.<name>      e.g. content.name.warehouse.stone  = "Stone warehouse"
//     content.name.<name>             e.g. content.name.stone            = "Stone"
//
// The kind-specific key wins, so a language can call the same id different things when it is a different
// kind of thing; the plain one is the fallback, and the id is the last resort.
#pragma once

#include <mine/registry.h>

#include <functional>
#include <string>
#include <string_view>

namespace mine {

/// What the label lookup is handed: an id in, the string for it out. \p text answers with **the id
/// itself** when it has no string for it - which is what t2d::Locale::text does - so "no label" can be
/// told apart from "a label that happens to be empty".
using TextLookup = std::function<std::string_view(std::string_view)>;

/// The name to show for the content \p kind / \p id: the kind-specific string, then the plain one, then
/// the name the registry knows it by ("#<id>" when the content is gone). \p from_table, when given, says
/// whether the string table had one - a caller that wants a *different* fallback (the kind's own word,
/// say) can then tell "the designer named this" from "this is just its id".
[[nodiscard]] std::string content_label(const ContentRegistry& registry, ContentKind kind, ContentId id,
                                        const TextLookup& text, bool* from_table = nullptr);

} // namespace mine
