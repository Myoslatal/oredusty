// Mefemma - the application shell: a window, a clock, a scene stack and the services a scene asks for.
//
// The shell is deliberately small. It does not know what a game is: it opens a window, loads the assets
// that travel with it, runs the top scene, draws what the scenes asked for, and turns the platform's input
// into the value a scene sees. Everything a game *is* - its screens, its rules, its world - lives in
// scenes, and everything it *adds* to the shell is a subclass of GameServices (scene.h).
//
// The one thing the shell owns beyond that is the frame: one DrawList per frame, handed to every scene in
// stack order and then to the renderer. A scene never touches the GPU.
#pragma once

#include <mefemma/assets.h>
#include <mefemma/data_paths.h>
#include <mefemma/draw_list.h>
#include <mefemma/input.h>
#include <mefemma/renderer2d.h>
#include <mefemma/scene.h>
#include <mefemma/settings.h>

#include <t2d/core/camera2d.h>
#include <t2d/core/rng.h>
#include <t2d/core/types.h>
#include <t2d/text/locale.h>

#include <ore/app/application.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace mefemma {

/// What a run was asked to do. Every field is optional in the sense that the defaults are the game's own
/// settings: a command line that says nothing runs the game as it shipped.
struct MefemmaOptions {
    /// A settings file that overrides assets/config/game.ecfg.
    std::string settings_path;
    /// An assets directory that overrides the one the executable finds beside itself.
    std::string assets_root;
    std::optional<t2d::Language> language;
    std::string font_path;
    std::string cjk_font_path;
    /// "title" (the default), "playground" or "openworld": which scene the process starts in. A scripted
    /// run and a screenshot need a way to skip the title.
    std::string start_scene;
    /// The open-world scene's chunk store. Empty means the scene's own default:
    /// "mefemma_openworld.t2dk" in the process' working directory.
    std::string world_file;
    /// The ABSOLUTE chunk the open-world scene's two test chunks start at (default {0,0}). A scripted run
    /// can put the content near the practical limit; relative cells and geometry stay small anyway.
    t2d::i64 world_origin_x = 0;
    t2d::i64 world_origin_y = 0;
    /// redirect.ecfg to read and replace. Empty means the default beside the executable.
    std::string redirect_path;
    /// A save directory given on the command line. It skips the first-run prompt, is not written to
    /// redirect.ecfg, and wins over the prompt for this run only - the way a scripted run says where to save.
    std::string save_path;
    std::optional<t2d::u64> seed;
    /// Where the pointer starts, in framebuffer pixels. A headless run has no mouse, so a scripted run that
    /// wants to show (or test) what the pointer does has to say where it is. A real pointer, once it moves,
    /// takes over.
    bool has_pointer = false;
    t2d::Vec2 pointer{};
};

class MefemmaApp final : public ore::Application, public GameServices {
public:
    explicit MefemmaApp(MefemmaOptions options);
    ~MefemmaApp() override;

    // --- GameServices, for the scenes ---------------------------------------------------------------
    [[nodiscard]] const InputState& input() const override { return input_; }
    [[nodiscard]] const Settings& settings() const override { return settings_; }
    [[nodiscard]] const t2d::Locale& locale() const override;
    [[nodiscard]] std::string_view text(std::string_view id) const override;
    [[nodiscard]] t2d::TextMetrics measure_text(std::string_view text, t2d::u16 size_px) const override;
    [[nodiscard]] t2d::Language language() const override { return language_; }
    void set_language(t2d::Language language) override;
    [[nodiscard]] t2d::Rng& random() override { return random_; }
    [[nodiscard]] f32 elapsed_seconds() const override { return elapsed_seconds_; }
    [[nodiscard]] t2d::u64 frame() const override { return frame_; }
    [[nodiscard]] t2d::Vec2 viewport() const override;
    void push_scene(std::unique_ptr<Scene> scene) override;
    void pop_scene() override;
    void replace_scene(std::unique_ptr<Scene> scene) override;
    void quit() override;

protected:
    void on_configure(ore::AppConfig& config) override;
    void on_start() override;
    void on_update(f32 delta_seconds) override;
    void on_render(ore::RenderFrame& frame) override;
    void on_resize(u32 width, u32 height) override;
    void on_shutdown() override;
    [[nodiscard]] ore::ConstSpan<ore::CliOption> cli_options() const override;

private:
    /// Copies the platform's input into the value a scene sees, deriving this frame's edges.
    void sync_input();
    void cycle_language();
    /// The debug overlay is the shell's own: one line with the frame's numbers, toggled with F1. A scene
    /// that wants its own debug output draws it itself.
    void draw_debug_overlay(DrawList& out) const;
    /// Builds the scene the run should start in (title, playground or openworld), with the save directory
    /// resolved into the open-world default when the command line did not name a store.
    [[nodiscard]] std::unique_ptr<Scene> make_first_scene();
    /// Puts the first scene of the run on the stack: the title, unless the command line named another - or,
    /// while the run has no configured save directory yet, the first-run prompt.
    void start_first_scene();
    /// Resolves redirect_path_ and save_directory_ from --save-path/redirect.ecfg and sets prompt_pending_.
    /// Runs before assets and the language: <save dir>/config.ecfg is part of choosing the startup language.
    void resolve_save_directory();
    /// Reads <save dir>/config.ecfg and makes its language the runtime one, unless --lang named another.
    /// When \p live is true it also reloads the string table and fonts (used when the first-run prompt chose
    /// the directory).
    void apply_saved_language(bool live);

    MefemmaOptions options_{};
    DataPaths paths_{};
    Settings settings_{};
    std::optional<t2d::EcfgError> settings_error_{};

    /// The redirect file this run uses (default beside the executable, or --redirect) and the save
    /// directory resolved from it - empty until the first-run prompt is answered or a valid file is read.
    std::string redirect_path_;
    std::string save_directory_;
    /// True when no save directory is configured yet and the first-run prompt must be the only scene.
    bool prompt_pending_ = false;

    Scope<Assets> assets_{};
    Scope<Renderer2D> renderer_{};
    SceneStack scenes_{};
    InputState input_{};
    t2d::Rng random_{1};
    t2d::Locale fallback_locale_{};

    DrawList draw_list_{};
    f32 elapsed_seconds_ = 0.0f;
    f32 smoothed_seconds_ = 1.0f / 60.0f;
    t2d::u64 frame_ = 0;
    /// The frame's numbers are a developer's overlay, not part of a game's screen: off until F1 asks for
    /// them, so a start screen is the screen the designer drew.
    bool debug_visible_ = false;
    /// True once a real pointer has been seen. Until then a scripted --pointer is the pointer.
    bool mouse_seen_ = false;
    t2d::Language language_ = t2d::Language::English;
};

} // namespace mefemma
