#include <mefemma/scenes/playground_scene.h>

#include <t2d/core/log.h>

#include <cmath>
#include <format>
#include <string>

namespace mefemma {
namespace {

constexpr u32 kBackground = rgba(16, 18, 24);
constexpr u32 kGrid = rgba(38, 43, 55);
constexpr u32 kGridMajor = rgba(56, 64, 82);
constexpr u32 kAxis = rgba(120, 170, 220);
constexpr u32 kAccent = rgba(255, 196, 84);
constexpr u32 kText = rgba(232, 234, 240);
constexpr u32 kDim = rgba(148, 154, 170);
constexpr u32 kPanel = rgba(14, 16, 21, 210);
constexpr u32 kPanelEdge = rgba(56, 62, 76);

/// One world unit is one pixel at zoom 1. The grid is drawn every kGridStep units, and coarser when the
/// view is zoomed far enough out that the lines would be the picture.
constexpr f32 kGridStep = 64.0f;

template <class... Args>
[[nodiscard]] std::string format_localized(std::string_view pattern, Args&&... args) {
    return std::vformat(pattern, std::make_format_args(args...));
}

} // namespace

std::unique_ptr<Scene> make_playground_scene() { return std::make_unique<PlaygroundScene>(); }

PlaygroundScene::PlaygroundScene() {
    // A cell is a pixel at zoom 1; the limits are the shell's, not a game's: below a tenth of a pixel the
    // grid is noise, above eight a pixel fills a screen.
    camera_.set_zoom_limits(0.1f, 8.0f);
    camera_.set_zoom(1.0f);
    camera_.look_at(t2d::Vec2{0.0f, 0.0f});
}

void PlaygroundScene::on_enter(GameServices& services) {
    camera_.set_viewport(services.viewport());
    T2D_INFO("scene: playground (camera at {:.1f},{:.1f}, zoom {:.2f})", camera_.centre().x, camera_.centre().y,
             camera_.zoom());
}

void PlaygroundScene::update(GameServices& services, f32 delta_seconds) {
    seconds_ += delta_seconds;
    const InputState& input = services.input();
    camera_.set_viewport(services.viewport());

    // The camera is the only thing a blank scene can drive: the arrow keys (or WASD) pan, the wheel zooms
    // about the pointer, and the middle button drags the map the way a map is dragged everywhere else.
    constexpr f32 kPanPixelsPerSecond = 720.0f;
    t2d::Vec2 look{};
    if (input.key_down(Key::Left) || input.key_down(Key::A)) look.x -= 1.0f;
    if (input.key_down(Key::Right) || input.key_down(Key::D)) look.x += 1.0f;
    if (input.key_down(Key::Up) || input.key_down(Key::W)) look.y -= 1.0f;
    if (input.key_down(Key::Down) || input.key_down(Key::S)) look.y += 1.0f;
    if (look.x != 0.0f || look.y != 0.0f) {
        const f32 step = kPanPixelsPerSecond * delta_seconds;
        camera_.pan(t2d::Vec2{-look.x * step, -look.y * step});
    }
    if (input.mouse_down(MouseButton::Middle)) {
        camera_.pan(t2d::Vec2{input.mouse_delta_x(), input.mouse_delta_y()});
    }
    if (input.scroll_y() != 0.0f) {
        camera_.zoom_at(t2d::Vec2{input.mouse_x(), input.mouse_y()},
                        input.scroll_y() > 0.0f ? 1.15f : (1.0f / 1.15f));
    }
    if (input.key_pressed(Key::Equal) || input.key_pressed(Key::KpAdd)) {
        camera_.zoom_at(services.viewport() * 0.5f, 1.25f);
    }
    if (input.key_pressed(Key::Minus) || input.key_pressed(Key::KpSubtract)) {
        camera_.zoom_at(services.viewport() * 0.5f, 0.8f);
    }
    if (input.key_pressed(Key::C)) {
        camera_.look_at(t2d::Vec2{0.0f, 0.0f});
        camera_.set_zoom(1.0f);
    }
    if (input.key_pressed(Key::Escape)) {
        services.pop_scene();
        return;
    }

    pointer_inside_ = input.cursor_inside_window();
    pointer_world_ = camera_.world_of(t2d::Vec2{input.mouse_x(), input.mouse_y()});
}

void PlaygroundScene::draw(GameServices& services, DrawList& out) const {
    const t2d::Vec2 view = services.viewport();
    out.rect(Aabb2{t2d::Vec2{0.0f, 0.0f}, view}, kBackground);

    // The visible world rectangle, from the camera itself: no scene keeps a second copy of what the view
    // covers. A coarser grid when the lines would be closer than 12 pixels.
    const t2d::Vec2 top_left = camera_.world_of(t2d::Vec2{0.0f, 0.0f});
    const t2d::Vec2 bottom_right = camera_.world_of(view);
    f32 grid = kGridStep;
    while (grid * camera_.zoom() < 12.0f && grid < 65536.0f) grid *= 4.0f;
    const f32 thickness = 1.0f;

    const i32 first_x = t2d::floor_to_i32(top_left.x / grid) - 1;
    const i32 last_x = t2d::floor_to_i32(bottom_right.x / grid) + 1;
    const i32 first_y = t2d::floor_to_i32(top_left.y / grid) - 1;
    const i32 last_y = t2d::floor_to_i32(bottom_right.y / grid) + 1;
    // A line budget: at a zoom where a 1280 px screen is ten thousand grid lines, drawing them all would
    // cost more than the scene is worth. The grid is a reading aid, and it stops being one first.
    constexpr i32 kMaxLines = 400;
    for (i32 index = first_x; index <= last_x && index - first_x < kMaxLines; ++index) {
        const f32 x = camera_.screen_of(t2d::Vec2{static_cast<f32>(index) * grid, 0.0f}).x;
        out.rect(Aabb2{t2d::Vec2{x, 0.0f}, t2d::Vec2{x + thickness, view.y}},
                 index % 4 == 0 ? kGridMajor : kGrid);
    }
    for (i32 index = first_y; index <= last_y && index - first_y < kMaxLines; ++index) {
        const f32 y = camera_.screen_of(t2d::Vec2{0.0f, static_cast<f32>(index) * grid}).y;
        out.rect(Aabb2{t2d::Vec2{0.0f, y}, t2d::Vec2{view.x, y + thickness}},
                 index % 4 == 0 ? kGridMajor : kGrid);
    }
    // The world's origin gets two axes, so a pan can be read without a HUD.
    if (top_left.x <= 0.0f && bottom_right.x >= 0.0f) {
        const f32 x = camera_.screen_of(t2d::Vec2{0.0f, 0.0f}).x;
        out.rect(Aabb2{t2d::Vec2{x, 0.0f}, t2d::Vec2{x + 2.0f, view.y}}, kAxis);
    }
    if (top_left.y <= 0.0f && bottom_right.y >= 0.0f) {
        const f32 y = camera_.screen_of(t2d::Vec2{0.0f, 0.0f}).y;
        out.rect(Aabb2{t2d::Vec2{0.0f, y}, t2d::Vec2{view.x, y + 2.0f}}, kAxis);
    }

    // A few squares orbit the origin: proof that the world is a coordinate space and not a background.
    for (i32 index = 0; index < 8; ++index) {
        const f32 angle = seconds_ * 0.6f + static_cast<f32>(index) * (6.2831853f / 8.0f);
        const t2d::Vec2 world{std::cos(angle) * 260.0f, std::sin(angle) * 260.0f};
        const t2d::Vec2 at = camera_.screen_of(world);
        const f32 size = std::max(3.0f, 14.0f * camera_.zoom());
        out.rect(Aabb2{t2d::Vec2{at.x - size, at.y - size}, t2d::Vec2{at.x + size, at.y + size}}, kAccent);
    }

    // The pointer's cell: the world is divided into 64 unit cells for the demo, and the one under the
    // pointer is outlined. **This is where a game's pointer action goes** - a click that places a thing,
    // selects a unit, opens a panel. The framework deliberately does nothing with it.
    if (pointer_inside_) {
        const f32 cell = 64.0f;
        const f32 cell_x = std::floor(pointer_world_.x / cell) * cell;
        const f32 cell_y = std::floor(pointer_world_.y / cell) * cell;
        const t2d::Vec2 min = camera_.screen_of(t2d::Vec2{cell_x, cell_y});
        const t2d::Vec2 max = camera_.screen_of(t2d::Vec2{cell_x + cell, cell_y + cell});
        out.rect_outline(Aabb2{min, max}, 2.0f, kAccent);
    }

    // The scene's own chrome: what this place is, and the keys it answers. A game's own HUD goes here.
    const Aabb2 panel{t2d::Vec2{16.0f, 16.0f}, t2d::Vec2{16.0f + 560.0f, 16.0f + 104.0f}};
    out.rect(panel, kPanel);
    out.rect_outline(panel, 1.0f, kPanelEdge);
    out.text(std::string(services.text("play.title")), t2d::Vec2{28.0f, 26.0f}, 22, kAccent);
    out.text(std::string(services.text("play.note")), t2d::Vec2{28.0f, 58.0f}, 15, kDim);
    out.text(std::string(services.text("play.hint")), t2d::Vec2{28.0f, 84.0f}, 13, kText);

    if (pointer_inside_) {
        const i32 cell_x = t2d::floor_to_i32(pointer_world_.x / 64.0f);
        const i32 cell_y = t2d::floor_to_i32(pointer_world_.y / 64.0f);
        const std::string line =
            format_localized(services.text("play.cell"), pointer_world_.x, pointer_world_.y, cell_x, cell_y);
        out.text(line, t2d::Vec2{20.0f, view.y - 32.0f}, 13, kDim);
    } else {
        out.text(std::string(services.text("play.pointer.outside")), t2d::Vec2{20.0f, view.y - 32.0f}, 13, kDim);
    }
}

} // namespace mefemma
