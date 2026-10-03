#include <t2d/render/game_app.h>

#include <t2d/core/log.h>
#include <t2d/core/time.h>
#include <t2d/render/atlas.h>

#include <ore/core/file.h>

#include <algorithm>
#include <format>

namespace t2d {
namespace {

constexpr u32 kClearColor = 0xFF1A1622u;   ///< night sky, matches the level mood

[[nodiscard]] Vec2 clamp_camera(const Vec2& center, const Vec2& half_size, const TileMap& map,
                                const PlayerTuning& tuning) {
    const Vec2 world = map.world_size();
    Vec2 clamped = center;
    if (world.x > half_size.x * 2.0f) clamped.x = clamp(clamped.x, half_size.x, world.x - half_size.x);
    else clamped.x = world.x * 0.5f;
    if (world.y > half_size.y * 2.0f) clamped.y = clamp(clamped.y, half_size.y, world.y - half_size.y);
    else clamped.y = world.y * 0.5f;
    (void)tuning;
    return clamped;
}

} // namespace

GameOptions parse_game_options(const ore::CommandLine& cli) {
    GameOptions options;
    if (const auto mode = cli.value("mode"); mode.has_value()) {
        if (*mode == "single") options.mode = GameOptions::Mode::SinglePlayer;
        else if (*mode == "host") options.mode = GameOptions::Mode::HostAndPlay;
        else if (*mode == "join") options.mode = GameOptions::Mode::JoinRemote;
    }
    if (const auto port = cli.uint_value("port"); port.has_value()) options.port = static_cast<u16>(*port);
    if (const auto address = cli.value("connect"); address.has_value()) {
        options.connect_address = *address;
        options.mode = GameOptions::Mode::JoinRemote;
    }
    if (const auto name = cli.value("name"); name.has_value()) options.player_name = *name;
    if (const auto level = cli.value("level"); level.has_value()) options.level_path = *level;
    if (const auto debug = cli.bool_value("debug-draw"); debug.has_value()) options.show_debug = *debug;
    return options;
}

ore::ConstSpan<ore::CliOption> GameApp::cli_options() const {
    static const ore::CliOption kOptions[] = {
        {"mode", "<single|host|join>", "single player (default), host an online game, or join one"},
        {"port", "<port>", "UDP port to listen on in host mode (default 7777)"},
        {"connect", "<host:port>", "join a remote server (implies --mode join)"},
        {"name", "<player>", "player name shown in the HUD"},
        {"level", "<path>", "ASCII level file; the built-in demo level is used when omitted"},
        {"debug-draw", "<0|1>", "draw player bounding boxes and velocity vectors"},
    };
    return ore::ConstSpan<ore::CliOption>(kOptions, std::size(kOptions));
}

void GameApp::on_configure(ore::AppConfig& config) {
    config.name = std::format("Tile2D - {}", options_.player_name);
    config.window.title = std::format("Tile2D ({})", options_.player_name);
    config.window.width = 1280;
    config.window.height = 720;
    config.depth_format = VK_FORMAT_UNDEFINED;  // pure 2D: no depth buffer needed
    config.stats_interval = 0.0f;
}

void GameApp::on_start() {
    ore::rhi::GraphicsContext& context = this->context();
    started_ms_ = now_ms();

    // ---------------------------------------------------------------- level ---
    TileMap map;
    Tileset tileset = Tileset::default_platformer();
    const std::string level_path = options_.level_path.empty()
                                       ? ore::find_data_file("assets/levels/demo.txt")
                                       : options_.level_path;
    if (const auto rows = ore::read_text_file(level_path); rows.has_value()) {
        std::vector<std::string> lines;
        std::string current;
        for (char character : *rows) {
            if (character == '\n') {
                if (!current.empty() && current.back() == '\r') current.pop_back();
                if (!current.empty()) lines.push_back(current);
                current.clear();
            } else {
                current.push_back(character);
            }
        }
        if (!current.empty()) lines.push_back(current);
        if (const auto parsed = TileMap::from_ascii(lines, TileMap::default_legend(), 16.0f); parsed.has_value()) {
            map = *parsed;
            T2D_INFO("level '{}': {}x{} tiles", level_path, map.width(), map.height());
        }
    }
    if (map.empty()) {
        map = TileMap(64, 32, 16.0f);
        for (i32 x = 0; x < 64; ++x) {
            map.set(x, 24, 3);
            map.set(x, 25, 2);
            for (i32 y = 26; y < 32; ++y) map.set(x, y, 2);
        }
        for (i32 x = 10; x < 14; ++x) map.set(x, 20, 4);
        T2D_WARN("level '{}' could not be read, using a flat placeholder", level_path);
    }

    // ------------------------------------------------------------ simulation --
    WorldConfig world_config;
    world_config.map = map;
    world_config.tileset = tileset;

    ServerConfig server_config;
    server_config.world = world_config;
    server_config.local_client = true;
    server_config.port = options_.mode == GameOptions::Mode::HostAndPlay ? options_.port : 0;
    server_ = ServerHost::create(server_config);
    if (server_ == nullptr) {
        T2D_FATAL("could not start the server thread");
        return;
    }
    // The server owns one end of the shared channel; the other end becomes the local client's link.
    // Handing it over is the whole "single player is just a local client" trick.
    shared_client_link_ = Scope<net::SharedLink>(server_->take_local_client_link());
    if (shared_client_link_ == nullptr) {
        T2D_FATAL("the server did not expose the local client end of the shared channel");
        return;
    }
    server_->start();

    if (options_.mode != GameOptions::Mode::JoinRemote) {
        link_ = shared_client_link_.get();
    } else {
        net::UdpTransport::Config transport_config;
        transport_config.port = 0;  // ephemeral
        transport_config.server_mode = false;
        transport_config.link.kcp.no_delay = true;
        transport_ = net::UdpTransport::create(transport_config);
        const auto endpoint = net::Endpoint::parse(options_.connect_address, options_.port);
        if (transport_ == nullptr || !endpoint.has_value()) {
            T2D_FATAL("cannot connect to '{}'", options_.connect_address);
            return;
        }
        link_ = transport_->connect(*endpoint);
        T2D_INFO("joining {} over KCP/UDP from local port {}", endpoint->to_string(), transport_->port());
    }

    if (link_ == nullptr) {
        T2D_FATAL("no link for the local client");
        return;
    }

    ClientConfig client_config;
    client_config.player_name = options_.player_name;
    client_ = LocalClient::create(client_config, link_);
    client_->start(now_ms());

    // ------------------------------------------------------------- renderer ---
    SpriteBatch::Options batch_options;
    batch_options.color_format = renderer().color_format();
    batch_options.vertex_shader_path = shader_path("sprite.vert.spv");
    batch_options.fragment_shader_path = shader_path("sprite.frag.spv");
    batch_ = SpriteBatch::create(context, batch_options);
    if (batch_ == nullptr) T2D_FATAL("the sprite batch could not be created");

    tile_atlas_ = context.create_texture(make_tileset_atlas(), true, "tileset.atlas");
    font_atlas_ = context.create_texture(make_font_atlas(), false, "font.atlas");
    ore::rhi::SamplerDesc sampler_desc;
    sampler_desc.min_filter = VK_FILTER_NEAREST;
    sampler_desc.mag_filter = VK_FILTER_NEAREST;
    sampler_desc.mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_ = ore::rhi::Sampler::create(context.device(), sampler_desc);

    // The font atlas is drawn in a second pass (different texture), the tiles and sprites share the
    // first one.
    debug_ = options_.show_debug;
    T2D_INFO("client ready: mode {} (link {})", static_cast<int>(options_.mode), link_->kind());
}

PlayerCommand GameApp::sample_input() const {
    PlayerCommand command;
    const ore::Window* window = this->window();
    if (window == nullptr) {
        // Headless: walk right and jump periodically so screenshots show something alive.
        const u64 elapsed = now_ms() - started_ms_;
        if (elapsed % 2000 < 1200) command.buttons |= kButtonRight;
        else command.buttons |= kButtonLeft;
        if ((elapsed / 700) % 2 == 0) command.buttons |= kButtonJumpHeld | kButtonJumpPressed;
        return command;
    }
    const ore::InputState& input = window->input();
    if (input.key_down(ore::Key::A) || input.key_down(ore::Key::Left)) command.buttons |= kButtonLeft;
    if (input.key_down(ore::Key::D) || input.key_down(ore::Key::Right)) command.buttons |= kButtonRight;
    if (input.key_down(ore::Key::S) || input.key_down(ore::Key::Down)) command.buttons |= kButtonDown;
    if (input.key_down(ore::Key::Space) || input.key_down(ore::Key::W) || input.key_down(ore::Key::Up)) {
        command.buttons |= kButtonJumpHeld;
    }
    if (input.key_pressed(ore::Key::Space) || input.key_pressed(ore::Key::W) || input.key_pressed(ore::Key::Up)) {
        command.buttons |= kButtonJumpPressed;
    }
    if (input.shift_down()) command.buttons |= kButtonRun;
    return command;
}

void GameApp::update_camera(f32 delta_seconds) {
    const LocalClient* client = client_.get();
    if (client == nullptr) return;
    const std::vector<RenderPlayer>& players = client->render_players();
    Vec2 target = camera_center_;
    for (const RenderPlayer& player : players) {
        if (!player.local) continue;
        target = Vec2{player.position.x + 5.0f, player.position.y + 7.0f};
        break;
    }
    const f32 smoothing = 1.0f - std::exp(-8.0f * delta_seconds);
    camera_center_ = lerp(camera_center_, target, smoothing);
    if (const ore::Window* window = this->window(); window != nullptr) {
        const f32 zoom = clamp(zoom_, 1.0f, 4.0f);
        camera_size_ = Vec2{static_cast<f32>(window->width()) / zoom, static_cast<f32>(window->height()) / zoom};
    }
    if (client->map() != nullptr) {
        camera_center_ = clamp_camera(camera_center_, camera_size_ * 0.5f, *client->map(), PlayerTuning{});
    }
    if (const ore::Window* window = this->window(); window != nullptr) {
        const f32 scroll = window->input().scroll_y();
        if (scroll != 0.0f) zoom_ = clamp(zoom_ * (1.0f + scroll * 0.1f), 1.0f, 4.0f);
    }
}

void GameApp::on_update(f32 delta_seconds) {
    if (client_ == nullptr) return;
    const PlayerCommand command = sample_input();
    client_->update(now_ms(), command);

    if (ore::Window* window = this->window(); window != nullptr) {
        const ore::InputState& input = window->input();
        if (input.key_pressed(ore::Key::F1)) debug_ = !debug_;
        if (input.key_pressed(ore::Key::F2)) hud_scale_ = hud_scale_ > 1.4f ? 1.0f : hud_scale_ + 0.5f;
        if (input.key_pressed(ore::Key::Q)) zoom_ = clamp(zoom_ - 0.25f, 1.0f, 4.0f);
        if (input.key_pressed(ore::Key::E)) zoom_ = clamp(zoom_ + 0.25f, 1.0f, 4.0f);
    }
    update_camera(delta_seconds);

    // The simulation ticks are driven by the client; keep the world tick in sync for rendering.
    if (server_ != nullptr) {
        const ServerStats stats = server_->stats();
        (void)stats;
    }
}

void GameApp::draw_world(const Aabb2& view) {
    const LocalClient* client = client_.get();
    if (client == nullptr || client->world() == nullptr) return;
    const World& world = *client->world();

    map_renderer_.reset_stats();
    map_renderer_.options().draw_debug_boxes = debug_;
    map_renderer_.draw_map(*batch_, world.map(), world.tileset(), view);
    map_renderer_.draw_pickups(*batch_, world.pickups());
    map_renderer_.draw_players(*batch_, client->render_players(), client->local_player_id(), world.tuning());
    if (debug_) map_renderer_.draw_debug(*batch_, client->render_players(), world.tuning());
}

void GameApp::draw_hud() {
    const LocalClient* client = client_.get();
    if (client == nullptr) return;
    const ClientStats& stats = client->stats();
    const Predictor::Stats& prediction = client->prediction_stats();

    const f32 x = camera_center_.x - camera_size_.x * 0.5f + 6.0f;
    const f32 y = camera_center_.y - camera_size_.y * 0.5f + 6.0f;
    const f32 line = (static_cast<f32>(kFontGlyphHeight) + 3.0f) * hud_scale_;
    const u32 text_color = 0xFFE8F0F8u;
    const u32 warn_color = 0xFF60D0FFu;

    // The HUD is drawn in world space (the camera is axis aligned), in two columns of fixed width so
    // the numbers do not jitter.
    batch_->draw_rect(Aabb2{Vec2{x - 4.0f, y - 4.0f},
                            Vec2{x + 168.0f * hud_scale_, y + line * 9.0f + 4.0f}},
                      0xA0000000u);

    const f32 right = x + 170.0f * hud_scale_;
    batch_->draw_text(x, y, hud_scale_, text_color, "TILE2D");
    batch_->draw_text(x, y + line, hud_scale_, text_color,
                      std::format("LINK {}", link_ != nullptr ? link_->kind() : "none"));
    batch_->draw_text(x, y + line * 2.0f, hud_scale_, text_color, std::format("STATUS {}", client->status()));
    batch_->draw_text(x, y + line * 3.0f, hud_scale_, text_color,
                      std::format("TICK {}  RTT {}", stats.server_tick, stats.rtt_ms));
    batch_->draw_text(x, y + line * 4.0f, hud_scale_, text_color,
                      std::format("SNAP {} ({} full {} delta)", stats.snapshots_received,
                                  stats.full_snapshots_received, stats.delta_snapshots_received));
    batch_->draw_text(x, y + line * 5.0f, hud_scale_, stats.desyncs == 0 ? text_color : warn_color,
                      std::format("DESYNC {}  DROP {}", stats.desyncs, stats.snapshots_dropped));
    batch_->draw_text(x, y + line * 6.0f, hud_scale_, text_color,
                      std::format("PRED {} PEND {}", stats.mispredictions, stats.pending_commands));
    batch_->draw_text(x, y + line * 7.0f, hud_scale_, text_color,
                      std::format("ERR {:.2f} AVG {:.2f}", static_cast<f64>(prediction.last_error),
                                  static_cast<f64>(prediction.average_error)));
    batch_->draw_text(x, y + line * 8.0f, hud_scale_, text_color,
                      std::format("TILES {} SPRITES {}", map_renderer_.stats().tiles_drawn,
                                  map_renderer_.stats().sprites));

    if (server_ != nullptr) {
        const ServerStats server_stats = server_->stats();
        batch_->draw_text(right, y, hud_scale_, text_color,
                          std::format("SERVER {} PLAYERS {}", server_stats.tick, server_stats.players));
        batch_->draw_text(right, y + line, hud_scale_, text_color,
                          std::format("LOCAL {} REMOTE {}", server_stats.local_players, server_stats.remote_players));
        batch_->draw_text(right, y + line * 2.0f, hud_scale_, text_color,
                          std::format("TICKMS {:.2f}", static_cast<f64>(server_stats.average_tick_ms)));
        batch_->draw_text(right, y + line * 3.0f, hud_scale_, text_color,
                          std::format("OUT {:.1f} KB/S", server_stats.bytes_sent_per_second / 1024.0));
    }

    // Coin counter for the local player, centred at the top.
    if (const World* world = client->world(); world != nullptr) {
        if (const PlayerState* local = world->find_player(client->local_player_id())) {
            const std::string coins = std::format("COINS {}", local->coins);
            const f32 width = font_text_width(coins, hud_scale_ * 1.5f);
            batch_->draw_text(camera_center_.x - width * 0.5f, y, hud_scale_ * 1.5f, 0xFFF6C846u, coins);
        }
    }
}

void GameApp::on_render(ore::RenderFrame& frame) {
    VkClearValue clear{};
    clear.color = {{0.043f, 0.035f, 0.078f, 1.0f}};
    renderer().begin_pass(clear, 1.0f);

    const Aabb2 view = Aabb2::from_center(camera_center_, camera_size_ * 0.5f);

    // Pass 1: tiles, pickups and players share the tileset atlas.
    batch_->begin(frame, camera_center_, camera_size_, *tile_atlas_, sampler_->handle());
    draw_world(view);
    batch_->end();

    // Pass 2: text uses the font atlas, so it needs its own batch (one extra draw call).
    batch_->begin(frame, camera_center_, camera_size_, *font_atlas_, sampler_->handle());
    draw_hud();
    batch_->end();

    frame.counters.draw_calls = batch_->draw_calls() + 1;
    frame.counters.triangles = batch_->quads() * 2;
    renderer().end_pass();
}

void GameApp::on_shutdown() {
    if (client_ != nullptr) client_->disconnect();
    if (server_ != nullptr) server_->stop();
    batch_.reset();
    tile_atlas_.reset();
    font_atlas_.reset();
    sampler_.reset();
}

} // namespace t2d
