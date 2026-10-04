// Mine - loading content into the registry, from every source the game has.
//
// A content file is a .ecfg document whose tables are named after the content kinds (see
// content_kind_name): every key inside such a table is a piece of content, and everything below it is
// the designer's data, which this loader does not interpret.
//
//     item::
//         <name>::
//             ...            # fields are the designer's to define
//     structure::
//         <name>::
//             ...
//
// Registering the names is all the engine needs: ids are handed out in registration order, and a save
// stores the name -> id table it was written with, so the fields can change shape without breaking
// existing saves.
#pragma once

#include <mine/registry.h>
#include <mine/types/tile_definition.h>

#include <t2d/core/ecfg.h>

#include <format>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace mine {

struct ContentLoadReport {
    usize registered = 0;   ///< names that entered the registry now
    usize already_present = 0; ///< names that were already registered (loading the same file twice)
    /// Tables whose name is not a content kind: reported instead of ignored, so a typo in the data
    /// file is visible.
    std::vector<std::string> unknown_tables;
};

/// Every plot definition a load read, by kind and name: what building a layer out of the content
/// needs - the picture the content is drawn with, and whether the map may turn copies of it around
/// (types/tile_definition.h). The pipeline reads the definitions anyway, so that a field the engine
/// cannot read is reported while the file loads rather than when a layer is built; this is that same
/// read, kept instead of thrown away.
///
/// The normal size is one entry per registered plot content - a handful - and a layer build looks a
/// name up once per placement, so the lookup is a scan over a vector rather than an index.
class ContentDefinitions {
public:
    void add(types::TileDefinition definition);
    /// The definition \p kind and \p name have, or nullptr when the load read none: content a mod
    /// registered through the C ABI has no file behind it, and is then a plot with no picture and no
    /// turning around rather than a plot that cannot be placed.
    [[nodiscard]] const types::TileDefinition* find(ContentKind kind, std::string_view name) const;
    /// How many definitions the load read: what a log line reports, and what makes "the data said
    /// nothing about any plot" visible instead of silent.
    [[nodiscard]] usize size() const { return definitions_.size(); }

private:
    std::vector<types::TileDefinition> definitions_;
};

/// Every piece of content \p document declares, in file order (ids are kNoContent: nothing is
/// registered). register_content_from_ecfg() is this walk plus registration; a loader that fills the
/// registry from several sources needs the list *before* it registers anything, so a name another
/// source already took can be reported instead of silently merged.
[[nodiscard]] std::vector<ContentEntry> content_declarations(const t2d::EcfgDocument& document,
                                                             std::vector<std::string>* unknown_tables = nullptr);

/// One content entry's picture. The engine does not read the designer's fields - with one exception:
/// it has to be able to draw what the designer describes, so "image" is read, and its path is kept
/// exactly as written. Resolving that path against whatever the content came from is the caller's job.
struct ContentImage {
    ContentKind kind = ContentKind::Item;
    std::string name;   ///< the content entry it belongs to
    std::string path;   ///< the image field as written ("art/wall.png")
};

/// Every "image" field \p document declares, in file order. Entries without one are simply absent.
[[nodiscard]] std::vector<ContentImage> content_images(const t2d::EcfgDocument& document);

/// One picture a content file ships, and what the engine made of it.
struct ResolvedImage {
    ContentKind kind = ContentKind::Item;
    std::string content;   ///< the content entry it belongs to
    std::string source;    ///< the file that declared it
    std::string path;      ///< as written in the file ("art/wall.png")
    std::string resolved;  ///< absolute path, empty when it could not be resolved
    bool ok = false;
    std::string error;     ///< why not, when ok is false
};

