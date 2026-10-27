// The two scenes that ship as examples: the commands they draw, and the one thing each of them does with
// the input. They are scenes, so a fake GameServices drives them without a window or a GPU - which is the
// property the framework is built for.
#include <mefemma/scenes/playground_scene.h>
#include <mefemma/scenes/title_scene.h>

#include <support/test_support.h>

#include <functional>
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

T2D_TEST(the_title_pushes_the_playground_and_the_escape_quits) {
    TitleScene title;
    {
        FakeServices services;
        services.input_mutable().set_key(Key::Enter, true);
        title.update(services, 1.0f / 60.0f);
        T2D_REQUIRE(services.pushed.size() == 1u);
        T2D_CHECK_EQ(services.pushed[0], std::string("playground"));
        T2D_CHECK_EQ(services.replaced, std::string());
        T2D_CHECK_FALSE(services.quit_called);
    }
    {
        FakeServices services;
        services.input_mutable().set_key(Key::Escape, true);
        title.update(services, 1.0f / 60.0f);
        T2D_CHECK(services.quit_called);
        T2D_CHECK(services.pushed.empty());
    }
}

T2D_TEST(the_title_draws_its_picture_its_name_and_its_keys) {
    TitleScene title;
    FakeServices services;
    DrawList list;
    title.draw(services, list);
    T2D_CHECK_GT(list.size(), 10u);
    T2D_CHECK(has_picture(list, "placeholder"));
    T2D_CHECK(has_text(list, "title.name"));
    T2D_CHECK(has_text(list, "title.tagline"));
    T2D_CHECK(has_text(list, "title.start"));
    T2D_CHECK(has_text(list, "title.quit"));
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
