// Mine - the game executable. For now it is the start screen: pick a world, a seed and a role.
#include <mine/app.h>

#include <ore/ore.h>

#include <t2d/core/log.h>

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

    mine::MineApp application(std::move(options));
    return application.run(argc, argv);
}
