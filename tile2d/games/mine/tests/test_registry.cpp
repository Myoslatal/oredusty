// The content registry and the per-save name -> id tables.
//
// The scenario these tests exist for: a save stores numbers, the content data changes between
// versions (entries added, removed, reordered), and the save must still mean the same thing when it
// is loaded. Numbers alone cannot do that - the table the save was written with is what makes the
// meaning explicit.
#include <mine/registry.h>

#include <t2d/core/bitstream.h>

#include <support/test_support.h>

#include <string>
#include <vector>

using namespace mine;
using t2d::ByteWriter;
using t2d::ConstSpan;

namespace {

/// Encodes a table the way the save format does, but from arbitrary entries: the malformed cases need
/// to inject ids and names the production encoder would never emit.
[[nodiscard]] std::vector<u8> encode_entries(const std::vector<ContentEntry>& entries) {
    std::vector<u8> bytes;
    ByteWriter writer(bytes);
    writer.write_u32(ContentTable::kMagic);
    writer.write_u8(ContentTable::kVersion);
    writer.write_varint(static_cast<u32>(kContentKindCount));
    for (usize kind = 0; kind < kContentKindCount; ++kind) {
        u32 count = 0;
        for (const ContentEntry& entry : entries) {
            if (static_cast<usize>(entry.kind) == kind) ++count;
        }
        writer.write_varint(count);
        for (const ContentEntry& entry : entries) {
            if (static_cast<usize>(entry.kind) != kind) continue;
            writer.write_varint(entry.id);
            writer.write_string(entry.name);
        }
    }
    return bytes;
}

/// A registry holding "alpha", "beta", "gamma" as items (ids 1, 2, 3).
[[nodiscard]] ContentRegistry make_alpha_beta_gamma() {
    ContentRegistry registry;
    registry.register_content(ContentKind::Item, "alpha");
    registry.register_content(ContentKind::Item, "beta");
    registry.register_content(ContentKind::Item, "gamma");
    return registry;
}

} // namespace

T2D_TEST(ids_are_handed_out_in_registration_order_per_kind) {
    ContentRegistry registry;
    T2D_CHECK_EQ(registry.register_content(ContentKind::Item, "alpha"), 1u);
    T2D_CHECK_EQ(registry.register_content(ContentKind::Item, "beta"), 2u);
    T2D_CHECK_EQ(registry.register_content(ContentKind::Structure, "alpha"), 1u); // other kind, own ids
    T2D_CHECK_EQ(registry.register_content(ContentKind::Structure, "cave"), 2u);
    T2D_CHECK_EQ(registry.register_content(ContentKind::Machine, "drill"), 1u);

    T2D_CHECK_EQ(registry.count(ContentKind::Item), 2u);
    T2D_CHECK_EQ(registry.count(ContentKind::Structure), 2u);
    T2D_CHECK_EQ(registry.count(ContentKind::Recipe), 0u);
    T2D_CHECK_EQ(registry.total_count(), 5u);

    T2D_CHECK_EQ(registry.find(ContentKind::Item, "beta"), 2u);
    T2D_CHECK_EQ(registry.find(ContentKind::Item, "cave"), kNoContent); // exists, but not as an item
    T2D_CHECK_EQ(registry.find(ContentKind::Structure, "cave"), 2u);
    T2D_CHECK_EQ(registry.find(ContentKind::Item, "nope"), kNoContent);

    const ContentEntry* entry = registry.find(ContentKind::Item, 2u);
    T2D_REQUIRE(entry != nullptr);
    T2D_CHECK_EQ(entry->name, std::string("beta"));
    T2D_CHECK_EQ(entry->kind, ContentKind::Item);
    T2D_CHECK(registry.find(ContentKind::Item, kNoContent) == nullptr);
    T2D_CHECK(registry.find(ContentKind::Item, 3u) == nullptr);

    // Ids stay compact and contiguous, which is what lets a save store them as varints.
    for (usize index = 0; index < registry.entries(ContentKind::Item).size(); ++index) {
        T2D_CHECK_EQ(registry.entries(ContentKind::Item)[index].id, static_cast<ContentId>(index + 1));
    }
    T2D_CHECK_EQ(std::string(content_kind_name(ContentKind::Machine)), std::string("machine"));
}

