// Mine - the application shell: the start screen, the session screen and the sandbox.
//
// The shell deliberately does not create a server or a client yet. Layer content is not authored
// (see docs/GAME_DESIGN.md), so the session screen reports the session it *would* run instead of
// pretending to run one - and the moment content data exists, begin_session() is where the world,
// the server thread and the client are created.
//
// The sandbox screen is the one screen that is not waiting for content: it renders a single layer of
// whatever the designer's data registers, so the data can be looked at while it is being written
// (sandbox.h). It still contains no content of its own.
//
// Every label goes through the locale: the interface has no hard coded prose, and the fonts are chosen
// per language (a CJK face for Chinese, the Latin face for everything else).
#pragma once

#include <mine/menu.h>
#include <mine/mod_package.h>
#include <mine/registry.h>
#include <mine/sandbox.h>
#include <mine/session.h>

#include <t2d/render/sprite_batch.h>
#include <t2d/render/text_renderer.h>
#include <t2d/text/font.h>
#include <t2d/text/locale.h>

#include <ore/app/application.h>

#include <optional>
#include <string>
#include <vector>

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

    /// Sandbox: the designer's content files (repeated --content), the layer size, the layout file that
    /// is loaded at startup and written by F2, and the debug fill.
    std::vector<std::string> content_paths;
    u32 grid_width = SandboxModel::kDefaultWidth;
    u32 grid_height = SandboxModel::kDefaultHeight;
    /// How many tile layers the sandbox map has, and which one the brush writes into. What each layer
    /// means is the designer's business; the sandbox only numbers them.
    i32 tile_layers = 1;
    i32 start_layer = 0;
    std::string layout_path;
    /// Written once at shutdown; for a scripted run that has no keyboard to press F2 on.
    std::string save_layout_path;
    /// "none", "bands" or "scatter" - a view of the palette, never content of its own.
    std::string fill;
    /// Which tile layers --fill writes into: "" (the active layer), "all", or a layer number.
    std::string fill_layer;
    /// Directories of mod packages (repeated --mods). Their content joins the registry after the
    /// game's own, and their native modules are loaded into this process.
    std::vector<std::string> mod_directories;
    /// Write the layer as text to the log at shutdown (a scripted run has no keyboard for F4).
    bool dump_layer = false;
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
    void on_resize(u32 width, u32 height) override;
    void on_shutdown() override;
    [[nodiscard]] ore::ConstSpan<ore::CliOption> cli_options() const override;

private:
    enum class Screen : u8 { Start, Session, Sandbox };

    void handle_start_input();
    void handle_session_input();
    void handle_sandbox_input();
    void begin_session(const SessionConfig& session);
    void open_sandbox();
    void apply_fill();
    void reload_content();
    void save_layout();
    void load_layout();
    void dump_layer();
    /// Sizes the cells so the whole layer fits the grid area, then centres it.
    void fit_sandbox_view();
    void load_localisation();
    void load_fonts();
    void draw_start_screen();
    void draw_session_screen();
    void draw_sandbox_screen();

    [[nodiscard]] t2d::Aabb2 sandbox_grid_area() const;
    [[nodiscard]] t2d::Aabb2 sandbox_panel_area() const;
    [[nodiscard]] t2d::Aabb2 sandbox_status_area() const;
    /// Replaces the status line; \p error picks the colour.
    void set_status(std::string_view text, bool error = false);

    /// Draws one localized line and returns the y for the next one.
    f32 draw_line(f32 x, f32 y, u16 size_px, u32 color, std::string_view text, f32 max_width = 0.0f);
    /// Draws one line clipped to \p max_width with an ellipsis: a wrapped path in a status bar would
    /// land on the line below it.
    f32 draw_fitted(f32 x, f32 y, u16 size_px, u32 color, std::string_view text, f32 max_width);
    /// A localized label followed by a value at a fixed column. A non zero \p value_max_width clips
    /// the value instead of letting it run into the next column.
    f32 draw_pair(f32 x, f32 y, f32 value_column, u16 size_px, std::string_view label,
                  std::string_view value, u32 label_color, u32 value_color, f32 value_max_width = 0.0f);

    MineOptions options_{};
    Screen screen_ = Screen::Start;
    MenuModel menu_{};
    SessionConfig session_{};
    /// Filled from the designer's content data; a save stores the ids this hands out plus the table
    /// that maps them back to names (see registry.h).
    ContentRegistry registry_{};
    /// The single layer the sandbox screen paints on, and the message the last action left behind.
    SandboxModel sandbox_{};
    /// The mod packages that are loaded. Owns their libraries, so it outlives every call into them.
    ModHost mods_{};
    std::string status_{};
    bool status_is_error_ = false;

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
