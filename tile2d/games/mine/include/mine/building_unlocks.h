// Mine - which buildings a save has **unlocked** (docs/GAME_DESIGN.md section 1.44).
//
// Unlocking is a **save attribute**: one answer per building content, shared by every plot of that
// content, and written into a save - never a definition attribute. The definition only says what a brand
// new save starts with (`building:: default_unlock`), which is why this class is told the definitions
// when a save starts and when content is reloaded.
//
// What it holds is one row per building, in the order the build sidebar offers them: kind by kind
// (Structure, Machine, Drill, Warehouse, Floor, Ore), registration order inside a kind.
//
// The encoded form (serialize/apply_saved) is the save half: magic 'UNL1', a version, a varint count,
// then (kind, id, unlocked) per entry. The ids inside belong to the **content table of the save that
// wrote them**, so reading one has to go through a ContentRemap - a save that stored id 7 meaning
// "stone" says nothing about what id 7 means in the running registry (registry.h).
#pragma once

#include <mine/content_loader.h>
#include <mine/registry.h>

#include <t2d/core/types.h>

#include <vector>

namespace mine {

using t2d::u8;
using t2d::usize;

/// Which buildings a save has unlocked: one state per building content, shared by all of its plots.
class BuildingUnlocks {
public:
    /// One row of the table: which building, and whether it may be built.
    struct Entry {
        ContentKind kind = ContentKind::Structure;
        ContentId id = kNoContent;
        bool unlocked = false;
    };

    /// "UNL1" as a little endian u32: 'U' is the low byte.
    static constexpr u32 kMagic = 0x314C4E55u;
    /// Bumped when the encoding changes; a blob written by another version is refused, never guessed.
    static constexpr u8 kVersion = 1;

    /// A **new save**: forgets everything and takes the state each building's definition says it starts
    /// with (`building:: default_unlock`; true when the data does not say). Content that is not a
    /// building - a floor, an ore - gets no row, so it can never be "unlocked".
    void reset(const ContentRegistry& registry, const ContentDefinitions& definitions);

    /// A **content reload that kept the ids**: buildings that are still registered keep their state,
    /// content that appeared in this load starts at its definition's default, and what went away is
    /// dropped. A reload that **renumbers** the content cannot use this - the state would move to
    /// another building - and goes through serialize()/apply_saved() with a ContentRemap instead
    /// (app.cpp).
    void refresh(const ContentRegistry& registry, const ContentDefinitions& definitions);

    /// Whether the building \p kind / \p id may be built. Content that is not in the table is locked:
    /// there is no state behind it to say otherwise.
    [[nodiscard]] bool unlocked(ContentKind kind, ContentId id) const;
    /// Sets it, and returns false - changing nothing - for content that is not in the table: unlocking a
    /// building that does not exist would be a state with nothing behind it.
    bool set_unlocked(ContentKind kind, ContentId id, bool unlocked);

    [[nodiscard]] usize size() const { return entries_.size(); }
    /// The table, in the order reset() built it: what a test reads and what a save writes.
    [[nodiscard]] const std::vector<Entry>& entries() const { return entries_; }

    /// The table as the bytes of a save. **Every** row is written, locked ones included: a save that
    /// stored only the unlocked ones would silently fall back to today's defaults for the rest when the
    /// designer changes one of them.
    [[nodiscard]] std::vector<u8> serialize() const;

    /// Reads a blob written by serialize(). The saved ids are translated with \p remap (by name), entries
    /// whose content is gone from this build are skipped, and current buildings the blob does not mention
    /// keep the state reset()/refresh() gave them. A blob that cannot be read **completely** - wrong
    /// magic or version, a truncated stream, a kind out of range, id zero or past the limit, a bool that
    /// is not 0/1, the same building twice, trailing bytes - returns false and changes nothing.
    bool apply_saved(t2d::ConstSpan<const u8> data, const ContentRemap& remap);

private:
    /// Where \p kind / \p id sits in the table, or entries_.size() when it has no row.
    [[nodiscard]] usize index_of(ContentKind kind, ContentId id) const;

    std::vector<Entry> entries_;
};

} // namespace mine