T2D_TEST(registering_the_same_name_twice_keeps_the_id) {
    ContentRegistry registry;
    const ContentId first = registry.register_content(ContentKind::Item, "alpha");
    T2D_CHECK_EQ(registry.register_content(ContentKind::Item, "alpha"), first);
    T2D_CHECK_EQ(registry.count(ContentKind::Item), 1u);
    T2D_CHECK_EQ(registry.register_content(ContentKind::Item, "beta"), 2u);

    // Unusable names are refused instead of entering the table.
    T2D_CHECK_EQ(registry.register_content(ContentKind::Item, ""), kNoContent);
    T2D_CHECK_EQ(registry.register_content(ContentKind::Item, std::string(129, 'x')), kNoContent);
    T2D_CHECK_EQ(registry.count(ContentKind::Item), 2u);
}

T2D_TEST(a_table_round_trips_through_the_save_encoding) {
    ContentRegistry registry = make_alpha_beta_gamma();
    registry.register_content(ContentKind::Layer, "layer.one");
    registry.register_content(ContentKind::Channel, "hand.feed");

    const ContentTable table = registry.table();
    T2D_CHECK_EQ(table.entries.size(), 5u);
    T2D_CHECK_EQ(table.count(ContentKind::Item), 3u);
    T2D_CHECK_EQ(table.find_id(ContentKind::Item, "gamma"), 3u);
    T2D_REQUIRE(table.find(ContentKind::Item, 2u) != nullptr);
    T2D_CHECK_EQ(table.find(ContentKind::Item, 2u)->name, std::string("beta"));

    const std::vector<u8> bytes = table.serialize();
    T2D_CHECK_GT(bytes.size(), 0u);
    // The encoder in registry.cpp and the format the tests write by hand must agree.
    T2D_CHECK(encode_entries(table.entries) == bytes);

    ContentTable decoded;
    T2D_REQUIRE(ContentTable::deserialize(ConstSpan<const u8>(bytes.data(), bytes.size()), decoded));
    T2D_CHECK(decoded.entries == table.entries);
    T2D_CHECK(decoded.serialize() == bytes); // stable: re-saving does not churn the file

    // What the registry writes and what a save writes are the same thing.
    T2D_CHECK(ContentTable::from_registry(registry).serialize() == bytes);
}

T2D_TEST(loading_a_save_translates_ids_by_name) {
    // The save was written when only these three items existed.
    const ContentRegistry old_registry = make_alpha_beta_gamma();
    const ContentTable saved = old_registry.table();
    T2D_CHECK_EQ(saved.find_id(ContentKind::Item, "beta"), 2u);

    // The current build registers the same content in a different order, and adds one.
    ContentRegistry current;
    current.register_content(ContentKind::Item, "gamma");
    current.register_content(ContentKind::Item, "alpha");
    current.register_content(ContentKind::Item, "delta");
    current.register_content(ContentKind::Item, "beta");
    T2D_CHECK_EQ(current.find(ContentKind::Item, "beta"), 4u); // the number changed

    const ContentRemap remap = ContentRemap::build(saved, current);
    T2D_CHECK(remap.complete());
    T2D_CHECK_EQ(remap.missing().size(), 0u);
    T2D_CHECK_EQ(remap.translated_count(), 3u);

    // Every saved id lands on the content with the same name, whatever its number is now.
    T2D_CHECK_EQ(remap.to_current(ContentKind::Item, saved.find_id(ContentKind::Item, "alpha")),
                 current.find(ContentKind::Item, "alpha"));
    T2D_CHECK_EQ(remap.to_current(ContentKind::Item, saved.find_id(ContentKind::Item, "beta")),
                 current.find(ContentKind::Item, "beta"));
    T2D_CHECK_EQ(remap.to_current(ContentKind::Item, saved.find_id(ContentKind::Item, "gamma")),
                 current.find(ContentKind::Item, "gamma"));

    // The reverse direction answers "which number did this save use for that content".
    T2D_CHECK_EQ(remap.to_saved(ContentKind::Item, current.find(ContentKind::Item, "beta")), 2u);
    T2D_CHECK_EQ(remap.to_saved(ContentKind::Item, current.find(ContentKind::Item, "delta")), kNoContent);

    // Out of range and empty slots translate to nothing instead of reading past the table.
    T2D_CHECK_EQ(remap.to_current(ContentKind::Item, kNoContent), kNoContent);
    T2D_CHECK_EQ(remap.to_current(ContentKind::Item, 99u), kNoContent);
    T2D_CHECK_EQ(remap.to_saved(ContentKind::Item, 99u), kNoContent);
    T2D_CHECK_EQ(remap.to_current(ContentKind::Layer, 1u), kNoContent); // the save had no layers
}

