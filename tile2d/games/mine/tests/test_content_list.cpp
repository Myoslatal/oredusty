// The content list: what the registry was filled from, one line per source, failures included.
//
// The list is the same data the game draws, so these checks are about what a designer would read off
// the screen: which packs are here, what each one added, and which ones did not load.
#include <mine/content_list.h>
#include <mine/content_pack.h>
#include <mine/registry.h>

#include <support/test_support.h>

#include <t2d/core/ecfg.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace mine;
namespace fs = std::filesystem;

namespace {

/// A scratch directory of packs or packages, removed when the test ends.
class Scratch {
public:
    Scratch() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path() / std::format("t2d_list_{}", stamp);
        std::error_code code;
        fs::remove_all(root_, code);
        fs::create_directories(root_, code);
    }
    ~Scratch() {
        std::error_code code;
        fs::remove_all(root_, code);
    }
    T2D_NON_COPYABLE(Scratch);

    void write(const std::string& relative, const std::string& text) const {
        const fs::path path = root_ / relative;
        std::error_code code;
        fs::create_directories(path.parent_path(), code);
        std::ofstream file(path);
        file << text;
    }
    [[nodiscard]] std::string root() const { return root_.string(); }

private:
    fs::path root_;
};

/// The list the game builds after a load.
[[nodiscard]] ContentListModel list_of(const ContentPipeline& pipeline) {
    ContentListModel list;
    const ContentPipelineReport& report = pipeline.report();
    list.set_sources(report.sources, report.errors, report.warnings);
    return list;
}

[[nodiscard]] const ContentSource* find_source(const ContentListModel& list, std::string_view id) {
    for (const ContentSource& source : list.sources()) {
        if (source.id == id) return &source;
    }
    return nullptr;
}

[[nodiscard]] std::vector<std::string> names_of(const ContentSource& source) {
    std::vector<std::string> names;
    for (const ContentEntry& entry : source.entries) names.push_back(entry.name);
    return names;
}

/// True when one of the load's own messages mentions \p needle. A message a source already carries
/// on its own line is not in this list at all.
[[nodiscard]] bool has_issue(const ContentListModel& list, std::string_view needle) {
    for (const ContentListIssue& issue : list.issues()) {
        if (issue.text.find(needle) != std::string::npos) return true;
    }
    return false;
}

/// Ten sources with one entry each: enough lines to scroll, and small enough to read.
[[nodiscard]] std::vector<ContentSource> ten_sources() {
    std::vector<ContentSource> sources;
    for (usize index = 0; index < 10; ++index) {
        ContentSource source;
        source.kind = SourceKind::Pack;
        source.id = std::format("pack{}", index);
        source.entries.push_back(
            ContentEntry{ContentKind::Item, static_cast<ContentId>(index + 1), std::format("thing{}", index)});
        sources.push_back(std::move(source));
    }
    return sources;
}

} // namespace

