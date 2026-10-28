// The two scenes that ship as examples: the start screen and the playground. A fake GameServices drives them
// without a window or a GPU, which is the property the framework is built for.
//
// The start screen is the one with behaviour a player can see: five rows, keyboard and mouse selection, a
// note for the rows that are not implemented yet, and quit. The playground is the camera example.
#include <mefemma/scenes/playground_scene.h>
#include <mefemma/scenes/title_scene.h>

#include <support/test_support.h>

#include <memory>
#include <optional>
#include <string>
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

private:
    InputState input_{};
    Settings settings_{};
    t2d::Locale locale_{};
    t2d::Rng rng_{1};
};

[[nodiscard]] bool has_text(const DrawList& list, std::string_view text) {
    for (const DrawCommand& command : list.commands()) {
        if (command.kind == DrawCommand::Kind::Text && command.text == text) return true;
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

/// One key press, held for exactly one frame: what a tap looks like to the scene.
void press_key(TitleScene& scene, FakeServices& services, Key key) {
    services.input_mutable().begin_frame();
    services.input_mutable().set_key(key, true);
    scene.update(services, 1.0f / 60.0f);
    services.input_mutable().set_key(key, false);
}

void click_at(TitleScene& scene, FakeServices& services, t2d::Vec2 position) {
    services.input_mutable().begin_frame();
    services.input_mutable().set_cursor_inside(true);
    services.input_mutable().set_mouse_position(position.x, position.y);
    services.input_mutable().set_mouse_button(MouseButton::Left, true);
    scene.update(services, 1.0f / 60.0f);
    services.input_mutable().set_mouse_button(MouseButton::Left, false);
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
    T2D_CHECK(start.modal_open());
    T2D_CHECK(start.modal_action() == StartAction::Settings);
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
    DrawList list;
    start.draw(services, list);
    T2D_CHECK(has_text(list, "start.new_game"));
    T2D_CHECK(has_text(list, "start.not_implemented"));
    T2D_CHECK(has_text(list, "start.back"));
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

T2D_TEST_MAIN
