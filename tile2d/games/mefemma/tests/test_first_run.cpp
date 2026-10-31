// Mefemma's first-run save path and the settings a save directory keeps, without a window: redirect.ecfg's
// reader/writer and path rules, the UTF-8 path field model the prompt uses, and the save directory's
// config.ecfg reader/writer. All are mefemma_core work, so this suite needs no GPU and no scene; the
// first-run screen and the shell wiring are tested where that code lands.
#include <mefemma/config.h>
#include <mefemma/path_input.h>
#include <mefemma/redirect.h>

#include <support/test_support.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

using namespace mefemma;

namespace {

namespace fs = std::filesystem;

/// A directory under the system temporary directory that this run alone uses, removed when the test ends.
class ScratchDir {
public:
    ScratchDir() {
        static std::atomic<unsigned> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() /
                ("t2d_mefemma_first_run_" + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1)));
        std::error_code code;
        fs::create_directories(path_, code);
    }
    ~ScratchDir() {
        std::error_code code;
        fs::remove_all(path_, code);
    }
    T2D_NON_MOVABLE(ScratchDir);

    [[nodiscard]] const fs::path& path() const { return path_; }
    [[nodiscard]] std::string file(const std::string& name) const { return (path_ / name).string(); }

private:
    fs::path path_;
};

