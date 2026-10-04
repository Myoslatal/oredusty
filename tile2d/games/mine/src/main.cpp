// Mine - the game executable. For now it is the start screen: pick a world, a language, a seed and a
// role.
#include <mine/app.h>

#include <ore/ore.h>

#include <t2d/core/log.h>
#include <t2d/text/locale.h>

#include <cstdlib>
#include <filesystem>
#include <format>

int main(int argc, char** argv) {
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
    for (const std::string& path : cli.values("pack")) options.pack_paths.push_back(path);
    for (const std::string& directory : cli.values("packs")) options.pack_directories.push_back(directory);
    for (const std::string& directory : cli.values("mods")) options.mod_directories.push_back(directory);
    // A "packs" directory beside the game is a default, not a promise: when it is not there, nothing
    // is reported, because nobody asked for it. An explicit --packs that is missing *is* reported.
    if (options.pack_directories.empty()) {
        std::error_code code;
        if (std::filesystem::is_directory("packs", code)) options.pack_directories.push_back("packs");
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

    mine::MineApp application(std::move(options));
    return application.run(argc, argv);
}