T2D_TEST(the_three_stages_are_one_list_in_load_order) {
    ContentRegistry registry;
    ContentPipeline pipeline;
    pipeline.set_base_files({std::string(T2D_SOURCE_DIR) + "/games/mine/tests/data/placeholder_content.ecfg"});
    pipeline.set_pack_directories({T2D_TEST_PACKS_DIR});
    pipeline.set_mod_directories({T2D_TEST_MODS_DIR});
    const ContentPipelineReport& report = pipeline.load(registry);
    T2D_REQUIRE(report.clean());
    const ContentListModel list = list_of(pipeline);

    const ContentListTotals totals = list.totals();
    T2D_CHECK_EQ(totals.files, 1u);
    T2D_CHECK_EQ(totals.packs, 3u);
    T2D_CHECK_EQ(totals.mods, 2u);
    T2D_CHECK_EQ(totals.native_mods, 1u);
    T2D_CHECK_EQ(totals.failed, 0u);
    T2D_CHECK_EQ(totals.partial, 0u);
    T2D_CHECK(totals.clean());
    T2D_CHECK(list.issues().empty());
    // What the sources say they added is exactly what the registry holds: a list that disagrees with
    // the registry would be a list of something else.
    T2D_CHECK_EQ(totals.entries, registry.total_count());
    T2D_CHECK_EQ(list.source_count(), report.sources.size());
    T2D_CHECK_EQ(list.row_count(), list.source_count());   // nothing is open yet

    // The game's own file first, then the packs, then the mods.
    T2D_CHECK_EQ(list.source(0).kind, SourceKind::File);
    T2D_CHECK_EQ(list.source(0).id, std::string("placeholder_content"));
    T2D_CHECK_EQ(list.source(0).entries.size(), report.base_registered);
    T2D_CHECK_EQ(list.source(1).kind, SourceKind::Pack);
    T2D_CHECK_EQ(list.source(1).id, std::string("base_pack"));
    T2D_CHECK_EQ(list.source(1).name, std::string("Base pack"));
    T2D_CHECK_EQ(list.source(1).version, std::string("2.0"));
    T2D_CHECK_EQ(list.source(1).images, 2u);
    T2D_CHECK_EQ(list.source(1).images_failed, 0u);
    T2D_CHECK(list.source(1).clean());
    T2D_CHECK_EQ(std::string(source_kind_id(list.source(1).kind)), std::string("content.kind.pack"));
    T2D_CHECK_EQ(std::string(source_status_id(list.source(1))), std::string("content.status.ok"));
    T2D_CHECK_EQ(list.source(list.source_count() - 1).kind, SourceKind::Mod);

    // A source's entries are the names it registered and the ids a save would store.
    const ContentSource* base = find_source(list, "base_pack");
    T2D_REQUIRE(base != nullptr);
    T2D_CHECK_EQ(names_of(*base).size(), 2u);
    T2D_CHECK_EQ(base->entries[0].name, std::string("pack_ore"));
    T2D_CHECK_EQ(base->entries[0].kind, ContentKind::Item);
    T2D_CHECK_EQ(base->entries[0].id, registry.find(ContentKind::Item, "pack_ore"));

    const ContentSource* native = find_source(list, "example_native");
    T2D_REQUIRE(native != nullptr);
    T2D_CHECK(native->native);
    T2D_CHECK_EQ(native->requirements.size(), 1u);
    T2D_CHECK_EQ(native->requirements[0], std::string("example_data"));
    // The three drills are not written in any file: the native module registered them while it ran,
    // and they are on the list like anything else.
    T2D_CHECK_EQ(native->entries.size(), 3u);
    T2D_CHECK_EQ(native->entries[0].name, std::string("mod_tier_1_drill"));
    const ContentSource* data_only = find_source(list, "example_data");
    T2D_REQUIRE(data_only != nullptr);
    T2D_CHECK_FALSE(data_only->native);
}

