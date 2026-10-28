// The save library: one directory per slot, one auto.t2dsave inside it, and the names a player types for
// them - checked the way a file system checks a directory name, CJK included (mine/save_library.h,
// docs/GAME_DESIGN.md section 1.48).
#include <mine/save_library.h>

#include <support/test_support.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace mine;

namespace {

namespace fs = std::filesystem;

/// A scratch library root, removed when the test ends.
class Scratch {
public:
    Scratch() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path() / std::format("t2d_save_library_{}", stamp);
        std::error_code code;
        fs::remove_all(root_, code);
        fs::create_directories(root_, code);
    }
    ~Scratch() {
        std::error_code code;
        fs::remove_all(root_, code);
    }
    T2D_NON_COPYABLE(Scratch);

    [[nodiscard]] std::string root() const { return root_.string(); }

    /// Writes a slot's automatic save, as old as \p age: the tests need an order, not real seconds.
    void write_autosave(std::string_view name, std::string_view bytes, std::chrono::seconds age) const {
        const fs::path directory = root_ / std::string(name);
        std::error_code code;
        fs::create_directories(directory, code);
        const fs::path file = directory / std::string(kAutosaveFileName);
        std::ofstream out(file, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.close();
        fs::last_write_time(file, fs::file_time_type::clock::now() - age, code);
    }

private:
    fs::path root_;
};

[[nodiscard]] std::string repeated(std::string_view unit, usize count) {
    std::string out;
    out.reserve(unit.size() * count);
    for (usize index = 0; index < count; ++index) out += unit;
    return out;
}

} // namespace

T2D_TEST(a_slot_is_a_directory_with_one_autosave) {
    Scratch scratch;
    SaveLibrary library(scratch.root());
    T2D_CHECK_EQ(library.slots().size(), 0u);
    T2D_CHECK_FALSE(library.latest_autosave().has_value());

    const char* error = "untouched";
    SaveSlot made;
    T2D_CHECK(library.create("first mine", error, made));
    T2D_CHECK_EQ(std::string(error), std::string(""));
    T2D_CHECK_EQ(made.name, std::string("first mine"));
    T2D_CHECK(fs::is_directory(made.directory));
    T2D_CHECK_EQ(made.autosave_path, library.autosave_path("first mine"));
    T2D_CHECK_EQ(fs::path(made.autosave_path).filename().string(), std::string("auto.t2dsave"));
    T2D_CHECK_FALSE(made.has_autosave);

    // A slot that was just made exists but has nothing to show yet.
    std::vector<SaveSlot> slots = library.slots();
    T2D_REQUIRE(slots.size() == 1u);
    T2D_CHECK_EQ(slots[0].name, std::string("first mine"));
    T2D_CHECK_FALSE(slots[0].has_autosave);
    T2D_CHECK_FALSE(library.latest_autosave().has_value());

    // The write a save makes is what the slot then reports.
    scratch.write_autosave("first mine", "T2D", std::chrono::seconds(3));
    slots = library.slots();
    T2D_REQUIRE(slots.size() == 1u);
    T2D_CHECK(slots[0].has_autosave);
    // The file time is not formatted for a failure message, so it is compared rather than printed.
    T2D_CHECK(slots[0].modified == fs::last_write_time(slots[0].autosave_path));
    T2D_REQUIRE(library.latest_autosave().has_value());
    T2D_CHECK_EQ(library.latest_autosave()->name, std::string("first mine"));
}

T2D_TEST(a_name_a_slot_cannot_have) {
    const std::vector<std::pair<std::string, std::string_view>> bad = {
        {"", "save.name.empty"},
        {"\nname", "save.name.invalid"},
        {"bad\x7F", "save.name.invalid"},
        {"na/me", "save.name.invalid"},
        {"na\\me", "save.name.invalid"},
        {"na:me", "save.name.invalid"},
        {"na*me", "save.name.invalid"},
        {"na?me", "save.name.invalid"},
        {"na\"me", "save.name.invalid"},
        {"na<me", "save.name.invalid"},
        {"na>me", "save.name.invalid"},
        {"na|me", "save.name.invalid"},
        {" leading", "save.name.invalid"},
        {"trailing ", "save.name.invalid"},
        {"trailing.", "save.name.invalid"},
        {".", "save.name.invalid"},
        {"..", "save.name.invalid"},
        {"con", "save.name.invalid"},
        {"CON", "save.name.invalid"},
        {"Con.txt", "save.name.invalid"},
        {"com1", "save.name.invalid"},
        {"LPT9", "save.name.invalid"},
        {"bad\xFF", "save.name.invalid"},
        {std::string(49, 'a'), "save.name.long"},
        // 43 CJK characters are 129 bytes: under the character limit, over the byte one.
        {repeated("\xE7\x9F\xBF", 43), "save.name.long"},
    };
    for (const auto& [name, wanted] : bad) {
        const std::string_view id = save_name_error_id(name);
        T2D_CHECK_MSG(id == wanted, "'{}' was '{}', wanted '{}'", name, id, wanted);
    }

    const std::vector<std::string> good = {
        "a", "my mine", "mine 1", "a.b", "CONSOLE", "我的矿场", "tab\tname", std::string(48, 'a'),
        repeated("\xE7\x9F\xBF", 42),
    };
    for (const std::string& name : good) {
        const std::string_view id = save_name_error_id(name);
        T2D_CHECK_MSG(id.empty(), "'{}' was refused with '{}'", name, id);
    }

    // Every id a refusal can produce is named once, for the string table task-16 will fill in.
    for (const std::string_view id : kSaveNameErrorIds) T2D_CHECK(!id.empty());
}

