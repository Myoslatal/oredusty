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

#include <mine/content_list.h>
#include <mine/content_pack.h>
#include <mine/menu.h>
#include <mine/mod_package.h>
#include <mine/registry.h>
#include <mine/sandbox.h>
#include <mine/session.h>
#include <mine/world.h>

#include <t2d/render/sprite_batch.h>
#include <t2d/render/text_renderer.h>
#include <t2d/text/font.h>
#include <t2d/text/locale.h>

#include <ore/app/application.h>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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

    /// The designer's own content packs (repeated --content): pack directories loaded with the game's
    /// own content, before anything else - the same stage, so the game's ids still come first.
    std::vector<std::string> content_paths;
    u32 grid_width = SandboxModel::kDefaultWidth;
    u32 grid_height = SandboxModel::kDefaultHeight;
    /// How many tile layers the sandbox map has, and which one the brush writes into. What each layer
    /// means is the designer's business; the sandbox only numbers them.
    i32 tile_layers = 1;
    i32 start_layer = 0;
    std::string layout_path;
    /// Open the camera here instead of fitting the whole map: "x,y" or "x,y,zoom" in cell coordinates
    /// and pixels per cell (see SandboxModel::look_at). A map of hundreds of cells per side does not
    /// fit a window at a readable zoom, so a scripted run has to say where to look.
    bool has_view = false;
    t2d::Vec2 view_cell{};
    f32 view_zoom = 0.0f;   ///< 0 keeps the fitted zoom
    /// Written once at shutdown; for a scripted run that has no keyboard to press F2 on.
    std::string save_layout_path;
    /// "none", "bands" or "scatter" - a view of the palette, never content of its own.
    std::string fill;
    /// Which tile layers --fill writes into: "" (the active layer), "all", or a layer number.
    std::string fill_layer;
    /// What the game loads into its content registry, in this order: the game's own content pack and
    /// whatever --content added to it, then content packs (--packs directories, each a pack or a
    /// directory of packs), then mod packages (--mods, which may also carry native code).
    std::vector<std::string> pack_directories;
    std::vector<std::string> mod_directories;
    /// Write the layer as text to the log at shutdown (a scripted run has no keyboard for F4).
    bool dump_layer = false;
    /// Start in the playtest: the same layer, drawn as the game draws it (no panel, no grid lines, no
    /// labels, no dimming), driven by the game's input - the camera and the pointer, nothing else.
    bool playtest = false;
    /// Where the playtest's pointer starts, in cell coordinates. A scripted run has no mouse, and a
    /// playtest screenshot without the pointer would not show the half of it that is the pointer.
    bool has_pointer = false;
    t2d::Vec2 pointer_cell{};
    /// Open the list of loaded content packs and mods at startup: a scripted run has no keyboard to
    /// press the key that opens it on.
    bool content_list = false;

    /// The world view: which mine layer to enter, and whether to start there. A session enters the
    /// mine with Enter; a scripted run says so on the command line.
    i32 mine_layer = 0;
    bool world_view = false;
    /// What the world's generator puts in the layer while the layer rules do not exist yet: "none" (an
    /// empty layer, which is what a session enters until then), or "bands" / "scatter" - the debug
    /// generators of world.h, a view of the registry rather than content of its own.
    std::string layer_fill;
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
    enum class Screen : u8 { Start, Session, Sandbox, Content, World };

    void handle_start_input();
    void handle_session_input();
    /// The content list: move, open a source, reload. Escape goes back to the screen it was opened
    /// from, because the list is a view of the game, not a place in it.
    void handle_content_input();
    /// Opens the list of everything the registry was filled from; Escape returns to \p from.
    void open_content_list(Screen from);
    /// Rebuilds the list from the last load. Called when the screen opens and after every reload, so
    /// what it shows is always the registry that is actually in force.
    void refresh_content_list();
    /// The playtest half of the sandbox screen: the camera and the pointer, and nothing that edits.
    void handle_playtest_input(f32 delta_seconds);
    void handle_sandbox_input(f32 delta_seconds);
    /// Switches between editing the layer and playtesting it. The camera keeps its place; only the
    /// viewport changes, because the playtest draws over the space the editor's chrome occupies.
    void set_playtest(bool on);
    void begin_session(const SessionConfig& session);
    void open_sandbox();
    /// Enters the mine: builds the world from the session and the options and steps into a layer. The
    /// session screen's Enter, and --world-view for a scripted run.
    void open_world();
    /// Builds p index and makes it the layer being played. Only one layer is held, and the generator
    /// is deterministic, so walking layers and coming back gives the same mine (world.h).
    void enter_mine_layer(i32 index);
    /// The game's own input: the camera and the pointer, nothing else (docs/GAME_DESIGN.md section 3).
    void handle_world_input(f32 delta_seconds);
    /// Sizes the camera so the whole layer fits the window, then keeps the view inside the map.
    void fit_world_view();
    void clamp_world_view();
    /// Points the world's pointer at \p screen; off the map it points at nothing.
    void point_world_at(t2d::Vec2 screen);
    /// What the pointer is over: the topmost plot at the cell, with its footprint and its mirroring.
    [[nodiscard]] std::string world_cell_text(GridPos cell) const;
    /// The layer, what it holds, and what the two passes did last frame.
    [[nodiscard]] std::string world_summary_text() const;
    [[nodiscard]] std::string world_passes_text() const;
    [[nodiscard]] t2d::Aabb2 world_area() const;
    void apply_fill();
    /// Loads the game's content, its packs and its mods into registry_ through the pipeline.
    const ContentPipelineReport& load_content();
    /// The same, then re-points the sandbox's cells at the ids the registry now hands out.
    void reload_content();
    void save_layout();
    void load_layout();
    void dump_layer();
    /// Sizes the cells so the whole layer fits the grid area, then centres it.
    void fit_sandbox_view();
    void load_localisation();
    void load_fonts();
    /// Decodes every picture the content ships into **its own texture**, so the sandbox can draw what
    /// the content describes instead of a colour standing in for it. Every source is in here: the
    /// game's own files, the packs and the mods. There is no atlas on purpose (docs/MODS.md section 0):
    /// one picture per content entry, in whatever size it was drawn in.
    void load_content_images();
    /// The texture one content entry is drawn with, or nullptr when it has no picture.
    [[nodiscard]] ore::rhi::Texture* texture_of(ContentKind kind, std::string_view name) const;
    /// The cells of the sandbox map a frame has to draw, and how many cells one drawn quad covers.
    /// A map of hundreds of cells per side cannot be drawn cell by cell at every zoom: past a quad
    /// budget the frame draws a sampled overview instead, one quad per block of cells.
    struct SandboxDrawRange {
        i32 x0 = 0, y0 = 0, x1 = -1, y1 = -1;   ///< inclusive, clipped to the map
        i32 step = 1;                            ///< cells per drawn quad
        [[nodiscard]] bool empty() const { return x1 < x0 || y1 < y0; }
    };
    [[nodiscard]] SandboxDrawRange sandbox_draw_range() const;
    /// The palette rows the panel actually shows: shared by the panel pass and the picture pass.
    struct PaletteLayout {
        f32 left = 0.0f, right = 0.0f, first_y = 0.0f, row_height = 0.0f;
        /// The colour chip on the left of a row: a square, and the same rectangle in both passes, so
        /// the picture drawn over it lands on the colour instead of beside it.
        f32 swatch = 0.0f;
        f32 swatch_offset = 0.0f;   ///< the chip's top edge, measured down from the row's y
        usize first_visible = 0, visible = 0;
    };
    [[nodiscard]] PaletteLayout palette_layout() const;
    /// What the art pass cost: it issues its own batches (one per distinct picture on screen), so the
    /// frame's counters have to be told about them rather than reading the last batch.
    struct ImagePassCost {
        u32 draw_calls = 0;
        usize quads = 0;
        u32 dropped = 0;
    };
    /// Draws the pictures of the visible cells and of the palette. Each content entry has its own
    /// texture, and a batch binds one texture, so this is one batch per distinct picture on screen -
    /// the price of not packing the art into an atlas.
    [[nodiscard]] ImagePassCost draw_sandbox_images(ore::RenderFrame& frame, const ore::Mat4& view_projection);
    /// Draws everything \p layer holds that \p camera covers, bottom tile layer to top: the picture a
    /// plot has, or - when it has none - the colour its name derives, each over its whole footprint and
    /// mirrored when it is. A plot is drawn in its own tile layer's turn either way, so art from below
    /// never covers it; the pictures are grouped into one batch per distinct one on screen
    /// (docs/MODS.md section 0).
    [[nodiscard]] ImagePassCost draw_world_layer(ore::RenderFrame& frame, const ore::Mat4& view_projection,
                                                 const MineLayer& layer, const t2d::Camera2D& camera);
    void draw_start_screen();
    void draw_session_screen();
    void draw_sandbox_screen();
    /// Every source the registry was filled from, one line each, with the selected source's detail and
    /// the load's own messages under it.
    void draw_content_screen();
    /// The layer as the game draws it, plus the pointer and one line saying what it is over.
    void draw_playtest_screen();
    /// The mine, in three passes that have to land in this order: what is under the layer (the void and
    /// the map's bounds), the layer itself (draw_world_layer), and the chrome over it
    /// (draw_world_overlay).
    void draw_world_screen();
    /// The game's own chrome over the layer: the pointer, and the two lines that say what the layer is,
    /// what the pointer is over and what the passes did. Drawn after the layer, so a zoomed-in map
    /// cannot paint over the status bar (measured: it used to).
    void draw_world_overlay();

    /// Where the content screen puts things. The lines of the list, the room left for the selected
    /// source and the room the load's messages take all come from one place, so the pass that draws
    /// and the pass that decides how many lines fit cannot disagree about either.
    struct ContentLayout {
        t2d::Aabb2 panel{};
        f32 left = 0.0f, right = 0.0f;
        f32 badge_width = 0.0f;    ///< the widest of FILE / PACK / MOD, so the ids line up
        f32 status_width = 0.0f;   ///< the widest of OK / PARTIAL / FAILED, so the counts line up
        f32 detail_column = 0.0f;  ///< where a detail line's value starts
        f32 list_top = 0.0f, list_bottom = 0.0f;
        f32 detail_y = 0.0f;
        f32 issues_y = 0.0f;
        usize visible_rows = 0;
        usize issues_shown = 0;    ///< messages drawn; the rest are counted on one more line
        bool issues_more = false;
    };
    [[nodiscard]] ContentLayout content_layout() const;

    [[nodiscard]] t2d::Aabb2 sandbox_grid_area() const;
    [[nodiscard]] t2d::Aabb2 sandbox_panel_area() const;
    [[nodiscard]] t2d::Aabb2 sandbox_status_area() const;
    /// Replaces the status line; \p error picks the colour.
    void set_status(std::string_view text, bool error = false);

    /// What is at \p pos, topmost layer first ("L1 machine #3 name | L0 structure #1 name", or "EMPTY").
    /// The status bar and the playtest's HUD both report a cell through this, so the two never disagree.
    [[nodiscard]] std::string cell_text(GridPos pos) const;
    /// The view as the status bar reports it: pixels per cell, visible cells, and a warning when the
    /// last frame lost quads.
    [[nodiscard]] std::string view_text() const;

    /// One row of the interface at \p size_px: the font's own line box, never tighter than the
    /// 1.45 x size rhythm the panels are laid out with. A row, the background behind it and the text
    /// in it all come from this one number, which is what keeps a highlight on top of its text.
    [[nodiscard]] f32 line_for(u16 size_px) const;
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
    /// The mine the session is playing: its seed, its layer shape, the generator that describes its
    /// layers, and the one layer that is in memory. Built when the mine is entered.
    MineWorld world_{};
    /// The world view's camera. A god view like the sandbox's - there is no player character anywhere
    /// in this file (docs/GAME_DESIGN.md section 1.11) - but this one looks at the game's own layer.
    t2d::Camera2D world_camera_{};
    /// What the game's pointer is over, if anything. Not the sandbox's cursor: the two are different
    /// views with different pointers, and neither moves the other.
    std::optional<GridPos> world_pointer_{};
    /// What the two passes did in the frame that was last drawn: scenery brought up to date, and plots
    /// that took their turn. The HUD says so, because "the mine runs" has to be visible somewhere.
    usize world_refreshed_ = 0;
    usize world_ticked_ = 0;
    /// Everything the game's registry is filled from. Owns the loaded libraries, so it outlives every
    /// call into them.
    ContentPipeline content_{};
    std::string status_{};
    bool status_is_error_ = false;
    /// True while the sandbox is being playtested rather than edited. It is a mode of the sandbox
    /// screen, not a fourth screen: the layer, the camera and the content stay the same, only the
    /// chrome and the input change.
    bool playtest_ = false;
    /// True once a pointer has actually been seen. The playtest's pointer follows the mouse, but a
    /// scripted run has no mouse at all (--pointer puts the pointer somewhere on purpose, and the
    /// debug input server may move it later): until one exists, the scripted pointer is the pointer.
    bool mouse_seen_ = false;
    /// The content list, and the screen Escape goes back to. It is opened from the start screen and
    /// from the sandbox, and both expect to get where they came from.
    ContentListModel content_list_{};
    Screen return_screen_ = Screen::Start;

    t2d::Locale locale_{};
    Scope<t2d::Font> latin_font_;
    Scope<t2d::Font> cjk_font_;
    t2d::FontSet fonts_{};
    Scope<t2d::TextRenderer> text_;

    Scope<t2d::SpriteBatch> batch_;
    Scope<ore::rhi::Sampler> sampler_;
    /// Content art: one texture per content entry that ships a picture, keyed "kind:name".
    std::unordered_map<std::string, Scope<ore::rhi::Texture>> content_textures_;
    Scope<ore::rhi::Sampler> image_sampler_;
    std::vector<std::string> image_errors_;
    /// The game's own content pack, found once at startup: beside the executable, or in the source
    /// tree when the game is being run out of a build directory (vanilla_content_pack). It is loaded
    /// before anything a command line names.
    std::string shipped_content_;
    /// Quads the batch refused in the last frame (both passes): a frame that lost quads is a frame the
    /// screen must not pretend is complete.
    u32 dropped_quads_ = 0;
    f32 unit_ = 2.0f;      ///< layout unit derived from the window height
    u16 body_px_ = 16;     ///< text size the interface is drawn at
};

} // namespace mine
