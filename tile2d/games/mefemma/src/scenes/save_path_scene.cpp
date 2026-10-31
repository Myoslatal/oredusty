#include <mefemma/scenes/save_path_scene.h>

#include <mefemma/redirect.h>

#include <t2d/core/executable.h>
#include <t2d/core/log.h>
#include <t2d/text/utf8.h>

#include <algorithm>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace mefemma {
namespace {

constexpr u32 kBackground = rgba(10, 11, 15);
constexpr u32 kPanel = rgba(30, 25, 18);
constexpr u32 kPanelEdge = rgba(255, 166, 0);
constexpr u32 kPanelHairline = rgba(255, 196, 84, 90);
constexpr u32 kField = rgba(8, 9, 12);
constexpr u32 kFieldEdge = rgba(120, 130, 150);
constexpr u32 kText = rgba(240, 232, 214);
constexpr u32 kDim = rgba(160, 150, 132);
constexpr u32 kAccent = rgba(255, 196, 84);
constexpr u32 kBad = rgba(240, 120, 110);
constexpr u32 kCaret = rgba(255, 220, 150);

/// True for text with nothing but ASCII whitespace in it: " " is as empty as "" for the prompt.
[[nodiscard]] bool is_blank(std::string_view text) {
    for (const char character : text) {
        if (character != ' ' && character != '\t' && character != '\n' && character != '\r' &&
            character != '\f' && character != '\v') {
            return false;
        }
    }
    return true;
}

template <class... Args>
[[nodiscard]] std::string format_localized(std::string_view pattern, Args&&... args) {
    return std::vformat(pattern, std::make_format_args(args...));
}

[[nodiscard]] u16 fitted_size(GameServices& services, std::string_view text, u16 base, f32 max_width) {
    u16 size = base;
    while (size > 8 && services.measure_text(text, size).width > max_width) --size;
    return size;
}

void centred_text(DrawList& out, GameServices& services, std::string_view text, t2d::Vec2 centre, u16 size,
                  u32 color) {
    const t2d::TextMetrics metrics = services.measure_text(text, size);
    out.text(std::string(text), t2d::Vec2{centre.x - metrics.width * 0.5f, centre.y - metrics.height * 0.5f},
             size, color);
}

/// What the field can show, and where the caret sits inside it (a byte offset on a character boundary).
struct PathFieldView {
    std::string shown;
    usize caret = 0;
};

[[nodiscard]] std::vector<usize> utf8_boundaries(std::string_view text) {
    std::vector<usize> boundaries{0};
    for (usize index = 0; index < text.size();) {
        const t2d::Codepoint point = t2d::utf8_decode(text, index);
        index += point.size == 0 ? 1 : point.size;
        boundaries.push_back(index);
    }
    return boundaries;
}

/// The part of the path the field has room for, with the caret at its real position.
///
/// The whole path is shown when it fits. When it does not, a window that contains the caret is shown: a
/// tail with a leading "..." when the caret is in the text's end, or a prefix with a trailing "..." when
/// the caret is near the start. Text after the caret is therefore visible too - moving home must not
/// blank the answer the player is editing.
[[nodiscard]] PathFieldView fitted_path(GameServices& services, std::string_view text, usize caret, u16 size,
                                        f32 max_width) {
    if (services.measure_text(text, size).width <= max_width) {
        return PathFieldView{std::string(text), caret};
    }

    const std::vector<usize> boundaries = utf8_boundaries(text);
    usize tail_start = text.size();
    for (const usize boundary : boundaries) {
        const std::string candidate = "..." + std::string(text.substr(boundary));
        if (services.measure_text(candidate, size).width <= max_width) {
            tail_start = boundary;
            break;
        }
    }

    if (caret < tail_start) {
        // The caret is before the tail window: show the beginning instead, keeping the caret in view.
        usize prefix_end = 0;
        for (const usize boundary : boundaries) {
            if (boundary < caret) continue;
            const std::string candidate = std::string(text.substr(0, boundary)) + "...";
            if (services.measure_text(candidate, size).width > max_width) break;
            prefix_end = boundary;
        }
        if (prefix_end >= caret) {
            PathFieldView view;
            view.shown = std::string(text.substr(0, prefix_end));
            if (prefix_end < text.size()) view.shown += "...";
            view.caret = caret;
            return view;
        }
    }

    PathFieldView view;
    view.shown = "..." + std::string(text.substr(tail_start));
    view.caret = caret >= tail_start ? 3u + (caret - tail_start) : 0u;
    return view;
}

} // namespace

