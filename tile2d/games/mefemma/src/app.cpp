#include <mefemma/app.h>

#include <mefemma/config.h>
#include <mefemma/redirect.h>
#include <mefemma/scenes/open_world_scene.h>
#include <mefemma/scenes/playground_scene.h>
#include <mefemma/scenes/save_path_scene.h>
#include <mefemma/scenes/title_scene.h>

#include <t2d/core/executable.h>
#include <t2d/core/log.h>
#include <t2d/core/time.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>

namespace mefemma {
namespace {

/// A localized pattern has placeholders a runtime string fills, so vformat rather than format.
template <class... Args>
[[nodiscard]] std::string format_localized(std::string_view pattern, Args&&... args) {
    return std::vformat(pattern, std::make_format_args(args...));
}

/// An absolute, lexically normal path for a command-line option. When the platform will not resolve it the
/// original text is returned: a useful diagnostic beats an invented path.
[[nodiscard]] std::string normalized_absolute(std::string_view path) {
    std::error_code code;
    std::filesystem::path result{std::string(path)};
    if (!result.is_absolute()) {
        result = std::filesystem::absolute(result, code);
        if (code) return std::string(path);
    }
    return result.lexically_normal().string();
}

/// The input enums mirror the engine's values; if either side ever renumbers, this is where the build
/// says so instead of the game quietly reading the wrong key.
static_assert(static_cast<int>(Key::Escape) == static_cast<int>(ore::Key::Escape));
static_assert(static_cast<int>(Key::Enter) == static_cast<int>(ore::Key::Enter));
static_assert(static_cast<int>(Key::Left) == static_cast<int>(ore::Key::Left));
static_assert(static_cast<int>(Key::Right) == static_cast<int>(ore::Key::Right));
static_assert(static_cast<int>(Key::A) == static_cast<int>(ore::Key::A));
static_assert(static_cast<int>(Key::W) == static_cast<int>(ore::Key::W));
static_assert(static_cast<int>(Key::F1) == static_cast<int>(ore::Key::F1));
static_assert(static_cast<int>(Key::Count) == static_cast<int>(ore::Key::Count));
static_assert(static_cast<int>(MouseButton::Left) == static_cast<int>(ore::MouseButton::Left));
static_assert(static_cast<int>(MouseButton::Middle) == static_cast<int>(ore::MouseButton::Middle));

} // namespace

MefemmaApp::MefemmaApp(MefemmaOptions options) : options_(std::move(options)) {
    // The paths and the settings are resolved before the window exists: on_configure() needs the size and
    // the title, and the window cannot be changed after it is created.
    paths_ = options_.assets_root.empty()
                 ? DataPaths::resolve(t2d::executable_path(), std::string(T2D_SOURCE_DIR) + "/games/mefemma")
                 : DataPaths(options_.assets_root);
    const std::string settings_path = options_.settings_path.empty() ? paths_.settings() : options_.settings_path;
    t2d::EcfgError error;
    settings_ = Settings::load(settings_path, &error);
    if (!error.message.empty()) {
        settings_error_ = std::move(error);
        T2D_WARN("settings '{}': {}", settings_path, settings_error_->describe(settings_path));
    }

    // The language the run uses: the command line wins, then the settings file, then English. The strings
    // themselves are loaded later, when the assets are.
    if (options_.language.has_value()) {
        language_ = *options_.language;
    } else if (const std::optional<t2d::Language> parsed = t2d::parse_language(settings_.language);
               parsed.has_value()) {
        language_ = *parsed;
    } else {
        T2D_WARN("settings: '{}' is not a language (try en, zh-Hans or zh-Hant); using English", settings_.language);
    }
}

MefemmaApp::~MefemmaApp() = default;

void MefemmaApp::on_configure(ore::AppConfig& config) {
    config.name = "Mefemma";
    config.window.title = settings_.title;
    config.window.width = settings_.width;
    config.window.height = settings_.height;
    config.window.vsync = settings_.vsync;
    config.vsync = settings_.vsync;
    // The interface is pure 2D: a depth attachment would be an image nobody reads, and a wrong depth format
    // is a pipeline that draws nothing.
    config.depth_format = VK_FORMAT_UNDEFINED;
    config.stats_interval = 0.0f;
    // A frame can draw a full-screen background, a scene's own art and the debug line; 4 MiB of ring keeps
    // a busy frame from being dropped because the upload ran out.
    config.upload_segment_size = 4ull << 20;
    // Escape belongs to the screens: the title quits, the playground goes back. The shell must not quit
    // before the scene has had its say.
    config.quit_on_escape = false;
}

ore::ConstSpan<ore::CliOption> MefemmaApp::cli_options() const {
    static const ore::CliOption kOptions[] = {
        {"settings", "<path>", "a settings file (default: assets/config/game.ecfg)"},
        {"assets", "<dir>", "an assets directory (default: the one beside the executable)"},
        {"lang", "<code>", "interface language: en, zh-Hans, zh-Hant"},
        {"font", "<path>", "Latin font file (default: the first system font found)"},
        {"cjk-font", "<path>", "CJK font file used for Chinese text"},
        {"seed", "<n>", "the game's random seed (default: the settings file's)"},
        {"scene", "<title|playground|openworld>", "which scene to start in (default: title)"},
        {"redirect", "<path>", "the redirect.ecfg file (default: beside the executable)"},
        {"save-path", "<dir>", "the save directory; skips the first-run prompt and does not write redirect.ecfg"},
        {"world-file", "<path>", "the open-world scene's chunk store (default: <save dir>/mefemma_openworld.t2dk)"},
        {"world-origin", "<x,y>", "absolute chunk origin of the openworld content (default: 0,0)"},
        {"pointer", "<x,y>", "put the pointer at this pixel (a scripted run has no mouse)"},
    };
    return ore::ConstSpan<ore::CliOption>(kOptions, std::size(kOptions));
}

void MefemmaApp::on_start() {
    // The save directory is decided before any asset exists: <save dir>/config.ecfg is part of choosing the
    // startup language, and no scene may open a save store before the directory is configured. The language
    // is then --lang > <save dir>/config.ecfg > assets/config/game.ecfg; this reads the middle one without
    // writing anything.
    resolve_save_directory();
    apply_saved_language(false);

    assets_ = std::make_unique<Assets>(context(), paths_, options_.font_path, options_.cjk_font_path);
    assets_->load_text();
    assets_->set_language(language_);

    Renderer2DOptions renderer_options;
    renderer_options.color_format = renderer().color_format();
    renderer_options.depth_format = renderer().depth_format();
    renderer_options.vertex_shader = shader_path("sprite.vert.spv");
    renderer_options.fragment_shader = shader_path("sprite.frag.spv");
    renderer_ = Renderer2D::create(context(), renderer_options);
    if (renderer_ == nullptr) T2D_FATAL("renderer: the 2D renderer could not be created");
    renderer_->set_fonts(assets_->fonts());

    random_.seed(options_.seed.value_or(settings_.seed));

    if (prompt_pending_) {
        // The prompt is the ONLY scene started: nothing behind it may open a store or a save list before the
        // directory exists. Its callback finishes the decision and applies any config.ecfg the chosen
        // directory already has, so choosing an existing save directory picks up its language immediately.
        scenes_.push(make_save_path_scene(
            redirect_path_, default_save_directory(redirect_path_), [this](std::string_view path) {
                save_directory_ = std::string(path);
                prompt_pending_ = false;
                apply_saved_language(true);
                return make_first_scene();
            }));
    } else {
        start_first_scene();
    }
    scenes_.begin_frame(*this);

    T2D_INFO("mefemma: assets '{}', language {}, {} scene(s)", paths_.root(), t2d::language_code(language_),
             scenes_.size());
}

void MefemmaApp::resolve_save_directory() {
    // The redirect file is the package's own record; --redirect points at another one (a test, a per-player
    // setup), and --save-path is a one-run override that writes nothing. A --save-path that cannot be used is
    // not a decision: fall through to the redirect file/prompt instead of starting a scene that would write
    // somewhere nobody chose.
    redirect_path_ =
        options_.redirect_path.empty() ? default_redirect_path() : normalized_absolute(options_.redirect_path);
    T2D_INFO("mefemma: redirect '{}'", redirect_path_);

    bool configured = false;
    if (!options_.save_path.empty()) {
        std::string error;
        const std::optional<std::string> normalized =
            normalize_save_path(options_.save_path, t2d::parent_directory_of(redirect_path_), &error);
        if (normalized.has_value()) {
            save_directory_ = *normalized;
            configured = true;
            if (!ensure_save_directory(save_directory_, &error)) {
                T2D_WARN("first-run: save directory '{}': {}", save_directory_, error);
            } else {
                T2D_INFO("first-run: save directory '{}' (from --save-path; redirect not written)", save_directory_);
            }
        } else {
            T2D_WARN("first-run: --save-path '{}': {} (falling back to the redirect file)", options_.save_path,
                     error);
        }
    }

    if (!configured) {
        t2d::EcfgError redirect_error;
        const std::optional<RedirectConfig> redirect = load_redirect(redirect_path_, &redirect_error);
        if (redirect.has_value()) {
            std::string error;
            const std::optional<std::string> normalized = normalize_save_path(
                redirect->save_path, t2d::parent_directory_of(redirect_path_), &error);
            if (normalized.has_value()) {
                save_directory_ = *normalized;
                configured = true;
                if (!ensure_save_directory(save_directory_, &error)) {
                    // A configured directory that cannot be created is still the player's choice: report it
                    // and let the game run, exactly like a failing save would.
                    T2D_WARN("first-run: save directory '{}': {}", save_directory_, error);
                } else {
                    T2D_INFO("first-run: configured save directory '{}'", save_directory_);
                }
            } else {
                T2D_WARN("first-run: redirect '{}': {}", redirect_path_, error);
            }
        } else {
            std::error_code code;
            if (std::filesystem::exists(redirect_path_, code)) {
                // An existing but unreadable/invalid file is worth a line; a missing one is the normal
                // first run and says nothing until the player has answered.
                T2D_WARN("first-run: redirect '{}': {}", redirect_path_,
                         redirect_error.describe(redirect_path_));
            }
        }
    }
    prompt_pending_ = !configured;
}

void MefemmaApp::apply_saved_language(bool live) {
    // --lang wins over the save directory's file; the packaged assets/config/game.ecfg is the last fallback
    // (language_ already holds it from the constructor). A missing file is silent, an invalid one is logged
    // and skipped, and nothing here writes.
    if (options_.language.has_value() || save_directory_.empty()) return;

    const std::string file = config_path(save_directory_);
    std::error_code code;
    const bool file_exists = std::filesystem::exists(file, code);
    t2d::EcfgError error;
    const std::optional<GameConfig> config = load_game_config(file, &error);
    if (!config.has_value()) {
        if (file_exists) T2D_WARN("{}", error.describe(file));
        return;
    }
    if (!config->language.has_value()) return;

    const t2d::Language saved = *config->language;
    const bool changed = saved != language_;
    language_ = saved;
    T2D_INFO("config: language {} from '{}'", t2d::language_code(language_), file);
    if (!live || !changed || assets_ == nullptr) return;
    assets_->set_language(language_);
    if (renderer_ != nullptr) renderer_->set_fonts(assets_->fonts());
}

std::unique_ptr<Scene> MefemmaApp::make_first_scene() {
    const std::string& scene = options_.start_scene;
    if (scene.empty() || scene == "title") return make_title_scene();
    if (scene == "playground") return make_playground_scene();
    if (scene == "openworld") {
        // The open-world store lives in the configured save directory unless the command line named one.
        std::string world_file = options_.world_file;
        if (world_file.empty() && !save_directory_.empty()) {
            world_file = (std::filesystem::path(save_directory_) / "mefemma_openworld.t2dk").string();
        }
        return make_open_world_scene(world_file, t2d::ChunkKey{options_.world_origin_x, options_.world_origin_y});
    }
    T2D_WARN("scene: unknown start scene '{}' (title, playground, openworld); starting at the title", scene);
    return make_title_scene();
}

void MefemmaApp::start_first_scene() { scenes_.push(make_first_scene()); }

void MefemmaApp::on_update(f32 delta_seconds) {
    sync_input();
    smoothed_seconds_ = smoothed_seconds_ + (delta_seconds - smoothed_seconds_) * 0.1f;
    elapsed_seconds_ += delta_seconds;
    ++frame_;

    if (input_.key_pressed(Key::F1)) debug_visible_ = !debug_visible_;
    if (input_.key_pressed(Key::F2)) cycle_language();

    // A frame that took a second (a breakpoint, a resize, a stalled compositor) must not hand a scene a
    // second of simulation: one step, and the next frame carries on. This is the only place the shell
    // touches a scene's time.
    const f32 step = std::min(delta_seconds, 0.1f);
    scenes_.begin_frame(*this);
    scenes_.update(*this, step);
    if (scenes_.empty()) quit();

    // Headless runs have no vsync: without pacing the loop would spin and a screenshot would be taken
    // before the interface settled.
    if (config().headless) {
        constexpr f32 kFrameSeconds = 1.0f / 60.0f;
        const f32 remaining = kFrameSeconds - delta_seconds;
        if (remaining > 0.0f) t2d::sleep_ms(static_cast<u32>(remaining * 1000.0f + 0.5f));
    }
}

void MefemmaApp::sync_input() {
    input_.begin_frame();
    const ore::InputState& source = ore::Application::input();
    // Key::Unknown is 0 and the engine's own is -1; every real key is 1..Count-1 on both sides, which the
    // static asserts above pin down.
    //
    // **The press edge comes from the platform, not from a down-state transition.** A tap can begin and end
    // between two frames - the debug input server's "key press" is exactly that - and copying only the
    // held state would drop it: the key is already up again by the time the frame looks. Setting it down
    // for the press and then to its real state gives the press, the release, and the right final state in
    // one call each, without asking the engine for key_released(), which is not in the published surface.
    for (u16 code = 1; code < static_cast<u16>(Key::Count); ++code) {
        const auto key = static_cast<Key>(code);
        const auto engine_key = static_cast<ore::Key>(code);
        if (source.key_pressed(engine_key)) input_.set_key(key, true);
        input_.set_key(key, source.key_down(engine_key));
    }
    for (u16 button = 0; button < static_cast<u16>(MouseButton::Count); ++button) {
        const auto mouse_button = static_cast<MouseButton>(button);
        const auto engine_button = static_cast<ore::MouseButton>(button);
        if (source.mouse_pressed(engine_button)) input_.set_mouse_button(mouse_button, true);
        input_.set_mouse_button(mouse_button, source.mouse_down(engine_button));
    }
    bool inside = source.cursor_inside_window();
    if (inside || source.mouse_delta_x() != 0.0f || source.mouse_delta_y() != 0.0f) mouse_seen_ = true;
    f32 pointer_x = source.mouse_x();
    f32 pointer_y = source.mouse_y();
    if (!mouse_seen_ && options_.has_pointer) {
        // A headless run has no mouse at all. The scripted pointer is the pointer until a real one moves.
        inside = true;
        pointer_x = options_.pointer.x;
        pointer_y = options_.pointer.y;
    }
    input_.set_cursor_inside(inside);
    if (inside) input_.set_mouse_position(pointer_x, pointer_y);
    input_.add_scroll(source.scroll_x(), source.scroll_y());
    input_.set_text(source.text_input());
}

void MefemmaApp::set_language(t2d::Language language) {
    if (language == language_) return;
    language_ = language;
    // Assets::set_language() logs the language and its string counts; the shell does not repeat that.
    if (assets_ != nullptr) {
        assets_->set_language(language_);
        if (renderer_ != nullptr) renderer_->set_fonts(assets_->fonts());
    }
    // The Settings menu and F2 are user choices, so they are the one path that writes the save directory's
    // config.ecfg. A startup --lang never comes here, and an unconfigured run (still at the prompt) has no
    // file to write.
    if (save_directory_.empty()) return;
    std::string error;
    if (!save_game_config(config_path(save_directory_), language_, &error)) {
        T2D_WARN("config: cannot save language '{}': {}", t2d::language_code(language_), error);
        return;
    }
    T2D_INFO("config: language {} written to '{}'", t2d::language_code(language_),
             config_path(save_directory_));
}

void MefemmaApp::cycle_language() {
    set_language(static_cast<t2d::Language>((static_cast<usize>(language_) + 1) % t2d::kLanguageCount));
}

void MefemmaApp::on_render(ore::RenderFrame& frame) {
    // The frame is built on the CPU first: every scene gets to ask for what it wants before a pass exists,
    // so a picture that has never been seen can be decoded and uploaded **outside** the render pass. A
    // texture created while the pass is recording would submit its own commands from inside it, which is
    // the kind of thing that works until it does not.
    draw_list_.clear();
    scenes_.draw(*this, draw_list_);
    if (debug_visible_) draw_debug_overlay(draw_list_);
    if (assets_ != nullptr) {
        for (const DrawCommand& command : draw_list_.commands()) {
            if (command.kind == DrawCommand::Kind::Sprite) (void)assets_->texture(command.texture);
        }
    }

    VkClearValue clear{};
    clear.color = {{0.055f, 0.058f, 0.070f, 1.0f}};
    renderer().begin_pass(clear, 1.0f);

    if (renderer_ != nullptr && assets_ != nullptr) renderer_->draw(frame, draw_list_, *assets_);
    const Renderer2D::Stats stats = renderer_ != nullptr ? renderer_->stats() : Renderer2D::Stats{};
    frame.counters.draw_calls = stats.draw_calls;
    frame.counters.triangles = stats.quads * 2;

    renderer().end_pass();
}

void MefemmaApp::draw_debug_overlay(DrawList& out) const {
    const t2d::Vec2 view = viewport();
    const f32 fps = smoothed_seconds_ > 0.0f ? 1.0f / smoothed_seconds_ : 0.0f;
    const u32 draw_calls = renderer_ != nullptr ? renderer_->stats().draw_calls : 0;
    const usize scene_count = scenes_.size();
    const std::string line =
        format_localized(text("debug.line"), fps, smoothed_seconds_ * 1000.0f, draw_calls, scene_count);

    // Bottom right: the bottom left is where a scene's own status line tends to be, and the shell should not
    // be the thing that covers it.
    constexpr f32 kWidth = 560.0f;
    const Aabb2 panel{Vec2{view.x - kWidth - 12.0f, view.y - 40.0f}, Vec2{view.x - 12.0f, view.y - 12.0f}};
    out.rect(panel, rgba(10, 11, 14, 190));
    out.rect_outline(panel, 1.0f, rgba(60, 64, 74));
    out.text(line, Vec2{panel.min.x + 8.0f, view.y - 34.0f}, 13, rgba(190, 196, 206));
    if (!assets_->texture_errors().empty()) {
        const std::string missing =
            format_localized(text("debug.missing"), assets_->texture_errors().size());
        out.text(missing, Vec2{panel.min.x + 8.0f, view.y - 60.0f}, 13, rgba(240, 120, 110));
    }
}

void MefemmaApp::on_resize(u32 width, u32 height) {
    (void)width;
    (void)height;
    // Scenes ask for viewport() every frame, so a resize needs no bookkeeping beyond the renderer's own.
}

void MefemmaApp::on_shutdown() {
    scenes_.clear();   // the scenes go before the services they were handed
    renderer_.reset();
    assets_.reset();
}

// --- GameServices ------------------------------------------------------------------------------------

const t2d::Locale& MefemmaApp::locale() const {
    return assets_ != nullptr ? assets_->locale() : fallback_locale_;
}

std::string_view MefemmaApp::text(std::string_view id) const {
    return assets_ != nullptr ? assets_->locale().text(id) : id;
}

t2d::TextMetrics MefemmaApp::measure_text(std::string_view value, t2d::u16 size_px) const {
    return renderer_ != nullptr ? renderer_->measure(value, size_px) : t2d::TextMetrics{};
}

t2d::Vec2 MefemmaApp::viewport() const {
    return t2d::Vec2{static_cast<f32>(renderer().width()), static_cast<f32>(renderer().height())};
}

void MefemmaApp::push_scene(std::unique_ptr<Scene> scene) { scenes_.push(std::move(scene)); }

void MefemmaApp::pop_scene() { scenes_.pop(); }

void MefemmaApp::replace_scene(std::unique_ptr<Scene> scene) { scenes_.replace(std::move(scene)); }

void MefemmaApp::quit() { ore::Application::quit(); }

} // namespace mefemma
