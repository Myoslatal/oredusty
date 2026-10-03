// Mine - the application shell: the start screen and the session screen.
//
// The shell deliberately does not create a server or a client yet. Layer content is not authored
// (see docs/GAME_DESIGN.md), so the session screen reports the session it *would* run instead of
// pretending to run one - and the moment content data exists, begin_session() is where the world,
// the server thread and the client are created.
#pragma once

#include <mine/menu.h>
#include <mine/registry.h>
#include <mine/session.h>

#include <t2d/render/sprite_batch.h>

#include <ore/app/application.h>

#include <string>

namespace mine {

using t2d::Scope;

struct MineOptions {
    /// Skip the start screen and go straight to the session screen (headless screenshots, tests).
    bool start_immediately = false;
    SessionConfig session{};
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
    void on_shutdown() override;
    [[nodiscard]] ore::ConstSpan<ore::CliOption> cli_options() const override;

private:
    enum class Screen : u8 { Start, Session };

    void handle_start_input();
    void begin_session(const SessionConfig& session);
    void draw_start_screen();
    void draw_session_screen();

    MineOptions options_{};
    Screen screen_ = Screen::Start;
    MenuModel menu_{};
    SessionConfig session_{};
    /// Filled from the designer's content data; a save stores the ids this hands out plus the table
    /// that maps them back to names (see registry.h).
    ContentRegistry registry_{};
    Scope<t2d::SpriteBatch> batch_;
    Scope<ore::rhi::Texture> font_atlas_;
    Scope<ore::rhi::Sampler> sampler_;
};

} // namespace mine