T2D_TEST(a_pack_that_does_not_parse_is_a_line_and_not_a_silence) {
    Scratch scratch;
    scratch.write("good.ecfg", "pack::\n    id:\"good\"\nitem::\n    fine::\n");
    scratch.write("bad.ecfg", "item::\n    good_one::\n    bad_one:unquoted\n");
    ContentRegistry registry;
    ContentPipeline pipeline;
    pipeline.set_pack_directories({scratch.root()});
    const ContentPipelineReport& report = pipeline.load(registry);
    T2D_CHECK_FALSE(report.clean());
    const ContentListModel list = list_of(pipeline);

    // Both files are packs, and the broken one says why it is not there.
    T2D_CHECK_EQ(list.totals().packs, 2u);
    T2D_CHECK_EQ(list.totals().failed, 1u);
    const ContentSource* bad = find_source(list, "bad");
    T2D_REQUIRE(bad != nullptr);
    T2D_CHECK_EQ(bad->kind, SourceKind::Pack);
    T2D_CHECK_FALSE(bad->ok);
    T2D_CHECK(bad->entries.empty());
    T2D_CHECK(bad->error.find(":3:") != std::string::npos);   // the reader says where
    T2D_CHECK_EQ(std::string(source_status_id(*bad)), std::string("content.status.failed"));
    const ContentSource* good = find_source(list, "good");
    T2D_REQUIRE(good != nullptr);
    T2D_CHECK(good->clean());
    T2D_CHECK_EQ(good->entries.size(), 1u);

    // The pack's own line carries the message, so the message block does not say it again.
    T2D_CHECK(list.issues().empty());

    // A warning is nobody's line: a table that is not a content kind lands in the message block.
    Scratch typo;
    typo.write("typo.ecfg", "pack::\n    id:\"typo\"\nstructurs::\n    wall::\nitem::\n    ore::\n");
    ContentRegistry typo_registry;
    ContentPipeline typo_pipeline;
    typo_pipeline.set_pack_directories({typo.root()});
    T2D_CHECK(typo_pipeline.load(typo_registry).clean());
    const ContentListModel typo_list = list_of(typo_pipeline);
    T2D_REQUIRE(typo_list.issues().size() == 1u);
    T2D_CHECK_FALSE(typo_list.issues()[0].error);
    T2D_CHECK(typo_list.issues()[0].text.find("structurs") != std::string::npos);
    T2D_CHECK(typo_list.totals().clean());   // the pack loaded: a warning is not a failure
}

T2D_TEST(a_source_that_lost_a_name_reads_as_partial) {
    Scratch scratch;
    scratch.write("base.ecfg", "item::\n    shared_thing::\n");
    scratch.write("pack.ecfg", "pack::\n    id:\"greedy\"\nitem::\n    shared_thing::\n    its_own::\n");
    ContentRegistry registry;
    ContentPipeline pipeline;
    pipeline.set_base_files({scratch.root() + "/base.ecfg"});
    pipeline.set_pack_files({scratch.root() + "/pack.ecfg"});
    const ContentPipelineReport& report = pipeline.load(registry);
    T2D_CHECK_FALSE(report.clean());
    const ContentListModel list = list_of(pipeline);

    const ContentSource* greedy = find_source(list, "greedy");
    T2D_REQUIRE(greedy != nullptr);
    T2D_CHECK(greedy->ok);              // it loaded
    T2D_CHECK_FALSE(greedy->clean());   // and lost a name doing it
    T2D_CHECK_EQ(std::string(source_status_id(*greedy)), std::string("content.status.partial"));
    T2D_CHECK(greedy->error.find("shared_thing") != std::string::npos);
    T2D_CHECK_EQ(names_of(*greedy).size(), 1u);   // its_own; shared_thing kept its first owner
    T2D_CHECK_EQ(list.totals().partial, 1u);
    T2D_CHECK_EQ(list.totals().failed, 0u);
    T2D_CHECK_FALSE(list.totals().clean());
    T2D_CHECK_EQ(list.totals().entries, registry.total_count());
    T2D_CHECK(list.issues().empty());
}

T2D_TEST(a_pack_whose_requirement_is_missing_is_listed_as_failed) {
    Scratch scratch;
    scratch.write("needy.ecfg", "pack::\n    id:\"needy\"\n    requires:[\"nope\"]\nitem::\n    thing::\n");
    ContentRegistry registry;
    ContentPipeline pipeline;
    pipeline.set_pack_directories({scratch.root()});
    const ContentPipelineReport& report = pipeline.load(registry);
    T2D_CHECK_FALSE(report.clean());
    const ContentListModel list = list_of(pipeline);

    const ContentSource* needy = find_source(list, "needy");
    T2D_REQUIRE(needy != nullptr);
    T2D_CHECK_FALSE(needy->ok);
    T2D_CHECK_EQ(needy->requirements.size(), 1u);
    T2D_CHECK_EQ(needy->requirements[0], std::string("nope"));
    T2D_CHECK(needy->entries.empty());
    T2D_CHECK_EQ(list.totals().failed, 1u);
    T2D_CHECK_EQ(list.totals().entries, 0u);
    T2D_CHECK_EQ(registry.total_count(), 0u);
    // The pipeline's own line ("'needy' requires 'nope', which is not loaded") is not the pack's error,
    // so it stays in the message block: the reason a requirement failed is not always the pack's fault.
    T2D_CHECK(has_issue(list, "requires"));
}

