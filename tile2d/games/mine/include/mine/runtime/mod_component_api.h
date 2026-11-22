// Mine - the C-compatible part of the mod surface that registers runtime types.
//
// A native mod that wants its content to carry data of its own, or its own tile layout, hands the host
// descriptors here. The structs are the same standard-layout definitions the runtime core uses; they
// are aliased in one place so a module includes one header and names one type.
//
// The host validates every field before it stores anything: descriptor size, ABI hash, type id, schema
// version, interface tables and tile component lists. A failed call returns non zero and logs the
// reason; nothing is half registered.
//
// TileTypeDesc appends shared/shared_count: one const void* per component, supplied by the module's
// static data and valid while the module is loaded. ComponentDesc appends save/load callbacks for the
// optional CMPT chunk. A descriptor built against the previous header (the prefix before those fields)
// is still accepted; its appended fields are treated as null.
#pragma once

#include <mine/runtime/component.h>
#include <mine/runtime/content/content_ids.h>
#include <mine/runtime/tile_type.h>

#include <t2d/core/types.h>

namespace mine {

using t2d::u32;
using t2d::u64;

/// The C names a module uses. They are aliases, not a second layout: a module and the game compile the
/// same definition, and the static assertions in the runtime headers are the contract.
using MineComponentDesc = runtime::ComponentDesc;
using MineTileTypeDesc = runtime::TileTypeDesc;
using MineInterfaceBinding = runtime::InterfaceBinding;
using MineInterfaceTableHeader = runtime::InterfaceTableHeader;
using MineComponentTypeId = runtime::ComponentTypeId;
using MineInterfaceId = runtime::InterfaceId;
using MineTileTypeId = runtime::TileTypeId;

/// The ABI hash every descriptor registered through this surface must carry. Appending fields keeps
/// this value and is gated by struct_size instead; a module built against a different base layout fails
/// the hash and is refused before its data is read.
using runtime::kModComponentAbiHash;

/// Function pointer shapes the host appends to MineModApi. They take the same opaque \p self the
/// module got in on_load().
using MineFindComponentFn = u64 (*)(void* self, const char* name);
using MineFindTileTypeFn = u64 (*)(void* self, const char* name);
using MineRegisterComponentFn = int (*)(void* self, const MineComponentDesc* desc);
using MineRegisterTileTypeFn = int (*)(void* self, const MineTileTypeDesc* desc);
/// \p expected_version is the component schema version the module was built against. The host refuses
/// a mismatch, a missing interface, and a table whose header is too small or has version 0. On success
/// \p previous receives the table that was active before, which the module may call.
using MineOverrideInterfaceFn = int (*)(void* self, MineComponentTypeId type_id, MineInterfaceId interface_id,
                                        u32 expected_version, const void* replacement, const void** previous);

} // namespace mine