/// Resolves every "image" \p document declares, against the directory of \p source_path - the file that
/// declares it, whichever kind of source that file is: the game's own content, a pack and a mod's
/// content file all name their art the same way, relative to themselves.
///
/// A picture has to be there and has to be a PNG, because that is what the engine decodes; anything
/// else is a record with ok == false and an error saying so, never a silent blank. The caller decides
/// how to report it: a record carries everything a message needs, including where it came from.
[[nodiscard]] std::vector<ResolvedImage> resolve_content_images(const t2d::EcfgDocument& document,
                                                                const std::string& source_path);

/// What one register_declared_content() call did.
struct ContentRegistrationReport {
    usize registered = 0;
    /// Names that were already taken. They are refused, never merged: who owns an id has to be
    /// unambiguous, and "the second file silently won" is how a save gets corrupted.
    std::vector<ContentEntry> collisions;
};

/// Registers \p declared into \p registry, refusing (and listing) names that are already taken.
/// \p registered_out, when given, collects what this call actually added.
[[nodiscard]] ContentRegistrationReport register_declared_content(ContentRegistry& registry,
                                                                  t2d::ConstSpan<const ContentEntry> declared,
                                                                  std::vector<ContentEntry>* registered_out = nullptr);

/// The result of ordering a set of things that require each other.
struct RequirementOrder {
    std::vector<usize> order;         ///< indices, everything a thing requires comes before it
    std::vector<usize> dropped;       ///< indices that cannot load: a missing requirement, or a cycle
    std::vector<std::string> errors;  ///< why, in the caller's own words ("'a' requires 'b', ...")
};

/// Orders \p count things so that what a thing requires comes first. \p id_of names one, \p requires_of
/// lists what it needs. A requirement nobody provides drops the thing that wanted it; anything left
/// over is a cycle. Both are reported rather than guessed at: a load order that is not what the data
/// asked for is worse than no load order.
template <class IdOf, class RequiresOf>
[[nodiscard]] RequirementOrder order_by_requirements(usize count, IdOf&& id_of, RequiresOf&& requires_of) {
    RequirementOrder result;
    std::map<std::string, usize> by_id;
    for (usize index = 0; index < count; ++index) by_id.emplace(id_of(index), index);

    std::vector<bool> placed(count, false);
    std::vector<bool> dropped(count, false);
    usize dropped_count = 0;
    for (usize index = 0; index < count; ++index) {
        for (const std::string& need : requires_of(index)) {
            if (by_id.find(need) != by_id.end()) continue;
            result.errors.push_back(std::format("'{}' requires '{}', which is not loaded", id_of(index), need));
            dropped[index] = true;
            ++dropped_count;
            break;
        }
    }
    while (result.order.size() + dropped_count < count) {
        bool progress = false;
        for (usize index = 0; index < count; ++index) {
            if (placed[index] || dropped[index]) continue;
            bool ready = true;
            for (const std::string& need : requires_of(index)) {
                const auto found = by_id.find(need);
                if (found == by_id.end() || !placed[found->second]) {
                    ready = false;
                    break;
                }
            }
            if (!ready) continue;
            placed[index] = true;
            result.order.push_back(index);
            progress = true;
        }
        if (!progress) break;
    }
    for (usize index = 0; index < count; ++index) {
        if (placed[index] || dropped[index]) continue;
        result.errors.push_back(std::format("'{}': its requirements form a cycle", id_of(index)));
        dropped[index] = true;
    }
    for (usize index = 0; index < count; ++index) {
        if (dropped[index]) result.dropped.push_back(index);
    }
    return result;
}

/// Registers every content name found in \p document. Tables that are not named after a content kind
/// are listed in the report and skipped.
[[nodiscard]] ContentLoadReport register_content_from_ecfg(ContentRegistry& registry,
                                                           const t2d::EcfgDocument& document);

/// Convenience: parses \p text and registers it.
[[nodiscard]] ContentLoadReport register_content_from_ecfg_text(ContentRegistry& registry,
                                                                std::string_view text,
                                                                t2d::EcfgError* error = nullptr);

} // namespace mine