T2D_TEST(a_mod_package_that_is_refused_is_still_a_line) {
    Scratch scratch;
    scratch.write("broken/mod.ecfg", "id:\"broken\"\ntiers:3\n");       // an unknown manifest key
    scratch.write("fine/mod.ecfg", "id:\"fine\"\ncontent:[\"c.ecfg\"]\n");
    scratch.write("fine/c.ecfg", "item::\n    fine_thing::\n");
    scratch.write("one/mod.ecfg", "id:\"same\"\n");
    scratch.write("two/mod.ecfg", "id:\"same\"\n");
    ContentRegistry registry;
    ContentPipeline pipeline;
    pipeline.set_mod_directories({scratch.root()});
    const ContentPipelineReport& report = pipeline.load(registry);
    T2D_CHECK_FALSE(report.clean());
    const ContentListModel list = list_of(pipeline);

    // Three packages loaded, and the two the host refused are on the list with the folder they live in
    // as their name: a manifest that does not parse has no id to show.
    T2D_CHECK_EQ(list.totals().mods, 4u);
    T2D_CHECK_EQ(list.totals().failed, 2u);
    const ContentSource* broken = find_source(list, "broken");
    T2D_REQUIRE(broken != nullptr);
    T2D_CHECK_EQ(broken->kind, SourceKind::Mod);
    T2D_CHECK_FALSE(broken->ok);
    T2D_CHECK(broken->error.find("is not a manifest key") != std::string::npos);
    T2D_CHECK_EQ(std::string(source_status_id(*broken)), std::string("content.status.failed"));

    usize same_count = 0;
    usize same_failed = 0;
    for (const ContentSource& source : list.sources()) {
        if (source.id != "same") continue;
        ++same_count;
        if (source.ok) continue;
        ++same_failed;
        T2D_CHECK(source.error.find("is defined twice") != std::string::npos);
    }
    T2D_CHECK_EQ(same_count, 2u);   // both packages claiming the id are on the list
    T2D_CHECK_EQ(same_failed, 1u);  // and exactly one of them is running

    const ContentSource* fine = find_source(list, "fine");
    T2D_REQUIRE(fine != nullptr);
    T2D_CHECK(fine->clean());
    T2D_CHECK_EQ(fine->entries.size(), 1u);
    T2D_CHECK(fine->path.find("fine") != std::string::npos);   // the folder the package lives in
    T2D_CHECK_EQ(list.totals().entries, registry.total_count());
}