std::unique_ptr<Scene> make_save_path_scene(std::string redirect_path, std::string default_save_path,
                                            SavePathScene::StartFactory start) {
    return std::make_unique<SavePathScene>(std::move(redirect_path), std::move(default_save_path), std::move(start));
}

SavePathScene::SavePathScene(std::string redirect_path, std::string default_save_path, StartFactory start)
    : redirect_path_(std::move(redirect_path)),
      default_save_path_(std::move(default_save_path)),
      start_(std::move(start)) {}

void SavePathScene::on_enter(GameServices& services) {
    (void)services;
    input_.set_text(default_save_path_);
    T2D_INFO("first-run: save path prompt (redirect '{}', default '{}')", redirect_path_, default_save_path_);
}

void SavePathScene::clear_error() {
    error_kind_ = ErrorKind::None;
    error_detail_.clear();
}

void SavePathScene::update(GameServices& services, f32 delta_seconds) {
    (void)delta_seconds;
    const InputState& input = services.input();
    bool edited = false;
    if (!input.text_input().empty()) edited = input_.insert(input.text_input()) > 0;
    if (input.key_pressed(Key::Backspace)) edited = input_.backspace() || edited;
    if (input.key_pressed(Key::Delete)) edited = input_.erase() || edited;
    if (edited) clear_error();

    if (input.key_pressed(Key::Left)) input_.move_left();
    if (input.key_pressed(Key::Right)) input_.move_right();
    if (input.key_pressed(Key::Home)) input_.home();
    if (input.key_pressed(Key::End)) input_.end();

    if (input.key_pressed(Key::Enter) || input.key_pressed(Key::KpEnter)) {
        submit(services);
        return;
    }
    if (input.key_pressed(Key::Escape)) services.quit();
}

void SavePathScene::submit(GameServices& services) {
    if (is_blank(input_.text())) {
        error_kind_ = ErrorKind::Empty;
        error_detail_.clear();
        T2D_WARN("first-run: a save directory is required");
        return;
    }

    // A relative answer is relative to the redirect file's own directory, which is the directory the game
    // was configured in - not the player's working directory.
    const std::string base = t2d::parent_directory_of(redirect_path_);
    std::string reason;
    const std::optional<std::string> normalized = normalize_save_path(input_.text(), base, &reason);
    if (!normalized.has_value()) {
        error_kind_ = ErrorKind::Failed;
        error_detail_ = std::move(reason);
        T2D_WARN("first-run: save path '{}': {}", input_.text(), error_detail_);
        return;
    }
    if (!ensure_save_directory(*normalized, &reason)) {
        error_kind_ = ErrorKind::Failed;
        error_detail_ = std::move(reason);
        T2D_WARN("first-run: {}", error_detail_);
        return;
    }
    if (!save_redirect(redirect_path_, *normalized, &reason)) {
        error_kind_ = ErrorKind::Failed;
        error_detail_ = std::move(reason);
        T2D_WARN("first-run: {}", error_detail_);
        return;
    }

    clear_error();
    T2D_INFO("first-run: save directory '{}' recorded in '{}'", *normalized, redirect_path_);
    if (start_) {
        if (std::unique_ptr<Scene> next = start_(*normalized)) services.replace_scene(std::move(next));
    }
}

