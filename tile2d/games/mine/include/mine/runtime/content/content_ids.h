// Mine - stable ids for the built-in content runtime.
//
// A component, interface or tile type id is the FNV-1a hash of the name it is written down under, not a
// number handed out in registration order: a code table, a mod and the game must name the same thing by
// the same number even when they register in a different order or not all of them are present. Renaming
// anything here changes its id and is an ABI break; add a new name instead.
//
// The hashes are compile-time constants and are deliberately not exposed through the C surface: a mod
// computes them with the same two constants (FNV offset and prime) or includes this header. The value 0 is
// reserved for "none" on every id type.
#pragma once

#include <mine/runtime/component.h>
#include <mine/runtime/tile_type.h>

#include <string_view>

namespace mine::runtime {
namespace content {

using t2d::u32;
using t2d::u64;

/// 64-bit FNV-1a over the bytes of \p text. Stable across compilers and builds.
[[nodiscard]] constexpr u64 hash_name(std::string_view text) {
    u64 hash = 14695981039346656037ull;
    for (const char character : text) {
        hash ^= static_cast<u64>(static_cast<unsigned char>(character));
        hash *= 1099511628211ull;
    }
    return hash;
}

/// 32-bit FNV-1a for interface ids, which have their own id space.
[[nodiscard]] constexpr u32 hash_interface(std::string_view text) {
    u32 hash = 2166136261u;
    for (const char character : text) {
        hash ^= static_cast<u32>(static_cast<unsigned char>(character));
        hash *= 16777619u;
    }
    return hash;
}

/// Content with no identity: an empty slot, a job with nothing to mine.
inline constexpr u32 kNoContentId = 0;
/// The version every built-in component and tile type declares.
inline constexpr u32 kBuiltinContentVersion = 1;

// --- component types -----------------------------------------------------------------------------
inline constexpr ComponentTypeId kContainerComponent = hash_name("mine.component.container");
inline constexpr ComponentTypeId kClickComponent = hash_name("mine.component.click");
inline constexpr ComponentTypeId kDragComponent = hash_name("mine.component.drag");
inline constexpr ComponentTypeId kTickerComponent = hash_name("mine.component.ticker");
inline constexpr ComponentTypeId kPreviewComponent = hash_name("mine.component.preview");
inline constexpr ComponentTypeId kBuildingComponent = hash_name("mine.component.building");
inline constexpr ComponentTypeId kDrillComponent = hash_name("mine.component.drill");
/// The task data block (docs/GAME_DESIGN.md section 1.51). It carries no interface yet: the task system
/// that will give it state and behaviour comes later, and the id is reserved here so it does not move.
inline constexpr ComponentTypeId kTaskComponent = hash_name("mine.component.task");

// --- interfaces a component type carries ---------------------------------------------------------
inline constexpr InterfaceId kContainerInterface = hash_interface("mine.interface.container");
inline constexpr InterfaceId kClickInterface = hash_interface("mine.interface.click");
inline constexpr InterfaceId kDragInterface = hash_interface("mine.interface.drag");
/// The drag interface is the one the task calls DragTarget: a thing a dragged drop may land on. The
/// component that carries it is the drag component (DragData), so both names are kept.
inline constexpr InterfaceId kDragTargetInterface = kDragInterface;
inline constexpr InterfaceId kTickerInterface = hash_interface("mine.interface.ticker");
inline constexpr InterfaceId kPreviewInterface = hash_interface("mine.interface.preview");
inline constexpr InterfaceId kBuildingInterface = hash_interface("mine.interface.building");
inline constexpr InterfaceId kDrillInterface = hash_interface("mine.interface.drill");
/// The task chain: count/current/step/advance. Carried by the task component of a task block
/// (docs/GAME_DESIGN.md section 1.52).
inline constexpr InterfaceId kTaskInterface = hash_interface("mine.interface.task");

// --- tile types ----------------------------------------------------------------------------------
inline constexpr TileTypeId kFloorTileType = hash_name("mine.tile.floor");
inline constexpr TileTypeId kOreTileType = hash_name("mine.tile.ore");
inline constexpr TileTypeId kWarehouseTileType = hash_name("mine.tile.warehouse");
inline constexpr TileTypeId kDrillTileType = hash_name("mine.tile.drill");
/// The 2x1 task block: a task data block, a container that receives items, and the usual drag,
/// preview and building halves (docs/GAME_DESIGN.md section 1.51).
inline constexpr TileTypeId kTaskBlockTileType = hash_name("mine.tile.task_block");

static_assert(kContainerComponent != kNoComponentType, "container component id must not be zero");
static_assert(kClickComponent != kNoComponentType, "click component id must not be zero");
static_assert(kDragComponent != kNoComponentType, "drag component id must not be zero");
static_assert(kTickerComponent != kNoComponentType, "ticker component id must not be zero");
static_assert(kPreviewComponent != kNoComponentType, "preview component id must not be zero");
static_assert(kBuildingComponent != kNoComponentType, "building component id must not be zero");
static_assert(kDrillComponent != kNoComponentType, "drill component id must not be zero");
static_assert(kTaskComponent != kNoComponentType, "task component id must not be zero");
static_assert(kContainerInterface != kNoInterface, "container interface id must not be zero");
static_assert(kClickInterface != kNoInterface, "click interface id must not be zero");
static_assert(kDragInterface != kNoInterface, "drag interface id must not be zero");
static_assert(kTickerInterface != kNoInterface, "ticker interface id must not be zero");
static_assert(kPreviewInterface != kNoInterface, "preview interface id must not be zero");
static_assert(kBuildingInterface != kNoInterface, "building interface id must not be zero");
static_assert(kDrillInterface != kNoInterface, "drill interface id must not be zero");
static_assert(kTaskInterface != kNoInterface, "task interface id must not be zero");
static_assert(kFloorTileType != kNoTileType, "floor tile type id must not be zero");
static_assert(kOreTileType != kNoTileType, "ore tile type id must not be zero");
static_assert(kWarehouseTileType != kNoTileType, "warehouse tile type id must not be zero");
static_assert(kDrillTileType != kNoTileType, "drill tile type id must not be zero");
static_assert(kTaskBlockTileType != kNoTileType, "task block tile type id must not be zero");

} // namespace content
} // namespace mine::runtime