void write_text(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

[[nodiscard]] std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

T2D_TEST(redirect_round_trips_a_value_with_a_quote_and_a_backslash) {
    ScratchDir dir;
    const std::string path = dir.file("redirect.ecfg");
    // Quotes and backslashes are the two bytes the .ecfg string parser treats specially; a save directory
    // may legitimately contain both on POSIX, so they have to survive a save/load round trip.
    const std::string value = (dir.path() / "sa\"ves\\x").string();
    std::string error;
    T2D_CHECK_MSG(save_redirect(path, value, &error), "save failed: {}", error);
    T2D_CHECK(error.empty());
    T2D_CHECK(fs::is_regular_file(path));

    t2d::EcfgError load_error;
    const std::optional<RedirectConfig> config = load_redirect(path, &load_error);
    T2D_REQUIRE(config.has_value());
    T2D_CHECK_EQ(config->save_path, value);
    T2D_CHECK(load_error.message.empty());
}

T2D_TEST(a_missing_malformed_wrong_or_empty_redirect_is_not_a_config) {
    ScratchDir dir;
    t2d::EcfgError error;

    // Missing file: the first-run answer, with a reason for the caller that wants to log it.
    T2D_CHECK_FALSE(load_redirect(dir.file("missing.ecfg"), &error).has_value());
    T2D_CHECK_FALSE(error.message.empty());

    // A parse error is not a configuration either.
    write_text(dir.path() / "malformed.ecfg", "save_path:\"unterminated\n");
    error = {};
    T2D_CHECK_FALSE(load_redirect(dir.file("malformed.ecfg"), &error).has_value());
    T2D_CHECK_FALSE(error.message.empty());

    // The field has to be there, be a quoted string, and hold something.
    write_text(dir.path() / "missing-field.ecfg", "other:1\n");
    error = {};
    T2D_CHECK_FALSE(load_redirect(dir.file("missing-field.ecfg"), &error).has_value());
    T2D_CHECK(error.message.find("save_path") != std::string::npos);

    write_text(dir.path() / "wrong-type.ecfg", "save_path:42\n");
    error = {};
    T2D_CHECK_FALSE(load_redirect(dir.file("wrong-type.ecfg"), &error).has_value());
    T2D_CHECK(error.message.find("quoted string") != std::string::npos);

    write_text(dir.path() / "empty.ecfg", "save_path:\"\"\n");
    error = {};
    T2D_CHECK_FALSE(load_redirect(dir.file("empty.ecfg"), &error).has_value());
    T2D_CHECK(error.message.find("empty") != std::string::npos);

    // Unknown fields are a newer game's business, not a reason to lose the save directory.
    write_text(dir.path() / "extra-fields.ecfg", "other:1\nsave_path:\"/saves/here\"\nmore:true\n");
    error = {};
    const std::optional<RedirectConfig> config = load_redirect(dir.file("extra-fields.ecfg"), &error);
    T2D_REQUIRE(config.has_value());
    T2D_CHECK_EQ(config->save_path, std::string("/saves/here"));
}

T2D_TEST(save_redirect_creates_the_parent_and_atomically_replaces_an_old_file) {
    ScratchDir dir;
    const std::string path = dir.file("nested/deep/redirect.ecfg");
    T2D_CHECK_FALSE(fs::exists(path));

    std::string error;
    T2D_CHECK_MSG(save_redirect(path, "/tmp/one", &error), "save failed: {}", error);
    T2D_CHECK(fs::is_regular_file(path));
    T2D_CHECK_FALSE(fs::exists(path + ".tmp"));   // the temporary was renamed, not left behind
    T2D_CHECK(read_text(path).find("save_path:") != std::string::npos);

    t2d::EcfgError load_error;
    std::optional<RedirectConfig> config = load_redirect(path, &load_error);
    T2D_REQUIRE(config.has_value());
    T2D_CHECK_EQ(config->save_path, std::string("/tmp/one"));

    // A second save replaces the first: the target is renamed over, so opening an old file never shows the
    // new value half written.
    T2D_CHECK(save_redirect(path, "/tmp/two", &error));
    config = load_redirect(path, &load_error);
    T2D_REQUIRE(config.has_value());
    T2D_CHECK_EQ(config->save_path, std::string("/tmp/two"));
    T2D_CHECK_FALSE(fs::exists(path + ".tmp"));

    // A parent that is a file is a readable failure, not a crash.
    error.clear();
    const fs::path blocker = dir.path() / "blocker";
    write_text(blocker, "x");
    T2D_CHECK_FALSE(save_redirect((blocker / "redirect.ecfg").string(), "/tmp/three", &error));
    T2D_CHECK_FALSE(error.empty());
}

T2D_TEST(the_redirect_file_lives_beside_the_executable_and_saves_its_sibling) {
    const std::string path = default_redirect_path();
    T2D_CHECK(path.ends_with("redirect.ecfg"));
    T2D_CHECK_GE(path.size(), std::string("redirect.ecfg").size());

    const std::string saves = default_save_directory("/some/where/redirect.ecfg");
    T2D_CHECK_EQ(saves, (fs::path("/some/where") / "saves").string());
    T2D_CHECK(saves.ends_with("saves"));

    // A redirect file with no directory in its path still gets a usable answer rather than "/saves".
    T2D_CHECK_EQ(default_save_directory("redirect.ecfg"), std::string("saves"));
}

T2D_TEST(normalize_save_path_makes_typed_text_absolute_and_lexically_normal) {
    ScratchDir dir;
    const std::string base = dir.path().string();
    std::string error;

    const std::optional<std::string> relative = normalize_save_path("saves", base, &error);
    T2D_REQUIRE(relative.has_value());
    T2D_CHECK_EQ(*relative, (fs::path(base) / "saves").lexically_normal().string());
    T2D_CHECK(fs::path(*relative).is_absolute());

    // "a/../b" is a legal way to type a folder; the stored value is the simple one.
    const std::optional<std::string> messy = normalize_save_path("a/../b", base, &error);
    T2D_REQUIRE(messy.has_value());
    T2D_CHECK_EQ(*messy, (fs::path(base) / "b").lexically_normal().string());

    // Surrounding whitespace is not part of a path.
    const std::optional<std::string> trimmed = normalize_save_path("  saves\t", base, &error);
    T2D_REQUIRE(trimmed.has_value());
    T2D_CHECK_EQ(*trimmed, (fs::path(base) / "saves").lexically_normal().string());

    // An absolute path ignores the base but is still normalised.
    const std::string absolute = (dir.path() / "absolute" / ".." / "saves").string();
    const std::optional<std::string> absolute_result = normalize_save_path(absolute, "/ignored", &error);
    T2D_REQUIRE(absolute_result.has_value());
    T2D_CHECK_EQ(*absolute_result, (dir.path() / "saves").lexically_normal().string());

    // ~ expands from HOME; a machine with no HOME gets a reason instead of a literal "~" directory.
    const char* home = std::getenv("HOME");
    const std::optional<std::string> home_result = normalize_save_path("~/saves", base, &error);
    if (home != nullptr && *home != '\0') {
        T2D_REQUIRE(home_result.has_value());
        T2D_CHECK_EQ(*home_result, (fs::path(home) / "saves").lexically_normal().string());
    } else {
        T2D_CHECK_FALSE(home_result.has_value());
        T2D_CHECK_FALSE(error.empty());
    }

    // Empty input and control bytes are refused with a reason.
    error.clear();
    T2D_CHECK_FALSE(normalize_save_path("", base, &error).has_value());
    T2D_CHECK_FALSE(error.empty());
    error.clear();
    T2D_CHECK_FALSE(normalize_save_path("   \t", base, &error).has_value());
    T2D_CHECK_FALSE(error.empty());
    error.clear();
    T2D_CHECK_FALSE(normalize_save_path("bad\nname", base, &error).has_value());
    T2D_CHECK_FALSE(error.empty());
    error.clear();
    T2D_CHECK_FALSE(normalize_save_path(std::string("bad\0name", 8), base, &error).has_value());
    T2D_CHECK_FALSE(error.empty());

    // normalize() does not touch the file system: an existing FILE at the path is fine here, and
    // ensure_save_directory() is the step that refuses it.
    const fs::path existing_file = dir.path() / "already-a-file";
    write_text(existing_file, "x");
    error.clear();
    const std::optional<std::string> file_result = normalize_save_path("already-a-file", base, &error);
    T2D_CHECK(file_result.has_value());
    T2D_CHECK_EQ(*file_result, existing_file.lexically_normal().string());
}

T2D_TEST(ensure_save_directory_creates_nested_dirs_and_refuses_a_file) {
    ScratchDir dir;
    std::string error;

    const std::string nested = dir.file("a/b/c");
    T2D_CHECK_MSG(ensure_save_directory(nested, &error), "create failed: {}", error);
    T2D_CHECK(fs::is_directory(nested));

    // An existing directory is success, not an error.
    error.clear();
    T2D_CHECK(ensure_save_directory(nested, &error));
    T2D_CHECK(error.empty());

    // A regular file at the path is the one thing create_directories() cannot fix.
    const fs::path file = dir.path() / "a-file";
    write_text(file, "x");
    error.clear();
    T2D_CHECK_FALSE(ensure_save_directory(file.string(), &error));
    T2D_CHECK_FALSE(error.empty());

    error.clear();
    T2D_CHECK_FALSE(ensure_save_directory("", &error));
    T2D_CHECK_FALSE(error.empty());
}

T2D_TEST(path_input_inserts_utf8_and_moves_the_caret_by_codepoint) {
    PathInput input;
    T2D_CHECK(input.empty());
    T2D_CHECK_EQ(input.length(), 0u);
    T2D_CHECK_EQ(input.caret(), 0u);
    T2D_CHECK_EQ(PathInput::max_codepoints(), 512u);

    T2D_CHECK_EQ(input.insert("abc"), 3u);
    T2D_CHECK_EQ(input.text(), std::string_view("abc"));
    T2D_CHECK_EQ(input.caret(), 3u);
    T2D_CHECK_EQ(input.insert("中文"), 2u);
    T2D_CHECK_EQ(input.text(), std::string_view("abc中文"));
    T2D_CHECK_EQ(input.caret(), input.text().size());
    T2D_CHECK_EQ(input.length(), 5u);
    T2D_CHECK_EQ(input.insert("😀"), 1u);   // a four-byte character is one insertion and one backspace
    T2D_CHECK_EQ(input.length(), 6u);

    // The caret is a byte offset on a character boundary: one move_left is one character, not one byte.
    input.set_text("a中b");
    T2D_CHECK_EQ(input.caret(), input.text().size());
    input.home();
    T2D_CHECK_EQ(input.caret(), 0u);
    input.move_right();
    T2D_CHECK_EQ(input.caret(), 1u);
    input.move_right();
    T2D_CHECK_EQ(input.caret(), 4u);
    input.move_right();
    T2D_CHECK_EQ(input.caret(), 5u);
    input.move_right();
    T2D_CHECK_EQ(input.caret(), 5u);   // at the end
    input.move_left();
    T2D_CHECK_EQ(input.caret(), 4u);

    // Inserting in the middle puts the text at the caret and leaves the caret after it.
    input.set_text("ab");
    input.home();
    input.move_right();
    T2D_CHECK_EQ(input.insert("中"), 1u);
    T2D_CHECK_EQ(input.text(), std::string_view("a中b"));
    T2D_CHECK_EQ(input.caret(), 4u);
}

T2D_TEST(path_input_backspace_and_erase_work_on_whole_codepoints) {
    PathInput input;
    input.set_text("a中b😀");
    input.end();
    T2D_CHECK(input.backspace());
    T2D_CHECK_EQ(input.text(), std::string_view("a中b"));
    T2D_CHECK(input.backspace());
    T2D_CHECK_EQ(input.text(), std::string_view("a中"));
    T2D_CHECK_EQ(input.caret(), input.text().size());
    input.home();
    T2D_CHECK(input.erase());
    T2D_CHECK_EQ(input.text(), std::string_view("中"));
    T2D_CHECK(input.erase());
    T2D_CHECK(input.empty());
    T2D_CHECK_FALSE(input.erase());
    T2D_CHECK_FALSE(input.backspace());
}

T2D_TEST(path_input_respects_the_limit_and_filters_untypeable_bytes) {
    PathInput input;
    const std::string many(600, 'x');
    T2D_CHECK_EQ(input.insert(many), PathInput::kMaxCodepoints);
    T2D_CHECK_EQ(input.length(), PathInput::kMaxCodepoints);
    T2D_CHECK_EQ(input.text().size(), PathInput::kMaxCodepoints);
    T2D_CHECK_EQ(input.insert("y"), 0u);   // full
    input.backspace();
    T2D_CHECK_EQ(input.insert("y"), 1u);
    T2D_CHECK_EQ(input.length(), PathInput::kMaxCodepoints);

    // A path read from elsewhere goes through the same rules and keeps the caret at the end.
    input.set_text(many);
    T2D_CHECK_EQ(input.length(), PathInput::kMaxCodepoints);
    T2D_CHECK_EQ(input.caret(), input.text().size());
    input.clear();
    T2D_CHECK(input.empty());
    T2D_CHECK_EQ(input.caret(), 0u);

    // Newline, carriage return and DEL are skipped; tab is a byte a POSIX path may hold.
    T2D_CHECK_EQ(input.insert(std::string("a\nb\tc\rd") + '\x7f'), 5u);
    T2D_CHECK_EQ(input.text(), std::string_view("ab\tcd"));

    // A malformed byte is skipped, not stored as U+FFFD.
    std::string malformed = "a";
    malformed.push_back(static_cast<char>(0xFF));
    malformed += "b";
    input.clear();
    T2D_CHECK_EQ(input.insert(malformed), 2u);
    T2D_CHECK_EQ(input.text(), std::string_view("ab"));

    // set_text() truncates a too-long value rather than refusing it.
    input.set_text(std::string(600, 'z') + "\n" + "end");
    T2D_CHECK_EQ(input.length(), PathInput::kMaxCodepoints);
}

T2D_TEST(config_path_joins_the_save_directory_and_the_file_name) {
    T2D_CHECK_EQ(config_path("/home/player/saves"),
                 (fs::path("/home/player/saves") / "config.ecfg").string());
    // An empty save directory is a relative file in the working directory, never "/config.ecfg".
    T2D_CHECK_EQ(config_path(""), std::string("config.ecfg"));
    T2D_CHECK(config_path("/tmp/mefemma-saves").ends_with("config.ecfg"));
}

T2D_TEST(config_ecfg_round_trips_all_three_languages) {
    ScratchDir dir;
    const std::string path = config_path(dir.file("saves"));
    for (const t2d::Language language :
         {t2d::Language::English, t2d::Language::SimplifiedChinese, t2d::Language::TraditionalChinese}) {
        std::string error;
        T2D_CHECK_MSG(save_game_config(path, language, &error), "save failed: {}", error);
        T2D_CHECK(error.empty());
        T2D_CHECK(fs::is_regular_file(path));
        T2D_CHECK_FALSE(fs::exists(path + ".tmp"));

        t2d::EcfgError load_error;
        const std::optional<GameConfig> config = load_game_config(path, &load_error);
        T2D_REQUIRE(config.has_value());
        T2D_REQUIRE(config->language.has_value());
        T2D_CHECK(*config->language == language);
        const std::string want = std::format("language:\"{}\"", t2d::language_code(language));
        T2D_CHECK_MSG(read_text(path).find(want) != std::string::npos, "missing {}", want);
    }
}

T2D_TEST(config_ecfg_missing_malformed_or_wrong_language_is_not_a_config) {
    ScratchDir dir;
    t2d::EcfgError error;

    T2D_CHECK_FALSE(load_game_config(dir.file("missing.ecfg"), &error).has_value());
    T2D_CHECK_FALSE(error.message.empty());

    write_text(dir.path() / "malformed.ecfg", "language:\"unterminated\n");
    error = {};
    T2D_CHECK_FALSE(load_game_config(dir.file("malformed.ecfg"), &error).has_value());
    T2D_CHECK_FALSE(error.message.empty());

    write_text(dir.path() / "wrong-type.ecfg", "language:42\n");
    error = {};
    T2D_CHECK_FALSE(load_game_config(dir.file("wrong-type.ecfg"), &error).has_value());
    T2D_CHECK(error.message.find("quoted") != std::string::npos);

    // parse_language() is the same rule the command line and the string tables use.
    write_text(dir.path() / "unknown.ecfg", "language:\"fr\"\n");
    error = {};
    T2D_CHECK_FALSE(load_game_config(dir.file("unknown.ecfg"), &error).has_value());
    T2D_CHECK(error.message.find("not a language") != std::string::npos);

    // A valid file with no language is an empty config, not a failure: a file may hold fields this version
    // does not know yet, and a pre-language file must still load.
    write_text(dir.path() / "no-language.ecfg", "other:1\n");
    error = {};
    const std::optional<GameConfig> empty = load_game_config(dir.file("no-language.ecfg"), &error);
    T2D_REQUIRE(empty.has_value());
    T2D_CHECK_FALSE(empty->language.has_value());
    T2D_CHECK(error.message.empty());

    // Unknown fields beside a known one are ignored too.
    write_text(dir.path() / "extra-fields.ecfg", "language:\"en\"\nother:true\n");
    error = {};
    const std::optional<GameConfig> config = load_game_config(dir.file("extra-fields.ecfg"), &error);
    T2D_REQUIRE(config.has_value());
    T2D_REQUIRE(config->language.has_value());
    T2D_CHECK(*config->language == t2d::Language::English);
}

T2D_TEST(save_game_config_creates_the_parent_and_atomically_replaces_the_old_file) {
    ScratchDir dir;
    const std::string path = config_path(dir.file("nested/deep/saves"));
    T2D_CHECK_FALSE(fs::exists(path));

    std::string error;
    T2D_CHECK_MSG(save_game_config(path, t2d::Language::English, &error), "save failed: {}", error);
    T2D_CHECK(fs::is_directory(dir.file("nested/deep/saves")));
    T2D_CHECK(fs::is_regular_file(path));
    T2D_CHECK_FALSE(fs::exists(path + ".tmp"));

    t2d::EcfgError load_error;
    std::optional<GameConfig> config = load_game_config(path, &load_error);
    T2D_REQUIRE(config.has_value());
    T2D_REQUIRE(config->language.has_value());
    T2D_CHECK(*config->language == t2d::Language::English);

    // A second save replaces the first; the target is renamed over, so a reader never sees half a file.
    error.clear();
    T2D_CHECK(save_game_config(path, t2d::Language::TraditionalChinese, &error));
    T2D_CHECK_FALSE(fs::exists(path + ".tmp"));
    config = load_game_config(path, &load_error);
    T2D_REQUIRE(config.has_value());
    T2D_REQUIRE(config->language.has_value());
    T2D_CHECK(*config->language == t2d::Language::TraditionalChinese);
    T2D_CHECK(read_text(path).find("language:\"zh-Hant\"") != std::string::npos);

    // The save directory is normally a directory; when it is a regular file the write has to fail with a
    // reason rather than replacing the file with a folder.
    const fs::path blocker = dir.path() / "blocker";
    write_text(blocker, "x");
    error.clear();
    T2D_CHECK_FALSE(save_game_config(config_path(blocker.string()), t2d::Language::SimplifiedChinese, &error));
    T2D_CHECK_FALSE(error.empty());
    T2D_CHECK(fs::is_regular_file(blocker));
}

T2D_TEST_MAIN
