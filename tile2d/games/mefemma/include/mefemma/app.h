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
    /// "title" (the default) or "playground": which scene the process starts in. A scripted run and a
    /// screenshot need a way to skip the title.
    std::string start_scene;
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
    /// Puts the first scene of the run on the stack: the title, unless the command line named another.
    void start_first_scene();

    MefemmaOptions options_{};
    DataPaths paths_{};
    Settings settings_{};
    std::optional<t2d::EcfgError> settings_error_{};

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
