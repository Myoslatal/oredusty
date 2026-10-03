// Mine - the game executable. For now it is the start screen: pick a world, a language, a seed and a
// role.
#include <mine/app.h>

#include <ore/ore.h>

#include <t2d/core/log.h>
#include <t2d/text/locale.h>

#include <format>

int main(int argc, char** argv) {
    const ore::CommandLine cli = ore::CommandLine::parse(argc, argv);

    mine::MineOptions options;
    if (const auto world = cli.value("world"); world.has_value()) {
        options.session.mode = *world == "endless" ? mine::Mode::Endless : mine::Mode::Story;
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

    mine::MineApp application(std::move(options));
    return application.run(argc, argv);
}
