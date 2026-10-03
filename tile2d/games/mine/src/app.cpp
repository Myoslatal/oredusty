#include <mine/app.h>

#include <t2d/core/log.h>
#include <t2d/core/time.h>
#include <t2d/render/atlas.h>

#include <ore/core/image.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

namespace mine {
namespace {

/// Colours are built with make_rgba: the packed layout is 0xAABBGGRR, so writing a hex literal as if
/// it were 0xRRGGBBAA silently swaps red and blue.
struct Palette {
    u32 background = ore::make_rgba(20, 16, 14);
    u32 panel_fill = ore::make_rgba(34, 26, 22, 240);
    u32 panel_edge = ore::make_rgba(58, 44, 36);
    u32 text = ore::make_rgba(232, 224, 216);
    u32 text_dim = ore::make_rgba(154, 140, 130);
    u32 accent = ore::make_rgba(246, 200, 70);
    u32 highlight = ore::make_rgba(246, 200, 70, 70);
    u32 warning = ore::make_rgba(111, 208, 246);
};
constexpr Palette kPalette{};

/// Text is drawn with the framework's 5x7 bitmap font, so every distance is derived from glyph
/// metrics: the interface scales with the window instead of being pinned to one resolution.
[[nodiscard]] f32 line_height(f32 scale) {
    return (static_cast<f32>(t2d::kFontGlyphHeight) + 3.0f) * scale;
}

struct Layout {
    f32 scale = 2.0f;
    t2d::Aabb2 panel{};
    f32 padding = 0.0f;
};

[[nodiscard]] Layout make_layout(const ore::Renderer& renderer) {
    const f32 width = static_cast<f32>(renderer.width());
    const f32 height = static_cast<f32>(renderer.height());
    Layout layout;
    layout.scale = std::max(1.0f, std::floor(height / 300.0f));
    layout.padding = 8.0f * layout.scale;
    // The panel is sized in pixels from the text scale, and clamped to the window: the longest line
    // of the start screen has to fit inside it.
    const f32 panel_width = std::min(width - 4.0f * layout.padding, 330.0f * layout.scale);
    const f32 panel_height = std::min(height - 4.0f * layout.padding, 130.0f * layout.scale);
    layout.panel = t2d::Aabb2::from_center(t2d::Vec2{width * 0.5f, height * 0.5f},
                                          t2d::Vec2{panel_width * 0.5f, panel_height * 0.5f});
    return layout;
}

[[nodiscard]] std::string format_seed(u32 seed) { return std::format("{:08}", seed); }

} // namespace

MineApp::~MineApp() = default;

void MineApp::on_configure(ore::AppConfig& config) {
    config.name = "Mine - start screen";
    config.window.title = "Mine";
    config.window.width = 1280;
    config.window.height = 720;
    config.depth_format = VK_FORMAT_UNDEFINED; // the interface is pure 2D
    config.stats_interval = 0.0f;
}

ore::ConstSpan<ore::CliOption> MineApp::cli_options() const {
    static const ore::CliOption kOptions[] = {
        {"world", "<story|endless>", "which content pipeline produces the mine"},
        {"seed", "<n>", "seed for an endless world"},
        {"host", "<0|1>", "run as the host of the session"},
        {"connect", "<host:port>", "join a session instead of starting one"},
        {"start", "<0|1>", "skip the start screen and open the session screen directly"},
    };
    return ore::ConstSpan<ore::CliOption>(kOptions, std::size(kOptions));
}

void MineApp::on_start() {
    menu_.set_mode(options_.session.mode);
    menu_.set_seed(options_.session.seed);
    session_ = options_.session;
    if (options_.start_immediately) screen_ = Screen::Session;

    ore::rhi::GraphicsContext& context = this->context();
    t2d::SpriteBatch::Options batch_options;
    batch_options.color_format = renderer().color_format();
    batch_options.depth_format = renderer().depth_format();
    batch_options.max_quads = 4096;
    batch_options.vertex_shader_path = shader_path("sprite.vert.spv");
    batch_options.fragment_shader_path = shader_path("sprite.frag.spv");
    batch_ = t2d::SpriteBatch::create(context, batch_options);
    if (batch_ == nullptr) T2D_FATAL("the sprite batch could not be created");

    font_atlas_ = context.create_texture(t2d::make_font_atlas(), false, "mine.font");
    ore::rhi::SamplerDesc sampler_desc;
    sampler_desc.min_filter = VK_FILTER_NEAREST;
    sampler_desc.mag_filter = VK_FILTER_NEAREST;
    sampler_desc.mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_ = ore::rhi::Sampler::create(context.device(), sampler_desc);
}

void MineApp::on_update(f32 delta_seconds) {
    if (screen_ == Screen::Start) handle_start_input();
    // Headless runs have no vsync: without pacing the loop would spin and a screenshot would be
    // taken before the interface settled.
    if (config().headless) {
        constexpr f32 kFrameSeconds = 1.0f / 60.0f;
        const f32 remaining = kFrameSeconds - delta_seconds;
        if (remaining > 0.0f) t2d::sleep_ms(static_cast<u32>(remaining * 1000.0f + 0.5f));
    }
}

void MineApp::handle_start_input() {
    const ore::Window* window = this->window();
    if (window == nullptr) return;
    const ore::InputState& input = window->input();

    MenuAction action = MenuAction::None;
    const auto press = [&](ore::Key key, MenuKey mapped) {
        if (input.key_pressed(key)) action = menu_.handle(mapped);
    };
    press(ore::Key::Up, MenuKey::Up);
    press(ore::Key::W, MenuKey::Up);
    press(ore::Key::Down, MenuKey::Down);
    press(ore::Key::S, MenuKey::Down);
    press(ore::Key::Left, MenuKey::Left);
    press(ore::Key::A, MenuKey::Left);
    press(ore::Key::Right, MenuKey::Right);
    press(ore::Key::D, MenuKey::Right);
    press(ore::Key::R, MenuKey::Randomise);
    press(ore::Key::Enter, MenuKey::Confirm);
    press(ore::Key::Space, MenuKey::Confirm);
    press(ore::Key::Escape, MenuKey::Back);

    if (action == MenuAction::Quit) quit();
    else if (action == MenuAction::StartSession) begin_session(menu_.session_config());
}

void MineApp::begin_session(const SessionConfig& session) {
    session_ = session;
    screen_ = Screen::Session;
    T2D_INFO("session: world {} role {} seed {} (content registry: {} entries)", mode_name(session_.mode),
             role_name(session_.role), session_.seed, registry_.total_count());
    // Layer content is not authored yet (docs/GAME_DESIGN.md section 6): this is where the content
    // data would be loaded into registry_, where the layer data would be loaded or generated, and
    // where ServerHost/LocalClient would be created. The save would then store registry_.table()
    // alongside the ids it references.
}

void MineApp::draw_start_screen() {
    const Layout layout = make_layout(renderer());
    const f32 scale = layout.scale;
    const f32 left = layout.panel.min.x + layout.padding;
    const f32 right = layout.panel.max.x - layout.padding;

    batch_->draw_rect(layout.panel, kPalette.panel_fill);
    batch_->draw_rect_outline(layout.panel, 2.0f, kPalette.panel_edge);

    f32 y = layout.panel.min.y + layout.padding;
    batch_->draw_text(left, y, scale * 2.0f, kPalette.accent, "MINE");
    y += line_height(scale * 2.0f) + 2.0f;
    batch_->draw_text(left, y, scale, kPalette.text_dim, "SANDBOX / INDUSTRIAL AUTOMATION");
    y += line_height(scale) + 6.0f;
    batch_->draw_rect(t2d::Aabb2{t2d::Vec2{left, y}, t2d::Vec2{right, y + 2.0f}}, kPalette.panel_edge);
    y += 10.0f;

    const f32 value_column = left + 140.0f * scale;
    for (usize index = 0; index < menu_.row_count(); ++index) {
        const MenuRow& row = menu_.row(index);
        const bool focused = index == menu_.selected();
        const f32 row_height = line_height(scale) + 4.0f;
        if (focused) {
            batch_->draw_rect(t2d::Aabb2{t2d::Vec2{left - 4.0f, y - 2.0f},
                                         t2d::Vec2{right, y + row_height - 4.0f}},
                              kPalette.highlight);
        }
        const u32 color = focused ? kPalette.accent : kPalette.text;
        batch_->draw_text(left, y, scale, color, row.label);
        if (row.kind == RowKind::World) {
            batch_->draw_text(value_column, y, scale, color, std::format("< {} >", mode_name(menu_.mode())));
        } else if (row.kind == RowKind::Seed) {
            batch_->draw_text(value_column, y, scale, color, std::format("< {} >", format_seed(menu_.seed())));
        } else if (focused) {
            batch_->draw_text(value_column, y, scale, kPalette.text_dim, "< ENTER >");
        }
        y += row_height;
    }

    if (menu_.seed_visible()) {
        y += 4.0f;
        batch_->draw_text(left, y, scale, kPalette.text_dim,
                          std::format("SEED {} - PRESS R TO RANDOMISE", format_seed(menu_.seed())));
    }
    const f32 footer_y = layout.panel.max.y - layout.padding - line_height(scale) * 2.0f;
    batch_->draw_text(left, footer_y, scale, kPalette.text_dim, "UP/DOWN SELECT   LEFT/RIGHT CHANGE");
    batch_->draw_text(left, footer_y + line_height(scale), scale, kPalette.text_dim,
                      "ENTER CONFIRM   R RANDOM SEED   ESC QUIT");
}

void MineApp::draw_session_screen() {
    const Layout layout = make_layout(renderer());
    const f32 scale = layout.scale;
    const f32 left = layout.panel.min.x + layout.padding;
    const f32 right = layout.panel.max.x - layout.padding;

    batch_->draw_rect(layout.panel, kPalette.panel_fill);
    batch_->draw_rect_outline(layout.panel, 2.0f, kPalette.panel_edge);

    f32 y = layout.panel.min.y + layout.padding;
    batch_->draw_text(left, y, scale * 1.5f, kPalette.accent, "SESSION");
    y += line_height(scale * 1.5f) + 4.0f;
    batch_->draw_rect(t2d::Aabb2{t2d::Vec2{left, y}, t2d::Vec2{right, y + 2.0f}}, kPalette.panel_edge);
    y += 10.0f;

    const f32 value_column = left + 140.0f * scale;
    const auto line = [&](const char* label, const std::string& value, u32 color) {
        batch_->draw_text(left, y, scale, kPalette.text_dim, label);
        batch_->draw_text(value_column, y, scale, color, value);
        y += line_height(scale) + 4.0f;
    };
    line("WORLD", mode_name(session_.mode), kPalette.text);
    line("CONTENT", std::format("{} REGISTERED", registry_.total_count()), kPalette.text);
    line("ROLE", role_name(session_.role), kPalette.text);
    line("SEED", format_seed(session_.seed), kPalette.text);
    if (session_.role == Role::Join) line("CONNECT", session_.connect_address, kPalette.text);

    y += 6.0f;
    batch_->draw_rect(t2d::Aabb2{t2d::Vec2{left, y}, t2d::Vec2{right, y + 2.0f}}, kPalette.panel_edge);
    y += 10.0f;
    batch_->draw_text(left, y, scale, kPalette.warning, "LAYER CONTENT: NOT AUTHORED YET");
    y += line_height(scale) + 2.0f;
    batch_->draw_text(left, y, scale, kPalette.text_dim, "RESOURCES, STRUCTURES AND DEMANDS COME FROM THE");
    y += line_height(scale);
    batch_->draw_text(left, y, scale, kPalette.text_dim, "DESIGNER DATA (docs/GAME_DESIGN.md SECTION 7).");

    batch_->draw_text(left, layout.panel.max.y - layout.padding - line_height(scale), scale,
                      kPalette.text_dim, "ESC BACK TO THE START SCREEN");
}

void MineApp::on_render(ore::RenderFrame& frame) {
    VkClearValue clear{};
    clear.color = {{0.078f, 0.063f, 0.055f, 1.0f}};
    renderer().begin_pass(clear, 1.0f);

    const t2d::Vec2 view{static_cast<f32>(renderer().width()), static_cast<f32>(renderer().height())};
    batch_->begin(frame, t2d::sprite_view_projection(t2d::Vec2{view.x * 0.5f, view.y * 0.5f}, view),
                  *font_atlas_, sampler_->handle());
    batch_->draw_rect(t2d::Aabb2{t2d::Vec2{0.0f, 0.0f}, view}, kPalette.background);
    if (screen_ == Screen::Start) draw_start_screen();
    else draw_session_screen();
    batch_->end();

    frame.counters.draw_calls = batch_->draw_calls();
    frame.counters.triangles = batch_->quads() * 2;
    renderer().end_pass();
}

void MineApp::on_shutdown() {
    batch_.reset();
    font_atlas_.reset();
    sampler_.reset();
}

} // namespace mine
