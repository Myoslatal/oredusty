#include <mine/registry.h>

#include <t2d/core/bitstream.h>
#include <t2d/core/log.h>

#include <algorithm>

namespace mine {

using t2d::ByteReader;
using t2d::ByteWriter;
namespace {

/// Longest name a table accepts. Names are identifiers, not prose; the limit keeps a corrupt table
/// from asking for a huge allocation.
constexpr usize kMaxNameLength = 128;
/// Sanity limit on entries per kind in one table.
constexpr u32 kMaxEntriesPerKind = 1u << 16;

[[nodiscard]] bool valid_kind(ContentKind kind) { return static_cast<usize>(kind) < kContentKindCount; }

} // namespace

const char* content_kind_name(ContentKind kind) {
    switch (kind) {
        case ContentKind::Item: return "item";
        case ContentKind::Structure: return "structure";
        case ContentKind::Machine: return "machine";
        case ContentKind::Recipe: return "recipe";
        case ContentKind::Layer: return "layer";
        case ContentKind::Channel: return "channel";
        case ContentKind::Floor: return "floor";
        case ContentKind::Count: break;
    }
    return "?";
}

ContentId ContentRegistry::register_content(ContentKind kind, std::string_view name) {
    if (!valid_kind(kind)) return kNoContent;
    if (name.empty() || name.size() > kMaxNameLength) {
        T2D_ERROR("registry: '{}' is not a usable {} name (1..{} characters)", name, content_kind_name(kind),
                  kMaxNameLength);
        return kNoContent;
    }
    std::vector<ContentEntry>& entries = by_kind_[static_cast<usize>(kind)];
    for (const ContentEntry& entry : entries) {
        if (entry.name == name) return entry.id; // idempotent: re-loading data must not shift ids
    }
    const ContentId id = static_cast<ContentId>(entries.size()) + 1u;
    if (id >= kMaxContentId) {
        // A map cell packs an id into 20 bits (content_grid.h), so an id this large could not be
        // stored. Refusing here keeps that guarantee where ids are handed out.
        T2D_ERROR("registry: '{}' would be {} number {}, past the id limit of {}", name,
                  content_kind_name(kind), id, kMaxContentId - 1u);
        return kNoContent;
    }
    entries.push_back(ContentEntry{kind, id, std::string(name)});
    return id;
}

ContentId ContentRegistry::find(ContentKind kind, std::string_view name) const {
    if (!valid_kind(kind)) return kNoContent;
    for (const ContentEntry& entry : by_kind_[static_cast<usize>(kind)]) {
        if (entry.name == name) return entry.id;
    }
    return kNoContent;
}

const ContentEntry* ContentRegistry::find(ContentKind kind, ContentId id) const {
    if (!valid_kind(kind) || id == kNoContent) return nullptr;
    const std::vector<ContentEntry>& entries = by_kind_[static_cast<usize>(kind)];
    if (id > entries.size()) return nullptr;
    return &entries[id - 1]; // ids are handed out in order, so the index is exact
}

usize ContentRegistry::count(ContentKind kind) const {
    return valid_kind(kind) ? by_kind_[static_cast<usize>(kind)].size() : 0;
}

ConstSpan<ContentEntry> ContentRegistry::entries(ContentKind kind) const {
    if (!valid_kind(kind)) return {};
    const std::vector<ContentEntry>& entries = by_kind_[static_cast<usize>(kind)];
    return ConstSpan<ContentEntry>(entries.data(), entries.size());
}

usize ContentRegistry::total_count() const {
    usize total = 0;
    for (const std::vector<ContentEntry>& entries : by_kind_) total += entries.size();
    return total;
}

ContentTable ContentRegistry::table() const {
    ContentTable table;
    table.entries.reserve(total_count());
    for (usize kind = 0; kind < kContentKindCount; ++kind) {
        for (const ContentEntry& entry : by_kind_[kind]) table.entries.push_back(entry);
    }
    return table;
}

void ContentRegistry::clear() {
    for (std::vector<ContentEntry>& entries : by_kind_) entries.clear();
}

// ------------------------------------------------------------------- table ---

const ContentEntry* ContentTable::find(ContentKind kind, ContentId id) const {
    if (id == kNoContent) return nullptr;
    for (const ContentEntry& entry : entries) {
        if (entry.kind == kind && entry.id == id) return &entry;
    }
    return nullptr;
}

ContentId ContentTable::find_id(ContentKind kind, std::string_view name) const {
    for (const ContentEntry& entry : entries) {
        if (entry.kind == kind && entry.name == name) return entry.id;
    }
    return kNoContent;
}

usize ContentTable::count(ContentKind kind) const {
    usize total = 0;
    for (const ContentEntry& entry : entries) {
        if (entry.kind == kind) ++total;
    }
    return total;
}

ContentTable ContentTable::from_registry(const ContentRegistry& registry) { return registry.table(); }

std::vector<u8> ContentTable::serialize() const {
    std::vector<u8> bytes;
    bytes.reserve(64 + entries.size() * 16);
    ByteWriter writer(bytes);
    writer.write_u32(kMagic);
    writer.write_u8(kVersion);
    writer.write_varint(static_cast<u32>(kContentKindCount));
    for (usize kind = 0; kind < kContentKindCount; ++kind) {
        const auto content_kind = static_cast<ContentKind>(kind);
        u32 count = 0;
        for (const ContentEntry& entry : entries) {
            if (entry.kind == content_kind) ++count;
        }
        writer.write_varint(count);
        for (const ContentEntry& entry : entries) {
            if (entry.kind != content_kind) continue;
            writer.write_varint(entry.id);
            writer.write_string(entry.name);
        }
    }
    if (!writer.valid()) {
        T2D_ERROR("registry: the content table could not be encoded ({} entries)", entries.size());
        return {};
    }
    return bytes;
}

bool ContentTable::deserialize(ConstSpan<const u8> data, ContentTable& out) {
    ByteReader reader(data);
    if (reader.read_u32() != kMagic) return false;
    if (reader.read_u8() != kVersion) return false;
    const u32 kind_count = reader.read_varint();
    if (!reader.ok() || kind_count != kContentKindCount) return false;

    ContentTable parsed;
    for (u32 kind_index = 0; kind_index < kind_count; ++kind_index) {
        const auto kind = static_cast<ContentKind>(kind_index);
        const u32 entry_count = reader.read_varint();
        if (!reader.ok() || entry_count > kMaxEntriesPerKind) return false;
        for (u32 index = 0; index < entry_count; ++index) {
            ContentEntry entry;
            entry.kind = kind;
            entry.id = reader.read_varint();
            entry.name = reader.read_string();
            if (!reader.ok()) return false;
            if (entry.id == kNoContent || entry.id >= kMaxContentId) return false;
            if (entry.name.empty() || entry.name.size() > kMaxNameLength) return false;
            // Ids and names must be unique inside a kind: a table that repeats either one cannot be
            // translated unambiguously, so it is refused instead of being resolved by luck.
            if (parsed.find(kind, entry.id) != nullptr) return false;
            if (parsed.find_id(kind, entry.name) != kNoContent) return false;
            parsed.entries.push_back(std::move(entry));
        }
    }
    if (!reader.ok() || !reader.empty()) return false; // trailing bytes mean the layout is not ours
    out = std::move(parsed);
    return true;
}

// ------------------------------------------------------------------- remap ---

ContentRemap ContentRemap::build(const ContentTable& saved, const ContentRegistry& current) {
    ContentRemap remap;
    for (usize kind_index = 0; kind_index < kContentKindCount; ++kind_index) {
        const auto kind = static_cast<ContentKind>(kind_index);
        ContentId highest_saved = kNoContent;
        for (const ContentEntry& entry : saved.entries) {
            if (entry.kind == kind) highest_saved = std::max(highest_saved, entry.id);
        }
        remap.to_current_[kind_index].assign(static_cast<usize>(highest_saved) + 1u, kNoContent);
        remap.to_saved_[kind_index].assign(current.count(kind) + 1u, kNoContent);

        for (const ContentEntry& entry : saved.entries) {
            if (entry.kind != kind) continue;
            const ContentId current_id = current.find(kind, entry.name);
            if (current_id == kNoContent) {
                // The save references content this build does not have. Report it by name: silently
                // dropping it, or worse reusing the number, is how a save gets corrupted.
                remap.missing_.push_back(entry.name);
                continue;
            }
            remap.to_current_[kind_index][entry.id] = current_id;
            remap.to_saved_[kind_index][current_id] = entry.id;
            ++remap.translated_;
        }
    }
    if (!remap.missing_.empty()) {
        T2D_WARN("registry: this save references {} piece(s) of content that no longer exist (first: '{}')",
                 remap.missing_.size(), remap.missing_.front());
    }
    return remap;
}

ContentId ContentRemap::to_current(ContentKind kind, ContentId saved_id) const {
    if (!valid_kind(kind) || saved_id == kNoContent) return kNoContent;
    const std::vector<ContentId>& table = to_current_[static_cast<usize>(kind)];
    if (saved_id >= table.size()) return kNoContent;
    return table[saved_id];
}

ContentId ContentRemap::to_saved(ContentKind kind, ContentId current_id) const {
    if (!valid_kind(kind) || current_id == kNoContent) return kNoContent;
    const std::vector<ContentId>& table = to_saved_[static_cast<usize>(kind)];
    if (current_id >= table.size()) return kNoContent;
    return table[current_id];
}

} // namespace mine
