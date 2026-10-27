#include <mine/building_unlocks.h>

#include <mine/types/tile_definition.h>

#include <t2d/core/bitstream.h>
#include <t2d/core/log.h>

#include <algorithm>
#include <array>

namespace mine {

namespace {

/// The kinds whose entries can be buildings, in the order the build sidebar walks them: what reset() and
/// refresh() take, and therefore the order of the table (the same list BuildSidebar::rebuild walks).
constexpr std::array<ContentKind, 6> kBuildingKinds{ContentKind::Structure, ContentKind::Machine,
                                                    ContentKind::Drill,     ContentKind::Warehouse,
                                                    ContentKind::Floor,     ContentKind::Ore};

/// Sanity limit on the rows of one blob: a corrupt save must not ask for a huge table. Buildings are a
/// handful of content entries, so this is generous by orders of magnitude.
constexpr u32 kMaxEntries = 1u << 16;

/// One row as the save wrote it, before the remap names it in the running registry.
struct SavedEntry {
    ContentKind kind = ContentKind::Structure;
    ContentId id = kNoContent;
    bool unlocked = false;
};

} // namespace

void BuildingUnlocks::reset(const ContentRegistry& registry, const ContentDefinitions& definitions) {
    entries_.clear();
    for (const ContentKind kind : kBuildingKinds) {
        for (const ContentEntry& entry : registry.entries(kind)) {
            const types::TileDefinition* definition = definitions.find_tile(kind, entry.name);
            // A row per **building**: what else the registry holds is not buildable, so it is not a thing
            // a save can have unlocked.
            if (definition == nullptr || definition->building.empty()) continue;
            entries_.push_back(Entry{kind, entry.id, definition->building.default_unlock});
        }
    }
}

void BuildingUnlocks::refresh(const ContentRegistry& registry, const ContentDefinitions& definitions) {
    std::vector<Entry> next;
    for (const ContentKind kind : kBuildingKinds) {
        for (const ContentEntry& entry : registry.entries(kind)) {
            const types::TileDefinition* definition = definitions.find_tile(kind, entry.name);
            if (definition == nullptr || definition->building.empty()) continue;
            const usize at = index_of(kind, entry.id);
            // What is still here keeps what it had - the player's unlocks are not the data's to reset -
            // and a building this load brought in starts where its definition says.
            const bool unlocked =
                at < entries_.size() ? entries_[at].unlocked : definition->building.default_unlock;
            next.push_back(Entry{kind, entry.id, unlocked});
        }
    }
    entries_ = std::move(next);
}

usize BuildingUnlocks::index_of(ContentKind kind, ContentId id) const {
    for (usize index = 0; index < entries_.size(); ++index) {
        if (entries_[index].kind == kind && entries_[index].id == id) return index;
    }
    return entries_.size();
}

bool BuildingUnlocks::unlocked(ContentKind kind, ContentId id) const {
    const usize at = index_of(kind, id);
    return at < entries_.size() && entries_[at].unlocked;
}

bool BuildingUnlocks::set_unlocked(ContentKind kind, ContentId id, bool unlocked) {
    const usize at = index_of(kind, id);
    if (at >= entries_.size()) return false;
    entries_[at].unlocked = unlocked;
    return true;
}

std::vector<u8> BuildingUnlocks::serialize() const {
    std::vector<u8> bytes;
    bytes.reserve(8 + entries_.size() * 4);
    t2d::ByteWriter writer(bytes);
    writer.write_u32(kMagic);
    writer.write_u8(kVersion);
    writer.write_varint(static_cast<u32>(entries_.size()));
    for (const Entry& entry : entries_) {
        writer.write_u8(static_cast<u8>(entry.kind));
        writer.write_varint(entry.id);
        writer.write_bool(entry.unlocked);
    }
    if (!writer.valid()) {
        T2D_ERROR("unlocks: the table could not be encoded ({} entries)", entries_.size());
        return {};
    }
    return bytes;
}

bool BuildingUnlocks::apply_saved(t2d::ConstSpan<const u8> data, const ContentRemap& remap) {
    t2d::ByteReader reader(data);
    if (reader.read_u32() != kMagic) return false;
    if (reader.read_u8() != kVersion) return false;
    const u32 count = reader.read_varint();
    if (!reader.ok() || count > kMaxEntries) return false;
    std::vector<SavedEntry> parsed;
    parsed.reserve(count);
    for (u32 index = 0; index < count; ++index) {
        const auto kind = static_cast<ContentKind>(reader.read_u8());
        const ContentId id = reader.read_varint();
        const u8 unlocked = reader.read_u8();
        if (!reader.ok()) return false;
        if (static_cast<usize>(kind) >= kContentKindCount) return false;
        if (id == kNoContent || id >= kMaxContentId) return false;
        if (unlocked > 1u) return false;
        parsed.push_back(SavedEntry{kind, id, unlocked == 1u});
    }
    if (!reader.ok() || !reader.empty()) return false;   // trailing bytes mean the layout is not ours
    // One row per building: a blob that names the same one twice cannot say which of the two is its
    // state. Sorted first, so a corrupt blob cannot make this quadratic.
    std::sort(parsed.begin(), parsed.end(), [](const SavedEntry& left, const SavedEntry& right) {
        if (left.kind != right.kind) return static_cast<usize>(left.kind) < static_cast<usize>(right.kind);
        return left.id < right.id;
    });
    for (usize index = 1; index < parsed.size(); ++index) {
        if (parsed[index].kind == parsed[index - 1].kind && parsed[index].id == parsed[index - 1].id) {
            return false;
        }
    }
    // Nothing above changed this table, so a refused blob leaves it exactly as it was. What the blob does
    // say is translated **by name** and applied to what this build still has.
    for (const SavedEntry& saved : parsed) {
        const ContentId current = remap.to_current(saved.kind, saved.id);
        if (current == kNoContent) continue;   // the content is gone from this build
        const usize at = index_of(saved.kind, current);
        if (at >= entries_.size()) continue;   // it exists, but it is not a building any more
        entries_[at].unlocked = saved.unlocked;
    }
    return true;
}

} // namespace mine
