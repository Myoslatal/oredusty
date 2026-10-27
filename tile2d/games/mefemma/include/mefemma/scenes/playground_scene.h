// Mefemma - a playground: the shell running a scene of its own, with nothing to do in it.
//
// It exists to show the pieces working together - a camera, the frame's input, a grid drawn from world
// coordinates, a pointer, and the services a scene talks to - and to be replaced. It is **not** a game:
// there is no objective, no entity, no rule. A game starts by copying this file's shape into a scene of
// its own and putting its rules where the comment says so.
#pragma once

#include <mefemma/scene.h>

#include <t2d/core/camera2d.h>

#include <memory>

namespace mefemma {

class PlaygroundScene final : public Scene {
public:
    PlaygroundScene();

    [[nodiscard]] const char* name() const override { return "playground"; }

    void on_enter(GameServices& services) override;
    void update(GameServices& services, f32 delta_seconds) override;
    void draw(GameServices& services, DrawList& out) const override;

private:
    t2d::Camera2D camera_{};
    t2d::Vec2 pointer_world_{};
    bool pointer_inside_ = false;
    f32 seconds_ = 0.0f;
};

[[nodiscard]] std::unique_ptr<Scene> make_playground_scene();

} // namespace mefemma
