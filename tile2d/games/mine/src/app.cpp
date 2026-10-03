#include <mine/app.h>

#include <t2d/core/cli.h>
#include <t2d/core/log.h>
#include <t2d/core/time.h>

#include <ore/core/image.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

namespace mine {
namespace {

/// Colours are built with make_rgba: the packed layout is 0xAABBGGRR, so writing a hex literal as if it
/// were 0xRRGGBBAA silently swaps red and blue.
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

/// Fonts are looked up in the usual system locations; the command line can override both.
constexpr const char* kLatinCandidates[] = {
    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
};
constexpr const char* kCjkCandidates[] = {
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/noto-cjk/NotoSerifCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc",
};

/// Loads the first candidate that parses.
[[nodiscard]] Scope<t2d::Font> load_first_font(const char* const* candidates, usize count,
                                               const std::string& override_path) {
    const auto try_load = [](const std::string& path, u32 face) -> Scope<t2d::Font> {
        std::string error;
        std::optional<t2d::Font> font = t2d::Font::load(path, face, &error);
        if (!font.has_value()) {
            T2D_WARN("font '{}' face {}: {}", path, face, error);
            return nullptr;
        }
        T2D_INFO("font: '{} {}' ({} glyphs, {} outlines) from {}", font->family_name(), font->style_name(),
                 font->metrics().glyph_count, font->outline_kind(), path);
        return t2d::make_scope<t2d::Font>(std::move(*font));
    };
    if (!override_path.empty()) return try_load(override_path, 0);
    for (usize index = 0; index < count; ++index) {
        if (Scope<t2d::Font> font = try_load(candidates[index], 0); font != nullptr) return font;
    }
    return nullptr;
}

/// Picks the face of a CJK collection that matches the language: Simplified and Traditional Chinese
/// are different faces of the same file (Noto Sans CJK carries ten), and they really do draw some
/// characters differently.
[[nodiscard]] u32 pick_cjk_face(const std::string& path, t2d::Language language) {
    const std::vector<std::pair<std::string, std::string>> names = t2d::Font::face_names(path);
    if (names.size() <= 1) return 0;
    const char* wanted = language == t2d::Language::TraditionalChinese ? "TC" : "SC";
    for (usize index = 0; index < names.size(); ++index) {
        // "Noto Sans CJK SC" / "Noto Sans Mono CJK SC": the plain family is preferred over the mono one.
        if (names[index].first.find(wanted) == std::string::npos) continue;
        if (names[index].first.find("Mono") != std::string::npos) continue;
        return static_cast<u32>(index);
    }
    return 0;
}

[[nodiscard]] std::string format_localized(std::string_view pattern, u64 value) {
    // A localized pattern is a runtime string, so vformat rather than format.
    return std::vformat(pattern, std::make_format_args(value));
}

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
        {"lang", "<code>", "interface language: en, zh-Hans, zh-Hant"},
        {"font", "<path>", "Latin font file (default: the first system font found)"},
        {"cjk-font", "<path>", "CJK font file used for Chinese text"},
        {"ui-text", "<path>", "interface strings (.ecfg); defaults to assets/text/ui.ecfg"},
    };
    return ore::ConstSpan<ore::CliOption>(kOptions, std::size(kOptions));
}

void MineApp::load_localisation() {
    const std::string path = options_.ui_text_path.empty()
                                 ? std::string(T2D_SOURCE_DIR) + "/assets/text/ui.ecfg"
                                 : options_.ui_text_path;
    t2d::EcfgError error;
    std::vector<std::string> unknown;
    std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::load(path, &error);
    if (!document.has_value()) {
        T2D_WARN("interface strings '{}': {}", path, error.describe(path));
        return;
    }
    locale_ = t2d::Locale::from_ecfg(*document, &unknown);
    locale_.set_language(options_.language.value_or(t2d::Language::English));
    menu_.set_language(locale_.language());
    T2D_INFO("locale: {} ({} strings, {} missing) from {}", t2d::language_code(locale_.language()),
             locale_.count(locale_.language()), locale_.missing_for_active_language().size(), path);
}

