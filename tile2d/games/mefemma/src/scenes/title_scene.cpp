#include <mefemma/scenes/playground_scene.h>
#include <mefemma/scenes/title_scene.h>

#include <t2d/core/log.h>

#include <cmath>
#include <string>

namespace mefemma {
namespace {

constexpr u32 kBackground = rgba(13, 15, 20);
constexpr u32 kText = rgba(232, 234, 240);
constexpr u32 kDim = rgba(148, 154, 170);
constexpr u32 kAccent = rgba(255, 196, 84);
constexpr u32 kAccentDim = rgba(255, 196, 84, 70);
constexpr u32 kRule = rgba(52, 57, 70);

constexpr f32 kTau = 6.28318530718f;

} // namespace

std::unique_ptr<Scene> make_title_scene() { return std::make_unique<TitleScene>(); }

void TitleScene::on_enter(GameServices& services) {
    (void)services;
    T2D_INFO("scene: title");
}

void TitleScene::update(GameServices& services, f32 delta_seconds) {
    seconds_ += delta_seconds;
    const InputState& input = services.input();
    if (input.key_pressed(Key::Enter) || input.key_pressed(Key::Space)) {
        // **Push**, not replace: the title stays on the stack, paused, and the playground's Escape pops it
        // back. That is the whole reason a stack is more useful than one current screen.
        services.push_scene(make_playground_scene());
    } else if (input.key_pressed(Key::Escape)) {
        services.quit();
    }
}

void TitleScene::draw(GameServices& services, DrawList& out) const {
    const t2d::Vec2 view = services.viewport();
    const t2d::Vec2 middle{view.x * 0.5f, view.y * 0.5f};
    out.rect(Aabb2{t2d::Vec2{0.0f, 0.0f}, view}, kBackground);

    // Two rules frame the screen; the lower one is where the language hint sits.
    out.rect(Aabb2{t2d::Vec2{0.0f, 0.0f}, t2d::Vec2{view.x, 4.0f}}, kRule);
    out.rect(Aabb2{t2d::Vec2{0.0f, view.y - 4.0f}, t2d::Vec2{view.x, view.y}}, kRule);

    // A ring of slow dots around the picture: something moves, so a still frame is not mistaken for a
    // frozen program.
    const t2d::Vec2 logo_centre{view.x * 0.5f, view.y * 0.29f};
    for (i32 index = 0; index < 14; ++index) {
        const f32 angle = seconds_ * 0.35f + static_cast<f32>(index) * (kTau / 14.0f);
        const f32 radius = 118.0f + std::sin(seconds_ * 0.9f + static_cast<f32>(index)) * 10.0f;
        const t2d::Vec2 at{logo_centre.x + std::cos(angle) * radius,
                           logo_centre.y + std::sin(angle) * radius};
        const f32 size = 3.0f + 2.0f * (0.5f + 0.5f * std::sin(seconds_ * 2.0f + static_cast<f32>(index)));
        out.rect(Aabb2{t2d::Vec2{at.x - size, at.y - size}, t2d::Vec2{at.x + size, at.y + size}},
                 (index % 3 == 0) ? kAccent : kAccentDim);
    }

    // The picture the shell ships with. It is a placeholder: a game points this at its own art, and the
    // renderer draws a marked box if the file is missing rather than silently drawing nothing.
    out.sprite("placeholder",
               Aabb2{t2d::Vec2{logo_centre.x - 54.0f, logo_centre.y - 54.0f},
                     t2d::Vec2{logo_centre.x + 54.0f, logo_centre.y + 54.0f}});

    const std::string title(services.text("title.name"));
    const t2d::TextMetrics title_metrics = services.measure_text(title, 64);
    out.text(title, t2d::Vec2{middle.x - title_metrics.width * 0.5f, view.y * 0.43f}, 64, kAccent);

    const std::string tagline(services.text("title.tagline"));
    const t2d::TextMetrics tagline_metrics = services.measure_text(tagline, 18);
    out.text(tagline,
             t2d::Vec2{middle.x - tagline_metrics.width * 0.5f, view.y * 0.43f + title_metrics.height + 10.0f}, 18,
             kDim);

    // The start prompt blinks, slowly enough to read.
    if (std::fmod(seconds_, 1.4f) < 0.95f) {
        const std::string start(services.text("title.start"));
        const t2d::TextMetrics start_metrics = services.measure_text(start, 22);
        out.text(start, t2d::Vec2{middle.x - start_metrics.width * 0.5f, view.y * 0.66f}, 22, kText);
    }
    const std::string quit(services.text("title.quit"));
    const t2d::TextMetrics quit_metrics = services.measure_text(quit, 16);
    out.text(quit, t2d::Vec2{middle.x - quit_metrics.width * 0.5f, view.y * 0.72f}, 16, kDim);

    const std::string language(services.text("title.language"));
    const t2d::TextMetrics language_metrics = services.measure_text(language, 14);
    out.text(language, t2d::Vec2{middle.x - language_metrics.width * 0.5f, view.y - 72.0f}, 14, kDim);
}

} // namespace mefemma