T2D_TEST(a_tab_in_a_name_is_only_a_character) {
    // The name field inserts a tab for the Tab key, so a slot whose name has one has to be makeable.
    Scratch scratch;
    SaveLibrary library(scratch.root());
    const char* error = "";
    SaveSlot made;
    T2D_CHECK(library.create("two\twords", error, made));
    T2D_CHECK_EQ(made.name, std::string("two\twords"));
    T2D_CHECK(fs::is_directory(made.directory));
    const std::vector<SaveSlot> slots = library.slots();
    T2D_REQUIRE(slots.size() == 1u);
    T2D_CHECK_EQ(slots[0].name, std::string("two\twords"));
}

T2D_TEST(slots_come_newest_autosave_first_then_by_name) {
    Scratch scratch;
    SaveLibrary library(scratch.root());
    const char* error = "";
    SaveSlot made;
    T2D_CHECK(library.create("alpha", error, made));
    T2D_CHECK(library.create("beta", error, made));
    T2D_CHECK(library.create("gamma", error, made));

    // gamma has the newest automatic save, alpha an older one, beta none at all.
    scratch.write_autosave("alpha", "A", std::chrono::seconds(20));
    scratch.write_autosave("gamma", "G", std::chrono::seconds(5));

    const std::vector<SaveSlot> slots = library.slots();
    T2D_REQUIRE(slots.size() == 3u);
    T2D_CHECK_EQ(slots[0].name, std::string("gamma"));
    T2D_CHECK_EQ(slots[1].name, std::string("alpha"));
    T2D_CHECK_EQ(slots[2].name, std::string("beta"));
    T2D_CHECK(slots[0].has_autosave);
    T2D_CHECK(slots[1].has_autosave);
    T2D_CHECK_FALSE(slots[2].has_autosave);
    T2D_CHECK_GT(slots[0].modified, slots[1].modified);
    T2D_REQUIRE(library.latest_autosave().has_value());
    T2D_CHECK_EQ(library.latest_autosave()->name, std::string("gamma"));
}

T2D_TEST(a_taken_name_is_refused_without_touching_the_disk) {
    Scratch scratch;
    SaveLibrary library(scratch.root());
    const char* error = "";
    SaveSlot made;
    T2D_CHECK(library.create("taken", error, made));
    T2D_CHECK_FALSE(library.create("taken", error, made));
    T2D_CHECK_EQ(std::string(error), std::string("save.name.exists"));

    // A directory the library did not make is taken too: the name is the directory, not a record beside it.
    std::error_code code;
    fs::create_directories(fs::path(scratch.root()) / "by hand", code);
    T2D_CHECK_FALSE(library.create("by hand", error, made));
    T2D_CHECK_EQ(std::string(error), std::string("save.name.exists"));

    T2D_CHECK_FALSE(library.create("bad/name", error, made));
    T2D_CHECK_EQ(std::string(error), std::string("save.name.invalid"));
    T2D_CHECK_FALSE(fs::exists(fs::path(scratch.root()) / "bad", code));
    T2D_CHECK_FALSE(library.create("", error, made));
    T2D_CHECK_EQ(std::string(error), std::string("save.name.empty"));
}

T2D_TEST(find_and_the_paths_a_slot_needs) {
    Scratch scratch;
    SaveLibrary library(scratch.root());
    const char* error = "";
    SaveSlot made;
    T2D_REQUIRE(library.create("slot", error, made));

    T2D_CHECK_EQ(library.slot_directory("slot"), made.directory);
    T2D_CHECK_EQ(library.autosave_path("slot"), made.autosave_path);
    T2D_REQUIRE(library.find("slot").has_value());
    T2D_CHECK_FALSE(library.find("slot")->has_autosave);
    T2D_CHECK_FALSE(library.find("absent").has_value());

    scratch.write_autosave("slot", "T2D", std::chrono::seconds(1));
    T2D_REQUIRE(library.find("slot").has_value());
    T2D_CHECK(library.find("slot")->has_autosave);
}

T2D_TEST(a_library_that_cannot_make_its_root_says_failed) {
    Scratch scratch;
    // A file where the root directory would be: the library cannot make the slot under it.
    const fs::path blocker = fs::path(scratch.root()) / "blocked";
    std::ofstream(blocker) << "not a directory";
    SaveLibrary library((blocker / "root").string());
    const char* error = "";
    SaveSlot made;
    T2D_CHECK_FALSE(library.create("slot", error, made));
    T2D_CHECK_EQ(std::string(error), std::string("save.name.failed"));
    T2D_CHECK(library.slots().empty());
}

T2D_TEST(a_cjk_name_is_a_directory_like_any_other) {
    Scratch scratch;
    SaveLibrary library(scratch.root());
    const char* error = "";
    SaveSlot made;
    T2D_CHECK(library.create("我的矿场", error, made));
    T2D_CHECK_EQ(made.name, std::string("我的矿场"));
    T2D_CHECK(fs::is_directory(made.directory));

    const std::vector<SaveSlot> slots = library.slots();
    T2D_REQUIRE(slots.size() == 1u);
    T2D_CHECK_EQ(slots[0].name, std::string("我的矿场"));

    scratch.write_autosave("我的矿场", "T2D", std::chrono::seconds(0));
    T2D_REQUIRE(library.latest_autosave().has_value());
    T2D_CHECK_EQ(library.latest_autosave()->name, std::string("我的矿场"));
}

T2D_TEST_MAIN
