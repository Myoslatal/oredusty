// Mine - which content a save has **researched** (docs/GAME_DESIGN.md section 1.50).
//
// Research is a save attribute, like unlocking a building is: one answer per research node content,
// written into a save, never into a definition. A definition only says that the entry is a node at all
// (types/research.h); whether this mine has researched it is the save's own state.
//
// What it holds is one row per research node, in a fixed kind order and registration order inside a
// kind - the same shape BuildingUnlocks has, because a save reads and writes both the same way.
//
// The encoded form (serialize/apply_saved) is magic 'RST1', a version, a varint count, then
// (kind, id, researched) per row. The ids inside belong to the **content table of the save that wrote
// them**, so reading one goes through a ContentRemap - a save that stored id 7 meaning "stone" says
// nothing about what id 7 means in the running registry (registry.h).
#pragma once

#include <mine/content_loader.h>
#include <mine/registry.h>

#include <t2d/core/types.h>

#include <vector>

namespace mine {

using t2d::u8;
using t2d::usize;

/// Which research nodes a save has researched: one state per node content, shared by everything.
class ResearchState {
public:
    /// One row of the table: which node, and whether this mine has it.
    struct Entry {
        ContentKind kind = ContentKind::Structure;
        ContentId id = kNoContent;
        bool researched = false;
    };

    /// "RST1" as a little endian u32: 'R' is the low byte.
    static constexpr u32 kMagic = 0x31545352u;
    /// Bumped when the encoding changes; a blob written by another version is refused, never guessed.
    static constexpr u8 kVersion = 1;

    /// A **new save**: forgets everything and gives every entry that declares research:: a row, false.
    /// Content without a research node gets no row, so it can never be "researched".
    void reset(const ContentRegistry& registry, const ContentDefinitions& definitions);

    /// A **content reload that kept the ids**: nodes that are still registered keep their state, new
    /// content starts unresearched, and what went away is dropped. A reload that **renumbers** the content
    /// goes through serialize()/apply_saved() with a ContentRemap instead (app.cpp).
    void refresh(const ContentRegistry& registry, const ContentDefinitions& definitions);

    /// Whether \p kind / \p id has been researched. Content that is not in the table is not researched:
    /// there is no state behind it to say otherwise.
    [[nodiscard]] bool researched(ContentKind kind, ContentId id) const;
    /// Sets it, and returns false - changing nothing - for content that is not a node.
    bool set_researched(ContentKind kind, ContentId id, bool researched);

    [[nodiscard]] usize size() const { return entries_.size(); }
    /// The table, in the order reset() built it: what a test reads and what a save writes.
    [[nodiscard]] const std::vector<Entry>& entries() const { return entries_; }

    /// The table as the bytes of a save. **Every** row is written, unresearched ones included: a save
    /// that stored only the researched ones would fall back to today's defaults for the rest.
    [[nodiscard]] std::vector<u8> serialize() const;

    /// Reads a blob written by serialize(). The saved ids are translated with \p remap (by name), entries
    /// whose content is gone from this build are skipped, and current nodes the blob does not mention
    /// keep the state reset()/refresh() gave them. A blob that cannot be read **completely** - wrong
    /// magic or version, a truncated stream, a kind out of range, id zero or past the limit, a bool that
    /// is not 0/1, the same node twice, trailing bytes - returns false and changes nothing.
    bool apply_saved(t2d::ConstSpan<const u8> data, const ContentRemap& remap);

private:
    /// Where \p kind / \p id sits in the table, or entries_.size() when it has no row.
    [[nodiscard]] usize index_of(ContentKind kind, ContentId id) const;

    std::vector<Entry> entries_;
};

} // namespace mine