void MineApp::load_fonts() {
    latin_font_ = load_first_font(kLatinCandidates, std::size(kLatinCandidates), options_.font_path);
    const std::string cjk_path = [&] {
        if (!options_.cjk_font_path.empty()) return options_.cjk_font_path;
        for (const char* candidate : kCjkCandidates) {
            if (t2d::read_binary(candidate).has_value()) return std::string(candidate);
        }
        return std::string{};
    }();
    if (!cjk_path.empty()) {
        const u32 face = pick_cjk_face(cjk_path, locale_.language());
        std::string error;
        std::optional<t2d::Font> font = t2d::Font::load(cjk_path, face, &error);
        if (font.has_value()) {
            T2D_INFO("font: '{} {}' face {} ({} glyphs, {} outlines) from {}", font->family_name(),
                     font->style_name(), face, font->metrics().glyph_count, font->outline_kind(), cjk_path);
            cjk_font_ = t2d::make_scope<t2d::Font>(std::move(*font));
        } else {
            T2D_WARN("CJK font '{}' face {}: {}", cjk_path, face, error);
        }
    }
    fonts_.latin = latin_font_.get();
    fonts_.cjk = cjk_font_.get();
    if (fonts_.latin == nullptr) T2D_ERROR("no usable Latin font: text will not render");
}

void MineApp::on_start() {
    // The interface scale follows the window: 14 px text at 720p, 20 px at 1200p.
    const f32 height = static_cast<f32>(renderer().height());
    unit_ = std::max(1.0f, std::floor(height / 300.0f));
    body_px_ = static_cast<u16>(std::max(14.0f, 5.0f * unit_));

    load_localisation();
    menu_.set_mode(options_.session.mode);
    menu_.set_seed(options_.session.seed);
    session_ = options_.session;
    if (options_.start_immediately) screen_ = Screen::Session;

    ore::rhi::GraphicsContext& context = this->context();
    t2d::SpriteBatch::Options batch_options;
    batch_options.color_format = renderer().color_format();
    batch_options.depth_format = renderer().depth_format();
    batch_options.max_quads = 8192;
    batch_options.vertex_shader_path = shader_path("sprite.vert.spv");
    batch_options.fragment_shader_path = shader_path("sprite.frag.spv");
    batch_ = t2d::SpriteBatch::create(context, batch_options);
    if (batch_ == nullptr) T2D_FATAL("the sprite batch could not be created");

    load_fonts();
    t2d::GlyphAtlas::Options atlas_options;
    atlas_options.page_size = 2048;
    atlas_options.max_pages = 1;
    text_ = t2d::TextRenderer::create(context, atlas_options);
    if (text_ == nullptr) T2D_FATAL("the text renderer could not be created");
    // Rectangles come from the same texture as the text, so the batch samples the atlas's white texel.
    batch_->set_white_texel(text_->atlas().white_uv());

    ore::rhi::SamplerDesc sampler_desc;
    sampler_desc.min_filter = VK_FILTER_LINEAR; // glyphs are anti-aliased: linear keeps them smooth
    sampler_desc.mag_filter = VK_FILTER_LINEAR;
    sampler_desc.mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_ = ore::rhi::Sampler::create(context.device(), sampler_desc);
}

void MineApp::on_update(f32 delta_seconds) {
    if (screen_ == Screen::Start) handle_start_input();
    // Headless runs have no vsync: without pacing the loop would spin and a screenshot would be taken
    // before the interface settled.
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

    if (action == MenuAction::Quit) {
        quit();
        return;
    }
    if (menu_.language() != locale_.language()) {
        // Switching language may switch the CJK face too (Simplified and Traditional are different
        // faces), so the fonts are reloaded; the glyph cache keeps what it already rasterised.
        locale_.set_language(menu_.language());
        load_fonts();
    }
    if (action == MenuAction::StartSession) begin_session(menu_.session_config());
}

void MineApp::begin_session(const SessionConfig& session) {
    session_ = session;
    screen_ = Screen::Session;
    T2D_INFO("session: world {} role {} seed {} (content registry: {} entries)", mode_name(session_.mode),
             role_name(session_.role), session_.seed, registry_.total_count());
    // Layer content is not authored yet (docs/GAME_DESIGN.md section 7): this is where the content data
    // would be loaded into registry_, where the layer data would be loaded or generated, and where
    // ServerHost/LocalClient would be created. The save would then store registry_.table() alongside
    // the ids it references.
}

