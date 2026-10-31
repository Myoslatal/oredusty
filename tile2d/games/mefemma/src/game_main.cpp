// Mefemma - the game's own entry point, which lives in a code table rather than in the executable.
//
// The launcher (src/main.cpp) merges the table with whatever mods are beside it and calls
// mefemma_game_main(); everything below is what main() would otherwise be. What changed is where it was
// compiled to: the game's code is placed in memory at start-up, so a mod table merged after it can replace
// a function of it and the game's own calls follow (docs/TABLES.md).
//
// **This is the file a game edits to add its own command line.** A flag that changes how the game starts is
// parsed here, stored in MefemmaOptions (app.h), and read by the shell.
#include <mefemma/app.h>

#include <ore/ore.h>

#include <t2d/core/executable.h>
#include <t2d/core/log.h>
#include <t2d/sim/open_world.h>

#include <cerrno>
#include <cstdlib>
#include <string>

/// The one function a mod replaces to show that the merge reaches the game's own code: it is called below,
/// from inside the game, and a mod table that defines it takes the call over. Everything else about a mod
/// overriding the game is the same mechanism with a longer name (docs/TABLES.md).
extern "C" const char* mefemma_game_banner() { return "Mefemma, unmodified"; }

extern "C" int mefemma_game_main(int argc, char** argv) {
    T2D_INFO("game: {}", mefemma_game_banner());
    const ore::CommandLine cli = ore::CommandLine::parse(argc, argv);

    mefemma::MefemmaOptions options;
    if (const auto settings = cli.value("settings"); settings.has_value()) options.settings_path = *settings;
    if (const auto assets = cli.value("assets"); assets.has_value()) options.assets_root = *assets;
    if (const auto language = cli.value("lang"); language.has_value()) {
        const std::optional<t2d::Language> parsed = t2d::parse_language(*language);
        if (!parsed.has_value()) {
            T2D_ERROR("unknown language '{}' (try en, zh-Hans or zh-Hant)", *language);
            return 1;
        }
        options.language = *parsed;
    }
    if (const auto font = cli.value("font"); font.has_value()) options.font_path = *font;
    if (const auto cjk = cli.value("cjk-font"); cjk.has_value()) options.cjk_font_path = *cjk;
    if (const auto scene = cli.value("scene"); scene.has_value()) options.start_scene = *scene;
    if (const auto redirect = cli.value("redirect"); redirect.has_value()) options.redirect_path = *redirect;
    if (const auto save_path = cli.value("save-path"); save_path.has_value()) options.save_path = *save_path;
    if (const auto world_file = cli.value("world-file"); world_file.has_value()) options.world_file = *world_file;
    if (const auto world_origin = cli.value("world-origin"); world_origin.has_value()) {
        const std::size_t separator = world_origin->find(',');
        if (separator == std::string::npos) {
            T2D_ERROR("--world-origin wants chunk coordinates like 34359738364,-34359738364, got '{}'", *world_origin);
            return 1;
        }
        const std::string x_text = world_origin->substr(0, separator);
        const std::string y_text = world_origin->substr(separator + 1);
        char* x_end = nullptr;
        char* y_end = nullptr;
        errno = 0;
        const long long x = std::strtoll(x_text.c_str(), &x_end, 10);
        const long long y = std::strtoll(y_text.c_str(), &y_end, 10);
        const long long limit = static_cast<long long>(t2d::OpenWorld::kMaxAbsoluteChunk);
        const bool parsed = !x_text.empty() && !y_text.empty() && x_end != nullptr && *x_end == '\0' &&
                            y_end != nullptr && *y_end == '\0' && errno == 0;
        if (!parsed || x < -limit || x > limit || y < -limit || y > limit) {
            T2D_ERROR("--world-origin wants two whole absolute chunks inside -{}..{}, got '{}'", limit, limit,
                      *world_origin);
            return 1;
        }
        options.world_origin_x = static_cast<t2d::i64>(x);
        options.world_origin_y = static_cast<t2d::i64>(y);
    }
    if (const auto seed = cli.uint_value("seed"); seed.has_value()) options.seed = *seed;
    if (const auto pointer = cli.value("pointer"); pointer.has_value()) {
        const std::size_t separator = pointer->find(',');
        if (separator == std::string::npos) {
            T2D_ERROR("--pointer wants a position like 640,360, got '{}'", *pointer);
            return 1;
        }
        options.pointer.x = static_cast<mefemma::f32>(std::strtof(pointer->substr(0, separator).c_str(), nullptr));
        options.pointer.y = static_cast<mefemma::f32>(std::strtof(pointer->substr(separator + 1).c_str(), nullptr));
        options.has_pointer = true;
    }

    mefemma::MefemmaApp application(std::move(options));
    return application.run(argc, argv);
}
