// Mefemma - the start screen: the sign, the game's name and the way into the game.
//
// The screen is built from the designer's picture (assets/textures/start_screen.png): a wooden sign with a
// tan paper face. Everything drawn on top - the name plaque and the five rows - is placed in **image
// coordinates** (1920 x 1080) and mapped onto whatever the window is, so the menu stays inside the paper
// at every size and aspect ratio.
//
// The rows are New Game, Save List, Settings, Mods and Quit. **The first four only say that they are not
// implemented yet** - this screen is the framework's, and the game behind each row is the designer's.
// Quit is real because a start screen that cannot leave is not a start screen. The action of a row lives
// in activate(); replacing the placeholder there is where a game starts.
#pragma once

#include <mefemma/scene.h>

#include <array>
#include <memory>

namespace mefemma {

/// What a row of the start screen does.
enum class StartAction : t2d::u8 { NewGame, LoadGame, Settings, Mods, Quit, Count };
inline constexpr usize kStartActionCount = static_cast<usize>(StartAction::Count);

class TitleScene final : public Scene {
public:
    [[nodiscard]] const char* name() const override { return "title"; }

    void on_enter(GameServices& services) override;
    void update(GameServices& services, f32 delta_seconds) override;
    void draw(GameServices& services, DrawList& out) const override;

    // --- what the screen is showing, for a test and for a game that embeds this screen -----------------
    [[nodiscard]] usize selected() const { return selected_; }
    [[nodiscard]] StartAction selected_action() const { return static_cast<StartAction>(selected_); }
    [[nodiscard]] bool modal_open() const { return modal_open_; }
    [[nodiscard]] StartAction modal_action() const { return modal_action_; }
    static constexpr usize item_count() { return kStartActionCount; }

    /// The screen rectangle row \p index covers at \p viewport: the same layout draw() uses, so a test
    /// or an embedding can hit-test a row without copying the numbers.
    [[nodiscard]] static t2d::Aabb2 item_rect(t2d::Vec2 viewport, usize index);

private:
    void move_selection(i32 delta);
    void activate(GameServices& services);

    usize selected_ = 0;
    bool modal_open_ = false;
    StartAction modal_action_ = StartAction::NewGame;
    /// Whether the pointer was inside the window on the previous frame: entering it is a pointer event too,
    /// and it selects the row it landed on without fighting the keyboard when the pointer is merely resting.
    bool pointer_was_inside_ = false;
};

/// The factory the shell starts a run with. A game with its own start screen replaces the call in
/// MefemmaApp::start_first_scene() and keeps this one as a reference.
[[nodiscard]] std::unique_ptr<Scene> make_title_scene();

} // namespace mefemma