f32 MineApp::draw_line(f32 x, f32 y, u16 size_px, u32 color, std::string_view text, f32 max_width) {
    t2d::TextStyle style;
    style.size_px = size_px;
    style.color = color;
    return text_->draw(*batch_, text, fonts_, style, t2d::Vec2{x, y}, max_width);
}

f32 MineApp::draw_pair(f32 x, f32 y, f32 value_column, u16 size_px, std::string_view label,
                       std::string_view value, u32 label_color, u32 value_color) {
    draw_line(x, y, size_px, label_color, label);
    draw_line(value_column, y, size_px, value_color, value);
    return y + static_cast<f32>(size_px) * 1.45f;
}

void MineApp::draw_start_screen() {
    const f32 width = static_cast<f32>(renderer().width());
    const f32 height = static_cast<f32>(renderer().height());
    const f32 padding = 8.0f * unit_;
    const f32 panel_width = std::min(width - 4.0f * padding, 330.0f * unit_);
    const f32 panel_height = std::min(height - 4.0f * padding, 150.0f * unit_);
    const t2d::Aabb2 panel = t2d::Aabb2::from_center(t2d::Vec2{width * 0.5f, height * 0.5f},
                                                     t2d::Vec2{panel_width * 0.5f, panel_height * 0.5f});
    const f32 left = panel.min.x + padding;
    const f32 right = panel.max.x - padding;

    batch_->draw_rect(panel, kPalette.panel_fill);
    batch_->draw_rect_outline(panel, 2.0f, kPalette.panel_edge);

    f32 y = panel.min.y + padding;
    const u16 title_px = static_cast<u16>(body_px_ * 2);
    y += draw_line(left, y, title_px, kPalette.accent, locale_.text("title")) + 2.0f;
    y += draw_line(left, y, body_px_, kPalette.text_dim, locale_.text("subtitle")) + 6.0f;
    batch_->draw_rect(t2d::Aabb2{t2d::Vec2{left, y}, t2d::Vec2{right, y + 2.0f}}, kPalette.panel_edge);
    y += 10.0f;

    // The value column follows the widest label instead of a fixed offset: "WORLD" and "世界" are very
    // different widths, and a fixed column leaves one of them stranded.
    f32 label_width = 0.0f;
    t2d::TextStyle measure_style;
    measure_style.size_px = body_px_;
    for (usize index = 0; index < menu_.row_count(); ++index) {
        label_width = std::max(label_width, text_->measure(locale_.text(menu_.row(index).id), fonts_, measure_style).width);
    }
    const f32 value_column = left + label_width + 3.0f * unit_;
    const f32 row_height = static_cast<f32>(body_px_) * 1.45f;
    for (usize index = 0; index < menu_.row_count(); ++index) {
        const MenuRow& row = menu_.row(index);
        const bool focused = index == menu_.selected();
        if (focused) {
            batch_->draw_rect(t2d::Aabb2{t2d::Vec2{left - 4.0f, y - 2.0f},
                                         t2d::Vec2{right, y + row_height - 4.0f}},
                              kPalette.highlight);
        }
        const u32 color = focused ? kPalette.accent : kPalette.text;
        draw_line(left, y, body_px_, color, locale_.text(row.id));
        switch (row.kind) {
            case RowKind::World:
                draw_line(value_column, y, body_px_, color,
                          std::format("< {} >", locale_.text(menu_.mode() == Mode::Endless ? "value.endless"
                                                                                          : "value.story")));
                break;
            case RowKind::Language:
                draw_line(value_column, y, body_px_, color,
                          std::format("< {} >", t2d::language_name(menu_.language())));
                break;
            case RowKind::Seed:
                draw_line(value_column, y, body_px_, color, std::format("< {:08} >", menu_.seed()));
                break;
            case RowKind::Action:
                if (focused) draw_line(value_column, y, body_px_, kPalette.text_dim, "< ENTER >");
                break;
        }
        y += row_height;
    }

    if (menu_.seed_visible()) {
        y += 4.0f;
        draw_line(left, y, body_px_, kPalette.text_dim,
                  format_localized(locale_.text("seed.hint"), menu_.seed()));
    }

    f32 footer = panel.max.y - padding - static_cast<f32>(body_px_) * 2.9f;
    footer += draw_line(left, footer, body_px_, kPalette.text_dim, locale_.text("hint.rows")) + 2.0f;
    draw_line(left, footer, body_px_, kPalette.text_dim, locale_.text("hint.actions"));
}

