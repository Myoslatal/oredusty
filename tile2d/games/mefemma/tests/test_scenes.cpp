// The two scenes this suite drives: the start screen and the playground. A fake GameServices drives them
// without a window or a GPU, which is the property the framework is built for.
//
// The start screen is the one with behaviour a player can see: five rows, keyboard and mouse selection, the
// internal settings menu (language and back), a note for the three rows that are not implemented yet, and
// quit. The playground is the camera example.
#include <mefemma/redirect.h>
#include <mefemma/scenes/playground_scene.h>
#include <mefemma/scenes/save_path_scene.h>
#include <mefemma/scenes/title_scene.h>

#include <support/test_support.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace mefemma;

namespace {

class FakeServices final : public GameServices {
public:
    [[nodiscard]] const InputState& input() const override { return input_; }
    [[nodiscard]] InputState& input_mutable() { return input_; }
    [[nodiscard]] const Settings& settings() const override { return settings_; }
    [[nodiscard]] const t2d::Locale& locale() const override { return locale_; }
    [[nodiscard]] std::string_view text(std::string_view id) const override { return id; }
    [[nodiscard]] t2d::TextMetrics measure_text(std::string_view value, t2d::u16 size_px) const override {
        t2d::TextMetrics metrics;
        metrics.width = static_cast<t2d::f32>(value.size()) * static_cast<t2d::f32>(size_px) * 0.5f;
        metrics.height = static_cast<t2d::f32>(size_px) * 1.2f;
        metrics.ascent = static_cast<t2d::f32>(size_px);
        metrics.descent = static_cast<t2d::f32>(size_px) * 0.2f;
        metrics.lines = 1;
        metrics.characters = value.size();
        return metrics;
    }
    [[nodiscard]] t2d::Language language() const override { return language_; }
    void set_language(t2d::Language language) override {
        language_ = language;
        ++language_changes;
    }
    [[nodiscard]] t2d::Rng& random() override { return rng_; }
    [[nodiscard]] t2d::f32 elapsed_seconds() const override { return 0.0f; }
    [[nodiscard]] t2d::u64 frame() const override { return 0; }
    [[nodiscard]] t2d::Vec2 viewport() const override { return t2d::Vec2{1280.0f, 720.0f}; }

    void push_scene(std::unique_ptr<Scene> scene) override {
        pushed.push_back(scene != nullptr ? scene->name() : "");
    }
    void pop_scene() override { ++pops; }
    void replace_scene(std::unique_ptr<Scene> scene) override {
        replaced = scene != nullptr ? scene->name() : "";
    }
    void quit() override { quit_called = true; }

