// The native half of the runtime fixture: it registers a component and a tile type of its own, then
// overrides the built-in preview interface so an object that already exists sees the new table too.
//
// The library is a real shared object loaded by ModHost; nothing here is linked into the test. It is
// deliberately independent, like tests/mods/example_native: it includes the public mod/runtime headers
// and links nothing.
#include <mine/mod_api.h>

#include <mine/runtime/content/components.h>

#include <format>
#include <string>

using mine::MineComponentDesc;
using mine::MineComponentTypeId;
using mine::MineModApi;
using mine::MineModDesc;
using mine::MineTileTypeDesc;
using mine::u32;
using mine::u64;

namespace {

/// A component the game does not know: one word of data and no built-in interface. It is registered
/// through the mod surface, which is what proves a module can extend an object's layout.
constexpr u64 kMarkerComponent = mine::runtime::content::hash_name("example_runtime.marker");
constexpr u64 kMarkerTileType = mine::runtime::content::hash_name("example_runtime.marker_tile");
constexpr u64 kMarkerAltTileType = mine::runtime::content::hash_name("example_runtime.marker_alt_tile");
constexpr u32 kMarkerMagic = 0x4d41524bu; // "MARK"

struct MarkerData {
    u32 magic;
    u32 ticks;
};

/// The per-tile-type shared record. Two tile types use the same component with different values, which
/// is how the test proves the shared pointer comes from the type and not from the component.
struct MarkerShared {
    u32 bump;
};

MarkerShared g_marker_shared{31u};
MarkerShared g_marker_shared_alt{47u};

void marker_construct(void* data) {
    auto* marker = static_cast<MarkerData*>(data);
    marker->magic = kMarkerMagic;
    marker->ticks = 0;
}

void marker_destruct(void*) {}

/// Two-phase save: query the size first, then write. The shared value is mixed into the state so a load
/// with the wrong shared record produces the wrong ticks rather than silently working.
u32 marker_save(const void* shared, const void* data, void* out, u32 capacity) {
    if (shared == nullptr || data == nullptr) return 0;
    const auto* marker = static_cast<const MarkerData*>(data);
    const auto* record = static_cast<const MarkerShared*>(shared);
    constexpr u32 kSize = sizeof(u32);
    if (out == nullptr || capacity < kSize) return kSize;
    *static_cast<u32*>(out) = marker->ticks ^ record->bump;
    return kSize;
}

u32 marker_load(const void* shared, void* data, const void* in, u32 size) {
    if (shared == nullptr || data == nullptr || in == nullptr || size != sizeof(u32)) return 0;
    auto* marker = static_cast<MarkerData*>(data);
    const auto* record = static_cast<const MarkerShared*>(shared);
    marker->ticks = *static_cast<const u32*>(in) ^ record->bump;
    return sizeof(u32);
}

/// The tile type's opaque table: the game's tile layer does not call it yet, but registration stores it
/// and a lookup by name must answer with the same type.
struct MarkerTileFunctions {
    mine::runtime::InterfaceTableHeader header;
    const char* (*role)();
};

const char* marker_role() {
    return "example-runtime-marker";
}

const MarkerTileFunctions kMarkerTileFunctions{{static_cast<u32>(sizeof(MarkerTileFunctions)), 1},
                                               &marker_role};

/// The preview override. It calls the previous table first (which may itself be the game's), then appends
/// a part with a known angle: the test can tell the override ran both on an object built before it and
/// on one built after.
const mine::runtime::content::PreviewFunctions* g_previous_preview = nullptr;

mine::runtime::content::TilePreview marker_preview(const void* shared, void* data, float seconds) {
    mine::runtime::content::TilePreview preview{};
    if (g_previous_preview != nullptr) {
        preview = g_previous_preview->get_preview(shared, data, seconds);
    }
    if (preview.count < mine::runtime::content::kMaxPreviewParts) {
        mine::runtime::content::PreviewPart& part = preview.parts[preview.count++];
        part = mine::runtime::content::PreviewPart{};
        part.part = "";
        part.image = "";
        part.mirrored = 0;
        part.radians = 1.25f;
        part.pivot = mine::runtime::content::Vec2f{0.5f, 0.5f};
    }
    return preview;
}

const mine::runtime::content::PreviewFunctions kMarkerPreview{
    {static_cast<u32>(sizeof(mine::runtime::content::PreviewFunctions)), 1}, &marker_preview};

MineComponentDesc marker_component() {
    MineComponentDesc desc{};
    desc.struct_size = sizeof(MineComponentDesc);
    desc.type_id = kMarkerComponent;
    desc.name = "example:marker";
    desc.version = 1;
    desc.instance_size = static_cast<u32>(sizeof(MarkerData));
    desc.instance_align = static_cast<u32>(alignof(MarkerData));
    desc.construct = &marker_construct;
    desc.destruct = &marker_destruct;
    desc.interfaces = nullptr;
    desc.interface_count = 0;
    desc.abi_hash = mine::kModComponentAbiHash;
    desc.save = &marker_save;
    desc.load = &marker_load;
    return desc;
}

int on_load(const MineModApi* api, void* self) {
    if (api->abi_version != mine::kModApiVersion) return 2;

    [[maybe_unused]] const MineComponentDesc component = marker_component();
    [[maybe_unused]] const MineComponentTypeId tile_components[] = {
        kMarkerComponent, mine::runtime::content::kPreviewComponent};
    [[maybe_unused]] const void* marker_shared[] = {&g_marker_shared, nullptr};
    [[maybe_unused]] const void* marker_alt_shared[] = {&g_marker_shared_alt, nullptr};

#if defined(EXAMPLE_RUNTIME_BAD_ABI)
    MineComponentDesc bad = component;
    bad.abi_hash ^= 1u;
    if (api->register_component(self, &bad) == 0) return 20;
    return 0;
#elif defined(EXAMPLE_RUNTIME_DUPLICATE)
    if (api->register_component(self, &component) != 0) return 21;
    if (api->register_component(self, &component) == 0) return 22;
    return 0;
#elif defined(EXAMPLE_RUNTIME_MISSING_PREVIOUS)
    const void* previous = nullptr;
    if (api->override_component_interface(self, mine::runtime::content::kPreviewComponent, 0x12345678u, 1,
                                          &kMarkerPreview, &previous) == 0) {
        return 23;
    }
    return 0;
#elif defined(EXAMPLE_RUNTIME_BAD_VERSION)
    const void* previous = nullptr;
    if (api->override_component_interface(self, mine::runtime::content::kPreviewComponent,
                                          mine::runtime::content::kPreviewInterface, 99, &kMarkerPreview,
                                          &previous) == 0) {
        return 24;
    }
    return 0;
#else
    if (api->register_component(self, &component) != 0) return 10;
    if (api->find_component(self, "example:marker") != kMarkerComponent) return 11;

    MineTileTypeDesc tile{};
    tile.struct_size = sizeof(MineTileTypeDesc);
    tile.type_id = kMarkerTileType;
    tile.name = "example:marker";
    tile.version = 1;
    tile.components = tile_components;
    tile.component_count = 2;
    tile.functions = &kMarkerTileFunctions;
    tile.factory = nullptr;
    tile.shared = marker_shared;
    tile.shared_count = 2;
    if (api->register_tile_type(self, &tile) != 0) return 12;
    if (api->find_tile_type(self, "example:marker") != kMarkerTileType) return 13;

    MineTileTypeDesc alt = tile;
    alt.type_id = kMarkerAltTileType;
    alt.name = "example:marker_alt";
    alt.shared = marker_alt_shared;
    if (api->register_tile_type(self, &alt) != 0) return 15;
    if (api->find_tile_type(self, "example:marker_alt") != kMarkerAltTileType) return 16;

    const void* previous = nullptr;
    if (api->override_component_interface(self, mine::runtime::content::kPreviewComponent,
                                          mine::runtime::content::kPreviewInterface, 1, &kMarkerPreview,
                                          &previous) != 0) {
        return 14;
    }
    g_previous_preview = static_cast<const mine::runtime::content::PreviewFunctions*>(previous);
    return 0;
#endif
}

void on_unload(void*) {
    g_previous_preview = nullptr;
}

const MineModDesc g_desc{
    mine::kModApiVersion, sizeof(MineModDesc), "example_runtime", "Example (runtime types)", "1.0", &on_load,
    &on_unload,
};

} // namespace

MINE_MOD_EXPORT const mine::MineModDesc* mine_mod_entry() {
    return &g_desc;
}
