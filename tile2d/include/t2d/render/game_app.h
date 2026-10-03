// Tile2D - the playable client application.
//
// It drives the whole semi-separated design from the client side:
//   mode single : local client thread + server thread in this process, shared memory link
//   mode host   : the same, plus a UDP listener so other players can join over KCP
//   mode join   : local client thread talking KCP to somebody else's server (no local simulation)
// The client logic is identical in all three cases - only the link differs.
#pragma once

#include <t2d/client/local_client.h>
#include <t2d/core/types.h>
#include <t2d/render/sprite_batch.h>
#include <t2d/render/tilemap_renderer.h>
#include <t2d/server/server_host.h>

#include <ore/app/application.h>

#include <memory>
#include <string>

namespace t2d {

struct GameOptions {
    enum class Mode : u8 { SinglePlayer, HostAndPlay, JoinRemote };
    Mode mode = Mode::SinglePlayer;
    std::string player_name = "player";
    std::string connect_address = "127.0.0.1:7777";
    u16 port = 7777;
    std::string level_path;   ///< empty = the built-in demo level
    u32 tick_rate = kTickRate;
    bool show_debug = false;
};

/// Owns the server host, the transport and the local client, and renders whatever the client knows.
class GameApp final : public ore::Application {
public:
    explicit GameApp(GameOptions options) : options_(std::move(options)) {}

protected:
    void on_configure(ore::AppConfig& config) override;
    void on_start() override;
    void on_update(f32 delta_seconds) override;
    void on_render(ore::RenderFrame& frame) override;
    void on_shutdown() override;
    ore::ConstSpan<ore::CliOption> cli_options() const override;

private:
    [[nodiscard]] PlayerCommand sample_input() const;
    void draw_hud();
    void draw_world(const Aabb2& view);
    void update_camera(f32 delta_seconds);

    GameOptions options_{};
    Scope<ServerHost> server_;
    Scope<net::UdpTransport> transport_;
    Scope<net::SharedLink> shared_client_link_;
    Scope<LocalClient> client_;
    net::ILink* link_ = nullptr;

    Scope<SpriteBatch> batch_;
    Scope<ore::rhi::Texture> tile_atlas_;
    Scope<ore::rhi::Texture> font_atlas_;
    Scope<ore::rhi::Sampler> sampler_;
    TilemapRenderer map_renderer_{};
    Vec2 camera_center_{};
    Vec2 camera_size_{1280.0f, 720.0f};
    f32 zoom_ = 2.0f;
    bool debug_ = false;
    f32 hud_scale_ = 1.0f;
    u64 started_ms_ = 0;
};

/// Reads --mode/--port/--connect/--name/--level from a command line into GameOptions.
[[nodiscard]] GameOptions parse_game_options(const ore::CommandLine& cli);

} // namespace t2d
