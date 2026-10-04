// Mine - the game's own entry point, which now lives in a code table rather than in the executable.
//
// The launcher (src/main.cpp) merges the table with whatever mods are beside it and calls
// mine_game_main(); everything below is what used to be main(), unchanged. What changed is where it
// was compiled to: the game's code is placed in memory at start-up, so a mod table merged after it
// can replace a function of it and the game's own calls follow (docs/TABLES.md).
#include <mine/app.h>
#include <mine/content_search.h>

#include <ore/ore.h>

#include <t2d/core/executable.h>
#include <t2d/core/log.h>
#include <t2d/text/locale.h>

#include <cstdlib>
#include <filesystem>
#include <format>

/// The one function a mod replaces to show that the merge reaches the game's own code: it is called
/// below, from inside the game, and a mod table that defines it takes the call over. Everything else
/// about a mod overriding the game is the same mechanism with a longer name (docs/TABLES.md).
extern "C" const char* mine_game_banner() { return "Mine, unmodified"; }

extern "C" int mine_game_main(int argc, char** argv) {
    T2D_INFO("game: {}", mine_game_banner());
    const ore::CommandLine cli = ore::CommandLine::parse(argc, argv);

    mine::MineOptions options;
    if (const auto world = cli.value("world"); world.has_value()) {
        if (*world == "sandbox") options.session.mode = mine::Mode::Sandbox;
        else options.session.mode = *world == "endless" ? mine::Mode::Endless : mine::Mode::Story;
    }
    if (const auto seed = cli.uint_value("seed"); seed.has_value()) options.session.seed = *seed;
    if (const auto host = cli.bool_value("host"); host.has_value() && *host) options.session.role = mine::Role::Host;
    if (const auto connect = cli.value("connect"); connect.has_value()) {
        options.session.role = mine::Role::Join;
        options.session.connect_address = *connect;
    }
    if (const auto start = cli.bool_value("start"); start.has_value()) options.start_immediately = *start;
    if (const auto lang = cli.value("lang"); lang.has_value()) {
        const std::optional<t2d::Language> language = t2d::parse_language(*lang);
        if (!language.has_value()) {
            T2D_ERROR("unknown language '{}' (try en, zh-Hans or zh-Hant)", *lang);
            return 1;
        }
        options.language = *language;
    }
    if (const auto font = cli.value("font"); font.has_value()) options.font_path = *font;
    if (const auto cjk = cli.value("cjk-font"); cjk.has_value()) options.cjk_font_path = *cjk;
    if (const auto ui = cli.value("ui-text"); ui.has_value()) options.ui_text_path = *ui;

    // The sandbox: which content files describe the layer, how big it is, and what to do with it.
    for (const std::string& path : cli.values("content")) options.content_paths.push_back(path);
    if (const auto grid = cli.value("grid"); grid.has_value()) {
        const std::size_t separator = grid->find('x');
        if (separator == std::string::npos) {
            T2D_ERROR("--grid wants a size like 40x24, got '{}'", *grid);
            return 1;
        }
        options.grid_width = static_cast<mine::u32>(std::strtoul(grid->substr(0, separator).c_str(), nullptr, 10));
        options.grid_height = static_cast<mine::u32>(std::strtoul(grid->substr(separator + 1).c_str(), nullptr, 10));
        if (options.grid_width == 0 || options.grid_height == 0) {
            T2D_ERROR("--grid wants two positive numbers, got '{}'", *grid);
            return 1;
        }
    }
    if (const auto layers = cli.uint_value("tile-layers"); layers.has_value()) {
        options.tile_layers = static_cast<mine::i32>(*layers);
    }
    if (const auto layer = cli.uint_value("layer"); layer.has_value()) {
        options.start_layer = static_cast<mine::i32>(*layer);
    }
    if (const auto fill = cli.value("fill"); fill.has_value()) options.fill = *fill;
    if (const auto fill_layer = cli.value("fill-layer"); fill_layer.has_value()) options.fill_layer = *fill_layer;
    for (const std::string& directory : cli.values("packs")) options.pack_directories.push_back(directory);
    for (const std::string& directory : cli.values("mods")) options.mod_directories.push_back(directory);
    // Where the game looks without being told (mine/content_search.h): the "packs" directory beside
    // the executable - drop a pack or a mod there and run the game, wherever you are standing - and the
    // working directory's own "packs", which is the workspace a designer develops in. Neither is a
    // promise: a missing one is silent, because nobody asked for it, while an explicit --packs or
    // --mods that is missing *is* reported.
    //
    // The two lists default independently: --packs on its own still lets the game find its own mods.
    if (options.pack_directories.empty() || options.mod_directories.empty()) {
        const bool packs_by_default = options.pack_directories.empty();
        const bool mods_by_default = options.mod_directories.empty();
        const std::vector<std::string> defaults =
            mine::default_content_directories(t2d::executable_path(), std::filesystem::current_path().string());
        if (packs_by_default) options.pack_directories = defaults;
        if (mods_by_default) options.mod_directories = defaults;
        for (const std::string& directory : defaults) {
            // One line per directory, saying what it is being looked in for: "which directory did that
            // come from" is the first question a designer asks of a load they did not spell out.
            T2D_INFO("content: looking in '{}' for {} without being asked", directory,
                     packs_by_default && mods_by_default
                         ? "packs and mods"
                         : (packs_by_default ? "packs" : "mod packages"));
        }
    }
    if (const auto view = cli.value("view"); view.has_value()) {
        const std::size_t first = view->find(',');
        const std::size_t second = first == std::string::npos ? std::string::npos : view->find(',', first + 1);
        if (first == std::string::npos) {
            T2D_ERROR("--view wants x,y or x,y,zoom, got '{}'", *view);
            return 1;
        }
        const std::string y_text = view->substr(first + 1, second == std::string::npos ? second : second - first - 1);
        options.view_cell.x = static_cast<mine::f32>(std::strtof(view->substr(0, first).c_str(), nullptr));
        options.view_cell.y = static_cast<mine::f32>(std::strtof(y_text.c_str(), nullptr));
        if (second != std::string::npos) {
            options.view_zoom = static_cast<mine::f32>(std::strtof(view->substr(second + 1).c_str(), nullptr));
        }
        options.has_view = true;
    }
    if (const auto layout = cli.value("layout"); layout.has_value()) options.layout_path = *layout;
    if (const auto save = cli.value("save-layout"); save.has_value()) options.save_layout_path = *save;
    if (const auto dump = cli.bool_value("dump-layer"); dump.has_value()) options.dump_layer = *dump;
    if (const auto playtest = cli.bool_value("playtest"); playtest.has_value()) options.playtest = *playtest;
    if (const auto pointer = cli.value("pointer"); pointer.has_value()) {
        const std::size_t separator = pointer->find(',');
        if (separator == std::string::npos) {
            T2D_ERROR("--pointer wants a cell like 12,7, got '{}'", *pointer);
            return 1;
        }
        options.pointer_cell.x = static_cast<mine::f32>(std::strtof(pointer->substr(0, separator).c_str(), nullptr));
        options.pointer_cell.y = static_cast<mine::f32>(std::strtof(pointer->substr(separator + 1).c_str(), nullptr));
        options.has_pointer = true;
    }
    if (const auto list = cli.bool_value("content-list"); list.has_value()) options.content_list = *list;

    // The world view: straight into the mine, which layer, and what to put in it while the layer rules
    // are still the designer's to give.
    if (const auto view = cli.bool_value("world-view"); view.has_value()) options.world_view = *view;
    if (const auto layer = cli.uint_value("mine-layer"); layer.has_value()) {
        options.mine_layer = static_cast<mine::i32>(*layer);
    }
    if (const auto fill = cli.value("layer-fill"); fill.has_value()) options.layer_fill = *fill;

    mine::MineApp application(std::move(options));
    return application.run(argc, argv);
}