T2D_TEST(a_source_opens_to_show_what_it_registered) {
    std::vector<ContentSource> sources;
    ContentSource file;
    file.kind = SourceKind::File;
    file.id = "base";
    file.entries = {ContentEntry{ContentKind::Item, 1, "one"}, ContentEntry{ContentKind::Item, 2, "two"}};
    ContentSource nothing;
    nothing.kind = SourceKind::Pack;
    nothing.id = "empty_pack";   // it declares no content at all
    ContentSource mod;
    mod.kind = SourceKind::Mod;
    mod.id = "mod";
    mod.native = true;
    mod.entries = {ContentEntry{ContentKind::Machine, 1, "drill"}};
    sources = {file, nothing, mod};

    ContentListModel list;
    list.set_sources(sources);
    T2D_CHECK_EQ(list.row_count(), 3u);
    T2D_CHECK_EQ(list.selected(), 0u);
    T2D_CHECK_FALSE(list.row(0).is_entry());
    T2D_CHECK_EQ(list.row(0).depth(), 0);
    T2D_CHECK_EQ(list.source_count(), 3u);

    // Opening a source puts its entries under it, and the selection stays on the source.
    T2D_CHECK(list.toggle());
    T2D_CHECK_EQ(list.row_count(), 5u);
    T2D_CHECK(list.is_open(0));
    T2D_CHECK_EQ(list.selected(), 0u);
    T2D_CHECK_EQ(list.selected_source(), 0u);
    T2D_CHECK(list.row(1).is_entry());
    T2D_CHECK_EQ(list.row(1).depth(), 1);
    T2D_CHECK_EQ(list.row(1).source, 0u);
    T2D_CHECK_EQ(list.row(1).entry, 0);
    T2D_CHECK_EQ(list.source(list.row(1).source).entries[0].name, std::string("one"));
    T2D_CHECK_EQ(list.row(2).entry, 1);

    // A source with nothing inside cannot be opened: there is nothing a fold-out would show.
    list.select(3);
    T2D_CHECK_EQ(list.selected_source(), 1u);
    T2D_CHECK_FALSE(list.toggle());
    T2D_CHECK_FALSE(list.is_open(1));
    T2D_CHECK_EQ(list.row_count(), 5u);
    // Closing works the same way, and closing what is already closed changes nothing.
    list.select(0);
    T2D_CHECK_FALSE(list.set_open(0, false));
    T2D_CHECK_EQ(list.row_count(), 3u);
    T2D_CHECK_FALSE(list.set_open(0, false));

    // Open everything that can be opened, then close it all again.
    list.open_all();
    T2D_CHECK(list.is_open(0));
    T2D_CHECK_FALSE(list.is_open(1));
    T2D_CHECK(list.is_open(2));
    T2D_CHECK_EQ(list.row_count(), 6u);
    list.close_all();
    T2D_CHECK_EQ(list.row_count(), 3u);
    T2D_CHECK_FALSE(list.is_open(2));
    // A source index that is not in the list is not a crash.
    T2D_CHECK_FALSE(list.set_open(99, true));
    T2D_CHECK_FALSE(list.is_open(99));
}

T2D_TEST(the_selection_never_leaves_the_lines_that_fit) {
    ContentListModel list;
    list.set_sources(ten_sources());
    T2D_CHECK_EQ(list.row_count(), 10u);
    list.set_visible_rows(5);
    T2D_CHECK_EQ(list.visible_rows(), 5u);
    T2D_CHECK_EQ(list.first_visible(), 0u);

    // Down past the last visible line scrolls the list instead of running off the panel.
    list.move(5);
    T2D_CHECK_EQ(list.selected(), 5u);
    T2D_CHECK_EQ(list.first_visible(), 1u);
    list.move(-5);
    T2D_CHECK_EQ(list.selected(), 0u);
    T2D_CHECK_EQ(list.first_visible(), 0u);

    // The ends do not wrap: holding a key cannot spin the list.
    list.move(-3);
    T2D_CHECK_EQ(list.selected(), 0u);
    list.select_last();
    T2D_CHECK_EQ(list.selected(), list.row_count() - 1);
    T2D_CHECK_EQ(list.first_visible(), list.row_count() - 5u);
    list.move(3);
    T2D_CHECK_EQ(list.selected(), list.row_count() - 1);
    T2D_CHECK_EQ(list.first_visible(), list.row_count() - 5u);

    // A list that fits does not scroll at all: empty lines under the last entry look like entries that
    // failed to load.
    list.set_visible_rows(20);
    T2D_CHECK_EQ(list.first_visible(), 0u);
    list.select_last();
    T2D_CHECK_EQ(list.first_visible(), 0u);

    // Fewer lines than before keeps the selection inside them.
    list.set_visible_rows(3);
    T2D_CHECK_EQ(list.selected(), list.row_count() - 1);
    T2D_CHECK(list.first_visible() <= list.selected());
    T2D_CHECK(list.first_visible() + 3u > list.selected());

    // No room at all draws nothing, and the list does not pretend to be scrolled somewhere.
    list.set_visible_rows(0);
    T2D_CHECK_EQ(list.first_visible(), list.selected());
    list.set_visible_rows(4);
    T2D_CHECK(list.first_visible() + 4u > list.selected());
}

