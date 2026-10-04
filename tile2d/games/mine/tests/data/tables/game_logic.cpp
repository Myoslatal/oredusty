// The game's own code, in a code table. This translation unit is packed together with mine_core's
// registry.cpp - the game's real source, not a copy - so what the test loads is the game's own logic
// arriving as a table instead of being linked into the program.
//
// Three things are being shown at once: a game class (mine::ContentRegistry, with std::string and
// std::vector inside it) works in a table; the table calls back into the program that loaded it
// (t2d::log::info and host_service are undefined here and answered by the host); and one of this
// module's own functions is something a mod table can replace.
#include <mine/registry.h>

#include <t2d/core/log.h>

/// The host's own code. A table cannot define it, so the merge asks the running program for it.
extern "C" int host_service(int value);

/// What a mod table replaces, to show that the game's own call follows the merge.
extern "C" int game_bonus() { return 5; }

extern "C" int game_probe() {
    mine::ContentRegistry registry;
    const mine::ContentId ore = registry.register_content(mine::ContentKind::Item, "iron_ore");
    const mine::ContentId plate = registry.register_content(mine::ContentKind::Item, "iron_plate");
    // Registering the same name twice is idempotent: an id must not move because data was read again.
    if (registry.register_content(mine::ContentKind::Item, "iron_ore") != ore) return -1;
    if (registry.count(mine::ContentKind::Item) != 2) return -2;
    t2d::log::info("a code table registered {} item(s) of its own", registry.count(mine::ContentKind::Item));
    return static_cast<int>(ore * 100 + plate) + game_bonus() + host_service(1);
}