T2D_TEST(content_added_in_the_middle_would_silently_corrupt_a_number_only_save) {
    // This is the failure the table exists to prevent, spelled out.
    const ContentRegistry old_registry = make_alpha_beta_gamma();
    const ContentId saved_beta = old_registry.find(ContentKind::Item, "beta"); // 2
    T2D_CHECK_EQ(saved_beta, 2u);

    ContentRegistry current;
    current.register_content(ContentKind::Item, "delta"); // added first: every later id shifts
    current.register_content(ContentKind::Item, "alpha");
    current.register_content(ContentKind::Item, "beta");
    current.register_content(ContentKind::Item, "gamma");

    // Reading the number as-is would now mean a different resource entirely: the save's id 2 was
    // "beta" and the current registry hands id 2 to "alpha".
    T2D_REQUIRE(current.find(ContentKind::Item, saved_beta) != nullptr);
    T2D_CHECK_EQ(current.find(ContentKind::Item, saved_beta)->name, std::string("alpha"));

    // Translating by name gives the right one.
    const ContentRemap remap = ContentRemap::build(old_registry.table(), current);
    const ContentId resolved = remap.to_current(ContentKind::Item, saved_beta);
    T2D_REQUIRE(current.find(ContentKind::Item, resolved) != nullptr);
    T2D_CHECK_EQ(current.find(ContentKind::Item, resolved)->name, std::string("beta"));
    T2D_CHECK_EQ(resolved, 3u);
}

T2D_TEST(content_that_no_longer_exists_is_reported_not_remapped) {
    ContentRegistry old_registry = make_alpha_beta_gamma();
    old_registry.register_content(ContentKind::Item, "retired");
    const ContentTable saved = old_registry.table();

    ContentRegistry current;
    current.register_content(ContentKind::Item, "gamma");
    current.register_content(ContentKind::Item, "alpha");

    const ContentRemap remap = ContentRemap::build(saved, current);
    T2D_CHECK_FALSE(remap.complete());
    T2D_REQUIRE(remap.missing().size() == 2u);
    T2D_CHECK_EQ(remap.missing()[0], std::string("beta"));
    T2D_CHECK_EQ(remap.missing()[1], std::string("retired"));
    T2D_CHECK_EQ(remap.translated_count(), 2u);

    // The content that is gone translates to nothing; the rest still works.
    T2D_CHECK_EQ(remap.to_current(ContentKind::Item, saved.find_id(ContentKind::Item, "beta")), kNoContent);
    T2D_CHECK_EQ(remap.to_current(ContentKind::Item, saved.find_id(ContentKind::Item, "retired")), kNoContent);
    T2D_CHECK_EQ(remap.to_current(ContentKind::Item, saved.find_id(ContentKind::Item, "alpha")),
                 current.find(ContentKind::Item, "alpha"));
}

T2D_TEST(the_translation_is_per_kind) {
    ContentRegistry old_registry;
    old_registry.register_content(ContentKind::Item, "alpha");
    old_registry.register_content(ContentKind::Structure, "alpha");
    old_registry.register_content(ContentKind::Structure, "cave");
    const ContentTable saved = old_registry.table();

    ContentRegistry current;
    current.register_content(ContentKind::Structure, "cave");
    current.register_content(ContentKind::Structure, "alpha");
    current.register_content(ContentKind::Item, "alpha");

    const ContentRemap remap = ContentRemap::build(saved, current);
    T2D_CHECK(remap.complete());
    T2D_CHECK_EQ(remap.to_current(ContentKind::Item, 1u), current.find(ContentKind::Item, "alpha"));
    T2D_CHECK_EQ(remap.to_current(ContentKind::Structure, 1u), current.find(ContentKind::Structure, "alpha"));
    T2D_CHECK_EQ(remap.to_current(ContentKind::Structure, 2u), current.find(ContentKind::Structure, "cave"));
}