T2D_TEST(a_reload_puts_the_list_back_at_the_top_and_closed) {
    ContentListModel list;
    list.set_sources(ten_sources());
    list.open_all();
    T2D_CHECK_EQ(list.row_count(), 20u);
    list.select_last();
    T2D_CHECK_EQ(list.selected(), 19u);

    list.set_sources(ten_sources());
    T2D_CHECK_EQ(list.selected(), 0u);
    T2D_CHECK_EQ(list.first_visible(), 0u);
    T2D_CHECK_EQ(list.row_count(), 10u);
    T2D_CHECK_FALSE(list.is_open(0));

    // An empty list is a state, not a crash: this is what a run with nothing loaded shows.
    list.set_sources({});
    T2D_CHECK_EQ(list.source_count(), 0u);
    T2D_CHECK_EQ(list.row_count(), 0u);
    T2D_CHECK_EQ(list.selected(), 0u);
    T2D_CHECK_EQ(list.first_visible(), 0u);
    T2D_CHECK_EQ(list.totals().sources, 0u);
    T2D_CHECK(list.totals().clean());
    list.move(1);
    list.select_last();
    T2D_CHECK_FALSE(list.toggle());
    T2D_CHECK_EQ(list.selected(), 0u);
    // A source that is not there is an empty source, not a dangling reference.
    T2D_CHECK_EQ(list.source(7).kind, SourceKind::File);
    T2D_CHECK(list.source(7).entries.empty());
}

T2D_TEST(every_label_the_content_screen_shows_exists_in_the_shipped_string_table) {
    // The screen hands out locale ids, so a typo in one of them would be printed as the id itself.
    const std::string path = std::string(T2D_SOURCE_DIR) + "/assets/text/ui.ecfg";
    t2d::EcfgError error;
    std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::load(path, &error);
    if (!document.has_value()) {
        T2D_CHECK_MSG(false, "{}", error.describe(path));
        return;
    }

    std::vector<std::string> ids;
    for (const char* id : content_list_locale_ids()) ids.emplace_back(id);
    for (const SourceKind kind : {SourceKind::File, SourceKind::Pack, SourceKind::Mod}) {
        ids.emplace_back(source_kind_id(kind));
    }
    const ContentSource clean_source;
    ContentSource partial;
    partial.error = "something was lost";
    ContentSource failed;
    failed.ok = false;
    ids.emplace_back(source_status_id(clean_source));
    ids.emplace_back(source_status_id(partial));
    ids.emplace_back(source_status_id(failed));
    T2D_CHECK_GE(ids.size(), 20u);

    for (const char* language : {"en", "zh-Hans", "zh-Hant"}) {
        const t2d::EcfgValue* table = document->find(language);
        T2D_CHECK_MSG(table != nullptr && table->is_table(), "the string table '{}' is missing", language);
        if (table == nullptr) continue;
        for (const std::string& id : ids) {
            const t2d::EcfgValue* value = table->find(id);
            T2D_CHECK_MSG(value != nullptr && value->is_string() && !value->as_string().empty(),
                          "'{}' has no text in '{}'", id, language);
        }
    }

    // Three kinds and three states, all saying different things: a badge and a status column that read
    // the same would be saying nothing.
    T2D_CHECK_NE(std::string(source_kind_id(SourceKind::File)), std::string(source_kind_id(SourceKind::Pack)));
    T2D_CHECK_NE(std::string(source_kind_id(SourceKind::Pack)), std::string(source_kind_id(SourceKind::Mod)));
    T2D_CHECK_NE(std::string(source_status_id(clean_source)), std::string(source_status_id(partial)));
    T2D_CHECK_NE(std::string(source_status_id(partial)), std::string(source_status_id(failed)));
}

T2D_TEST_MAIN