void MineApp::draw_session_screen() {
    const f32 width = static_cast<f32>(renderer().width());
    const f32 height = static_cast<f32>(renderer().height());
    const f32 padding = 8.0f * unit_;
    const f32 panel_width = std::min(width - 4.0f * padding, 330.0f * unit_);
    const f32 panel_height = std::min(height - 4.0f * padding, 130.0f * unit_);
    const t2d::Aabb2 panel = t2d::Aabb2::from_center(t2d::Vec2{width * 0.5f, height * 0.5f},
                                                     t2d::Vec2{panel_width * 0.5f, panel_height * 0.5f});
    const f32 left = panel.min.x + padding;
    const f32 right = panel.max.x - padding;

    batch_->draw_rect(panel, kPalette.panel_fill);
    batch_->draw_rect_outline(panel, 2.0f, kPalette.panel_edge);

    f32 y = panel.min.y + padding;
    y += draw_line(left, y, static_cast<u16>(body_px_ * 1.5f), kPalette.accent, locale_.text("session.title"));
    batch_->draw_rect(t2d::Aabb2{t2d::Vec2{left, y}, t2d::Vec2{right, y + 2.0f}}, kPalette.panel_edge);
    y += 10.0f;

    f32 label_width = 0.0f;
    t2d::TextStyle measure_style;
    measure_style.size_px = body_px_;
    for (const char* id : {"session.world", "session.content", "session.role", "session.seed", "session.connect"}) {
        label_width = std::max(label_width, text_->measure(locale_.text(id), fonts_, measure_style).width);
    }
    const f32 value_column = left + label_width + 3.0f * unit_;
    y = draw_pair(left, y, value_column, body_px_, locale_.text("session.world"),
                  locale_.text(session_.mode == Mode::Endless ? "value.endless" : "value.story"), kPalette.text_dim,
                  kPalette.text);
    y = draw_pair(left, y, value_column, body_px_, locale_.text("session.content"),
                  format_localized(locale_.text("content.registered"), registry_.total_count()),
                  kPalette.text_dim, kPalette.text);
    y = draw_pair(left, y, value_column, body_px_, locale_.text("session.role"),
                  locale_.text(session_.role == Role::Host ? "role.host"
                                                           : (session_.role == Role::Join ? "role.join"
                                                                                          : "role.single")),
                  kPalette.text_dim, kPalette.text);
    y = draw_pair(left, y, value_column, body_px_, locale_.text("session.seed"),
                  std::format("{:08}", session_.seed), kPalette.text_dim, kPalette.text);
    if (session_.role == Role::Join) {
        y = draw_pair(left, y, value_column, body_px_, locale_.text("session.connect"), session_.connect_address,
                      kPalette.text_dim, kPalette.text);
    }

    y += 6.0f;
    batch_->draw_rect(t2d::Aabb2{t2d::Vec2{left, y}, t2d::Vec2{right, y + 2.0f}}, kPalette.panel_edge);
    y += 10.0f;
    y += draw_line(left, y, body_px_, kPalette.warning, locale_.text("session.pending")) + 2.0f;
    y += draw_line(left, y, body_px_, kPalette.text_dim, locale_.text("session.pending.line1"));
    draw_line(left, y, body_px_, kPalette.text_dim, locale_.text("session.pending.line2"));

    draw_line(left, panel.max.y - padding - static_cast<f32>(body_px_) * 1.45f, body_px_, kPalette.text_dim,
              locale_.text("session.back"));
}

void MineApp::on_render(ore::RenderFrame& frame) {
    VkClearValue clear{};
    clear.color = {{0.078f, 0.063f, 0.055f, 1.0f}};
    renderer().begin_pass(clear, 1.0f);

    const t2d::Vec2 view{static_cast<f32>(renderer().width()), static_cast<f32>(renderer().height())};
    // One pass, one texture: the glyph atlas holds both the text and the white texel rectangles use.
    batch_->begin(frame, t2d::sprite_view_projection(t2d::Vec2{view.x * 0.5f, view.y * 0.5f}, view),
                  *text_->texture(), sampler_->handle());
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
    text_.reset();
    cjk_font_.reset();
    latin_font_.reset();
    sampler_.reset();
}

} // namespace mine
