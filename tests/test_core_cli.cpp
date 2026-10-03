#include <ore/core/cli.h>

#include <support/test_support.h>

#include <string>
#include <vector>

using ore::CommandLine;

namespace {

CommandLine make_cli(std::vector<std::string> args) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (std::string& arg : args) argv.push_back(arg.data());
    return CommandLine::parse(static_cast<int>(argv.size()), argv.data());
}

} // namespace

ORE_TEST(cli_parses_flags_and_values) {
    const CommandLine cli = make_cli({"app", "--headless", "--frames", "240", "--size=1280x720", "asset.gltf"});
    ORE_CHECK_EQ(cli.program_name(), std::string("app"));
    ORE_CHECK(cli.has("headless"));
    ORE_CHECK_EQ(cli.uint_value("frames").value_or(0u), 240u);
    ORE_CHECK_EQ(cli.value("size").value_or(""), std::string("1280x720"));
    ORE_CHECK_EQ(cli.bool_value("headless").value_or(false), true);
    ORE_REQUIRE(cli.positional().size() == 1u);
    ORE_CHECK_EQ(cli.positional()[0], std::string("asset.gltf"));
}

ORE_TEST(cli_bool_forms) {
    ORE_CHECK_EQ(make_cli({"a", "--vsync=0"}).bool_value("vsync").value_or(true), false);
    ORE_CHECK_EQ(make_cli({"a", "--vsync=off"}).bool_value("vsync").value_or(true), false);
    ORE_CHECK_EQ(make_cli({"a", "--vsync=TRUE"}).bool_value("vsync").value_or(false), true);
    ORE_CHECK_FALSE(make_cli({"a"}).bool_value("vsync").has_value());
    ORE_CHECK_EQ(make_cli({"a", "--verbose"}).bool_value("verbose").value_or(false), true);
}

ORE_TEST(cli_missing_values_and_repeats) {
    const CommandLine cli = make_cli({"a", "--define", "ONE", "--define", "TWO", "--unknown"});
    ORE_REQUIRE(cli.values("define").size() == 2u);
    ORE_CHECK_EQ(cli.values("define")[1], std::string("TWO"));
    ORE_CHECK(cli.has("unknown"));
    ORE_CHECK_FALSE(cli.value("unknown").has_value());
    ORE_CHECK_FALSE(cli.int_value("nope").has_value());
}

ORE_TEST(cli_help_lists_options) {
    const ore::CliOption options[] = {
        {"frames", "<count>", "Render exactly N frames then exit"},
        {"headless", "", "Run without a window (offscreen rendering)"},
    };
    const CommandLine cli = make_cli({"app"});
    const std::string help = cli.help("--frames <count>", options);
    ORE_CHECK(help.find("--frames <count>") != std::string::npos);
    ORE_CHECK(help.find("--headless") != std::string::npos);
    ORE_CHECK(help.find("usage: app") != std::string::npos);
}

ORE_TEST_MAIN