    std::vector<std::string> pushed;
    std::string replaced;
    int pops = 0;
    bool quit_called = false;
    /// How many times the scene asked the shell to switch the interface language.
    int language_changes = 0;

private:
    InputState input_{};
    Settings settings_{};
    t2d::Locale locale_{};
    t2d::Rng rng_{1};
    t2d::Language language_ = t2d::Language::English;
};

[[nodiscard]] bool has_text(const DrawList& list, std::string_view text) {
    for (const DrawCommand& command : list.commands()) {
        if (command.kind == DrawCommand::Kind::Text && command.text == text) return true;
    }
    return false;
}

[[nodiscard]] bool has_text_containing(const DrawList& list, std::string_view fragment) {
    for (const DrawCommand& command : list.commands()) {
        if (command.kind == DrawCommand::Kind::Text &&
            command.text.find(std::string(fragment)) != std::string::npos) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool has_picture(const DrawList& list, std::string_view texture) {
    for (const DrawCommand& command : list.commands()) {
        if (command.kind == DrawCommand::Kind::Sprite && command.texture == texture) return true;
    }
    return false;
}

[[nodiscard]] const DrawCommand* find_picture(const DrawList& list, std::string_view texture) {
    for (const DrawCommand& command : list.commands()) {
        if (command.kind == DrawCommand::Kind::Sprite && command.texture == texture) return &command;
    }
    return nullptr;
}

namespace fs = std::filesystem;

/// A directory under the system temporary directory that this run alone uses, removed when the test ends.
class ScratchDir {
public:
    ScratchDir() {
        static std::atomic<unsigned> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() /
                ("t2d_mefemma_scenes_" + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1)));
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

/// What a confirmed first-run prompt starts: a scene whose name the fake shell records.
class ProbeScene final : public Scene {
public:
    explicit ProbeScene(std::string name) : name_(std::move(name)) {}
    [[nodiscard]] const char* name() const override { return name_.c_str(); }

private:
    std::string name_;
};

void write_text(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

/// One key press, held for exactly one frame: what a tap looks like to the scene.
void press_key(TitleScene& scene, FakeServices& services, Key key) {
    services.input_mutable().begin_frame();
    services.input_mutable().set_key(key, true);
    scene.update(services, 1.0f / 60.0f);
    services.input_mutable().set_key(key, false);
}

void press_key(SavePathScene& scene, FakeServices& services, Key key) {
    services.input_mutable().begin_frame();
    services.input_mutable().set_key(key, true);
    scene.update(services, 1.0f / 60.0f);
    services.input_mutable().set_key(key, false);
}

/// Text typed in one frame, the way InputState hands it to a scene.
void type_text(SavePathScene& scene, FakeServices& services, std::string_view text) {
    services.input_mutable().begin_frame();
    services.input_mutable().set_text(text);
    scene.update(services, 1.0f / 60.0f);
}

void click_at(TitleScene& scene, FakeServices& services, t2d::Vec2 position) {
    services.input_mutable().begin_frame();
    services.input_mutable().set_cursor_inside(true);
    services.input_mutable().set_mouse_position(position.x, position.y);
    services.input_mutable().set_mouse_button(MouseButton::Left, true);
    scene.update(services, 1.0f / 60.0f);
    services.input_mutable().set_mouse_button(MouseButton::Left, false);
}

/// The pointer comes into the window at \p position and rests there: a hover, with no button.
void hover_at(TitleScene& scene, FakeServices& services, t2d::Vec2 position) {
    services.input_mutable().begin_frame();
    services.input_mutable().set_cursor_inside(true);
    services.input_mutable().set_mouse_position(position.x, position.y);
    scene.update(services, 1.0f / 60.0f);
}

/// Selects Settings with the keyboard and opens the internal settings menu.
void open_settings(TitleScene& scene, FakeServices& services) {
    press_key(scene, services, Key::Down);
    press_key(scene, services, Key::Down);
    press_key(scene, services, Key::Enter);
}

/// The colour of the first plain rectangle with exactly p rect: how a test tells the highlighted row
/// (orange) from a resting one (wood) without copying the scene's palette.
[[nodiscard]] std::optional<t2d::u32> rect_color(const DrawList& list, const t2d::Aabb2& rect) {
    for (const DrawCommand& command : list.commands()) {
        if (command.kind == DrawCommand::Kind::Rect && command.rect == rect) return command.color;
    }
    return std::nullopt;
}

/// The x of the first full-height one-pixel line: a vertical grid line, which is what a horizontal pan
/// moves. The orbit squares and the background are deliberately not matched.
[[nodiscard]] std::optional<t2d::f32> first_vertical_line_x(const DrawList& list, t2d::f32 view_height) {
    for (const DrawCommand& command : list.commands()) {
        if (command.kind != DrawCommand::Kind::Rect) continue;
        if (command.rect.min.y > 0.5f || command.rect.max.y < view_height - 0.5f) continue;
        if (command.rect.width() > 1.5f) continue;
        return command.rect.min.x;
    }
    return std::nullopt;
}

} // namespace

T2D_TEST(the_start_screen_has_five_rows_and_selection_wraps) {
    TitleScene start;
    FakeServices services;
    T2D_CHECK_EQ(TitleScene::item_count(), 5u);
    T2D_CHECK_EQ(start.selected(), 0u);
    T2D_CHECK(start.selected_action() == StartAction::NewGame);

    press_key(start, services, Key::Down);
    T2D_CHECK_EQ(start.selected(), 1u);
    T2D_CHECK(start.selected_action() == StartAction::LoadGame);

    press_key(start, services, Key::Up);
    T2D_CHECK_EQ(start.selected(), 0u);

    // Up from the first row wraps to the last, down from the last wraps to the first.
    press_key(start, services, Key::Up);
    T2D_CHECK_EQ(start.selected(), 4u);
    T2D_CHECK(start.selected_action() == StartAction::Quit);
    press_key(start, services, Key::Down);
    T2D_CHECK_EQ(start.selected(), 0u);
}

T2D_TEST(enter_opens_the_placeholder_note_and_escape_closes_it) {
    TitleScene start;
    FakeServices services;
    press_key(start, services, Key::Enter);
    T2D_CHECK(start.modal_open());
    T2D_CHECK(start.modal_action() == StartAction::NewGame);
    T2D_CHECK_FALSE(services.quit_called);
    T2D_CHECK(services.pushed.empty());

    // Escape closes the note. It does not leave the game from a note: the player asked to go back.
    press_key(start, services, Key::Escape);
    T2D_CHECK_FALSE(start.modal_open());
    T2D_CHECK_FALSE(services.quit_called);
}

T2D_TEST(escape_on_the_menu_quits_and_the_quit_row_quits) {
    {
        TitleScene start;
        FakeServices services;
        press_key(start, services, Key::Escape);
        T2D_CHECK(services.quit_called);
    }
    {
        TitleScene start;
        FakeServices services;
        for (int index = 0; index < 4; ++index) press_key(start, services, Key::Down);
        T2D_CHECK_EQ(start.selected(), 4u);
        press_key(start, services, Key::Enter);
        T2D_CHECK(services.quit_called);
        T2D_CHECK_FALSE(start.modal_open());
    }
}

T2D_TEST(the_pointer_selects_a_row_and_a_click_opens_it) {
    TitleScene start;
    FakeServices services;
    const t2d::Aabb2 row = TitleScene::item_rect(services.viewport(), 2);
    const t2d::Vec2 centre = row.center();

    services.input_mutable().begin_frame();
    services.input_mutable().set_cursor_inside(true);
    services.input_mutable().set_mouse_position(centre.x, centre.y);
    start.update(services, 1.0f / 60.0f);
    T2D_CHECK_EQ(start.selected(), 2u);
    T2D_CHECK(start.selected_action() == StartAction::Settings);

    click_at(start, services, centre);
    // Settings is a real internal menu now: it is not the placeholder note, and it does not push a scene.
    T2D_CHECK(start.settings_open());
    T2D_CHECK_FALSE(start.modal_open());
    T2D_CHECK_EQ(start.settings_selected(), 0u);
    T2D_CHECK(start.settings_action() == SettingsAction::Language);
    T2D_CHECK_EQ(start.selected(), 2u);
    T2D_CHECK_FALSE(services.quit_called);
    T2D_CHECK(services.pushed.empty());
}

T2D_TEST(keyboard_navigation_is_not_undone_by_a_resting_pointer) {
    TitleScene start;
    FakeServices services;
    const t2d::Vec2 centre = TitleScene::item_rect(services.viewport(), 2).center();

    // The pointer comes into the window over row 2 and selects it.
    services.input_mutable().begin_frame();
    services.input_mutable().set_cursor_inside(true);
    services.input_mutable().set_mouse_position(centre.x, centre.y);
    start.update(services, 1.0f / 60.0f);
    T2D_CHECK_EQ(start.selected(), 2u);

    // The pointer does not move; the keyboard does. The selection follows the keyboard.
    press_key(start, services, Key::Down);
    T2D_CHECK_EQ(start.selected(), 3u);
    press_key(start, services, Key::Down);
    T2D_CHECK_EQ(start.selected(), 4u);
}

T2D_TEST(the_start_screen_draws_the_sign_the_name_and_the_five_rows) {
    TitleScene start;
    FakeServices services;
    DrawList list;
    start.draw(services, list);

    T2D_CHECK(has_picture(list, "start_screen"));
    const DrawCommand* sign = find_picture(list, "start_screen");
    T2D_REQUIRE(sign != nullptr);
    // A painted sign is filtered smoothly; a tile would keep the default nearest.
    T2D_CHECK(sign->filter == TextureFilter::Linear);
    T2D_CHECK(has_text(list, "start.title"));
    for (const char* id : {"start.new_game", "start.load_game", "start.settings", "start.mods", "start.quit"}) {
        T2D_CHECK_MSG(has_text(list, id), "missing '{}'", id);
    }
    T2D_CHECK(has_text(list, "start.hint"));
    T2D_CHECK_GE(list.text_count(), 7u);
    T2D_CHECK_GE(list.rect_count(), 10u);

    const t2d::Vec2 viewport = services.viewport();
    for (usize index = 0; index < TitleScene::item_count(); ++index) {
        const t2d::Aabb2 row = TitleScene::item_rect(viewport, index);
        T2D_CHECK(row.min.x >= 0.0f && row.max.x <= viewport.x);
        T2D_CHECK(row.min.y >= 0.0f && row.max.y <= viewport.y);
    }
}

T2D_TEST(the_placeholder_says_it_is_not_implemented) {
    TitleScene start;
    FakeServices services;
    press_key(start, services, Key::Enter);
    T2D_CHECK(start.modal_open());
    T2D_CHECK(start.modal_action() == StartAction::NewGame);
    T2D_CHECK_FALSE(start.settings_open());
    DrawList list;
    start.draw(services, list);
    T2D_CHECK(has_text(list, "start.new_game"));
    T2D_CHECK(has_text(list, "start.not_implemented"));
    T2D_CHECK(has_text(list, "start.back"));
}

T2D_TEST(enter_on_settings_opens_the_two_row_menu_and_back_closes_it) {
    TitleScene start;
    FakeServices services;
    press_key(start, services, Key::Down);
    press_key(start, services, Key::Down);
    T2D_CHECK(start.selected_action() == StartAction::Settings);

    press_key(start, services, Key::Enter);
    T2D_CHECK(start.settings_open());
    T2D_CHECK_FALSE(start.modal_open());
    T2D_CHECK_EQ(TitleScene::settings_item_count(), 2u);
    T2D_CHECK_EQ(start.settings_selected(), 0u);
    T2D_CHECK(start.settings_action() == SettingsAction::Language);
    T2D_CHECK_FALSE(services.quit_called);
    T2D_CHECK(services.pushed.empty());

    // W/S move like Up/Down, and both ends wrap.
    press_key(start, services, Key::S);
    T2D_CHECK_EQ(start.settings_selected(), 1u);
    T2D_CHECK(start.settings_action() == SettingsAction::Back);
    press_key(start, services, Key::W);
    T2D_CHECK_EQ(start.settings_selected(), 0u);
    press_key(start, services, Key::Up);
    T2D_CHECK_EQ(start.settings_selected(), 1u);
    press_key(start, services, Key::Down);
    T2D_CHECK_EQ(start.settings_selected(), 0u);
    press_key(start, services, Key::Down);

    // BACK has no value to change: left/right on it must not switch the language behind the player's back.
    press_key(start, services, Key::Right);
    press_key(start, services, Key::Left);
    T2D_CHECK_EQ(services.language_changes, 0);
    T2D_CHECK(services.language() == t2d::Language::English);

    press_key(start, services, Key::Enter);
    T2D_CHECK_FALSE(start.settings_open());
    T2D_CHECK_FALSE(start.modal_open());
    T2D_CHECK_FALSE(services.quit_called);
    // Back returns to the title with the selection still on Settings.
    T2D_CHECK_EQ(start.selected(), 2u);
    T2D_CHECK(start.selected_action() == StartAction::Settings);
    T2D_CHECK(services.pushed.empty());
    T2D_CHECK_EQ(services.pops, 0);
}

T2D_TEST(the_language_row_changes_the_language_by_keyboard_and_wraps) {
    TitleScene start;
    FakeServices services;
    open_settings(start, services);
    T2D_CHECK(start.settings_open());
    T2D_CHECK(services.language() == t2d::Language::English);

    // Forward wraps English -> Simplified -> Traditional -> English.
    press_key(start, services, Key::Right);
    T2D_CHECK(services.language() == t2d::Language::SimplifiedChinese);
    press_key(start, services, Key::Right);
    T2D_CHECK(services.language() == t2d::Language::TraditionalChinese);
    press_key(start, services, Key::Right);
    T2D_CHECK(services.language() == t2d::Language::English);
    // Backward wraps the other way; A/D are the same pair as Left/Right.
    press_key(start, services, Key::Left);
    T2D_CHECK(services.language() == t2d::Language::TraditionalChinese);
    press_key(start, services, Key::D);
    T2D_CHECK(services.language() == t2d::Language::English);
    press_key(start, services, Key::A);
    T2D_CHECK(services.language() == t2d::Language::TraditionalChinese);
    T2D_CHECK_EQ(services.language_changes, 6);

    // Enter and Space on LANGUAGE advance one language and keep the menu open.
    press_key(start, services, Key::Enter);
    T2D_CHECK(services.language() == t2d::Language::English);
    T2D_CHECK(start.settings_open());
    press_key(start, services, Key::Space);
    T2D_CHECK(services.language() == t2d::Language::SimplifiedChinese);
    T2D_CHECK(start.settings_open());
    T2D_CHECK_EQ(services.language_changes, 8);
}

T2D_TEST(escape_in_the_settings_menu_returns_to_the_title_and_does_not_quit) {
    TitleScene start;
    FakeServices services;
    open_settings(start, services);

    press_key(start, services, Key::Escape);
    T2D_CHECK_FALSE(start.settings_open());
    T2D_CHECK_FALSE(start.modal_open());
    T2D_CHECK_FALSE(services.quit_called);
    T2D_CHECK_EQ(start.selected(), 2u);
    T2D_CHECK(start.selected_action() == StartAction::Settings);

    // Escape on the title itself still leaves the game.
    press_key(start, services, Key::Escape);
    T2D_CHECK(services.quit_called);
}

T2D_TEST(the_settings_menu_draws_its_rows_the_endonym_and_the_hint_over_the_sign) {
    TitleScene start;
    FakeServices services;
    open_settings(start, services);
    DrawList list;
    start.draw(services, list);

    T2D_CHECK(has_picture(list, "start_screen"));
    T2D_CHECK(has_text(list, "start.title"));   // the title screen behind is still drawn
    T2D_CHECK(has_text(list, "settings.title"));
    T2D_CHECK(has_text(list, "settings.language"));
    T2D_CHECK(has_text(list, "settings.back"));
    T2D_CHECK(has_text(list, "settings.hint"));
    T2D_CHECK(has_text(list, std::format("< {} >", t2d::language_name(t2d::Language::English))));
    T2D_CHECK_FALSE(has_text(list, "start.not_implemented"));

    // The five title rows are behind the overlay, but none of them is the highlighted one now.
    const std::optional<t2d::u32> resting =
        rect_color(list, TitleScene::item_rect(services.viewport(), 2));
    T2D_REQUIRE(resting.has_value());
    T2D_CHECK(*resting != rgba(255, 166, 0));

    // The language row redraws in the language that was just selected.
    press_key(start, services, Key::Right);
    DrawList switched;
    start.draw(services, switched);
    T2D_CHECK(has_text(switched,
                       std::format("< {} >", t2d::language_name(t2d::Language::SimplifiedChinese))));
    T2D_CHECK(has_text(switched, "settings.language"));   // labels are ids in the fake table
}

T2D_TEST(the_settings_menu_answers_hover_and_clicks_and_ignores_the_title_behind) {
    TitleScene start;
    FakeServices services;
    const t2d::Vec2 viewport = services.viewport();

    // A click on Settings opens the menu and touches nothing else.
    click_at(start, services, TitleScene::item_rect(viewport, 2).center());
    T2D_REQUIRE(start.settings_open());
    T2D_CHECK_FALSE(start.modal_open());

    // Hovering a settings row selects it; a click on LANGUAGE advances, a click on BACK closes.
    hover_at(start, services, TitleScene::settings_item_rect(viewport, 1).center());
    T2D_CHECK_EQ(start.settings_selected(), 1u);
    T2D_CHECK(start.settings_action() == SettingsAction::Back);

    click_at(start, services, TitleScene::settings_item_rect(viewport, 0).center());
    T2D_CHECK(start.settings_open());
    T2D_CHECK(services.language() == t2d::Language::SimplifiedChinese);
    click_at(start, services, TitleScene::settings_item_rect(viewport, 1).center());
    T2D_CHECK_FALSE(start.settings_open());
    T2D_CHECK_EQ(start.selected(), 2u);

    // Open again; a click outside the panel (over a title row) is not a request, and the title rows
    // behind the menu must not react.
    press_key(start, services, Key::Enter);
    T2D_REQUIRE(start.settings_open());
    const int changes_before = services.language_changes;
    click_at(start, services, TitleScene::item_rect(viewport, 0).center());
    T2D_CHECK(start.settings_open());
    T2D_CHECK_FALSE(start.modal_open());
    T2D_CHECK_EQ(start.selected(), 2u);
    T2D_CHECK_EQ(services.language_changes, changes_before);
    T2D_CHECK_FALSE(services.quit_called);
    T2D_CHECK(services.pushed.empty());
}

T2D_TEST(the_playground_escape_pops_back) {
    PlaygroundScene playground;
    FakeServices services;
    playground.on_enter(services);
    services.input_mutable().set_key(Key::Escape, true);
    playground.update(services, 1.0f / 60.0f);
    T2D_CHECK_EQ(services.pops, 1);
    T2D_CHECK_FALSE(services.quit_called);
}

T2D_TEST(the_playground_draws_a_grid_its_chrome_and_the_pointer_cell) {
    PlaygroundScene playground;
    FakeServices services;
    playground.on_enter(services);

    DrawList without_pointer;
    playground.draw(services, without_pointer);
    T2D_CHECK_GT(without_pointer.rect_count(), 10u);
    T2D_CHECK(has_text(without_pointer, "play.title"));
    T2D_CHECK(has_text(without_pointer, "play.note"));
    T2D_CHECK(has_text(without_pointer, "play.hint"));
    T2D_CHECK(has_text(without_pointer, "play.pointer.outside"));

    services.input_mutable().begin_frame();
    services.input_mutable().set_cursor_inside(true);
    services.input_mutable().set_mouse_position(900.0f, 420.0f);
    playground.update(services, 1.0f / 60.0f);
    DrawList with_pointer;
    playground.draw(services, with_pointer);
    // The outline is four rectangles; nothing else about the frame changed shape.
    T2D_CHECK_EQ(with_pointer.rect_count(), without_pointer.rect_count() + 4u);
    T2D_CHECK(has_text(with_pointer, "play.cell"));
}

T2D_TEST(the_playground_pans_with_the_keyboard) {
    PlaygroundScene playground;
    FakeServices services;
    playground.on_enter(services);

    DrawList before;
    playground.draw(services, before);
    const std::optional<t2d::f32> first_x = first_vertical_line_x(before, services.viewport().y);
    T2D_REQUIRE(first_x.has_value());

    services.input_mutable().begin_frame();
    services.input_mutable().set_key(Key::D, true);
    playground.update(services, 0.05f);
    services.input_mutable().begin_frame();
    services.input_mutable().set_key(Key::D, true);

    DrawList after;
    playground.draw(services, after);
    const std::optional<t2d::f32> moved_x = first_vertical_line_x(after, services.viewport().y);
    T2D_REQUIRE(moved_x.has_value());
    T2D_CHECK_NE(*moved_x, *first_x);
}

T2D_TEST(the_first_run_prompt_prefills_the_default_save_directory) {
    ScratchDir dir;
    const std::string redirect = dir.file("redirect.ecfg");
    const std::string default_dir = dir.file("saves");
    SavePathScene prompt(redirect, default_dir, [](std::string_view) {
        return std::unique_ptr<Scene>(std::make_unique<ProbeScene>("after-first-run"));
    });
    FakeServices services;
    prompt.on_enter(services);

    T2D_CHECK_EQ(std::string(prompt.name()), std::string("first-run"));
    T2D_CHECK_EQ(prompt.redirect_path(), redirect);
    T2D_CHECK_EQ(prompt.default_save_path(), default_dir);
    T2D_CHECK_EQ(prompt.text(), std::string_view(default_dir));
    T2D_CHECK(prompt.error().empty());
}

T2D_TEST(the_first_run_prompt_edits_the_field_with_utf8_text_and_keys) {
    ScratchDir dir;
    SavePathScene prompt(dir.file("redirect.ecfg"), "base", [](std::string_view) {
        return std::unique_ptr<Scene>(std::make_unique<ProbeScene>("after-first-run"));
    });
    FakeServices services;
    prompt.on_enter(services);

    // Typed text arrives through text_input() and inserts at the caret (which starts at the end).
    type_text(prompt, services, "-中文");
    T2D_CHECK_EQ(prompt.text(), std::string_view("base-中文"));
    press_key(prompt, services, Key::Backspace);
    T2D_CHECK_EQ(prompt.text(), std::string_view("base-中"));
    press_key(prompt, services, Key::Backspace);
    T2D_CHECK_EQ(prompt.text(), std::string_view("base-"));   // one backspace is one codepoint

    // Home/End/Left/Right move the caret; Delete removes the character after it.
    press_key(prompt, services, Key::Home);
    press_key(prompt, services, Key::Delete);
    T2D_CHECK_EQ(prompt.text(), std::string_view("ase-"));
    press_key(prompt, services, Key::End);
    type_text(prompt, services, "x");
    T2D_CHECK_EQ(prompt.text(), std::string_view("ase-x"));
    press_key(prompt, services, Key::Home);
    press_key(prompt, services, Key::Right);
    type_text(prompt, services, "Z");
    T2D_CHECK_EQ(prompt.text(), std::string_view("aZse-x"));
}

T2D_TEST(the_first_run_prompt_confirms_creating_the_directory_and_redirect_file) {
    ScratchDir dir;
    const std::string redirect = dir.file("redirect.ecfg");
    const std::string default_dir = dir.file("saves");
    std::string chosen;
    bool started = false;
    SavePathScene prompt(redirect, default_dir, [&chosen, &started](std::string_view path) {
        chosen = std::string(path);
        started = true;
        return std::unique_ptr<Scene>(std::make_unique<ProbeScene>("after-first-run"));
    });
    FakeServices services;
    prompt.on_enter(services);
    press_key(prompt, services, Key::Enter);

    T2D_CHECK(started);
    T2D_CHECK_EQ(chosen, default_dir);
    T2D_CHECK(fs::path(chosen).is_absolute());
    T2D_CHECK(fs::is_directory(default_dir));
    T2D_CHECK(fs::is_regular_file(redirect));
    const std::optional<RedirectConfig> config = load_redirect(redirect);
    T2D_REQUIRE(config.has_value());
    T2D_CHECK_EQ(config->save_path, chosen);
    T2D_CHECK_EQ(services.replaced, std::string("after-first-run"));
    T2D_CHECK_FALSE(services.quit_called);
    T2D_CHECK(prompt.error().empty());
}

T2D_TEST(the_first_run_prompt_refuses_an_empty_answer_and_stays) {
    ScratchDir dir;
    const std::string redirect = dir.file("redirect.ecfg");
    SavePathScene prompt(redirect, "", [](std::string_view) {
        return std::unique_ptr<Scene>(std::make_unique<ProbeScene>("after-first-run"));
    });
    FakeServices services;
    prompt.on_enter(services);
    press_key(prompt, services, Key::Enter);

    T2D_CHECK_FALSE(fs::exists(redirect));
    T2D_CHECK(services.replaced.empty());
    T2D_CHECK_FALSE(services.quit_called);
    DrawList list;
    prompt.draw(services, list);
    T2D_CHECK(has_text(list, "savepath.error.empty"));
    T2D_CHECK(has_text(list, "savepath.title"));

    // Typing again is an edit and clears the error line.
    type_text(prompt, services, "saves");
    DrawList edited;
    prompt.draw(services, edited);
    T2D_CHECK_FALSE(has_text(edited, "savepath.error.empty"));
}

T2D_TEST(the_first_run_prompt_refuses_a_file_target_and_stays) {
    ScratchDir dir;
    const std::string redirect = dir.file("redirect.ecfg");
    const std::string blocked = dir.file("blocked-saves");
    write_text(blocked, "not a directory");
    SavePathScene prompt(redirect, blocked, [](std::string_view) {
        return std::unique_ptr<Scene>(std::make_unique<ProbeScene>("after-first-run"));
    });
    FakeServices services;
    prompt.on_enter(services);
    press_key(prompt, services, Key::Enter);

    T2D_CHECK_FALSE(fs::exists(redirect));
    T2D_CHECK(services.replaced.empty());
    T2D_CHECK_FALSE(prompt.error().empty());
    T2D_CHECK(fs::is_regular_file(blocked));
    DrawList list;
    prompt.draw(services, list);
    T2D_CHECK(has_text(list, "savepath.error.failed"));
}

T2D_TEST(the_first_run_prompt_escape_quits_without_writing) {
    ScratchDir dir;
    const std::string redirect = dir.file("redirect.ecfg");
    SavePathScene prompt(redirect, dir.file("saves"), [](std::string_view) {
        return std::unique_ptr<Scene>(std::make_unique<ProbeScene>("after-first-run"));
    });
    FakeServices services;
    prompt.on_enter(services);
    press_key(prompt, services, Key::Escape);

    T2D_CHECK(services.quit_called);
    T2D_CHECK(services.replaced.empty());
    T2D_CHECK_FALSE(fs::exists(redirect));
}

T2D_TEST(the_first_run_prompt_draws_the_question_the_field_and_the_hint) {
    ScratchDir dir;
    const std::string redirect = dir.file("redirect.ecfg");
    const std::string default_dir = dir.file("saves");
    SavePathScene prompt(redirect, default_dir, [](std::string_view) {
        return std::unique_ptr<Scene>(std::make_unique<ProbeScene>("after-first-run"));
    });
    FakeServices services;
    prompt.on_enter(services);

    DrawList list;
    prompt.draw(services, list);
    T2D_CHECK(has_text(list, "start.title"));
    T2D_CHECK(has_text(list, "savepath.title"));
    T2D_CHECK(has_text(list, "savepath.prompt"));
    T2D_CHECK(has_text(list, "savepath.hint"));
    T2D_CHECK(has_text(list, default_dir));   // the prefilled answer is the field's text

    // Moving the caret must not hide the suffix: the field shows the whole path when it fits, with the
    // caret at its real position.
    press_key(prompt, services, Key::Home);
    DrawList home;
    prompt.draw(services, home);
    T2D_CHECK(has_text(home, default_dir));
    press_key(prompt, services, Key::End);
    press_key(prompt, services, Key::Left);
    DrawList left;
    prompt.draw(services, left);
    T2D_CHECK(has_text(left, default_dir));

    // A path longer than the field is shown as a tail with a leading "...", so the caret and the useful end
    // of the path stay inside it.
    SavePathScene long_prompt(redirect, "/" + std::string(400, 'a') + "/saves", [](std::string_view) {
        return std::unique_ptr<Scene>(std::make_unique<ProbeScene>("after-first-run"));
    });
    long_prompt.on_enter(services);
    DrawList long_list;
    long_prompt.draw(services, long_list);
    T2D_CHECK(has_text_containing(long_list, "..."));
    T2D_CHECK(has_text_containing(long_list, "saves"));
}

T2D_TEST_MAIN
