// Mine - the application shell: the start screen and the session screen.
//
// The shell deliberately does not create a server or a client yet. Layer content is not authored
// (see docs/GAME_DESIGN.md), so the session screen reports the session it *would* run instead of
// pretending to run one - and the moment content data exists, begin_session() is where the world,
// the server thread and the client are created.
//
// Every label goes through the locale: the interface has no hard coded prose, and the fonts are chosen
// per language (a CJK face for Chinese, the Latin face for everything else).
#pragma once

#include <mine/menu.h>
#include <mine/registry.h>
#include <mine/session.h>

#include <t2d/render/sprite_batch.h>
#include <t2d/render/text_renderer.h>
#include <t2d/text/font.h>
#include <t2d/text/locale.h>

#include <ore/app/application.h>

#include <optional>
#include <string>

namespace mine {

using t2d::Scope;

struct MineOptions {
    /// Skip the start screen and go to the session screen directly (headless screenshots, tests).
    bool start_immediately = false;
    SessionConfig session{};
    /// Interface language; unset means English.
    std::optional<t2d::Language> language;
    /// Overrides for the font files, and for the interface strings.
    std::string font_path;
    std::string cjk_font_path;
    std::string ui_text_path;
};

class MineApp final : public ore::Application {
public:
    explicit MineApp(MineOptions options) : options_(std::move(options)) {}
    ~MineApp() override;

protected:
    void on_configure(ore::AppConfig& config) override;
    void on_start() override;
    void on_update(f32 delta_seconds) override;
    void on_render(ore::RenderFrame& frame) override;
    void on_shutdown() override;
    [[nodiscard]] ore::ConstSpan<ore::CliOption> cli_options() const override;

private:
    enum class Screen : u8 { Start, Session };

    void handle_start_input();
    void begin_session(const SessionConfig& session);
    void load_localisation();
    void load_fonts();
    void draw_start_screen();
    void draw_session_screen();

    /// Draws one localized line and returns the y for the next one.
    f32 draw_line(f32 x, f32 y, u16 size_px, u32 color, std::string_view text, f32 max_width = 0.0f);
    /// A localized label followed by a value at a fixed column.
    f32 draw_pair(f32 x, f32 y, f32 value_column, u16 size_px, std::string_view label,
                  std::string_view value, u32 label_color, u32 value_color);

    MineOptions options_{};
    Screen screen_ = Screen::Start;
    MenuModel menu_{};
    SessionConfig session_{};
    /// Filled from the designer's content data; a save stores the ids this hands out plus the table
    /// that maps them back to names (see registry.h).
    ContentRegistry registry_{};

    t2d::Locale locale_{};
    Scope<t2d::Font> latin_font_;
    Scope<t2d::Font> cjk_font_;
    t2d::FontSet fonts_{};
    Scope<t2d::TextRenderer> text_;

    Scope<t2d::SpriteBatch> batch_;
    Scope<ore::rhi::Sampler> sampler_;
    f32 unit_ = 2.0f;      ///< layout unit derived from the window height
    u16 body_px_ = 16;     ///< text size the interface is drawn at
};

} // namespace mine
