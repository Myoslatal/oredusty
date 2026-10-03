// Mine - loading the designer's content data into the registry.
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

#include <t2d/core/ecfg.h>

#include <string>
#include <vector>

namespace mine {

struct ContentLoadReport {
    usize registered = 0;   ///< names that entered the registry now
    usize already_present = 0; ///< names that were already registered (loading the same file twice)
    /// Tables whose name is not a content kind: reported instead of ignored, so a typo in the data
    /// file is visible.
    std::vector<std::string> unknown_tables;
};

/// Every piece of content \p document declares, in file order (ids are kNoContent: nothing is
/// registered). register_content_from_ecfg() is this walk plus registration; a mod loader needs the
/// list *before* it registers anything, so a name another mod already took can be reported instead of
/// silently merged.
[[nodiscard]] std::vector<ContentEntry> content_declarations(const t2d::EcfgDocument& document,
                                                             std::vector<std::string>* unknown_tables = nullptr);

/// Registers every content name found in \p document. Tables that are not named after a content kind
/// are listed in the report and skipped.
[[nodiscard]] ContentLoadReport register_content_from_ecfg(ContentRegistry& registry,
                                                           const t2d::EcfgDocument& document);

/// Convenience: parses \p text and registers it.
[[nodiscard]] ContentLoadReport register_content_from_ecfg_text(ContentRegistry& registry,
                                                                std::string_view text,
                                                                t2d::EcfgError* error = nullptr);

} // namespace mine
