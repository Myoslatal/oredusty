// The native half of the example mod. It is a shared library the game loads at run time, and it is
// deliberately independent: it includes only the mod API header and links nothing at all.
//
// What it does is *logic*: instead of listing content in a data file, it generates it from its own
// parameter ("tiers"), registers it through the host, and reads its own values back. A real mod would
// do the same with a recipe chain, a structure family or anything else that is easier to compute than
// to write out by hand.
#include <mine/mod_api.h>

#include <format>
#include <string>
#include <vector>

// The names a mod actually uses, brought in one by one: "using namespace mine" would also drag in
// every helper of the game's headers, which is not what a module should compile against.
using mine::ContentId;
using mine::i64;
using mine::MineModApi;
using mine::MineModDesc;

namespace {

/// Two build switches, so the loader's checks can be tested against a module that really is wrong:
/// one that claims another ABI, and one whose on_load() refuses to run.
#if defined(EXAMPLE_MOD_API_VERSION)
constexpr mine::u32 kDeclaredApi = EXAMPLE_MOD_API_VERSION;
#else
constexpr mine::u32 kDeclaredApi = mine::kModApiVersion;
#endif

/// Everything this module keeps alive lives here. It is file static because a shared library has its
/// own copy of its statics - which is exactly what makes unloading and loading it again start over.
std::vector<ContentId> g_registered;
std::string g_label;

int on_load(const MineModApi* api, void* self) {
#if defined(EXAMPLE_MOD_FAIL)
    (void)api;
    (void)self;
    return 7;   // a mod that decides it cannot run
#endif
    if (api->abi_version != mine::kModApiVersion) return 2;   // the host checks too; belt and braces
    const i64 tiers = api->mod_int(self, "tiers", 1);
    const char* label = api->mod_value(self, "label");
    g_label = label != nullptr ? label : "(none)";
    api->log(self, 0, std::format("generating {} tier(s), label '{}'", tiers, g_label).c_str());

    for (i64 tier = 1; tier <= tiers; ++tier) {
        const std::string name = std::format("mod_tier_{}_drill", tier);
        const ContentId id = api->register_content(
            self, static_cast<mine::u32>(mine::ContentKind::Machine), name.c_str());
        if (id == mine::kNoContent) return 1;   // a collision: refuse to run rather than half load
        g_registered.push_back(id);
    }

    // Reading back what the host registered proves the registry and the module agree.
    if (api->find_content(self, static_cast<mine::u32>(mine::ContentKind::Structure), "mod_wall") ==
        mine::kNoContent) {
        api->log(self, 2, "the data mod's structure is not registered");
        return 3;
    }
    return 0;
}

void on_unload(void* self) {
    g_registered.clear();
    g_label.clear();
    (void)self;
}

const MineModDesc g_desc{
    kDeclaredApi,
    sizeof(MineModDesc),
    "example_native",
    "Example (native)",
    "1.0",
    &on_load,
    &on_unload,
};

} // namespace

MINE_MOD_EXPORT const mine::MineModDesc* mine_mod_entry() { return &g_desc; }
