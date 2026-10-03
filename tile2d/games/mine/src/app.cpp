#include <mine/app.h>

#include <t2d/core/cli.h>
#include <t2d/core/log.h>
#include <t2d/core/time.h>

#include <ore/core/image.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
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
    u32 error = ore::make_rgba(240, 120, 110);
    u32 grid_background = ore::make_rgba(14, 12, 11);
    u32 grid_line = ore::make_rgba(48, 40, 34);
    u32 grid_edge = ore::make_rgba(96, 78, 62);
    u32 missing = ore::make_rgba(168, 66, 60);
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

/// A localized pattern is a runtime string, so vformat rather than format.
template <class... Args>
[[nodiscard]] std::string format_localized(std::string_view pattern, Args&&... args) {
    return std::vformat(pattern, std::make_format_args(args...));
}

/// Writes a whole file; false when it cannot be written.
[[nodiscard]] bool write_binary_file(const std::string& path, t2d::ConstSpan<const u8> bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

/// The longest prefix of \p value that fits \p max_width, with an ellipsis when something was
/// dropped. Characters are removed a UTF-8 character at a time, so a Chinese path is never cut in half.
[[nodiscard]] std::string fit_text(const t2d::TextRenderer& text, const t2d::FontSet& fonts,
                                   const t2d::TextStyle& style, std::string_view value, f32 max_width) {
    std::string fitted{value};
    if (max_width <= 0.0f || text.measure(fitted, fonts, style).width <= max_width) return fitted;
    constexpr std::string_view kEllipsis = "...";
    while (!fitted.empty() && text.measure(std::string(fitted) + std::string(kEllipsis), fonts, style).width > max_width) {
        usize cut = fitted.size() - 1;
        while (cut > 0 && (static_cast<u8>(fitted[cut]) & 0xC0u) == 0x80u) --cut;
        fitted.resize(cut);
    }
    return fitted.empty() ? fitted : fitted + std::string(kEllipsis);
}

/// A cell fill that reads as a colour instead of as a lamp: the palette colour pulled towards the
/// background, with the full strength colour kept for the label and the outline.
[[nodiscard]] u32 dim_color(u32 color, f32 factor) {
    const auto channel = [&](u32 shift) {
        return static_cast<u32>(std::clamp(static_cast<f32>((color >> shift) & 0xFFu) * factor, 0.0f, 255.0f));
    };
    return ore::make_rgba(channel(0), channel(8), channel(16));
}

} // namespace

MineApp::~MineApp() = default;

void MineApp::on_configure(ore::AppConfig& config) {
    config.name = "Mine";
    config.window.title = "Mine";
    config.window.width = 1280;
    config.window.height = 720;
    config.depth_format = VK_FORMAT_UNDEFINED; // the interface is pure 2D
    config.stats_interval = 0.0f;
    // Escape is handled per screen: on the start screen it quits, on the others it goes back to it -
    // which is what the session screen has been telling the player all along.
    config.quit_on_escape = false;
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
        {"content", "<path>", "content data (.ecfg) for the sandbox; repeatable"},
        {"grid", "<WxH>", "sandbox map size (default 40x24)"},
        {"tile-layers", "<n>", "how many tile layers the sandbox map has (default 1)"},
        {"layer", "<n>", "which tile layer the sandbox starts on (default 0)"},
        {"fill", "<none|bands|scatter>", "sandbox debug fill: the palette laid out, never content"},
        {"fill-layer", "<n|all>", "which tile layers the fill writes into (default: the active one)"},
        {"layout", "<path>", "sandbox layout file: loaded at startup, written by F2"},
        {"save-layout", "<path>", "write the sandbox layout once at shutdown (scripted runs)"},
        {"dump-layer", "<0|1>", "write the sandbox layer as text to the log at shutdown"},
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
    if (options_.start_immediately) {
        screen_ = options_.session.mode == Mode::Sandbox ? Screen::Sandbox : Screen::Session;
    }

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
    if (screen_ == Screen::Sandbox) open_sandbox();
}

