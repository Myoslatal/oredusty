// The settings file: the defaults a game runs on when it ships none, every field the shell reads, and a
// field that is present but wrong being reported instead of quietly replaced.
#include <mefemma/settings.h>

#include <support/test_support.h>

#include <t2d/core/ecfg.h>

#include <cstdio>
#include <filesystem>
#include <string>

using namespace mefemma;

namespace {

[[nodiscard]] Settings from_text(const std::string& text, t2d::EcfgError* error = nullptr) {
    const std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::parse(text, error);
    if (!document.has_value()) return Settings::defaults();
    return Settings::from_document(*document, error);
}

} // namespace

T2D_TEST(the_defaults_are_a_window_a_game_can_start_in) {
    const Settings settings;
    T2D_CHECK_EQ(settings.title, std::string("Mefemma"));
    T2D_CHECK_EQ(settings.width, 1280u);
    T2D_CHECK_EQ(settings.height, 720u);
    T2D_CHECK(settings.vsync);
    T2D_CHECK_EQ(settings.language, std::string("en"));
    T2D_CHECK_EQ(settings.seed, 1u);
    T2D_CHECK_FALSE(settings.document.has_value());
    T2D_CHECK(settings.find("anything") == nullptr);
}

T2D_TEST(every_field_the_shell_reads_is_read) {
    t2d::EcfgError error;
    const Settings settings = from_text(R"(
        title:"A Game"
        width:1600
        height:900
        vsync:false
        language:"zh-Hans"
        seed:4242
    )",
                                        &error);
    T2D_CHECK_MSG(error.message.empty(), "{}", error.describe("settings"));
    T2D_CHECK_EQ(settings.title, std::string("A Game"));
    T2D_CHECK_EQ(settings.width, 1600u);
    T2D_CHECK_EQ(settings.height, 900u);
    T2D_CHECK_FALSE(settings.vsync);
    T2D_CHECK_EQ(settings.language, std::string("zh-Hans"));
    T2D_CHECK_EQ(settings.seed, 4242u);
}

T2D_TEST(a_games_own_fields_are_kept_and_not_an_error) {
    const Settings settings = from_text(R"(
        title:"Mefemma"
        difficulty:"hard"
        window::
            scale:2
    )");
    T2D_REQUIRE(settings.document.has_value());
    const t2d::EcfgValue* difficulty = settings.find("difficulty");
    T2D_REQUIRE(difficulty != nullptr);
    T2D_CHECK(difficulty->is_string());
    T2D_CHECK_EQ(difficulty->as_string(), std::string("hard"));
    const t2d::EcfgValue* window = settings.find("window");
    T2D_REQUIRE(window != nullptr);
    T2D_CHECK(window->is_table());
    T2D_REQUIRE(window->find("scale") != nullptr);
    T2D_CHECK_EQ(window->find("scale")->as_int(), 2);
    T2D_CHECK(settings.find("missing") == nullptr);
}

T2D_TEST(a_field_of_the_wrong_type_is_reported_and_keeps_its_default) {
    t2d::EcfgError error;
    const Settings settings = from_text("width:\"wide\"", &error);
    T2D_CHECK_FALSE(error.message.empty());
    T2D_CHECK(error.message.find("width") != std::string::npos);
    T2D_CHECK_EQ(settings.width, 1280u);

    t2d::EcfgError vsync_error;
    (void)from_text("vsync:1", &vsync_error);
    T2D_CHECK_FALSE(vsync_error.message.empty());
    T2D_CHECK(vsync_error.message.find("vsync") != std::string::npos);

    t2d::EcfgError title_error;
    (void)from_text("title:12", &title_error);
    T2D_CHECK_FALSE(title_error.message.empty());
    T2D_CHECK(title_error.message.find("title") != std::string::npos);
}

T2D_TEST(a_window_size_outside_the_range_is_refused) {
    t2d::EcfgError error;
    const Settings small = from_text("width:10", &error);
    T2D_CHECK_FALSE(error.message.empty());
    T2D_CHECK_EQ(small.width, 1280u);

    t2d::EcfgError tall_error;
    const Settings tall = from_text("height:99999", &tall_error);
    T2D_CHECK_FALSE(tall_error.message.empty());
    T2D_CHECK_EQ(tall.height, 720u);
}

T2D_TEST(a_negative_seed_is_refused) {
    t2d::EcfgError error;
    const Settings settings = from_text("seed:-1", &error);
    T2D_CHECK_FALSE(error.message.empty());
    T2D_CHECK_EQ(settings.seed, 1u);
}

T2D_TEST(a_missing_file_is_the_defaults_and_not_an_error) {
    t2d::EcfgError error;
    const Settings settings = Settings::load("/nonexistent/mefemma/does-not-exist.ecfg", &error);
    T2D_CHECK(error.message.empty());
    T2D_CHECK_EQ(settings.title, std::string("Mefemma"));
    T2D_CHECK_EQ(settings.width, 1280u);
}

T2D_TEST(a_file_is_read_from_disk) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "mefemma-settings-test.ecfg";
    {
        std::FILE* file = std::fopen(path.string().c_str(), "wb");
        T2D_REQUIRE(file != nullptr);
        const std::string text = "title:\"From disk\"\nwidth:1024\nheight:768\n";
        T2D_CHECK_EQ(std::fwrite(text.data(), 1, text.size(), file), text.size());
        std::fclose(file);
    }
    t2d::EcfgError error;
    const Settings settings = Settings::load(path.string(), &error);
    std::filesystem::remove(path);
    T2D_CHECK_MSG(error.message.empty(), "{}", error.describe(path.string()));
    T2D_CHECK_EQ(settings.title, std::string("From disk"));
    T2D_CHECK_EQ(settings.width, 1024u);
    T2D_CHECK_EQ(settings.height, 768u);
}

T2D_TEST(a_file_that_does_not_parse_is_reported) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "mefemma-settings-broken.ecfg";
    {
        std::FILE* file = std::fopen(path.string().c_str(), "wb");
        T2D_REQUIRE(file != nullptr);
        const std::string text = "title: not quoted\n";
        T2D_CHECK_EQ(std::fwrite(text.data(), 1, text.size(), file), text.size());
        std::fclose(file);
    }
    t2d::EcfgError error;
    const Settings settings = Settings::load(path.string(), &error);
    std::filesystem::remove(path);
    T2D_CHECK_FALSE(error.message.empty());
    T2D_CHECK_EQ(settings.width, 1280u);
}

T2D_TEST_MAIN
