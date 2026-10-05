// Mine - the content registry, and the id tables that keep saves readable.
//
// Game content is supplied as data (docs/GAME_DESIGN.md section 6) and that data changes between
// versions: entries are added, reordered or removed. A save that stored bare numbers would silently
// change meaning when that happens - id 7 used to be one resource and is another one now - so every
// save carries the name -> id table it was written with, and loading translates the saved ids into
// the ids the running registry hands out, by name.
//
//   registry (runtime)   name -> id, ids handed out in registration order, per kind, from 1
//   save                 ids + the table that was in force when it was written
//   load                 ContentRemap::build(saved_table, current_registry) -> translate by name
//
// The registry itself contains no game content: the game fills it from the designer's data, and a
// dedicated server fills it from the same data, which is what keeps ids identical across processes.
#pragma once

#include <t2d/core/types.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace mine {

using t2d::ConstSpan;
using t2d::u16;
using t2d::u32;
using t2d::u8;
using t2d::usize;

/// The kinds of content a save can reference. This list is engineering, not content: it mirrors what
/// a save has to point at and grows when the designer introduces a new kind of data.
///
/// A new kind is **appended**: the value is packed into every map cell and written into every save, so
/// inserting one in the middle would renumber the content behind it. Appending one is what makes an
/// older save table readable: it has fewer kind sections than this build knows, and a kind it does not
/// have is a kind with no entries in it (ContentTable::deserialize).
enum class ContentKind : u8 { Item = 0, Structure, Machine, Recipe, Layer, Channel, Floor, Ore, Count };
inline constexpr usize kContentKindCount = static_cast<usize>(ContentKind::Count);
[[nodiscard]] const char* content_kind_name(ContentKind kind);

/// The kind a table's name is ("item", "structure", ...), or ContentKind::Count when the name is not a
/// kind. What a reader of data uses to turn a name in a file into a kind, without keeping a second list
/// of the names that would go stale the moment a kind is appended.
[[nodiscard]] ContentKind content_kind_from_name(std::string_view name);

/// Numeric id of one piece of content inside one registry. Ids are per kind, start at 1 and are handed
/// out in registration order, so they stay compact enough for a varint.
using ContentId = u32;
/// "Nothing": an empty slot, a missing reference, content that no longer exists.
inline constexpr ContentId kNoContent = 0;
/// Guard against a corrupt table asking for a huge allocation, and the width of the id field of a
/// packed map cell (content_grid.h): every id is < kMaxContentId.
inline constexpr ContentId kMaxContentId = 1u << 20;

struct ContentEntry {
    ContentKind kind = ContentKind::Item;
    ContentId id = kNoContent;
    std::string name;

    friend bool operator==(const ContentEntry&, const ContentEntry&) = default;
};

struct ContentTable;
class ContentRegistry;

/// Registers content by name and answers by id or by name.
class ContentRegistry {
public:
    /// Registers \p name in \p kind and returns its id. Registering the same name twice is idempotent
    /// (the existing id comes back), so loading the same data twice cannot shift the ids.
    ContentId register_content(ContentKind kind, std::string_view name);

    [[nodiscard]] ContentId find(ContentKind kind, std::string_view name) const;
    [[nodiscard]] const ContentEntry* find(ContentKind kind, ContentId id) const;
    [[nodiscard]] usize count(ContentKind kind) const;
    [[nodiscard]] ConstSpan<ContentEntry> entries(ContentKind kind) const;
    [[nodiscard]] usize total_count() const;

    /// Everything registered, in registration order: the table a save stores.
    [[nodiscard]] ContentTable table() const;

    void clear();

private:
    std::array<std::vector<ContentEntry>, kContentKindCount> by_kind_{};
};

/// The name -> id table of one save.
struct ContentTable {
    /// Bumped when the encoding changes; a save written by another version is refused, never guessed.
    /// Adding a content kind is not an encoding change: a table written before the kind existed has
    /// fewer kind sections, and the reader reads it with that kind empty (deserialize).
    static constexpr u8 kVersion = 1;
    /// "MCT1"
    static constexpr u32 kMagic = 0x3154434Du;

    /// Ordered by kind, then by id.
    std::vector<ContentEntry> entries;

    [[nodiscard]] const ContentEntry* find(ContentKind kind, ContentId id) const;
    [[nodiscard]] ContentId find_id(ContentKind kind, std::string_view name) const;
    [[nodiscard]] usize count(ContentKind kind) const;
    [[nodiscard]] bool empty() const { return entries.empty(); }

    [[nodiscard]] std::vector<u8> serialize() const;
    /// Rejects anything it cannot fully trust: wrong magic or version, a truncated stream, duplicate
    /// or zero ids, duplicate or empty names, trailing bytes.
    [[nodiscard]] static bool deserialize(ConstSpan<const u8> data, ContentTable& out);
    [[nodiscard]] static ContentTable from_registry(const ContentRegistry& registry);
};

/// Translates the ids of one save into the ids of the running registry, by name.
class ContentRemap {
public:
    /// Builds the translation. Names the running registry does not know end up in missing(): the
    /// caller decides whether that save can still be played.
    [[nodiscard]] static ContentRemap build(const ContentTable& saved, const ContentRegistry& current);

    /// Saved id -> id in the running registry (kNoContent when the content is gone).
    [[nodiscard]] ContentId to_current(ContentKind kind, ContentId saved_id) const;
    /// Id in the running registry -> the id this save used (kNoContent when the save never had it).
    [[nodiscard]] ContentId to_saved(ContentKind kind, ContentId current_id) const;

    [[nodiscard]] ConstSpan<std::string> missing() const { return ConstSpan<std::string>(missing_.data(), missing_.size()); }
    [[nodiscard]] bool complete() const { return missing_.empty(); }
    /// How many entries were resolved by name.
    [[nodiscard]] usize translated_count() const { return translated_; }

private:
    std::array<std::vector<ContentId>, kContentKindCount> to_current_{};
    std::array<std::vector<ContentId>, kContentKindCount> to_saved_{};
    std::vector<std::string> missing_;
    usize translated_ = 0;
};

} // namespace mine