T2D_TEST(malformed_tables_are_refused) {
    const ContentRegistry registry = make_alpha_beta_gamma();
    const std::vector<u8> good = registry.table().serialize();
    T2D_REQUIRE(!good.empty());

    const auto rejects = [&](const std::vector<u8>& bytes, const char* what) {
        ContentTable out;
        const bool accepted = ContentTable::deserialize(ConstSpan<const u8>(bytes.data(), bytes.size()), out);
        T2D_CHECK_MSG(!accepted, "{} was accepted", what);
        T2D_CHECK_MSG(out.entries.empty(), "{} wrote into the output table", what);
    };

    // Wrong magic and wrong version.
    std::vector<u8> bad_magic = good;
    bad_magic[0] = static_cast<u8>(bad_magic[0] ^ 0xFFu);
    rejects(bad_magic, "a table with the wrong magic");

    std::vector<u8> bad_version = good;
    bad_version[4] = static_cast<u8>(ContentTable::kVersion + 1u);
    rejects(bad_version, "a table from another version");

    // Truncations: every prefix short of the whole thing must be refused.
    for (usize size = 0; size < good.size(); ++size) {
        ContentTable out;
        T2D_CHECK(!ContentTable::deserialize(ConstSpan<const u8>(good.data(), size), out));
    }

    // Trailing bytes mean the layout is not ours.
    std::vector<u8> trailing = good;
    trailing.push_back(0x7Fu);
    rejects(trailing, "a table with trailing bytes");

    // Zero id, duplicate id, empty name, duplicate name.
    rejects(encode_entries({ContentEntry{ContentKind::Item, 0u, "alpha"}}), "a zero id");
    rejects(encode_entries({ContentEntry{ContentKind::Item, 1u, "alpha"},
                            ContentEntry{ContentKind::Item, 1u, "beta"}}),
            "a duplicate id");
    rejects(encode_entries({ContentEntry{ContentKind::Item, 1u, ""}}), "an empty name");
    rejects(encode_entries({ContentEntry{ContentKind::Item, 1u, "alpha"},
                            ContentEntry{ContentKind::Item, 2u, "alpha"}}),
            "a duplicate name");
    rejects(encode_entries({ContentEntry{ContentKind::Item, 1u, std::string(129, 'x')}}), "an oversized name");
    rejects(encode_entries({ContentEntry{ContentKind::Item, kMaxContentId + 1u, "alpha"}}),
            "an id past the sanity limit");

    // A table whose entry count disagrees with the kind count cannot be read as a table either.
    std::vector<u8> wrong_kinds;
    ByteWriter writer(wrong_kinds);
    writer.write_u32(ContentTable::kMagic);
    writer.write_u8(ContentTable::kVersion);
    writer.write_varint(static_cast<u32>(kContentKindCount) + 1u);
    rejects(wrong_kinds, "a table with an unknown number of kinds");
}

T2D_TEST(the_encoding_is_deterministic) {
    const ContentRegistry first = make_alpha_beta_gamma();
    const ContentRegistry second = make_alpha_beta_gamma();
    T2D_CHECK(first.table().serialize() == second.table().serialize());

    ContentRegistry reordered;
    reordered.register_content(ContentKind::Item, "gamma");
    reordered.register_content(ContentKind::Item, "alpha");
    reordered.register_content(ContentKind::Item, "beta");
    // A different registration order really does produce different ids - which is why the table
    // travels with the save instead of being assumed.
    T2D_CHECK(reordered.table().serialize() != first.table().serialize());

    ContentRegistry cleared = make_alpha_beta_gamma();
    cleared.clear();
    T2D_CHECK_EQ(cleared.total_count(), 0u);
    T2D_CHECK_EQ(cleared.register_content(ContentKind::Item, "alpha"), 1u); // ids restart cleanly
}

T2D_TEST(an_empty_registry_is_a_valid_save_table) {
    const ContentRegistry empty;
    const ContentTable table = empty.table();
    T2D_CHECK(table.empty());

    const std::vector<u8> bytes = table.serialize();
    T2D_CHECK_GT(bytes.size(), 0u);
    ContentTable decoded;
    T2D_REQUIRE(ContentTable::deserialize(ConstSpan<const u8>(bytes.data(), bytes.size()), decoded));
    T2D_CHECK(decoded.empty());

    ContentRegistry current = make_alpha_beta_gamma();
    const ContentRemap remap = ContentRemap::build(decoded, current);
    T2D_CHECK(remap.complete());
    T2D_CHECK_EQ(remap.translated_count(), 0u);
    T2D_CHECK_EQ(remap.to_current(ContentKind::Item, 1u), kNoContent);
    T2D_CHECK_EQ(remap.to_saved(ContentKind::Item, 1u), kNoContent);
}

T2D_TEST_MAIN
