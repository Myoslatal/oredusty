// Mefemma - scenes: the places a game is in, and the stack they live on.
//
// A scene is one screen or one mode: a title, a level, a menu, a pause overlay. It is **pure logic and a
// draw list** - no window, no GPU, no clock of its own - so a scene can be driven by a test at full speed
// and the same code runs in the game.
//
// The stack is how a game composes them:
//   * only the **top** scene is updated, which is what makes a pause menu a scene pushed on top;
//   * **every** scene is drawn, bottom to top, so an overlay scene is one more push and a HUD is a scene
//     that draws without updating anything;
//   * a scene that changes the stack does not change it in the middle of its own turn: the request is
//     queued and applied at the start of the next frame, so a scene can pop itself from inside update()
//     without walking out from under the loop.
//
// The services a scene is handed (GameServices) are the shell: input, settings, the string table, the
// clock and the stack. A game adds its own services by deriving from GameServices - the framework does
// not need to know what a game needs.
#pragma once

#include <mefemma/draw_list.h>
#include <mefemma/input.h>
#include <mefemma/settings.h>

#include <t2d/core/rng.h>
#include <t2d/core/types.h>
#include <t2d/text/locale.h>
#include <t2d/text/text_layout.h>

#include <memory>
#include <string_view>
#include <vector>

namespace mefemma {

using t2d::f32;
using t2d::i32;
using t2d::u16;
using t2d::u64;
using t2d::usize;

class Scene;

/// What a scene can ask of the shell.
///
/// Everything is a reference or a value a scene does not own: a scene never creates a window, a texture or
/// a font, and it never outlives the services that handed it one. The stack operations are requests - they
/// are applied by the SceneStack at the start of the next frame, never in the middle of a scene's update.
class GameServices {
public:
    virtual ~GameServices() = default;

    [[nodiscard]] virtual const InputState& input() const = 0;
    [[nodiscard]] virtual const Settings& settings() const = 0;
    /// The active language's string table. text() is the shortcut every label in a scene uses.
    [[nodiscard]] virtual const t2d::Locale& locale() const = 0;
    /// The string for \p id in the active language, with the engine's fallback chain: active, English, the
    /// id itself. A missing string is therefore visible as its id, never as a blank.
    [[nodiscard]] virtual std::string_view text(std::string_view id) const = 0;
    /// The box a line of text fills at \p size_px: what a caller measures before placing it. It is the same
    /// number the renderer will use, because both ask the same text engine.
    [[nodiscard]] virtual t2d::TextMetrics measure_text(std::string_view text, u16 size_px) const = 0;

    /// The game's own random stream. A game draws from it; the shell only seeds it (Settings::seed).
    [[nodiscard]] virtual t2d::Rng& random() = 0;
    [[nodiscard]] virtual f32 elapsed_seconds() const = 0;
    [[nodiscard]] virtual u64 frame() const = 0;
    /// The framebuffer size in pixels: what a scene lays out against, and what it gets again after a resize.
    [[nodiscard]] virtual t2d::Vec2 viewport() const = 0;

    virtual void push_scene(std::unique_ptr<Scene> scene) = 0;
    virtual void pop_scene() = 0;
    virtual void replace_scene(std::unique_ptr<Scene> scene) = 0;
    virtual void quit() = 0;
};

/// One screen or mode of a game.
class Scene {
public:
    virtual ~Scene() = default;

    /// A short name for logs. It is not translated: a log is read by the developer.
    [[nodiscard]] virtual const char* name() const { return "scene"; }

    /// Called when the scene becomes the top of the stack. A scene that was pushed on top of another sees
    /// the other one paused, not gone.
    virtual void on_enter(GameServices& services) { (void)services; }
    /// Called just before the scene leaves the stack.
    virtual void on_exit(GameServices& services) { (void)services; }
    /// Called when another scene is pushed on top of this one.
    virtual void on_pause(GameServices& services) { (void)services; }
    /// Called when the scene above this one is popped.
    virtual void on_resume(GameServices& services) { (void)services; }

    /// The frame's step. Only the top scene is updated; \p delta_seconds is the frame's own time, clamped
    /// by the shell so a long hitch cannot hand a scene a huge step.
    virtual void update(GameServices& services, f32 delta_seconds) {
        (void)services;
        (void)delta_seconds;
    }
    /// Everything the scene wants drawn this frame, in order. Every scene on the stack is drawn, bottom to
    /// top: a scene that wants to hide the one below draws a full-screen rectangle first.
    virtual void draw(GameServices& services, DrawList& out) const {
        (void)services;
        (void)out;
    }
};

/// The stack a game lives on. It owns its scenes and applies stack changes at a frame boundary.
class SceneStack {
public:
    SceneStack() = default;
    T2D_NON_MOVABLE(SceneStack);

    /// Asks for \p scene to be pushed on top. The request is applied by begin_frame() (or by the next
    /// update()), so the caller may be inside a scene's own update.
    void push(std::unique_ptr<Scene> scene);
    /// Asks for the top scene to be popped. Popping an empty stack does nothing.
    void pop();
    /// Asks for the top scene to be replaced by \p scene.
    void replace(std::unique_ptr<Scene> scene);

    /// Applies every pending stack change, in request order: exit/pause/enter/resume callbacks included.
    /// Safe to call more than once in a frame; the second call has nothing to do.
    void begin_frame(GameServices& services);
    /// Applies pending changes, then hands the frame to the top scene. Nothing on an empty stack.
    void update(GameServices& services, f32 delta_seconds);
    /// Hands every scene to \p out, bottom to top. Pending changes are **not** applied here: a host calls
    /// begin_frame() first, so what is drawn is the stack that was just updated.
    void draw(GameServices& services, DrawList& out) const;

    /// Drops every scene without callbacks. A host calls this on shutdown, after the frame loop has
    /// stopped: there is no next frame for an exit callback to mean anything in.
    void clear();

    [[nodiscard]] bool empty() const { return scenes_.empty(); }
    [[nodiscard]] usize size() const { return scenes_.size(); }
    [[nodiscard]] usize pending() const { return pending_.size(); }
    [[nodiscard]] Scene* top();
    [[nodiscard]] const Scene* top() const;

private:
    enum class OpKind : t2d::u8 { Push, Pop, Replace };
    struct Op {
        OpKind kind = OpKind::Push;
        std::unique_ptr<Scene> scene;
    };
    void apply(GameServices& services);

    std::vector<std::unique_ptr<Scene>> scenes_;
    std::vector<Op> pending_;
    bool applying_ = false;
};

} // namespace mefemma
