// Mefemma - the title scene, and an example of everything a scene is.
//
// A scene is a screen with two jobs: read the input and ask for a drawing. This one reads Enter (start the
// playground) and Escape (quit the game), and draws the game's name, a tagline, a picture and the two keys.
// It is a starting point, not a design: a game replaces it with its own title screen, keeping the shape.
#pragma once

#include <mefemma/scene.h>

#include <memory>

namespace mefemma {

class TitleScene final : public Scene {
public:
    [[nodiscard]] const char* name() const override { return "title"; }

    void on_enter(GameServices& services) override;
    void update(GameServices& services, f32 delta_seconds) override;
    void draw(GameServices& services, DrawList& out) const override;

private:
    /// Seconds since the scene was entered. Used only for the slow decoration and the blinking prompt: a
    /// scene that wanted a real clock would ask the services for one.
    f32 seconds_ = 0.0f;
};

/// The factory the shell starts a run with. A game that wants another first scene replaces the call in
/// MefemmaApp::start_first_scene() and keeps this one as a reference.
[[nodiscard]] std::unique_ptr<Scene> make_title_scene();

} // namespace mefemma
