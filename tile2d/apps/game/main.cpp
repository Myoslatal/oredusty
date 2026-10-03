// Tile2D - the playable client. Modes: single player, host (listen server + local player) and join.
#include <t2d/render/game_app.h>

#include <ore/ore.h>

int main(int argc, char** argv) {
    t2d::GameOptions options = t2d::parse_game_options(ore::CommandLine::parse(argc, argv));
    t2d::GameApp application(std::move(options));
    return application.run(argc, argv);
}
