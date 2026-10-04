#include <mine/app.h>

#include <t2d/core/cli.h>
#include <t2d/core/log.h>
#include <t2d/core/time.h>

#include <ore/core/image.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
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

/// The content files the game itself ships, in file name order. They are the game's own content - the
/// first stage of the load order (docs/MODS.md) - so a run does not have to be told about them. A
/// missing directory is reported rather than silently leaving the game empty.
[[nodiscard]] std::vector<std::string> shipped_content_files() {
    const std::string directory = std::string(T2D_SOURCE_DIR) + "/games/mine/content";
    std::error_code code;
    if (!std::filesystem::is_directory(directory, code)) {
        T2D_WARN("content: the game's own content directory '{}' is missing", directory);
        return {};
    }
    return ContentPack::scan_directory(directory);
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

/// Where a content entry's picture lives in the image atlas. Kind and name together, because two
/// kinds may use the same name.
[[nodiscard]] std::string image_key(ContentKind kind, std::string_view name) {
    return std::format("{}:{}", content_kind_name(kind), name);
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
    // A sandbox frame draws the map in two passes (colours, then pictures), each of which can hold up
    // to kMaxCellQuads quads of 20 bytes: 4 MiB of ring is what keeps a zoomed out map of hundreds of
    // cells per side inside one frame instead of being dropped.
    config.upload_segment_size = 4ull << 20;
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
        {"pack", "<file.ecfg>", "a content pack to load; repeatable"},
        {"packs", "<dir>", "directory of content packs (*.ecfg); repeatable, defaults to ./packs"},
        {"mods", "<dir>", "directory of mod packages to load; repeatable"},
        {"view", "<x,y[,zoom]>", "sandbox: look at this cell instead of fitting the whole map"},
        {"layout", "<path>", "sandbox layout file: loaded at startup, written by F2"},
        {"save-layout", "<path>", "write the sandbox layout once at shutdown (scripted runs)"},
        {"dump-layer", "<0|1>", "write the sandbox layer as text to the log at shutdown"},
        {"playtest", "<0|1>", "start the sandbox in the playtest: the layer as the game draws it"},
        {"pointer", "<x,y>", "playtest: put the pointer on this cell (a scripted run has no mouse)"},
        {"content-list", "<0|1>", "open the list of loaded content packs and mods at startup"},
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
    shipped_content_ = shipped_content_files();
    // The game's content, its packs and its mods, loaded once at startup: a session reports what it
    // has before anything is created, and the sandbox reloads the same set with F5.
    (void)load_content();
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
    // The map pass alone can want 16k quads (sandbox_draw_range), plus grid lines, labels and the
    // panel, so the batch is sized for the frame rather than for a screen of text.
    batch_options.max_quads = 32768;
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
    // Pack art gets its own sampler: nearest, so a picture drawn on a grid stays crisp and the atlas
    // cells cannot bleed into each other through a linear filter.
    ore::rhi::SamplerDesc image_sampler_desc;
    image_sampler_desc.min_filter = VK_FILTER_NEAREST;
    image_sampler_desc.mag_filter = VK_FILTER_NEAREST;
    image_sampler_desc.mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    image_sampler_ = ore::rhi::Sampler::create(context.device(), image_sampler_desc);
    if (screen_ == Screen::Sandbox) open_sandbox();
    // The mine, when a scripted run asks to be taken straight into it: the session screen's Enter is
    // the same call.
    if (options_.world_view) open_world();
    // The list is opened last: it reports what the load above actually put in the registry, and a
    // scripted run has no keyboard to press the key that opens it on.
    if (options_.content_list) open_content_list(screen_ == Screen::Sandbox ? Screen::Sandbox : Screen::Start);
}

void MineApp::on_update(f32 delta_seconds) {
    switch (screen_) {
        case Screen::Start: handle_start_input(); break;
        case Screen::Session: handle_session_input(); break;
        case Screen::Sandbox: handle_sandbox_input(delta_seconds); break;
        case Screen::Content: handle_content_input(); break;
        case Screen::World: handle_world_input(delta_seconds); break;
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
    // The input state comes from the application rather than from a window: a headless run has one
    // too, and the debug input server (ore/debug/input_server.h) feeds exactly this state, which is
    // what lets a scripted run drive the interface with no window and no keyboard.
    const ore::InputState& input = this->input();

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
    if (action == MenuAction::OpenContent) {
        open_content_list(Screen::Start);
        return;
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
    return y + line_for(size_px);
}

f32 MineApp::line_for(u16 size_px) const {
    t2d::TextStyle style;
    style.size_px = size_px;
    return std::max(static_cast<f32>(size_px) * 1.45f, t2d::line_box(fonts_, style).height);
}

void MineApp::draw_start_screen() {
    const f32 width = static_cast<f32>(renderer().width());
    const f32 height = static_cast<f32>(renderer().height());
    const f32 padding = 8.0f * unit_;
    const u16 title_px = static_cast<u16>(body_px_ * 2);
    const f32 row_line = line_for(body_px_);
    const f32 title_line = line_for(title_px);

    // The panel is as tall as what goes in it. A fixed height was tuned against text that was drawn
    // one ascent above its pen, and the same numbers now put the title outside the panel.
    constexpr f32 kAfterTitle = 2.0f;
    constexpr f32 kAfterSubtitle = 6.0f;
    constexpr f32 kRule = 2.0f;
    constexpr f32 kAfterRule = 10.0f;
    constexpr f32 kBeforeSeed = 4.0f;
    constexpr f32 kBeforeHints = 6.0f;
    constexpr f32 kBetweenHints = 2.0f;
    const f32 rows_height = static_cast<f32>(menu_.row_count()) * row_line;
    const f32 seed_height = menu_.seed_visible() ? kBeforeSeed + row_line : 0.0f;
    const f32 content = title_line + kAfterTitle + row_line + kAfterSubtitle + kRule + kAfterRule + rows_height +
                        seed_height + kBeforeHints + row_line * 2.0f + kBetweenHints;

    const f32 panel_width = std::min(width - 4.0f * padding, 330.0f * unit_);
    const f32 panel_height = std::min(height - 4.0f * padding, content + 2.0f * padding);
    const t2d::Aabb2 panel = t2d::Aabb2::from_center(t2d::Vec2{width * 0.5f, height * 0.5f},
                                                     t2d::Vec2{panel_width * 0.5f, panel_height * 0.5f});
    const f32 left = panel.min.x + padding;
    const f32 right = panel.max.x - padding;

    batch_->draw_rect(panel, kPalette.panel_fill);
    batch_->draw_rect_outline(panel, 2.0f, kPalette.panel_edge);

    f32 y = panel.min.y + padding;
    y += draw_line(left, y, title_px, kPalette.accent, locale_.text("title")) + kAfterTitle;
    y += draw_line(left, y, body_px_, kPalette.text_dim, locale_.text("subtitle")) + kAfterSubtitle;
    batch_->draw_rect(t2d::Aabb2{t2d::Vec2{left, y}, t2d::Vec2{right, y + kRule}}, kPalette.panel_edge);
    y += kAfterRule;

    // The value column follows the widest label instead of a fixed offset: "WORLD" and "世界" are very
    // different widths, and a fixed column leaves one of them stranded.
    f32 label_width = 0.0f;
    t2d::TextStyle measure_style;
    measure_style.size_px = body_px_;
    for (usize index = 0; index < menu_.row_count(); ++index) {
        label_width = std::max(label_width, text_->measure(locale_.text(menu_.row(index).id), fonts_, measure_style).width);
    }
    const f32 value_column = left + label_width + 3.0f * unit_;
    for (usize index = 0; index < menu_.row_count(); ++index) {
        const MenuRow& row = menu_.row(index);
        const bool focused = index == menu_.selected();
        if (focused) {
            // The row's background is the row's own line box: the pen is its top left corner, so the
            // text drawn at the same y sits inside it instead of hanging above it.
            batch_->draw_rect(t2d::Aabb2{t2d::Vec2{left - 4.0f, y}, t2d::Vec2{right, y + row_line}},
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
            case RowKind::Content: {
                const std::string value = format_localized(locale_.text("row.content.value"),
                                                           content_.report().packs, content_.report().mods);
                draw_line(value_column, y, body_px_, color, value);
                // The row has no left/right, so the only thing to say about it is that it opens.
                if (focused) {
                    t2d::TextStyle value_style;
                    value_style.size_px = body_px_;
                    draw_line(value_column + text_->measure(value, fonts_, value_style).width + 3.0f * unit_, y,
                              body_px_, kPalette.text_dim, "< ENTER >");
                }
                break;
            }
            case RowKind::Action:
                if (focused) draw_line(value_column, y, body_px_, kPalette.text_dim, "< ENTER >");
                break;
        }
        y += row_line;
    }

    if (menu_.seed_visible()) {
        y += kBeforeSeed;
        draw_line(left, y, body_px_, kPalette.text_dim,
                  format_localized(locale_.text("seed.hint"), menu_.seed()));
        y += row_line;
    }

    // The hints follow the rows instead of being pinned to the panel's bottom edge: the panel is
    // sized to its content, so there is nothing left to pin them to.
    y += kBeforeHints;
    y += draw_line(left, y, body_px_, kPalette.text_dim, locale_.text("hint.rows")) + kBetweenHints;
    draw_line(left, y, body_px_, kPalette.text_dim, locale_.text("hint.actions"));
}

void MineApp::draw_session_screen() {
    const f32 width = static_cast<f32>(renderer().width());
    const f32 height = static_cast<f32>(renderer().height());
    const f32 padding = 8.0f * unit_;
    const u16 title_px = static_cast<u16>(body_px_ * 1.5f);
    const f32 row_line = line_for(body_px_);

    // The panel is as tall as its content, like the start screen's: five rows (six when the session
    // connects to a server), the rule under the title, the three lines that say what is still
    // missing, and the way back.
    constexpr f32 kRule = 2.0f;
    constexpr f32 kAfterTitle = 0.0f;
    constexpr f32 kAfterRule = 10.0f;
    constexpr f32 kBeforeRule = 6.0f;
    constexpr f32 kBetweenPending = 2.0f;
    constexpr f32 kBeforeBack = 6.0f;
    const f32 rows_height = (session_.role == Role::Join ? 6.0f : 5.0f) * row_line;
    const f32 content = line_for(title_px) + kAfterTitle + kRule + kAfterRule + rows_height + kBeforeRule + kRule +
                        kAfterRule + row_line * 3.0f + kBetweenPending + kBeforeBack + row_line * 2.0f;
    const f32 panel_width = std::min(width - 4.0f * padding, 330.0f * unit_);
    const f32 panel_height = std::min(height - 4.0f * padding, content + 2.0f * padding);
    const t2d::Aabb2 panel = t2d::Aabb2::from_center(t2d::Vec2{width * 0.5f, height * 0.5f},
                                                     t2d::Vec2{panel_width * 0.5f, panel_height * 0.5f});
    const f32 left = panel.min.x + padding;
    const f32 right = panel.max.x - padding;

    batch_->draw_rect(panel, kPalette.panel_fill);
    batch_->draw_rect_outline(panel, 2.0f, kPalette.panel_edge);

    f32 y = panel.min.y + padding;
    y += draw_line(left, y, title_px, kPalette.accent, locale_.text("session.title")) + kAfterTitle;
    batch_->draw_rect(t2d::Aabb2{t2d::Vec2{left, y}, t2d::Vec2{right, y + kRule}}, kPalette.panel_edge);
    y += kAfterRule;

    f32 label_width = 0.0f;
    t2d::TextStyle measure_style;
    measure_style.size_px = body_px_;
    for (const char* id : {"session.world", "session.content", "session.packs", "session.role", "session.seed",
                           "session.connect"}) {
        label_width = std::max(label_width, text_->measure(locale_.text(id), fonts_, measure_style).width);
    }
    const f32 value_column = left + label_width + 3.0f * unit_;
    y = draw_pair(left, y, value_column, body_px_, locale_.text("session.world"),
                  locale_.text(session_.mode == Mode::Endless ? "value.endless" : "value.story"), kPalette.text_dim,
                  kPalette.text);
    y = draw_pair(left, y, value_column, body_px_, locale_.text("session.content"),
                  format_localized(locale_.text("content.registered"), registry_.total_count()),
                  kPalette.text_dim, kPalette.text);
    y = draw_pair(left, y, value_column, body_px_, locale_.text("session.packs"),
                  format_localized(locale_.text("session.packs.value"), content_.report().packs,
                                   content_.report().pack_content),
                  kPalette.text_dim, content_.report().clean() ? kPalette.text : kPalette.error);
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

    y += kBeforeRule;
    batch_->draw_rect(t2d::Aabb2{t2d::Vec2{left, y}, t2d::Vec2{right, y + kRule}}, kPalette.panel_edge);
    y += kAfterRule;
    y += draw_line(left, y, body_px_, kPalette.warning, locale_.text("session.pending")) + kBetweenPending;
    y += draw_line(left, y, body_px_, kPalette.text_dim, locale_.text("session.pending.line1"));
    y += draw_line(left, y, body_px_, kPalette.text_dim, locale_.text("session.pending.line2"));

    y += kBeforeBack;
    // Entering the mine is what a session is for: what is entered is the layer the world's generator
    // describes, which is empty until the designer's layer rules arrive (docs/GAME_DESIGN.md 7.3).
    y += draw_line(left, y, body_px_, kPalette.accent, locale_.text("session.enter"));
    draw_line(left, y, body_px_, kPalette.text_dim, locale_.text("session.back"));
}

// --- the content list --------------------------------------------------------------------------

void MineApp::refresh_content_list() {
    const ContentPipelineReport& loaded = content_.report();
    content_list_.set_sources(loaded.sources, loaded.errors, loaded.warnings);
}

void MineApp::open_content_list(Screen from) {
    return_screen_ = from;
    refresh_content_list();
    screen_ = Screen::Content;
    const ContentListTotals totals = content_list_.totals();
    T2D_INFO("content list: {} source(s), {} content, {} failed, {} message(s)", totals.sources, totals.entries,
             totals.failed, content_list_.issues().size());
}

MineApp::ContentLayout MineApp::content_layout() const {
    const f32 width = static_cast<f32>(renderer().width());
    const f32 height = static_cast<f32>(renderer().height());
    const f32 padding = 8.0f * unit_;
    const f32 line = line_for(body_px_);
    const u16 title_px = static_cast<u16>(body_px_ * 1.3f);
    // More messages than this and the rest are counted on one more line: the log has all of them, and
    // a panel that grows until the list has no room left is worse than a count.
    constexpr usize kMaxIssueLines = 3;

    ContentLayout layout;
    const f32 panel_width = std::min(width - 4.0f * padding, 470.0f * unit_);
    layout.panel = t2d::Aabb2::from_center(t2d::Vec2{width * 0.5f, height * 0.5f},
                                           t2d::Vec2{panel_width * 0.5f, 0.5f});
    layout.left = layout.panel.min.x + padding;
    layout.right = layout.panel.max.x - padding;

    // Two columns are as wide as their widest label in the language in force: "REQUIRES" and "依赖"
    // are very different widths, and a fixed column leaves one of them stranded.
    t2d::TextStyle style;
    style.size_px = body_px_;
    for (const char* id : {"content.kind.file", "content.kind.pack", "content.kind.mod"}) {
        layout.badge_width = std::max(layout.badge_width, text_->measure(locale_.text(id), fonts_, style).width);
    }
    for (const char* id : {"content.status.ok", "content.status.partial", "content.status.failed"}) {
        layout.status_width = std::max(layout.status_width, text_->measure(locale_.text(id), fonts_, style).width);
    }
    f32 label_width = 0.0f;
    for (const char* id : {"content.path", "content.name", "content.requires", "content.error"}) {
        label_width = std::max(label_width, text_->measure(locale_.text(id), fonts_, style).width);
    }
    layout.detail_column = layout.left + label_width + 2.0f * unit_;

    // The header: the title, the summary line, and the rule under them. What sits under the list: the
    // selected source (two lines) and the load's messages.
    const f32 header_height = line_for(title_px) + 4.0f + line + 8.0f;
    const usize issue_count = content_list_.issues().size();
    layout.issues_shown = std::min(issue_count, kMaxIssueLines);
    layout.issues_more = issue_count > layout.issues_shown;
    const f32 issues_height =
        issue_count == 0 ? 0.0f
                         : (1.0f + static_cast<f32>(layout.issues_shown) + (layout.issues_more ? 1.0f : 0.0f)) * line +
                               6.0f;
    const f32 footer_height = 2.0f * line + issues_height + 6.0f;
    const f32 hint_height = line + 6.0f;

    // The panel is as tall as its content, like the start and session screens': three packs do not need
    // a window-tall panel. Past the cap it stops growing and the list scrolls instead, which is what
    // keeps a hundred mods readable on one screen.
    constexpr usize kMinListRows = 4;
    constexpr usize kMaxListRows = 18;
    const usize wanted_rows = std::clamp(content_list_.row_count(), kMinListRows, kMaxListRows);
    const f32 wanted_height = padding * 2.0f + header_height + static_cast<f32>(wanted_rows) * line +
                              footer_height + hint_height;
    const f32 panel_height = std::min(height - 4.0f * padding, wanted_height);
    layout.panel = t2d::Aabb2::from_center(t2d::Vec2{width * 0.5f, height * 0.5f},
                                           t2d::Vec2{panel_width * 0.5f, panel_height * 0.5f});
    layout.list_top = layout.panel.min.y + padding + header_height;
    layout.list_bottom = layout.panel.max.y - padding - hint_height - footer_height;
    layout.detail_y = layout.list_bottom + 6.0f;
    layout.issues_y = layout.detail_y + 2.0f * line + (issue_count == 0 ? 0.0f : 6.0f);
    // Half a pixel of slack: the panel is sized from a whole number of rows, and dividing that height
    // back by the line height must not lose the last row to a rounding error.
    const f32 list_height = std::max(0.0f, layout.list_bottom - layout.list_top);
    layout.visible_rows = static_cast<usize>((list_height + 0.5f) / line);
    return layout;
}

void MineApp::handle_content_input() {
    const ore::InputState& input = this->input();
    // How many lines fit is decided by the layout the drawing pass uses, and the model is told before
    // it is asked to move: otherwise a key could scroll past the bottom of the panel.
    content_list_.set_visible_rows(content_layout().visible_rows);

    if (input.key_pressed(ore::Key::F5)) {
        reload_content();
        return;
    }
    const i32 page = static_cast<i32>(std::max<usize>(1, content_list_.visible_rows()));
    if (input.key_pressed(ore::Key::Up) || input.key_pressed(ore::Key::W)) content_list_.move(-1);
    if (input.key_pressed(ore::Key::Down) || input.key_pressed(ore::Key::S)) content_list_.move(1);
    if (input.key_pressed(ore::Key::PageUp)) content_list_.move(-page);
    if (input.key_pressed(ore::Key::PageDown)) content_list_.move(page);
    if (input.key_pressed(ore::Key::Home)) content_list_.select_first();
    if (input.key_pressed(ore::Key::End)) content_list_.select_last();
    if (input.key_pressed(ore::Key::Enter) || input.key_pressed(ore::Key::Space) ||
        input.key_pressed(ore::Key::Right)) {
        content_list_.toggle();
    }
    if (input.key_pressed(ore::Key::Left)) content_list_.set_open(content_list_.selected_source(), false);
    if (input.key_pressed(ore::Key::Escape)) screen_ = return_screen_;
}

void MineApp::draw_content_screen() {
    const ContentLayout layout = content_layout();
    const f32 padding = 8.0f * unit_;
    const f32 line = line_for(body_px_);
    const f32 gap = 2.0f * unit_;
    const u16 title_px = static_cast<u16>(body_px_ * 1.3f);
    const ContentListTotals totals = content_list_.totals();
    const f32 full_width = layout.right - layout.left;

    batch_->draw_rect(layout.panel, kPalette.panel_fill);
    batch_->draw_rect_outline(layout.panel, 2.0f, kPalette.panel_edge);

    f32 y = layout.panel.min.y + padding;
    y += draw_line(layout.left, y, title_px, kPalette.accent, locale_.text("content.title")) + 4.0f;
    draw_line(layout.left, y, body_px_, totals.clean() ? kPalette.text_dim : kPalette.error,
              format_localized(locale_.text("content.summary"), totals.sources, totals.entries, totals.failed));
    y += line;
    batch_->draw_rect(t2d::Aabb2{t2d::Vec2{layout.left, y}, t2d::Vec2{layout.right, y + 2.0f}}, kPalette.panel_edge);

    t2d::TextStyle style;
    style.size_px = body_px_;
    const auto width_of = [&](std::string_view text) { return text_->measure(text, fonts_, style).width; };

    if (content_list_.row_count() == 0) {
        draw_fitted(layout.left, layout.list_top, body_px_, kPalette.warning, locale_.text("content.empty"),
                    full_width);
    }

    // --- the list ---
    const usize first = content_list_.first_visible();
    f32 row_y = layout.list_top;
    for (usize index = first; index < content_list_.row_count() && index < first + layout.visible_rows; ++index) {
        const ContentListRow& row = content_list_.row(index);
        const ContentSource& source = content_list_.source(row.source);
        const bool focused = index == content_list_.selected();
        if (focused) {
            // The row's background is the row's own line box, exactly like the start screen's rows.
            batch_->draw_rect(t2d::Aabb2{t2d::Vec2{layout.left - 4.0f, row_y}, t2d::Vec2{layout.right, row_y + line}},
                              kPalette.highlight);
        }
        if (row.is_entry()) {
            // One name this source registered: the kind and the id a save would store, then the name.
            const ContentEntry& entry = source.entries[static_cast<usize>(row.entry)];
            const f32 indent = 3.0f * unit_;
            draw_fitted(layout.left + indent, row_y, body_px_, focused ? kPalette.accent : kPalette.text,
                        std::format("{} #{} {}", content_kind_name(entry.kind), entry.id, entry.name),
                        full_width - indent);
            row_y += line;
            continue;
        }

        draw_line(layout.left, row_y, body_px_, focused ? kPalette.accent : kPalette.text_dim,
                  locale_.text(source_kind_id(source.kind)));

        // The right hand side of a row is measured from the right edge inwards, so the status column
        // lines up whatever the language and whatever the counts are.
        const std::string status{locale_.text(source_status_id(source))};
        draw_line(layout.right - width_of(status), row_y, body_px_,
                  !source.ok ? kPalette.error : (source.error.empty() ? kPalette.text_dim : kPalette.warning), status);

        // The counts end where the widest status label begins, so the column lines up whatever each
        // row's own status is.
        std::string counts = format_localized(locale_.text("content.count"), source.entries.size());
        if (source.images > 0) counts += "  " + format_localized(locale_.text("content.art"), source.images);
        if (source.native) counts += "  " + std::string(locale_.text("content.native"));
        const f32 counts_x = layout.right - layout.status_width - gap * 2.0f - width_of(counts);
        draw_line(counts_x, row_y, body_px_, kPalette.text_dim, counts);

        f32 id_right = counts_x - gap;
        if (!source.version.empty()) {
            const f32 version_width = width_of(source.version);
            draw_line(id_right - version_width, row_y, body_px_, kPalette.text_dim, source.version);
            id_right -= version_width + gap;
        }
        const std::string& id = source.id.empty() ? source.path : source.id;
        const f32 id_x = layout.left + layout.badge_width + gap;
        draw_fitted(id_x, row_y, body_px_, focused ? kPalette.accent : kPalette.text, id,
                    std::max(16.0f, id_right - id_x));
        row_y += line;
    }

    // --- the selected source ---
    if (content_list_.row_count() > 0) {
        const ContentSource& selected = content_list_.source(content_list_.selected_source());
        const f32 value_width = std::max(16.0f, layout.right - layout.detail_column);
        f32 detail_y = draw_pair(layout.left, layout.detail_y, layout.detail_column, body_px_,
                                 locale_.text("content.path"), selected.path, kPalette.text_dim, kPalette.text,
                                 value_width);
        // One line for whatever else there is to say, and one thing only: what went wrong first, then
        // what the source needs, then the name it goes by.
        if (!selected.error.empty()) {
            draw_pair(layout.left, detail_y, layout.detail_column, body_px_, locale_.text("content.error"),
                      selected.error, kPalette.text_dim, kPalette.error, value_width);
        } else if (!selected.requirements.empty()) {
            std::string needs;
            for (const std::string& need : selected.requirements) {
                if (!needs.empty()) needs += ", ";
                needs += need;
            }
            draw_pair(layout.left, detail_y, layout.detail_column, body_px_, locale_.text("content.requires"),
                      needs, kPalette.text_dim, kPalette.text, value_width);
        } else if (!selected.name.empty() && selected.name != selected.id) {
            draw_pair(layout.left, detail_y, layout.detail_column, body_px_, locale_.text("content.name"),
                      selected.name, kPalette.text_dim, kPalette.text, value_width);
        }
    }

    // --- what the load itself reported ---
    const t2d::ConstSpan<ContentListIssue> issues = content_list_.issues();
    if (layout.issues_shown > 0) {
        f32 issue_y = layout.issues_y;
        draw_line(layout.left, issue_y, body_px_, kPalette.text_dim,
                  format_localized(locale_.text("content.messages"), issues.size()));
        issue_y += line;
        for (usize index = 0; index < layout.issues_shown; ++index) {
            const ContentListIssue& issue = issues[index];
            const std::string text = std::format("{} {}",
                                                 locale_.text(issue.error ? "content.issue.error" : "content.issue.warn"),
                                                 issue.text);
            draw_fitted(layout.left, issue_y, body_px_, issue.error ? kPalette.error : kPalette.warning, text,
                        full_width);
            issue_y += line;
        }
        if (layout.issues_more) {
            draw_line(layout.left, issue_y, body_px_, kPalette.text_dim,
                      format_localized(locale_.text("content.messages.more"), issues.size() - layout.issues_shown));
        }
    }

    draw_fitted(layout.left, layout.panel.max.y - padding - line, body_px_, kPalette.text_dim,
                locale_.text("content.hint.keys"), full_width);
}

// --- the sandbox -------------------------------------------------------------------------------

void MineApp::set_status(std::string_view text, bool error) {
    status_ = text;
    status_is_error_ = error;
    if (error) T2D_WARN("sandbox: {}", status_);
    else T2D_INFO("sandbox: {}", status_);
}

std::string MineApp::cell_text(GridPos pos) const {
    // The cell is reported as the whole stack, topmost first: on a multi layer map "what is here" is a
    // list, not one tile.
    std::string text;
    for (i32 layer = sandbox_.layer_count() - 1; layer >= 0; --layer) {
        const CellView value = sandbox_.cell(layer, pos);
        if (value.empty()) continue;
        if (!text.empty()) text += " | ";
        text += std::format("L{} {} #{} {}", layer, content_kind_name(value.kind), value.shown_id(), value.name);
        if (value.missing()) text += std::string(" ") + std::string(locale_.text("sandbox.cell.missing"));
    }
    if (text.empty()) text = locale_.text("sandbox.cell.empty");
    return text;
}

std::string MineApp::view_text() const {
    const t2d::TileRect visible = sandbox_.visible_cells();
    std::string text = format_localized(locale_.text("sandbox.view.value"),
                                        static_cast<t2d::f64>(sandbox_.cell_px()), std::max(visible.width, 0),
                                        std::max(visible.height, 0));
    // A frame that lost quads is not a frame to read a map off, so it says so on the line that
    // describes the view. The count is the previous frame's: the panel is drawn before this frame's
    // batches are submitted.
    if (dropped_quads_ > 0) {
        text += std::format("   {}", format_localized(locale_.text("sandbox.dropped"), dropped_quads_));
    }
    return text;
}

t2d::Aabb2 MineApp::sandbox_status_area() const {
    const f32 width = static_cast<f32>(renderer().width());
    const f32 height = static_cast<f32>(renderer().height());
    const f32 padding = 8.0f * unit_;
    const f32 line = line_for(body_px_);
    // Five label rows, the message and two hint lines.
    const f32 status_height = std::min(height * 0.5f, padding * 2.0f + line * 9.0f);
    return t2d::Aabb2{t2d::Vec2{0.0f, height - status_height}, t2d::Vec2{width, height}};
}

t2d::Aabb2 MineApp::sandbox_panel_area() const {
    const f32 width = static_cast<f32>(renderer().width());
    const t2d::Aabb2 status = sandbox_status_area();
    const f32 panel_width = std::min(width * 0.36f, 300.0f * unit_);
    return t2d::Aabb2{t2d::Vec2{width - panel_width, 0.0f}, t2d::Vec2{width, status.min.y}};
}

t2d::Aabb2 MineApp::sandbox_grid_area() const {
    const f32 width = static_cast<f32>(renderer().width());
    const f32 height = static_cast<f32>(renderer().height());
    // The playtest has no panel and no status bar, so the layer gets the whole window: this is the
    // viewport the camera is given, which is what makes the culling range cover the screen.
    if (playtest_) return t2d::Aabb2{t2d::Vec2{0.0f, 0.0f}, t2d::Vec2{std::max(1.0f, width), std::max(1.0f, height)}};
    const t2d::Aabb2 panel = sandbox_panel_area();
    const t2d::Aabb2 status = sandbox_status_area();
    // Anchored at the top left corner so the area's max is also the viewport size the camera expects.
    return t2d::Aabb2{t2d::Vec2{0.0f, 0.0f},
                      t2d::Vec2{std::max(1.0f, panel.min.x), std::max(1.0f, status.min.y)}};
}

void MineApp::set_playtest(bool on) {
    if (playtest_ == on) return;
    playtest_ = on;
    // Only the viewport changes: the camera keeps looking at the same cell, and the space the chrome
    // used to occupy now shows more of the map. A playtest that jumped somewhere else would be
    // useless for looking at the bit of the layer that was just being edited.
    const t2d::Aabb2 area = sandbox_grid_area();
    sandbox_.set_viewport(t2d::Vec2{area.max.x, area.max.y});
    sandbox_.clear_pointer();
    T2D_INFO("sandbox: playtest {}", on ? "on" : "off");
}

void MineApp::fit_sandbox_view() {
    const t2d::Aabb2 area = sandbox_grid_area();
    const f32 width = std::max(1.0f, area.max.x - area.min.x);
    const f32 height = std::max(1.0f, area.max.y - area.min.y);
    const f32 cell = std::min(width / static_cast<f32>(sandbox_.width()),
                              height / static_cast<f32>(sandbox_.height()));
    // Whole pixels while the cells are big enough to be seen one by one; below that the fraction is
    // what makes a 512 cell map fit at all, and rounding it down would clamp to the minimum zoom.
    sandbox_.set_cell_px(cell >= 1.0f ? std::floor(cell) : cell);
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
    if (shipped_content_.empty() && options_.content_paths.empty() && options_.pack_paths.empty() &&
        options_.pack_directories.empty() && options_.mod_directories.empty()) {
        sandbox_.rebind(registry_);
        set_status(locale_.text("sandbox.no.content"), true);
    } else {
        reload_content();
    }
    // A debug fill first, then the layout file: a map the designer saved on purpose must not be
    // overwritten by a fill they asked for to see the palette.
    apply_fill();
    if (!options_.layout_path.empty()) load_layout();
    // The playtest draws over the whole window, so a run that starts in it enters it before the view is
    // fitted: the fit then frames the layer in the space the playtest actually has.
    if (options_.playtest) set_playtest(true);
    fit_sandbox_view();
    // Where to look is decided last: a run that names a cell wants that cell, whatever the fit did.
    if (options_.has_view) {
        sandbox_.look_at(GridPos{static_cast<i32>(options_.view_cell.x), static_cast<i32>(options_.view_cell.y)},
                         options_.view_zoom);
    }
    // The pointer is placed after the mode: entering the playtest forgets a stale pointer on purpose.
    if (options_.has_pointer) {
        sandbox_.point_at_cell(
            GridPos{static_cast<i32>(options_.pointer_cell.x), static_cast<i32>(options_.pointer_cell.y)});
    }
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

// --- the mine ----------------------------------------------------------------------------------

t2d::Aabb2 MineApp::world_area() const {
    // The whole window: the game's view of a layer is the layer, and the HUD is drawn over the bottom
    // of it rather than beside it (docs/GAME_DESIGN.md section 3). Screen space and window pixels are
    // therefore the same thing, which is what lets the pointer be read straight off the input state.
    return t2d::Aabb2{t2d::Vec2{0.0f, 0.0f},
                      t2d::Vec2{static_cast<f32>(renderer().width()), static_cast<f32>(renderer().height())}};
}

void MineApp::open_world() {
    const LayerShape shape{options_.grid_width, options_.grid_height, options_.tile_layers};
    world_ = MineWorld(session_.seed, shape);
    // Which content a layer holds is the designer's (docs/GAME_DESIGN.md sections 7.3, 7.4, 7.8, 7.9),
    // so the generator in force places nothing: an empty layer is what a session enters until those
    // rules arrive. The debug fills are there to look at content before then, and they name no content
    // of their own - they lay out whatever the registry holds.
    if (options_.layer_fill == "bands") {
        world_.set_generator(debug_band_generator(registry_, shape));
    } else if (options_.layer_fill == "scatter") {
        world_.set_generator(debug_scatter_generator(registry_, shape));
    } else {
        if (!options_.layer_fill.empty() && options_.layer_fill != "none") {
            set_status(format_localized(locale_.text("world.fill.unknown"), options_.layer_fill), true);
        }
        world_.set_generator(empty_layer_generator(shape));
    }
    screen_ = Screen::World;
    world_pointer_.reset();
    enter_mine_layer(options_.mine_layer);
    if (options_.has_view) {
        world_camera_.look_at(
            t2d::Vec2{options_.view_cell.x + 0.5f, options_.view_cell.y + 0.5f});
        if (options_.view_zoom > 0.0f) world_camera_.set_zoom(options_.view_zoom);
        clamp_world_view();
    }
    // A scripted run has no mouse: the pointer it was given is the pointer, until a real one appears
    // (the debug input server can move one later).
    if (options_.has_pointer) {
        point_world_at(world_camera_.screen_of(
            t2d::Vec2{options_.pointer_cell.x + 0.5f, options_.pointer_cell.y + 0.5f}));
    }
}

void MineApp::enter_mine_layer(i32 index) {
    const i32 wanted = std::max(0, index);
    const MineLayer& layer = world_.enter(wanted, registry_, content_.report().definitions);
    const LayerBuildReport& report = world_.build_report();
    T2D_INFO("world: layer {} is {}x{} with {} tile layer(s): {} plot(s), {} ticking, {} mirrored, {} cell(s)",
             layer.index(), layer.width(), layer.height(), layer.layer_count(), layer.plot_count(),
             layer.ticking_count(), layer.mirrored_count(), layer.filled_cells());
    // Everything the generator asked for that could not be placed: a layer that quietly loses a
    // structure is a bug report nobody can act on (world.h).
    for (const std::string& error : report.errors) T2D_WARN("world: {}", error);
    fit_world_view();
}

void MineApp::fit_world_view() {
    const t2d::Aabb2 area = world_area();
    const f32 width = std::max(1.0f, area.max.x - area.min.x);
    const f32 height = std::max(1.0f, area.max.y - area.min.y);
    world_camera_.set_viewport(t2d::Vec2{width, height});
    const MineLayer& layer = world_.layer();
    const f32 cells_x = static_cast<f32>(std::max(1u, layer.width()));
    const f32 cells_y = static_cast<f32>(std::max(1u, layer.height()));
    // The layer is framed with room for the HUD, and a zoom that lands on whole pixels while the cells
    // are big enough to be seen one by one: a fractional zoom is what makes a 512 cell layer fit at
    // all, and rounding it down would clamp to the minimum.
    const f32 usable = std::max(1.0f, height - line_for(body_px_) * 2.0f - 32.0f * unit_);
    const f32 cell = std::min(width / cells_x, usable / cells_y);
    world_camera_.set_zoom(cell >= 1.0f ? std::floor(cell) : cell);
    world_camera_.look_at(t2d::Vec2{cells_x * 0.5f, cells_y * 0.5f});
    clamp_world_view();
}

void MineApp::clamp_world_view() {
    const MineLayer& layer = world_.layer();
    world_camera_.clamp_to(t2d::Aabb2{t2d::Vec2{0.0f, 0.0f},
                                      t2d::Vec2{static_cast<f32>(layer.width()),
                                                static_cast<f32>(layer.height())}});
}

void MineApp::point_world_at(t2d::Vec2 screen) {
    const t2d::Vec2 world = world_camera_.world_of(screen);
    const GridPos cell{t2d::floor_to_i32(world.x), t2d::floor_to_i32(world.y)};
    // Off the map is not a cell: the pointer remembers nothing rather than clamping to the nearest
    // edge, which is what a pointer that selects what is under it means (docs/GAME_DESIGN.md 1.11).
    world_pointer_ = world_.layer().inside(cell) ? std::optional<GridPos>{cell} : std::nullopt;
}

std::string MineApp::world_cell_text(GridPos cell) const {
    const types::SceneTile* plot = world_.layer().top_plot_at(cell);
    if (plot == nullptr) return std::string{locale_.text("sandbox.cell.empty")};
    std::string text = format_localized(locale_.text("world.cell.plot"), content_kind_name(plot->kind()),
                                        plot->id(), plot->name(registry_), plot->width(), plot->height(),
                                        plot->anchor().x, plot->anchor().y);
    // Whether the dice turned this one around is part of what it is: it is decided once, when the map
    // is built, and it is the only thing about a plot that a viewer cannot work out from the data.
    if (plot->mirrored()) text = format_localized(locale_.text("world.cell.mirrored"), text);
    return text;
}

std::string MineApp::world_summary_text() const {
    const MineLayer& layer = world_.layer();
    return format_localized(locale_.text("world.layer"), layer.index(), layer.width(), layer.height(),
                            layer.layer_count(), layer.plot_count(), layer.ticking_count(),
                            layer.mirrored_count());
}

std::string MineApp::world_passes_text() const {
    return format_localized(locale_.text("world.view"), world_camera_.zoom(),
                            static_cast<i32>(world_camera_.visible_cells().width),
                            static_cast<i32>(world_camera_.visible_cells().height), world_refreshed_,
                            world_ticked_);
}

void MineApp::handle_world_input(f32 delta_seconds) {
    const ore::InputState& input = this->input();
    const t2d::Aabb2 area = world_area();
    const t2d::Vec2 centre{area.max.x * 0.5f, area.max.y * 0.5f};
    const t2d::Vec2 mouse{input.mouse_x(), input.mouse_y()};

    // The game's input is the camera and the pointer (docs/GAME_DESIGN.md sections 1.11 and 3): there
    // is no character to move, and what a pointer can *do* to a cell is the action table the designer
    // has not given yet (section 7.14) - so this view flies over the mine and says what is under the
    // pointer, and nothing else.
    constexpr f32 kLookCellsPerSecond = 12.0f;
    const f32 step = kLookCellsPerSecond * world_camera_.zoom() * delta_seconds;
    t2d::Vec2 look{};
    if (input.key_down(ore::Key::Left) || input.key_down(ore::Key::A)) look.x -= 1.0f;
    if (input.key_down(ore::Key::Right) || input.key_down(ore::Key::D)) look.x += 1.0f;
    if (input.key_down(ore::Key::Up) || input.key_down(ore::Key::W)) look.y -= 1.0f;
    if (input.key_down(ore::Key::Down) || input.key_down(ore::Key::S)) look.y += 1.0f;
    if (look.x != 0.0f || look.y != 0.0f) {
        // pan() takes a drag: the map follows the pointer, so looking right drags the map left.
        world_camera_.pan(t2d::Vec2{-look.x * step, -look.y * step});
    }
    if (input.key_pressed(ore::Key::Equal) || input.key_pressed(ore::Key::KpAdd)) world_camera_.zoom_at(centre, 1.25f);
    if (input.key_pressed(ore::Key::Minus) || input.key_pressed(ore::Key::KpSubtract)) {
        world_camera_.zoom_at(centre, 0.8f);
    }
    if (input.mouse_down(ore::MouseButton::Middle)) {
        world_camera_.pan(t2d::Vec2{input.mouse_delta_x(), input.mouse_delta_y()});
    }
    if (input.scroll_y() != 0.0f) {
        world_camera_.zoom_at(mouse, input.scroll_y() > 0.0f ? 1.15f : 1.0f / 1.15f);
    }
    clamp_world_view();

    // The layers of the mine: entering one builds it, and only the one being played is held (world.h).
    if (input.key_pressed(ore::Key::LeftBracket) || input.key_pressed(ore::Key::PageDown)) {
        if (world_.layer_index() > 0) enter_mine_layer(world_.layer_index() - 1);
    }
    if (input.key_pressed(ore::Key::RightBracket) || input.key_pressed(ore::Key::PageUp)) {
        enter_mine_layer(world_.layer_index() + 1);
    }
    if (input.key_pressed(ore::Key::C)) fit_world_view();
    if (input.key_pressed(ore::Key::F5)) {
        // A reload changes the registry, so the layer is built again out of what is now registered:
        // content that disappeared is reported by the build rather than drawn as something it is not.
        reload_content();
        enter_mine_layer(world_.layer_index());
    }
    if (input.key_pressed(ore::Key::F6)) {
        open_content_list(Screen::World);
        return;
    }
    if (input.key_pressed(ore::Key::Escape)) {
        screen_ = Screen::Session;
        return;
    }

    // The two passes, once a frame: scenery is brought up to date only where something asked it to be,
    // and the plots that run take their turn on the cadence each one owns (types/entity_tile.h). What
    // they did is on the HUD - "the mine runs" has to be visible somewhere.
    world_refreshed_ = world_.refresh();
    world_ticked_ = world_.tick(delta_seconds);

    // The pointer selects what is under it, and is asked again every frame because the camera can move
    // under a mouse that is standing still - but only once there is a mouse, so a scripted run keeps
    // the pointer it was given instead of snapping it to the top left corner of the screen.
    if (input.cursor_inside_window() || input.mouse_delta_x() != 0.0f || input.mouse_delta_y() != 0.0f) {
        mouse_seen_ = true;
    }
    if (mouse_seen_) point_world_at(mouse);
}

ore::rhi::Texture* MineApp::texture_of(ContentKind kind, std::string_view name) const {
    const auto found = content_textures_.find(image_key(kind, name));
    return found == content_textures_.end() ? nullptr : found->second.get();
}

void MineApp::load_content_images() {
    image_errors_.clear();
    // Every picture gets its own texture (docs/MODS.md section 0), and a reload replaces them. A
    // reload happens between two frames, and the frame before it may still be on the GPU reading the
    // texture that is about to be destroyed - destroying a texture in use loses the device. So the
    // GPU is brought to a stop first: a reload is a rare, deliberate act (F5), and a frame of latency
    // is a small price for not corrupting the device on the designer's main loop.
    if (!content_textures_.empty()) renderer().wait_idle();
    content_textures_.clear();
    // One picture per content entry, and its own texture: there is no atlas to pack into, so a picture
    // is drawn in whatever size it was drawn in (docs/MODS.md section 0) and no cell can be too small
    // for it. The price is a batch per distinct picture on screen, which is what a batch binds.
    usize loaded = 0;
    u32 largest = 0;
    for (const ResolvedImage& image : content_.report().images) {
        if (!image.ok) continue;   // the content loader already reported why
        const std::optional<ore::Image> pixels = ore::Image::load_png(image.resolved);
        if (!pixels.has_value()) {
            image_errors_.push_back(std::format("{}: cannot be decoded", image.resolved));
            continue;
        }
        const std::string key = image_key(image.kind, image.content);
        Scope<ore::rhi::Texture> texture =
            context().create_texture(*pixels, false, std::format("content.{}", key));
        if (texture == nullptr) {
            image_errors_.push_back(std::format("{}: the texture could not be created", image.resolved));
            continue;
        }
        largest = std::max(largest, std::max(pixels->width, pixels->height));
        content_textures_.emplace(key, std::move(texture));
        ++loaded;
    }
    if (loaded > 0) {
        T2D_INFO("images: {} loaded, one texture per content entry (largest {} px)", loaded, largest);
    }
    for (const std::string& error : image_errors_) T2D_WARN("images: {}", error);
}

const ContentPipelineReport& MineApp::load_content() {
    // One place decides what the registry holds: the game's own files, then packs, then mods. Loading
    // again is a reload - the pipeline unloads the mods (on_unload, then the library closes) and clears
    // the registry first, so ids come out the same every time.
    //
    // The game's own content is the game: what it ships is loaded first, and --content adds to it
    // rather than replacing it.
    std::vector<std::string> base_files = shipped_content_;
    base_files.insert(base_files.end(), options_.content_paths.begin(), options_.content_paths.end());
    content_.set_base_files(std::move(base_files));
    content_.set_pack_files(options_.pack_paths);
    content_.set_pack_directories(options_.pack_directories);
    content_.set_mod_directories(options_.mod_directories);
    const ContentPipelineReport& report = content_.load(registry_);
    load_content_images();
    return report;
}

void MineApp::reload_content() {
    const ContentPipelineReport& loaded = load_content();
    // The list is a view of this load, so it is rebuilt with it: a reload that happened while the list
    // is open must not leave the list describing the load before it.
    refresh_content_list();
    // One rebind after the last source of names, so a single pass over the map sees the whole registry.
    const SandboxReloadReport remapped = sandbox_.rebind(registry_);
    std::string message = format_localized(locale_.text("sandbox.reloaded"), loaded.total_content,
                                           remapped.remapped_cells, remapped.lost_cells);
    if (loaded.packs > 0) {
        message += format_localized(locale_.text("sandbox.packs.loaded"), loaded.packs, loaded.pack_content);
    }
    if (loaded.mods > 0) {
        message += format_localized(locale_.text("sandbox.mods.loaded"), loaded.mods, loaded.native_mods,
                                    loaded.mod_content);
    }
    if (!loaded.clean()) {
        set_status(std::format("{}   {}", message, loaded.first_error()), true);
        return;
    }
    set_status(std::move(message));
}

void MineApp::save_layout() {
    if (options_.layout_path.empty()) {
        set_status(locale_.text("sandbox.no.path"), true);
        return;
    }
    const std::vector<u8> bytes = sandbox_.serialize();
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
    const ore::InputState& input = this->input();
    if (input.key_pressed(ore::Key::Escape)) {
        screen_ = Screen::Start;
        return;
    }
    // Entering the mine is what a session is for. Layer content is still the designer's to give
    // (docs/GAME_DESIGN.md section 7), so what is entered is a layer built from whatever the registry
    // holds - empty until the layer rules arrive.
    if (input.key_pressed(ore::Key::Enter) || input.key_pressed(ore::Key::Space)) open_world();
}

void MineApp::handle_playtest_input(f32 delta_seconds) {
    const ore::InputState& input = this->input();
    const t2d::Aabb2 area = sandbox_grid_area();
    const t2d::Vec2 centre{area.max.x * 0.5f, area.max.y * 0.5f};
    const t2d::Vec2 mouse{input.mouse_x(), input.mouse_y()};

    // What the game's input is (docs/GAME_DESIGN.md section 1.11): the camera and the pointer. There
    // is no character to move and nothing here paints - the brush belongs to the editor, and the
    // action table belongs to content the designer has not given yet (section 7.14).
    //
    // Held keys move the view continuously rather than one cell per press: a playtest is looked at,
    // and stepping a cell at a time is how the editor's cursor moves, not how a view does.
    constexpr f32 kLookCellsPerSecond = 12.0f;
    const f32 step = kLookCellsPerSecond * sandbox_.cell_px() * delta_seconds;
    t2d::Vec2 look{};   // in cells: where the view is asked to go
    if (input.key_down(ore::Key::Left) || input.key_down(ore::Key::A)) look.x -= 1.0f;
    if (input.key_down(ore::Key::Right) || input.key_down(ore::Key::D)) look.x += 1.0f;
    if (input.key_down(ore::Key::Up) || input.key_down(ore::Key::W)) look.y -= 1.0f;
    if (input.key_down(ore::Key::Down) || input.key_down(ore::Key::S)) look.y += 1.0f;
    if (look.x != 0.0f || look.y != 0.0f) {
        // pan() takes a drag: the map follows the pointer, so looking right drags the map left.
        sandbox_.pan(t2d::Vec2{-look.x * step, -look.y * step});
    }
    if (input.key_pressed(ore::Key::Equal) || input.key_pressed(ore::Key::KpAdd)) sandbox_.zoom_at(centre, 1.25f);
    if (input.key_pressed(ore::Key::Minus) || input.key_pressed(ore::Key::KpSubtract)) {
        sandbox_.zoom_at(centre, 0.8f);
    }
    if (input.mouse_down(ore::MouseButton::Middle)) {
        sandbox_.pan(t2d::Vec2{input.mouse_delta_x(), input.mouse_delta_y()});
    }
    if (input.scroll_y() != 0.0f) {
        sandbox_.zoom_at(mouse, input.scroll_y() > 0.0f ? 1.15f : 1.0f / 1.15f);
    }

    // The pointer selects what is under it, and is asked again every frame because the camera can move
    // under a mouse that is standing still - but only once there is a mouse. A run with none (headless,
    // --pointer) keeps the pointer it was given instead of snapping it to wherever "no mouse" is, which
    // is the top left corner of the screen and, on a centred map, off the map.
    if (input.cursor_inside_window() || input.mouse_delta_x() != 0.0f || input.mouse_delta_y() != 0.0f) {
        mouse_seen_ = true;
    }
    if (mouse_seen_) sandbox_.point_at(mouse);
}

void MineApp::handle_sandbox_input(f32 delta_seconds) {
    const ore::InputState& input = this->input();
    const t2d::Aabb2 area = sandbox_grid_area();
    const t2d::Vec2 viewport{area.max.x, area.max.y};
    const t2d::Vec2 centre{area.max.x * 0.5f, area.max.y * 0.5f};
    const t2d::Vec2 mouse{input.mouse_x(), input.mouse_y()};
    const bool over_grid = mouse.x >= area.min.x && mouse.x < area.max.x && mouse.y >= area.min.y &&
                           mouse.y < area.max.y;

    // Editing and playtesting share the keys that are about the layer and its files rather than about
    // the brush: reloading the content files while playtesting is the whole point of playtesting them.
    if (input.key_pressed(ore::Key::P)) set_playtest(!playtest_);
    if (input.key_pressed(ore::Key::F5)) reload_content();
    // The list is available while editing and while playtesting: it is the answer to "what did that
    // reload actually load", which is a question both modes ask.
    if (input.key_pressed(ore::Key::F6)) {
        open_content_list(Screen::Sandbox);
        return;
    }
    if (input.key_pressed(ore::Key::F2)) save_layout();
    if (input.key_pressed(ore::Key::F3)) load_layout();
    if (input.key_pressed(ore::Key::F4)) dump_layer();
    if (input.key_pressed(ore::Key::C)) fit_sandbox_view();
    if (input.key_pressed(ore::Key::Escape)) {
        // Escape leaves the playtest first, the sandbox second: inside a playtest it is the way back
        // to the editor, and only the editor's Escape is "back to the start screen".
        if (playtest_) set_playtest(false);
        else screen_ = Screen::Start;
        return;
    }
    if (playtest_) {
        handle_playtest_input(delta_seconds);
        return;
    }

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

MineApp::SandboxDrawRange MineApp::sandbox_draw_range() const {
    SandboxDrawRange range;
    // The camera already answers "which cells does the viewport touch" (t2d/core/camera2d.h): the
    // sandbox never walks the map, only the part of it that is on screen.
    const t2d::TileRect visible = sandbox_.visible_cells();
    if (visible.empty()) return range;
    range.x0 = std::max(visible.x, 0);
    range.y0 = std::max(visible.y, 0);
    range.x1 = std::min(visible.right() - 1, static_cast<i32>(sandbox_.width()) - 1);
    range.y1 = std::min(visible.bottom() - 1, static_cast<i32>(sandbox_.height()) - 1);
    if (range.x1 < range.x0 || range.y1 < range.y0) return range;
    // One quad per cell while that stays inside the budget; past it, one quad per block of cells,
    // sampled at the block's centre. Every layer draws the same range, so the budget is spent by the
    // whole stack: eight layers of a zoomed out map are eight times the quads.
    constexpr usize kMaxCellQuads = 16384;
    const usize cells =
        static_cast<usize>(range.x1 - range.x0 + 1) * static_cast<usize>(range.y1 - range.y0 + 1);
    range.step = draw_step_for(cells, sandbox_.layer_count(), kMaxCellQuads);
    return range;
}

MineApp::PaletteLayout MineApp::palette_layout() const {
    const t2d::Aabb2 panel = sandbox_panel_area();
    const f32 padding = 8.0f * unit_;
    const u16 title_px = static_cast<u16>(body_px_ * 1.15f);
    const f32 line = line_for(body_px_);
    PaletteLayout layout;
    layout.left = panel.min.x + padding;
    layout.right = panel.max.x - padding;
    layout.row_height = line;
    layout.swatch = std::min(10.0f, line - 6.0f);
    layout.swatch_offset = (line - layout.swatch) * 0.5f;
    // The panel title, its rule and the gap under them, exactly as the panel pass draws them: the
    // title's own line box, not a multiple of the size that happens to be close.
    t2d::TextStyle title_style;
    title_style.size_px = title_px;
    layout.first_y =
        panel.min.y + padding + text_->measure(locale_.text("sandbox.palette"), fonts_, title_style).height + 4.0f + 8.0f;
    const usize rows =
        static_cast<usize>(std::max(0.0f, (panel.max.y - padding - layout.first_y) / line));
    layout.visible = rows;
    layout.first_visible = sandbox_.palette_window_start(rows);
    return layout;
}

void MineApp::draw_sandbox_screen() {
    const t2d::Aabb2 area = sandbox_grid_area();
    const t2d::Aabb2 panel = sandbox_panel_area();
    const t2d::Aabb2 status = sandbox_status_area();
    const f32 padding = 8.0f * unit_;
    const f32 line = line_for(body_px_);
    const f32 cell = sandbox_.cell_px();

    // --- the layer ---
    batch_->draw_rect(area, kPalette.grid_background);
    const SandboxDrawRange range = sandbox_draw_range();
    const i32 step = range.step;
    const f32 span = cell * static_cast<f32>(step);
    const i32 sample = step / 2;   // the cell in the middle of a block names the block

    // A label budget: a layer full of content would otherwise spend every quad the batch has on text.
    constexpr usize kMaxLabels = 320;
    usize labels = 0;
    // Bottom to top, so a higher tile layer covers a lower one. The layer the brush writes into is
    // drawn at full strength with its labels, the others dimmed: that is what makes the stack read as
    // a stack instead of a pile of unrelated colours.
    for (i32 layer = 0; layer < sandbox_.layer_count(); ++layer) {
        const bool active = layer == sandbox_.active_layer();
        for (i32 y = range.y0; y <= range.y1; y += step) {
            for (i32 x = range.x0; x <= range.x1; x += step) {
                const GridPos pos{std::min(x + sample, range.x1), std::min(y + sample, range.y1)};
                const CellView value = sandbox_.cell(layer, pos);
                if (value.empty()) continue;
                const t2d::Vec2 at = sandbox_.screen_of_cell(GridPos{x, y});
                const t2d::Aabb2 rect{t2d::Vec2{at.x, at.y}, t2d::Vec2{at.x + span, at.y + span}};
                const u32 colour = debug_color_for(value.name);
                batch_->draw_rect(rect, dim_color(colour, active ? 0.45f : 0.20f));
                if (value.missing()) batch_->draw_rect_outline(rect, 2.0f, kPalette.missing);
                if (!active || step > 1 || cell < 12.0f || labels >= kMaxLabels) continue;
                t2d::TextStyle style;
                style.size_px = static_cast<u16>(std::clamp(cell * 0.5f, 9.0f, 16.0f));
                style.color = value.missing() ? kPalette.missing : colour;
                // The name when it fits, otherwise the id: on a small cell the number a save would
                // store is the more useful label anyway.
                std::string label{value.name};
                t2d::TextMetrics box = text_->measure(label, fonts_, style);
                if (box.width > cell - 4.0f) {
                    label = std::format("#{}", value.id);
                    box = text_->measure(label, fonts_, style);
                    if (box.width > cell - 4.0f) continue;
                }
                // Centred by the box the renderer actually fills, not by a guessed height.
                (void)text_->draw(*batch_, label, fonts_, style,
                                  t2d::Vec2{at.x + 2.0f, at.y + (cell - box.height) * 0.5f});
                ++labels;
            }
        }
    }

    // Grid lines span the whole area: one rect per column and per row instead of four per cell. Below
    // four pixels per cell they would be denser than the screen, so a zoomed out map is drawn without
    // them and reads as the picture it is.
    if (step == 1 && cell >= 4.0f) {
        for (i32 x = range.x0; x <= range.x1 + 1; ++x) {
            const f32 sx = sandbox_.screen_of_cell(GridPos{x, 0}).x;
            batch_->draw_rect(t2d::Aabb2{t2d::Vec2{sx, area.min.y}, t2d::Vec2{sx + 1.0f, area.max.y}},
                              kPalette.grid_line);
        }
        for (i32 y = range.y0; y <= range.y1 + 1; ++y) {
            const f32 sy = sandbox_.screen_of_cell(GridPos{0, y}).y;
            batch_->draw_rect(t2d::Aabb2{t2d::Vec2{area.min.x, sy}, t2d::Vec2{area.max.x, sy + 1.0f}},
                              kPalette.grid_line);
        }
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
        const PaletteLayout layout = palette_layout();
        const usize start = layout.first_visible;
        const usize rows = layout.visible;
        y = layout.first_y;
        for (usize index = start; index < sandbox_.palette_count() && index < start + rows; ++index) {
            const PaletteEntry& entry = sandbox_.palette(index);
            const bool focused = index == sandbox_.selected_palette();
            if (focused) {
                // The row's background is the row's own line box, exactly like the start screen's.
                batch_->draw_rect(t2d::Aabb2{t2d::Vec2{panel_left - 4.0f, y}, t2d::Vec2{panel_right, y + line}},
                                  kPalette.highlight);
            }
            batch_->draw_rect(t2d::Aabb2{t2d::Vec2{panel_left, y + layout.swatch_offset},
                                         t2d::Vec2{panel_left + layout.swatch,
                                                   y + layout.swatch_offset + layout.swatch}},
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
                           "sandbox.layout", "sandbox.layer", "sandbox.layer.filled", "sandbox.mods",
                           "sandbox.packs", "sandbox.view", "sandbox.cells"}) {
        label_width = std::max(label_width, text_->measure(locale_.text(id), fonts_, measure_style).width);
    }
    const f32 left = status.min.x + padding;
    const f32 middle = status.min.x + (status.max.x - status.min.x) * 0.5f;
    const f32 value_column = left + label_width + 2.0f * unit_;
    const f32 middle_column = middle + label_width + 2.0f * unit_;

    const CellView under = sandbox_.cell(sandbox_.cursor());
    const std::string cell_text_value = cell_text(sandbox_.cursor());
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
    const ContentPipelineReport& loaded = content_.report();
    const std::string packs_text =
        loaded.packs == 0 ? std::string(locale_.text("sandbox.packs.none"))
                          : format_localized(locale_.text("sandbox.packs.value"), loaded.packs,
                                             loaded.pack_content);
    const std::string mods_text =
        loaded.mods == 0 ? std::string(locale_.text("sandbox.mods.none"))
                         : format_localized(locale_.text("sandbox.mods.value"), loaded.mods, loaded.native_mods,
                                            loaded.mod_content);

    // A long path or a long content name is clipped at the column boundary: wrapping it would land
    // on the line below and make the status bar unreadable exactly when something went wrong.
    const f32 left_width = std::max(16.0f, middle - value_column - 8.0f);
    const f32 right_width = std::max(16.0f, status.max.x - padding - middle_column);
    const f32 full_width = status.max.x - 2.0f * padding;

    y = status.min.y + padding;
    draw_pair(left, y, value_column, body_px_, locale_.text("sandbox.cursor"),
              std::format("{},{}", sandbox_.cursor().x, sandbox_.cursor().y), kPalette.text_dim, kPalette.text,
              left_width);
    draw_pair(middle, y, middle_column, body_px_, locale_.text("sandbox.cell"), cell_text_value, kPalette.text_dim,
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
    y += line;
    // Packs and mods get a row of their own: they are the sources a designer is working on, and they
    // need to see at a glance whether what they are editing actually loaded.
    draw_pair(left, y, value_column, body_px_, locale_.text("sandbox.packs"), packs_text, kPalette.text_dim,
              loaded.clean() ? kPalette.text : kPalette.error, left_width);
    draw_pair(middle, y, middle_column, body_px_, locale_.text("sandbox.mods"), mods_text, kPalette.text_dim,
              loaded.clean() ? kPalette.text : kPalette.error, right_width);
    y += line;
    // What the view costs and what the map costs: the two numbers that say whether a map of this
    // size is actually being handled - the zoom the camera is at, how many cells are on screen, and
    // how many bytes the cells occupy (a layer nobody painted on occupies none).
    const std::string view_line = view_text();
    // Rounded up: a map that occupies 3840 bytes costs 4 KiB, not "0 KiB".
    const std::string storage_text =
        format_localized(locale_.text("sandbox.cells.value"), (sandbox_.cell_bytes() + 1023) / 1024);
    draw_pair(left, y, value_column, body_px_, locale_.text("sandbox.view"), view_line, kPalette.text_dim,
              kPalette.text, left_width);
    draw_pair(middle, y, middle_column, body_px_, locale_.text("sandbox.cells"), storage_text,
              kPalette.text_dim, kPalette.text, right_width);
    y += line + 4.0f;
    if (!status_.empty()) {
        draw_fitted(left, y, body_px_, status_is_error_ ? kPalette.error : kPalette.warning, status_, full_width);
    }
    y += line;
    draw_fitted(left, y, body_px_, kPalette.text_dim, locale_.text("sandbox.hint.keys"), full_width);
    draw_fitted(left, y + line, body_px_, kPalette.text_dim, locale_.text("sandbox.hint.keys2"), full_width);
}

void MineApp::draw_playtest_screen() {
    const t2d::Aabb2 area = sandbox_grid_area();
    const f32 padding = 8.0f * unit_;
    const f32 line = line_for(body_px_);
    const f32 cell = sandbox_.cell_px();
    const SandboxDrawRange range = sandbox_draw_range();
    const i32 step = range.step;
    const f32 span = cell * static_cast<f32>(step);
    const i32 sample = step / 2;   // the cell in the middle of a block names the block

    batch_->draw_rect(area, kPalette.grid_background);
    // Every layer bottom to top at full strength, and no grid lines, no labels and no dimming: those
    // are what the layer is edited against, and a playtest is for seeing what a player would.
    for (i32 layer = 0; layer < sandbox_.layer_count(); ++layer) {
        for (i32 y = range.y0; y <= range.y1; y += step) {
            for (i32 x = range.x0; x <= range.x1; x += step) {
                const GridPos pos{std::min(x + sample, range.x1), std::min(y + sample, range.y1)};
                const CellView value = sandbox_.cell(layer, pos);
                if (value.empty()) continue;
                const t2d::Vec2 at = sandbox_.screen_of_cell(GridPos{x, y});
                const t2d::Aabb2 rect{t2d::Vec2{at.x, at.y}, t2d::Vec2{at.x + span, at.y + span}};
                // Content without a picture gets the sandbox's name colour: the game would draw its
                // art there, and a playtest of a content set that has none has to show something.
                batch_->draw_rect(rect, debug_color_for(value.name));
                // The one editor marking the playtest keeps: content that went missing is a data
                // error, and playtesting is exactly when it would be noticed.
                if (value.missing()) batch_->draw_rect_outline(rect, 2.0f, kPalette.missing);
            }
        }
    }

    // The pointer marks the cell the game's actions would apply to (docs/GAME_DESIGN.md section 3).
    if (const std::optional<GridPos> hovered = sandbox_.hovered(); hovered.has_value()) {
        const t2d::Vec2 at = sandbox_.screen_of_cell(*hovered);
        batch_->draw_rect_outline(t2d::Aabb2{t2d::Vec2{at.x, at.y}, t2d::Vec2{at.x + cell, at.y + cell}}, 2.0f,
                                  kPalette.accent);
    }

    // One line of HUD: what the pointer is over, what the view costs, and the way back to editing. The
    // game's own HUD is not designed yet (docs/GAME_DESIGN.md section 7), so this says only what the
    // sandbox already knows and nothing about a game state that does not exist.
    const f32 inset = 4.0f * unit_;
    const t2d::Aabb2 bar{t2d::Vec2{area.min.x, area.max.y - line - inset * 2.0f},
                         t2d::Vec2{area.max.x, area.max.y}};
    batch_->draw_rect(bar, kPalette.panel_fill);
    batch_->draw_rect(t2d::Aabb2{t2d::Vec2{bar.min.x, bar.min.y}, t2d::Vec2{bar.max.x, bar.min.y + 2.0f}},
                      kPalette.panel_edge);

    t2d::TextStyle style;
    style.size_px = body_px_;
    const std::string hint{locale_.text("playtest.hint")};
    const f32 gap = 6.0f * unit_;
    const f32 hint_x = bar.max.x - padding - text_->measure(hint, fonts_, style).width;
    // The middle slot: what the view costs, or - when something went wrong - the message. A reload that
    // failed while playtesting must not be invisible, and a message about a reload that worked is not
    // shown: what it describes is on the screen.
    const bool failed = status_is_error_ && !status_.empty();
    const std::string middle = failed ? status_ : view_text();
    const f32 middle_width =
        std::min(text_->measure(middle, fonts_, style).width, std::max(16.0f, (hint_x - bar.min.x) * 0.5f));
    const f32 middle_x = hint_x - gap - middle_width;
    const f32 cell_x = bar.min.x + padding;
    const f32 cell_width = std::max(16.0f, middle_x - cell_x - gap);
    const f32 text_y = bar.min.y + inset;
    if (const std::optional<GridPos> hovered = sandbox_.hovered(); hovered.has_value()) {
        draw_fitted(cell_x, text_y, body_px_, kPalette.text,
                    format_localized(locale_.text("playtest.cell"), hovered->x, hovered->y, cell_text(*hovered)),
                    cell_width);
    } else {
        draw_fitted(cell_x, text_y, body_px_, kPalette.text_dim, locale_.text("playtest.off"), cell_width);
    }
    draw_fitted(middle_x, text_y, body_px_, failed ? kPalette.error : kPalette.text_dim, middle, middle_width);
    draw_line(hint_x, text_y, body_px_, kPalette.accent, hint);
}

void MineApp::draw_world_screen() {
    if (!world_.has_layer()) return;
    const t2d::Aabb2 area = world_area();
    const f32 padding = 8.0f * unit_;
    const f32 line = line_for(body_px_);
    const f32 inset = 4.0f * unit_;
    const MineLayer& layer = world_.layer();
    const f32 cell = world_camera_.zoom();

    // Where a plot lands on screen: its whole footprint, so a 3x3 is one picture and not nine.
    const auto rect_of = [&](const types::SceneTile& plot) {
        const t2d::Vec2 at = world_camera_.screen_of(t2d::Vec2{static_cast<f32>(plot.anchor().x),
                                                              static_cast<f32>(plot.anchor().y)});
        return t2d::Aabb2{at, t2d::Vec2{at.x + static_cast<f32>(plot.width()) * cell,
                                        at.y + static_cast<f32>(plot.height()) * cell}};
    };

    // The layer the way the game draws it: every tile layer bottom to top, art edge to edge, and no
    // grid lines, labels or dimming - those are what the sandbox edits against (docs/SANDBOX.md 9).
    batch_->draw_rect(area, kPalette.background);
    // The layer itself, marked out from the void around it: a mine is a bounded place, and an empty
    // layer has to read as "a map with nothing on it" rather than as a broken screen.
    const t2d::Vec2 corner = world_camera_.screen_of(t2d::Vec2{0.0f, 0.0f});
    const t2d::Aabb2 bounds{corner, t2d::Vec2{corner.x + static_cast<f32>(layer.width()) * cell,
                                              corner.y + static_cast<f32>(layer.height()) * cell}};
    batch_->draw_rect(bounds, kPalette.grid_background);
    batch_->draw_rect_outline(bounds, 2.0f, kPalette.grid_edge);
    const t2d::TileRect visible = world_camera_.visible_cells();
    if (!visible.empty()) {
        for (i32 tile_layer = 0; tile_layer < layer.layer_count(); ++tile_layer) {
            layer.for_each_plot_in(visible, [&](const types::SceneTile& plot) {
                if (plot.layer() != tile_layer) return;
                // Content with no picture gets the colour its name derives, the same way the sandbox
                // shows it: the world view is still a developer's view until the game's own interface
                // is designed (docs/GAME_DESIGN.md section 7).
                if (texture_of(plot.kind(), plot.name(registry_)) != nullptr) return;
                batch_->draw_rect(rect_of(plot), dim_color(debug_color_for(plot.name(registry_)), 0.45f));
            });
        }
    }

    // The pointer marks the cell the game's actions would apply to (docs/GAME_DESIGN.md section 3).
    if (world_pointer_.has_value()) {
        const t2d::Vec2 at = world_camera_.screen_of(t2d::Vec2{static_cast<f32>(world_pointer_->x),
                                                               static_cast<f32>(world_pointer_->y)});
        batch_->draw_rect_outline(t2d::Aabb2{at, t2d::Vec2{at.x + cell, at.y + cell}}, 2.0f, kPalette.accent);
    }

    // An empty layer says so, and says why: content is the designer's, and a screen that shows nothing
    // has to explain itself (the rule the sandbox's empty palette follows too).
    if (layer.plot_count() == 0) {
        t2d::TextStyle style;
        style.size_px = body_px_;
        const std::string text{locale_.text("world.empty")};
        const std::string hint{locale_.text("world.empty.hint")};
        const f32 text_width = text_->measure(text, fonts_, style).width;
        const f32 hint_width = text_->measure(hint, fonts_, style).width;
        const f32 centre_y = area.max.y * 0.5f;
        draw_line(area.max.x * 0.5f - text_width * 0.5f, centre_y - line, body_px_, kPalette.warning, text);
        draw_line(area.max.x * 0.5f - hint_width * 0.5f, centre_y, body_px_, kPalette.text_dim, hint);
    }

    // Two lines at the bottom: what the layer is and what the two passes did, then what the pointer is
    // over and how to move. The game's own HUD is not designed yet (section 7), so this says only what
    // the world already knows and nothing about a game state that does not exist.
    const f32 bar_height = line * 2.0f + inset * 2.0f;
    const t2d::Aabb2 bar{t2d::Vec2{area.min.x, area.max.y - bar_height}, t2d::Vec2{area.max.x, area.max.y}};
    batch_->draw_rect(bar, kPalette.panel_fill);
    batch_->draw_rect(t2d::Aabb2{t2d::Vec2{bar.min.x, bar.min.y}, t2d::Vec2{bar.max.x, bar.min.y + 2.0f}},
                      kPalette.panel_edge);

    t2d::TextStyle style;
    style.size_px = body_px_;
    const f32 left = bar.min.x + padding;
    const f32 right = bar.max.x - padding;
    const f32 top_y = bar.min.y + inset;
    const f32 bottom_y = top_y + line;

    const std::string hint{locale_.text("world.hint")};
    const f32 hint_width = text_->measure(hint, fonts_, style).width;
    const f32 hint_x = right - hint_width;
    const std::string summary = world_summary_text();
    const f32 summary_width =
        std::min(text_->measure(summary, fonts_, style).width, std::max(16.0f, hint_x - left - padding));
    draw_fitted(left, top_y, body_px_, kPalette.text, summary, summary_width);
    draw_line(hint_x, top_y, body_px_, kPalette.accent, hint);

    // The second line: what the pointer is over, and - on the right - what the frame cost. A reload
    // that failed has to be visible here too, so an error message takes the right hand slot.
    const bool failed = status_is_error_ && !status_.empty();
    const std::string passes = failed ? status_ : world_passes_text();
    const f32 passes_width =
        std::min(text_->measure(passes, fonts_, style).width, std::max(16.0f, (right - left) * 0.5f));
    if (world_pointer_.has_value()) {
        draw_fitted(left, bottom_y, body_px_, kPalette.text,
                    format_localized(locale_.text("world.cell"), world_pointer_->x, world_pointer_->y,
                                     world_cell_text(*world_pointer_)),
                    std::max(16.0f, right - passes_width - left - padding));
    } else {
        draw_fitted(left, bottom_y, body_px_, kPalette.text_dim, locale_.text("world.off"),
                    std::max(16.0f, right - passes_width - left - padding));
    }
    draw_fitted(right - passes_width, bottom_y, body_px_, failed ? kPalette.error : kPalette.text_dim, passes,
                passes_width);
}

MineApp::ImagePassCost MineApp::draw_sandbox_images(ore::RenderFrame& frame,
                                                    const ore::Mat4& view_projection) {
    ImagePassCost cost;
    if (content_textures_.empty()) return cost;
    const f32 cell = sandbox_.cell_px();
    const SandboxDrawRange range = sandbox_draw_range();
    const i32 step = range.step;
    const f32 span = cell * static_cast<f32>(step);
    const i32 sample = step / 2;

    // One quad of art, and which content entry it belongs to. They are collected first and drawn
    // grouped, because a batch binds one texture and every content entry has its own: the frame issues
    // one batch per distinct picture rather than one per frame.
    struct Quad {
        std::string key;
        t2d::Aabb2 rect{};
        u32 color = 0xFFFFFFFFu;
    };
    // Collected per picture, in the order the pictures first appear: one batch per distinct content on
    // screen rather than one per run of cells (a checkerboard of two floors must not be two thousand
    // batches). Within a picture the quads keep the order they were collected in, and since the cells
    // are walked layer by layer, a picture first seen on a higher layer is drawn after the ones below
    // it - which is what keeps a machine's art on top of the floor it stands on.
    std::vector<std::pair<std::string, std::vector<Quad>>> groups;
    std::unordered_map<std::string, usize> group_of;
    const auto add_quad = [&](Quad quad) {
        const auto found = group_of.find(quad.key);
        if (found == group_of.end()) {
            group_of.emplace(quad.key, groups.size());
            groups.emplace_back(quad.key, std::vector<Quad>{});
            groups.back().second.push_back(std::move(quad));
            return;
        }
        groups[found->second].second.push_back(std::move(quad));
    };

    // Bottom to top, like the panel pass, so a higher layer's art covers a lower one's. A layer the
    // brush is not on is dimmed here too, or the art would undo the dimming the colours got.
    for (i32 layer = 0; layer < sandbox_.layer_count(); ++layer) {
        const bool active = layer == sandbox_.active_layer();
        for (i32 y = range.y0; y <= range.y1; y += step) {
            for (i32 x = range.x0; x <= range.x1; x += step) {
                const GridPos pos{std::min(x + sample, range.x1), std::min(y + sample, range.y1)};
                const CellView value = sandbox_.cell(layer, pos);
                if (value.empty()) continue;
                if (texture_of(value.kind, value.name) == nullptr) continue;
                const t2d::Vec2 at = sandbox_.screen_of_cell(GridPos{x, y});
                // The editor insets a picture so the cell under it stays readable; the playtest draws
                // it edge to edge, which is how a tilemap draws a tile, and dims nothing.
                const f32 inset = playtest_ ? 0.0f : span * 0.08f;
                add_quad(Quad{image_key(value.kind, value.name),
                              t2d::Aabb2{t2d::Vec2{at.x + inset, at.y + inset},
                                         t2d::Vec2{at.x + span - inset, at.y + span - inset}},
                              (playtest_ || active) ? 0xFFFFFFFFu : 0x80FFFFFFu});
            }
        }
    }

    // The palette's swatches: the same picture the cell would get, so a designer can tell two entries
    // apart by looking at them. The playtest has no palette, so it draws none.
    if (!playtest_ && sandbox_.palette_count() > 0) {
        const PaletteLayout layout = palette_layout();
        for (usize index = layout.first_visible; index < sandbox_.palette_count() &&
                                                  index < layout.first_visible + layout.visible; ++index) {
            const PaletteEntry& entry = sandbox_.palette(index);
            if (texture_of(entry.kind, entry.name) == nullptr) continue;
            const f32 y = layout.first_y + static_cast<f32>(index - layout.first_visible) * layout.row_height;
            add_quad(Quad{image_key(entry.kind, entry.name),
                          t2d::Aabb2{t2d::Vec2{layout.left, y + layout.swatch_offset},
                                     t2d::Vec2{layout.left + layout.swatch,
                                               y + layout.swatch_offset + layout.swatch}},
                          0xFFFFFFFFu});
        }
    }
    // Each picture fills its whole texture: there is no atlas, so the uv rectangle is the texture.
    const t2d::Aabb2 full_uv{t2d::Vec2{0.0f, 0.0f}, t2d::Vec2{1.0f, 1.0f}};
    for (const std::pair<std::string, std::vector<Quad>>& group : groups) {
        const auto texture = content_textures_.find(group.first);
        if (texture == content_textures_.end()) continue;
        batch_->begin(frame, view_projection, *texture->second, image_sampler_->handle());
        for (const Quad& quad : group.second) batch_->draw_quad(quad.rect, full_uv, quad.color);
        batch_->end();
        cost.draw_calls += batch_->draw_calls();
        cost.quads += batch_->quads();
        cost.dropped += batch_->dropped_quads();
    }
    return cost;
}

MineApp::ImagePassCost MineApp::draw_layer_images(ore::RenderFrame& frame, const ore::Mat4& view_projection,
                                                  const MineLayer& layer, const t2d::Camera2D& camera) {
    ImagePassCost cost;
    if (content_textures_.empty() || layer.plot_count() == 0) return cost;
    const t2d::TileRect visible = camera.visible_cells();
    if (visible.empty()) return cost;
    const f32 cell = camera.zoom();

    // One quad of art, and which picture it belongs to. They are collected first and drawn grouped: a
    // batch binds one texture and every content entry has its own, so the frame issues one batch per
    // distinct picture rather than one per plot (docs/MODS.md section 0).
    struct Quad {
        std::string key;
        t2d::Aabb2 rect{};
        t2d::Aabb2 uv{};
    };
    std::vector<std::pair<std::string, std::vector<Quad>>> groups;
    std::unordered_map<std::string, usize> group_of;
    const auto add_quad = [&](Quad quad) {
        const auto found = group_of.find(quad.key);
        if (found == group_of.end()) {
            group_of.emplace(quad.key, groups.size());
            groups.emplace_back(quad.key, std::vector<Quad>{});
            groups.back().second.push_back(std::move(quad));
            return;
        }
        groups[found->second].second.push_back(std::move(quad));
    };

    // Each picture fills its whole texture: there is no atlas, so the uv rectangle is the texture - and
    // a mirrored plot is that same rectangle with its u axis reversed. Which way a plot faces was
    // decided when the map was built, and it is about drawing only (docs/GAME_DESIGN.md section 1.14).
    const t2d::Aabb2 full_uv{t2d::Vec2{0.0f, 0.0f}, t2d::Vec2{1.0f, 1.0f}};
    const t2d::Aabb2 mirrored_uv{t2d::Vec2{1.0f, 0.0f}, t2d::Vec2{0.0f, 1.0f}};

    // Bottom tile layer to top, so a machine's art lands on the floor it stands on. The walk is the
    // layer's own spatial index over the cells the viewport covers: the frame costs what is on screen,
    // not what the layer holds.
    for (i32 tile_layer = 0; tile_layer < layer.layer_count(); ++tile_layer) {
        layer.for_each_plot_in(visible, [&](const types::SceneTile& plot) {
            if (plot.layer() != tile_layer) return;
            const ContentEntry* entry = registry_.find(plot.kind(), plot.id());
            if (entry == nullptr) return;   // the content is gone: there is nothing to draw it with
            const std::string key = image_key(plot.kind(), entry->name);
            if (content_textures_.find(key) == content_textures_.end()) return;
            const t2d::Vec2 at = camera.screen_of(
                t2d::Vec2{static_cast<f32>(plot.anchor().x), static_cast<f32>(plot.anchor().y)});
            // A plot is drawn over its whole footprint: a 3x3 structure is one picture stretched over
            // nine cells, which is what having a footprint rather than nine cells is for.
            add_quad(Quad{key,
                          t2d::Aabb2{at, t2d::Vec2{at.x + static_cast<f32>(plot.width()) * cell,
                                                   at.y + static_cast<f32>(plot.height()) * cell}},
                          plot.mirrored() ? mirrored_uv : full_uv});
        });
    }

    for (const std::pair<std::string, std::vector<Quad>>& group : groups) {
        const auto texture = content_textures_.find(group.first);
        if (texture == content_textures_.end()) continue;
        batch_->begin(frame, view_projection, *texture->second, image_sampler_->handle());
        for (const Quad& quad : group.second) batch_->draw_quad(quad.rect, quad.uv, 0xFFFFFFFFu);
        batch_->end();
        cost.draw_calls += batch_->draw_calls();
        cost.quads += batch_->quads();
        cost.dropped += batch_->dropped_quads();
    }
    return cost;
}

void MineApp::on_resize(u32 width, u32 height) {
    (void)width;
    (void)height;
    // A resize is a refit in both modes, and the first one is not optional: a scaled display reports the
    // window at its logical size before the compositor hands over the real framebuffer, so the fit that
    // ran at startup was made against the wrong viewport. Switching modes is the case that keeps the
    // view (set_playtest), not resizing.
    if (screen_ == Screen::Sandbox && batch_ != nullptr) fit_sandbox_view();
    if (screen_ == Screen::World && batch_ != nullptr && world_.has_layer()) fit_world_view();
}

void MineApp::on_render(ore::RenderFrame& frame) {
    VkClearValue clear{};
    clear.color = {{0.078f, 0.063f, 0.055f, 1.0f}};
    renderer().begin_pass(clear, 1.0f);

    const t2d::Vec2 view{static_cast<f32>(renderer().width()), static_cast<f32>(renderer().height())};
    const ore::Mat4 view_projection =
        t2d::sprite_view_projection(t2d::Vec2{view.x * 0.5f, view.y * 0.5f}, view);
    // One pass, one texture: the glyph atlas holds both the text and the white texel rectangles use.
    batch_->begin(frame, view_projection, *text_->texture(), sampler_->handle());
    batch_->draw_rect(t2d::Aabb2{t2d::Vec2{0.0f, 0.0f}, view}, kPalette.background);
    switch (screen_) {
        case Screen::Start: draw_start_screen(); break;
        case Screen::Session: draw_session_screen(); break;
        case Screen::Sandbox: playtest_ ? draw_playtest_screen() : draw_sandbox_screen(); break;
        case Screen::Content: draw_content_screen(); break;
        case Screen::World: draw_world_screen(); break;
    }
    batch_->end();
    dropped_quads_ = batch_->dropped_quads();
    // The frame's counters are the frame's: the chrome batch is what has just ended, and the art pass
    // below adds the batches of its own.
    u32 draw_calls = batch_->draw_calls();
    usize quads = batch_->quads();

    // Content art comes after the colours and in its own batches: a content entry that ships a picture
    // is drawn with it, which is what makes the sandbox a view of the real thing.
    if (screen_ == Screen::Sandbox) {
        const ImagePassCost cost = draw_sandbox_images(frame, view_projection);
        draw_calls += cost.draw_calls;
        quads += cost.quads;
        dropped_quads_ += cost.dropped;
    } else if (screen_ == Screen::World) {
        const ImagePassCost cost =
            draw_layer_images(frame, view_projection, world_.layer(), world_camera_);
        draw_calls += cost.draw_calls;
        quads += cost.quads;
        dropped_quads_ += cost.dropped;
    }

    frame.counters.draw_calls = draw_calls;
    frame.counters.triangles = static_cast<u32>(quads) * 2;
    renderer().end_pass();
}

void MineApp::on_shutdown() {
    // A scripted run has no keyboard to press F2 or F4 on, so both can happen on the way out.
    if (options_.dump_layer && options_.session.mode == Mode::Sandbox) {
        T2D_INFO("sandbox dump:\n{}", sandbox_.dump_text());
    }
    if (!options_.save_layout_path.empty() && options_.session.mode == Mode::Sandbox) {
        const std::vector<u8> bytes = sandbox_.serialize();
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