void MineApp::on_update(f32 delta_seconds) {
    switch (screen_) {
        case Screen::Start: handle_start_input(); break;
        case Screen::Session: handle_session_input(); break;
        case Screen::Sandbox: handle_sandbox_input(); break;
    }
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
    if (session_.mode == Mode::Sandbox) {
        // Not a session: one local layer, no server, no demands, no progression.
        screen_ = Screen::Sandbox;
        open_sandbox();
        return;
    }
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

f32 MineApp::draw_fitted(f32 x, f32 y, u16 size_px, u32 color, std::string_view text, f32 max_width) {
    t2d::TextStyle style;
    style.size_px = size_px;
    style.color = color;
    return draw_line(x, y, size_px, color, fit_text(*text_, fonts_, style, text, max_width));
}

f32 MineApp::draw_pair(f32 x, f32 y, f32 value_column, u16 size_px, std::string_view label,
                       std::string_view value, u32 label_color, u32 value_color, f32 value_max_width) {
    draw_line(x, y, size_px, label_color, label);
    if (value_max_width > 0.0f) draw_fitted(value_column, y, size_px, value_color, value, value_max_width);
    else draw_line(value_column, y, size_px, value_color, value);
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
                          std::format("< {} >", locale_.text(mode_value_id(menu_.mode()))));
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

// --- the sandbox -------------------------------------------------------------------------------

void MineApp::set_status(std::string_view text, bool error) {
    status_ = text;
    status_is_error_ = error;
    if (error) T2D_WARN("sandbox: {}", status_);
    else T2D_INFO("sandbox: {}", status_);
}

t2d::Aabb2 MineApp::sandbox_status_area() const {
    const f32 width = static_cast<f32>(renderer().width());
    const f32 height = static_cast<f32>(renderer().height());
    const f32 padding = 8.0f * unit_;
    const f32 line = static_cast<f32>(body_px_) * 1.45f;
    // Four label rows, the message and two hint lines.
    const f32 status_height = std::min(height * 0.45f, padding * 2.0f + line * 7.0f);
    return t2d::Aabb2{t2d::Vec2{0.0f, height - status_height}, t2d::Vec2{width, height}};
}

t2d::Aabb2 MineApp::sandbox_panel_area() const {
    const f32 width = static_cast<f32>(renderer().width());
    const t2d::Aabb2 status = sandbox_status_area();
    const f32 panel_width = std::min(width * 0.36f, 300.0f * unit_);
    return t2d::Aabb2{t2d::Vec2{width - panel_width, 0.0f}, t2d::Vec2{width, status.min.y}};
}

t2d::Aabb2 MineApp::sandbox_grid_area() const {
    const t2d::Aabb2 panel = sandbox_panel_area();
    const t2d::Aabb2 status = sandbox_status_area();
    // Anchored at the top left corner so the area's max is also the viewport size the camera expects.
    return t2d::Aabb2{t2d::Vec2{0.0f, 0.0f},
                      t2d::Vec2{std::max(1.0f, panel.min.x), std::max(1.0f, status.min.y)}};
}

void MineApp::fit_sandbox_view() {
    const t2d::Aabb2 area = sandbox_grid_area();
    const f32 width = std::max(1.0f, area.max.x - area.min.x);
    const f32 height = std::max(1.0f, area.max.y - area.min.y);
    const f32 cell = std::min(width / static_cast<f32>(sandbox_.width()),
                              height / static_cast<f32>(sandbox_.height()));
    sandbox_.set_cell_px(std::floor(cell));
    sandbox_.center_view(t2d::Vec2{width, height});
    const t2d::Vec2 centered = sandbox_.origin();
    sandbox_.set_origin(t2d::Vec2{centered.x + area.min.x, centered.y + area.min.y});
}

void MineApp::open_sandbox() {
    // Coming back from the start screen keeps the layer that was painted: a reload re-points every
    // cell by name anyway, so there is nothing to lose by staying.
    if (sandbox_.width() != options_.grid_width || sandbox_.height() != options_.grid_height ||
        sandbox_.layer_count() != std::clamp(options_.tile_layers, 1, SandboxModel::kMaxLayers)) {
        sandbox_ = SandboxModel(options_.grid_width, options_.grid_height, options_.tile_layers);
    }
    sandbox_.set_active_layer(options_.start_layer);
    sandbox_.set_content_paths(options_.content_paths);
    if (options_.content_paths.empty()) {
        sandbox_.rebuild_palette(registry_);
        set_status(locale_.text("sandbox.no.content"), true);
    } else {
        reload_content();
    }
    // A debug fill first, then the layout file: a map the designer saved on purpose must not be
    // overwritten by a fill they asked for to see the palette.
    apply_fill();
    if (!options_.layout_path.empty()) load_layout();
    fit_sandbox_view();
    T2D_INFO("sandbox: {}x{} map, {} tile layer(s), {} palette entries, {} cells filled", sandbox_.width(),
             sandbox_.height(), sandbox_.layer_count(), sandbox_.palette_count(), sandbox_.filled_cells());
}

void MineApp::apply_fill() {
    if (options_.fill.empty() || options_.fill == "none") return;
    if (options_.fill != "bands" && options_.fill != "scatter") {
        set_status(format_localized(locale_.text("sandbox.fill.unknown"), options_.fill), true);
        return;
    }
    // Which layers the fill writes into: the active one by default, all of them when a designer wants
    // to see how a whole stack composes. Every layer of an "all" scatter gets its own seed, so the
    // layers are distinguishable instead of three copies of the same picture.
    i32 first = sandbox_.active_layer();
    i32 last = first;
    if (options_.fill_layer == "all") {
        first = 0;
        last = sandbox_.layer_count() - 1;
    } else if (!options_.fill_layer.empty()) {
        const unsigned long parsed = std::strtoul(options_.fill_layer.c_str(), nullptr, 10);
        first = last = static_cast<i32>(parsed);
    }
    for (i32 layer = first; layer <= last; ++layer) {
        sandbox_.set_active_layer(layer);
        if (options_.fill == "bands") sandbox_.fill_bands();
        else sandbox_.fill_scatter(session_.seed + static_cast<u64>(layer));
    }
    sandbox_.set_active_layer(options_.start_layer);
}

void MineApp::reload_content() {
    const SandboxReloadReport report = sandbox_.reload(registry_, options_.content_paths);
    if (!report.parsed) {
        const std::string first = report.errors.empty() ? std::string("-") : report.errors.front();
        set_status(format_localized(locale_.text("sandbox.reload.failed"), first), true);
        return;
    }
    std::string message = format_localized(locale_.text("sandbox.reloaded"), report.added, report.removed,
                                           report.remapped_cells, report.lost_cells);
    if (!report.unknown_tables.empty()) {
        message += format_localized(locale_.text("sandbox.reload.unknown"), report.unknown_tables.size(),
                                    report.unknown_tables.front());
    }
    set_status(std::move(message));
}

void MineApp::save_layout() {
    if (options_.layout_path.empty()) {
        set_status(locale_.text("sandbox.no.path"), true);
        return;
    }
    const std::vector<u8> bytes = sandbox_.serialize(registry_);
    if (!write_binary_file(options_.layout_path, t2d::ConstSpan<const u8>(bytes.data(), bytes.size()))) {
        set_status(format_localized(locale_.text("sandbox.save.failed"), options_.layout_path), true);
        return;
    }
    set_status(format_localized(locale_.text("sandbox.saved"), bytes.size(), options_.layout_path));
}

void MineApp::load_layout() {
    if (options_.layout_path.empty()) {
        set_status(locale_.text("sandbox.no.path"), true);
        return;
    }
    const std::optional<std::vector<u8>> bytes = t2d::read_binary(options_.layout_path);
    if (!bytes.has_value()) {
        set_status(format_localized(locale_.text("sandbox.load.failed"), options_.layout_path), true);
        return;
    }
    const SandboxLoadReport report =
        sandbox_.deserialize(t2d::ConstSpan<const u8>(bytes->data(), bytes->size()), registry_);
    if (!report.ok) {
        set_status(format_localized(locale_.text("sandbox.load.failed"), report.error), true);
        return;
    }
    set_status(format_localized(locale_.text("sandbox.loaded"), report.translated, report.missing));
}

void MineApp::dump_layer() {
    T2D_INFO("sandbox dump:\n{}", sandbox_.dump_text());
    set_status(format_localized(locale_.text("sandbox.dumped"), sandbox_.filled_cells()));
}

void MineApp::handle_session_input() {
    const ore::Window* window = this->window();
    if (window == nullptr) return;
    if (window->input().key_pressed(ore::Key::Escape)) screen_ = Screen::Start;
}

void MineApp::handle_sandbox_input() {
    const ore::Window* window = this->window();
    if (window == nullptr) return;
    const ore::InputState& input = window->input();
    const t2d::Aabb2 area = sandbox_grid_area();
    const t2d::Vec2 viewport{area.max.x, area.max.y};
    const t2d::Vec2 centre{area.max.x * 0.5f, area.max.y * 0.5f};
    const t2d::Vec2 mouse{input.mouse_x(), input.mouse_y()};
    const bool over_grid = mouse.x >= area.min.x && mouse.x < area.max.x && mouse.y >= area.min.y &&
                           mouse.y < area.max.y;

    if (input.key_pressed(ore::Key::Escape)) {
        screen_ = Screen::Start;
        return;
    }
    if (input.key_pressed(ore::Key::F5)) reload_content();
    if (input.key_pressed(ore::Key::F2)) save_layout();
    if (input.key_pressed(ore::Key::F3)) load_layout();
    if (input.key_pressed(ore::Key::F4)) dump_layer();
    if (input.key_pressed(ore::Key::C)) fit_sandbox_view();

    const auto step = [&](ore::Key key, i32 dx, i32 dy) {
        if (!input.key_pressed(key)) return;
        sandbox_.move_cursor(dx, dy);
        sandbox_.scroll_to_show(sandbox_.cursor(), viewport);
    };
    step(ore::Key::Left, -1, 0);
    step(ore::Key::A, -1, 0);
    step(ore::Key::Right, 1, 0);
    step(ore::Key::D, 1, 0);
    step(ore::Key::Up, 0, -1);
    step(ore::Key::W, 0, -1);
    step(ore::Key::Down, 0, 1);
    step(ore::Key::S, 0, 1);
    if (input.key_pressed(ore::Key::Space) || input.key_pressed(ore::Key::Enter)) sandbox_.paint();
    if (input.key_pressed(ore::Key::X) || input.key_pressed(ore::Key::Backspace) ||
        input.key_pressed(ore::Key::Delete)) {
        sandbox_.erase();
    }
    if (input.key_pressed(ore::Key::Tab)) sandbox_.cycle_palette(input.shift_down() ? -1 : 1);
    // Which tile layer the brush writes into: the stack is walked with [ ] or PageUp/PageDown.
    if (input.key_pressed(ore::Key::LeftBracket) || input.key_pressed(ore::Key::PageDown)) sandbox_.cycle_layer(-1);
    if (input.key_pressed(ore::Key::RightBracket) || input.key_pressed(ore::Key::PageUp)) sandbox_.cycle_layer(1);
    if (input.key_pressed(ore::Key::Equal) || input.key_pressed(ore::Key::KpAdd)) sandbox_.zoom_at(centre, 1.25f);
    if (input.key_pressed(ore::Key::Minus) || input.key_pressed(ore::Key::KpSubtract)) {
        sandbox_.zoom_at(centre, 0.8f);
    }

    // The mouse inspects and paints. It only takes the cursor over when it actually moved, so a mouse
    // resting over the layer does not fight the arrow keys.
    const bool moved = input.mouse_delta_x() != 0.0f || input.mouse_delta_y() != 0.0f;
    const bool painting = input.mouse_down(ore::MouseButton::Left);
    const bool erasing = input.mouse_down(ore::MouseButton::Right);
    if (over_grid && (moved || painting || erasing)) {
        const GridPos under = sandbox_.cell_at_screen(mouse);
        if (sandbox_.inside(under)) {
            sandbox_.set_cursor(under);
            if (painting) sandbox_.paint();
            if (erasing) sandbox_.erase();
        }
    }
    if (input.mouse_down(ore::MouseButton::Middle)) {
        sandbox_.pan(t2d::Vec2{input.mouse_delta_x(), input.mouse_delta_y()});
    }
    if (input.scroll_y() != 0.0f) {
        sandbox_.zoom_at(mouse, input.scroll_y() > 0.0f ? 1.15f : 1.0f / 1.15f);
    }
}

void MineApp::draw_sandbox_screen() {
    const t2d::Aabb2 area = sandbox_grid_area();
    const t2d::Aabb2 panel = sandbox_panel_area();
    const t2d::Aabb2 status = sandbox_status_area();
    const f32 padding = 8.0f * unit_;
    const f32 line = static_cast<f32>(body_px_) * 1.45f;
    const f32 cell = sandbox_.cell_px();

    // --- the layer ---
    batch_->draw_rect(area, kPalette.grid_background);
    const GridPos first = sandbox_.cell_at_screen(t2d::Vec2{area.min.x, area.min.y});
    const GridPos last = sandbox_.cell_at_screen(t2d::Vec2{area.max.x - 1.0f, area.max.y - 1.0f});
    const i32 x0 = std::max(first.x, 0);
    const i32 y0 = std::max(first.y, 0);
    const i32 x1 = std::min(last.x, static_cast<i32>(sandbox_.width()) - 1);
    const i32 y1 = std::min(last.y, static_cast<i32>(sandbox_.height()) - 1);

    // A label budget: a layer full of content would otherwise spend every quad the batch has on text.
    constexpr usize kMaxLabels = 320;
    usize labels = 0;
    // Bottom to top, so a higher tile layer covers a lower one. The layer the brush writes into is
    // drawn at full strength with its labels, the others dimmed: that is what makes the stack read as
    // a stack instead of a pile of unrelated colours.
    for (i32 layer = 0; layer < sandbox_.layer_count(); ++layer) {
        const bool active = layer == sandbox_.active_layer();
        for (i32 y = y0; y <= y1; ++y) {
            for (i32 x = x0; x <= x1; ++x) {
                const GridPos pos{x, y};
                const SandboxCell& value = sandbox_.cell(layer, pos);
                if (value.empty()) continue;
                const t2d::Vec2 at = sandbox_.screen_of_cell(pos);
                const t2d::Aabb2 rect{t2d::Vec2{at.x, at.y}, t2d::Vec2{at.x + cell, at.y + cell}};
                const u32 colour = debug_color_for(value.name);
                batch_->draw_rect(rect, dim_color(colour, active ? 0.45f : 0.20f));
                if (value.missing()) batch_->draw_rect_outline(rect, 2.0f, kPalette.missing);
                if (!active || cell < 12.0f || labels >= kMaxLabels) continue;
                t2d::TextStyle style;
                style.size_px = static_cast<u16>(std::clamp(cell * 0.5f, 9.0f, 16.0f));
                style.color = value.missing() ? kPalette.missing : colour;
                // The name when it fits, otherwise the id: on a small cell the number a save would
                // store is the more useful label anyway.
                std::string label = value.name;
                if (text_->measure(label, fonts_, style).width > cell - 4.0f) {
                    label = std::format("#{}", value.id);
                    if (text_->measure(label, fonts_, style).width > cell - 4.0f) continue;
                }
                const f32 text_height = static_cast<f32>(style.size_px) * 1.2f;
                (void)text_->draw(*batch_, label, fonts_, style,
                                  t2d::Vec2{at.x + 2.0f, at.y + (cell - text_height) * 0.5f});
                ++labels;
            }
        }
    }

    // Grid lines span the whole area: one rect per column and per row instead of four per cell.
    for (i32 x = x0; x <= x1 + 1; ++x) {
        const f32 sx = sandbox_.screen_of_cell(GridPos{x, 0}).x;
        batch_->draw_rect(t2d::Aabb2{t2d::Vec2{sx, area.min.y}, t2d::Vec2{sx + 1.0f, area.max.y}},
                          kPalette.grid_line);
    }
    for (i32 y = y0; y <= y1 + 1; ++y) {
        const f32 sy = sandbox_.screen_of_cell(GridPos{0, y}).y;
        batch_->draw_rect(t2d::Aabb2{t2d::Vec2{area.min.x, sy}, t2d::Vec2{area.max.x, sy + 1.0f}},
                          kPalette.grid_line);
    }
    const t2d::Vec2 layer_at = sandbox_.screen_of_cell(GridPos{0, 0});
    batch_->draw_rect_outline(t2d::Aabb2{t2d::Vec2{layer_at.x, layer_at.y},
                                         t2d::Vec2{layer_at.x + static_cast<f32>(sandbox_.width()) * cell,
                                                   layer_at.y + static_cast<f32>(sandbox_.height()) * cell}},
                              2.0f, kPalette.grid_edge);

    const t2d::Vec2 cursor_at = sandbox_.screen_of_cell(sandbox_.cursor());
    batch_->draw_rect_outline(t2d::Aabb2{t2d::Vec2{cursor_at.x, cursor_at.y},
                                         t2d::Vec2{cursor_at.x + cell, cursor_at.y + cell}},
                              2.0f, kPalette.accent);

    // --- the palette: the designer's registered content, with the ids a save would store ---
    batch_->draw_rect(panel, kPalette.panel_fill);
    batch_->draw_rect_outline(panel, 2.0f, kPalette.panel_edge);
    const f32 panel_left = panel.min.x + padding;
    const f32 panel_right = panel.max.x - padding;
    f32 y = panel.min.y + padding;
    y += draw_line(panel_left, y, static_cast<u16>(body_px_ * 1.15f), kPalette.accent,
                   locale_.text("sandbox.palette")) + 4.0f;
    batch_->draw_rect(t2d::Aabb2{t2d::Vec2{panel_left, y}, t2d::Vec2{panel_right, y + 2.0f}},
                      kPalette.panel_edge);
    y += 8.0f;

    t2d::TextStyle measure_style;
    measure_style.size_px = body_px_;
    if (sandbox_.palette_count() == 0) {
        draw_line(panel_left, y, body_px_, kPalette.warning, locale_.text("sandbox.palette.empty"));
        draw_line(panel_left, y + line, body_px_, kPalette.text_dim, locale_.text("sandbox.palette.hint"));
    } else {
        const usize rows = static_cast<usize>(std::max(0.0f, (panel.max.y - padding - y) / line));
        const usize start = sandbox_.palette_window_start(rows);
        for (usize index = start; index < sandbox_.palette_count() && index < start + rows; ++index) {
            const PaletteEntry& entry = sandbox_.palette(index);
            const bool focused = index == sandbox_.selected_palette();
            if (focused) {
                batch_->draw_rect(t2d::Aabb2{t2d::Vec2{panel_left - 4.0f, y - 1.0f},
                                             t2d::Vec2{panel_right, y + line - 4.0f}},
                                  kPalette.highlight);
            }
            batch_->draw_rect(t2d::Aabb2{t2d::Vec2{panel_left, y + 2.0f},
                                         t2d::Vec2{panel_left + 10.0f, y + line - 6.0f}},
                              debug_color_for(entry.name));
            const std::string kind = std::format("{} #{}", content_kind_name(entry.kind), entry.id);
            const f32 kind_width = text_->measure(kind, fonts_, measure_style).width;
            const f32 name_x = panel_left + 16.0f;
            draw_fitted(name_x, y, body_px_, focused ? kPalette.accent : kPalette.text, entry.name,
                        std::max(8.0f, panel_right - kind_width - 8.0f - name_x));
            draw_line(panel_right - kind_width, y, body_px_, kPalette.text_dim, kind);
            y += line;
        }
    }

    // --- the status bar ---
    batch_->draw_rect(status, kPalette.panel_fill);
    batch_->draw_rect(t2d::Aabb2{t2d::Vec2{status.min.x, status.min.y}, t2d::Vec2{status.max.x, status.min.y + 2.0f}},
                      kPalette.panel_edge);

    f32 label_width = 0.0f;
    for (const char* id : {"sandbox.cursor", "sandbox.cell", "sandbox.grid", "sandbox.filled", "sandbox.content",
                           "sandbox.layout", "sandbox.layer", "sandbox.layer.filled"}) {
        label_width = std::max(label_width, text_->measure(locale_.text(id), fonts_, measure_style).width);
    }
    const f32 left = status.min.x + padding;
    const f32 middle = status.min.x + (status.max.x - status.min.x) * 0.5f;
    const f32 value_column = left + label_width + 2.0f * unit_;
    const f32 middle_column = middle + label_width + 2.0f * unit_;

    // The cell is reported as the whole stack at the cursor, topmost first: on a multi layer map
    // "what is here" is a list, not one tile.
    const SandboxCell& under = sandbox_.cell(sandbox_.cursor());
    std::string cell_text;
    for (i32 layer = sandbox_.layer_count() - 1; layer >= 0; --layer) {
        const SandboxCell& value = sandbox_.cell(layer, sandbox_.cursor());
        if (value.empty()) continue;
        if (!cell_text.empty()) cell_text += " | ";
        cell_text += std::format("L{} {} #{} {}", layer, content_kind_name(value.kind), value.id, value.name);
        if (value.missing()) cell_text += std::string(" ") + std::string(locale_.text("sandbox.cell.missing"));
    }
    if (cell_text.empty()) cell_text = locale_.text("sandbox.cell.empty");
    std::string content_text{locale_.text("sandbox.content.none")};
    if (!sandbox_.content_paths().empty()) {
        content_text.clear();
        for (const std::string& path : sandbox_.content_paths()) {
            if (!content_text.empty()) content_text += ", ";
            content_text += path;
        }
    }
    const std::string layout_text =
        options_.layout_path.empty() ? std::string(locale_.text("sandbox.layout.none")) : options_.layout_path;

    // A long path or a long content name is clipped at the column boundary: wrapping it would land
    // on the line below and make the status bar unreadable exactly when something went wrong.
    const f32 left_width = std::max(16.0f, middle - value_column - 8.0f);
    const f32 right_width = std::max(16.0f, status.max.x - padding - middle_column);
    const f32 full_width = status.max.x - 2.0f * padding;

    y = status.min.y + padding;
    draw_pair(left, y, value_column, body_px_, locale_.text("sandbox.cursor"),
              std::format("{},{}", sandbox_.cursor().x, sandbox_.cursor().y), kPalette.text_dim, kPalette.text,
              left_width);
    draw_pair(middle, y, middle_column, body_px_, locale_.text("sandbox.cell"), cell_text, kPalette.text_dim,
              under.missing() ? kPalette.missing : kPalette.text, right_width);
    y += line;
    draw_pair(left, y, value_column, body_px_, locale_.text("sandbox.grid"),
              std::format("{}x{}", sandbox_.width(), sandbox_.height()), kPalette.text_dim, kPalette.text,
              left_width);
    draw_pair(middle, y, middle_column, body_px_, locale_.text("sandbox.filled"),
              std::format("{} / {}", sandbox_.filled_cells(),
                          static_cast<usize>(sandbox_.width()) * sandbox_.height() *
                              static_cast<usize>(sandbox_.layer_count())),
              kPalette.text_dim, kPalette.text, right_width);
    y += line;
    draw_pair(left, y, value_column, body_px_, locale_.text("sandbox.layer"),
              std::format("{} / {}", sandbox_.active_layer() + 1, sandbox_.layer_count()), kPalette.text_dim,
              kPalette.accent, left_width);
    draw_pair(middle, y, middle_column, body_px_, locale_.text("sandbox.layer.filled"),
              std::format("{} / {}", sandbox_.filled_cells(sandbox_.active_layer()),
                          static_cast<usize>(sandbox_.width()) * sandbox_.height()),
              kPalette.text_dim, kPalette.text, right_width);
    y += line;
    draw_pair(left, y, value_column, body_px_, locale_.text("sandbox.content"), content_text, kPalette.text_dim,
              kPalette.text, left_width);
    draw_pair(middle, y, middle_column, body_px_, locale_.text("sandbox.layout"), layout_text, kPalette.text_dim,
              kPalette.text, right_width);
    y += line + 4.0f;
    if (!status_.empty()) {
        draw_fitted(left, y, body_px_, status_is_error_ ? kPalette.error : kPalette.warning, status_, full_width);
    }
    y += line;
    draw_fitted(left, y, body_px_, kPalette.text_dim, locale_.text("sandbox.hint.keys"), full_width);
    draw_fitted(left, y + line, body_px_, kPalette.text_dim, locale_.text("sandbox.hint.keys2"), full_width);
}

void MineApp::on_resize(u32 width, u32 height) {
    (void)width;
    (void)height;
    // The layer is fitted to the grid area, so a resize is a refit.
    if (screen_ == Screen::Sandbox && batch_ != nullptr) fit_sandbox_view();
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
    switch (screen_) {
        case Screen::Start: draw_start_screen(); break;
        case Screen::Session: draw_session_screen(); break;
        case Screen::Sandbox: draw_sandbox_screen(); break;
    }
    batch_->end();

    frame.counters.draw_calls = batch_->draw_calls();
    frame.counters.triangles = batch_->quads() * 2;
    renderer().end_pass();
}

void MineApp::on_shutdown() {
    // A scripted run has no keyboard to press F2 or F4 on, so both can happen on the way out.
    if (options_.dump_layer && options_.session.mode == Mode::Sandbox) {
        T2D_INFO("sandbox dump:\n{}", sandbox_.dump_text());
    }
    if (!options_.save_layout_path.empty() && options_.session.mode == Mode::Sandbox) {
        const std::vector<u8> bytes = sandbox_.serialize(registry_);
        if (write_binary_file(options_.save_layout_path, t2d::ConstSpan<const u8>(bytes.data(), bytes.size()))) {
            T2D_INFO("sandbox: wrote {} bytes to '{}'", bytes.size(), options_.save_layout_path);
        } else {
            T2D_WARN("sandbox: could not write '{}'", options_.save_layout_path);
        }
    }
    batch_.reset();
    text_.reset();
    cjk_font_.reset();
    latin_font_.reset();
    sampler_.reset();
}

} // namespace mine
