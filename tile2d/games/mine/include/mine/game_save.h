// Mine - saving a layer, and loading it back (docs/SAVE.md, docs/GAME_DESIGN.md section 1.46).
//
// A save is the **whole playable state of one layer**: the session that made it, the content table it was
// written against, every plot (identity, footprint, mirroring, container, cadence, build state), the drops
// lying on it, the sweeps in progress, which buildings are unlocked, and where the view was. The container
// is the framework's (t2d::save, docs/SAVE.md); the chunks are this game's, and nothing else reads them.
//
// Two rules make a load safe. The saved content table is translated **by name** (registry.h), and a save
// that names content this build does not have is refused rather than quietly losing a plot. And a plot the
// layer refuses to place stops the load: a save never comes back as half a world. Chunks this build does
// not know are ignored, so a newer save can still be read by an older game.
//
// The drill keeps **no random state** in a save: its dice are seeded from what it is and where it stands
// (types/drill.h), so a restored drill throws like one that was built there, not like the one written
// down. What is saved is what it produced, and the time its cadence had carried.
//
// File names: this module only makes and reads a t2d::save::File; writing it to disk is the caller's job
// (t2d::save::write_file, with the .t2dsave extension - see t2d::save::with_extension).
#pragma once

#include <mine/build_animation.h>
#include <mine/building_unlocks.h>
#include <mine/content_loader.h>
#include <mine/registry.h>
#include <mine/session.h>
#include <mine/world.h>

#include <t2d/save/archive.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mine {

/// The chunks this game writes into a .t2dsave; each is at kGameSaveVersion's chunk version (1).
namespace game_save_chunk {

inline constexpr t2d::u32 kMeta = t2d::save::fourcc('M', 'E', 'T', 'A');
inline constexpr t2d::u32 kContent = t2d::save::fourcc('C', 'O', 'N', 'T');
inline constexpr t2d::u32 kUnlocks = t2d::save::fourcc('U', 'N', 'L', 'K');
inline constexpr t2d::u32 kPlots = t2d::save::fourcc('P', 'L', 'O', 'T');
inline constexpr t2d::u32 kEntities = t2d::save::fourcc('E', 'N', 'T', 'T');
inline constexpr t2d::u32 kBuilds = t2d::save::fourcc('B', 'U', 'I', 'L');

} // namespace game_save_chunk

/// What a mine save says it is, and the version of the chunks above.
inline constexpr std::string_view kGameSaveName = "mine";
inline constexpr t2d::u32 kGameSaveVersion = 1;
/// The version each of the chunks above is written at; a load refuses a chunk at any other one.
inline constexpr t2d::u32 kGameSaveChunkVersion = 1;

/// What a load found: whether it succeeded, why not, and what it brought back. A load refuses as a whole,
/// so a successful one restores every record it read and leaves \c warnings empty; the field is the shape
/// a future chunk version (or a skippable record) would report in, and callers may rely on it being there.
struct GameLoadReport {
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
    usize plots = 0;
    usize entities = 0;
};

/// Everything a save brings back: the session it was, the layer rebuilt, what is being built, what is
/// unlocked, and where the view was. The world owns the layer, and the plots the animation names are the
/// world's - so the two move together.
struct GameSaveData {
    SessionConfig session;
    std::unique_ptr<MineWorld> world;
    BuildingUnlocks unlocks;
    BuildAnimation builds;
    f32 world_seconds = 0.0f;
    t2d::Vec2 camera_center{};
    f32 camera_zoom = 1.0f;
    usize plots = 0;
    usize entities = 0;
};

/// Writes \p world - the layer it holds and everything on it - into a save archive. A world without a
/// layer writes a save that says so (and load_game_save() refuses it); the app only saves while a layer
/// is being played.
[[nodiscard]] t2d::save::File make_game_save(const MineWorld& world, const SessionConfig& session,
                                             const BuildingUnlocks& unlocks, const BuildAnimation& builds,
                                             f32 world_seconds, t2d::Vec2 camera_center, f32 camera_zoom,
                                             const ContentRegistry& registry);

/// Rebuilds the state make_game_save() wrote. Anything that cannot come back exactly - a chunk that is
/// missing or at a version this build does not know, content the registry no longer has, a chunk that
/// cannot be read, a plot the layer refuses to place, a container with less room than the save says -
/// refuses the whole load instead of returning half a mine. The reason is in \p report.
[[nodiscard]] std::optional<GameSaveData> load_game_save(const t2d::save::File& file,
                                                         const ContentRegistry& registry,
                                                         const ContentDefinitions& definitions,
                                                         GameLoadReport* report = nullptr);

} // namespace mine