void SavePathScene::draw(GameServices& services, DrawList& out) const {
    const t2d::Vec2 view = services.viewport();
    out.rect(Aabb2{Vec2{0.0f, 0.0f}, view}, kBackground);

    const f32 panel_width = std::min(view.x * 0.80f, 980.0f);
    const f32 panel_height = std::min(view.y * 0.58f, 380.0f);
    const Aabb2 panel{Vec2{(view.x - panel_width) * 0.5f, (view.y - panel_height) * 0.5f},
                      Vec2{(view.x + panel_width) * 0.5f, (view.y + panel_height) * 0.5f}};
    const f32 centre_x = (panel.min.x + panel.max.x) * 0.5f;
    const f32 pad = std::clamp(view.x * 0.045f, 18.0f, 48.0f);
    const f32 text_width = panel.width() - pad * 2.0f;

    out.rect(panel.translated(Vec2{6.0f, 8.0f}), rgba(0, 0, 0, 120));
    out.rect(panel, kPanel);
    out.rect_outline(panel, 2.0f, kPanelEdge);
    out.rect_outline(panel.expanded(-10.0f), 1.0f, kPanelHairline);

    // The game's name is small, the question is the point, and the field is where the answer goes.
    const std::string game_title(services.text("start.title"));
    centred_text(out, services, game_title, Vec2{centre_x, panel.min.y + 24.0f},
                 fitted_size(services, game_title, 14, text_width), kDim);

    const std::string heading(services.text("savepath.title"));
    centred_text(out, services, heading, Vec2{centre_x, panel.min.y + panel_height * 0.17f},
                 fitted_size(services, heading, 34, text_width), kAccent);

    const std::string prompt(services.text("savepath.prompt"));
    centred_text(out, services, prompt, Vec2{centre_x, panel.min.y + panel_height * 0.28f},
                 fitted_size(services, prompt, 17, text_width), kText);

    const f32 field_top = panel.min.y + panel_height * 0.38f;
    const f32 field_height = std::clamp(panel_height * 0.15f, 42.0f, 60.0f);
    const Aabb2 field{Vec2{panel.min.x + pad, field_top}, Vec2{panel.max.x - pad, field_top + field_height}};
    out.rect(field.translated(Vec2{2.0f, 3.0f}), rgba(0, 0, 0, 120));
    out.rect(field, kField);
    out.rect_outline(field, 2.0f, kFieldEdge);

    const u16 path_size = static_cast<u16>(std::clamp(view.y / 34.0f, 14.0f, 24.0f));
    const PathFieldView field_view =
        fitted_path(services, input_.text(), input_.caret(), path_size, field.width() - 28.0f);
    const t2d::TextMetrics metrics = services.measure_text(field_view.shown, path_size);
    const t2d::Vec2 pen{field.min.x + 14.0f, (field.min.y + field.max.y) * 0.5f - metrics.height * 0.5f};
    out.text(field_view.shown, pen, path_size, kText);
    // Blink twice a second; a test's fake clock reports 0, which is an "on" frame.
    if (static_cast<u32>(services.elapsed_seconds() * 2.0f) % 2u == 0u) {
        const t2d::TextMetrics before_caret =
            services.measure_text(field_view.shown.substr(0, field_view.caret), path_size);
        const f32 caret_x = pen.x + before_caret.width + 1.0f;
        out.rect(Aabb2{Vec2{caret_x, pen.y}, Vec2{caret_x + 2.0f, pen.y + metrics.height}}, kCaret);
    }

    if (error_kind_ != ErrorKind::None) {
        const std::string line =
            error_kind_ == ErrorKind::Empty
                ? std::string(services.text("savepath.error.empty"))
                : format_localized(services.text("savepath.error.failed"), error_detail_);
        centred_text(out, services, line, Vec2{centre_x, field.max.y + 34.0f},
                     fitted_size(services, line, 16, text_width), kBad);
    }

    const std::string hint(services.text("savepath.hint"));
    centred_text(out, services, hint, Vec2{centre_x, panel.max.y - 32.0f},
                 fitted_size(services, hint, 15, text_width), kDim);
}

} // namespace mefemma
