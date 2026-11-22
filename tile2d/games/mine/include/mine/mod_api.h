// Mine - the one interface a native mod compiles against.
//
// A mod is a shared library that the game loads at run time. It was compiled by whoever wrote it, with
// whatever compiler and standard library they like, so **nothing but plain data and function pointers
// crosses this line**: no std::string, no exceptions, no C++ objects with a layout this header does
// not spell out. The header itself is C++ because a mod is C++, but the ABI it declares is C.
//
// Growing the API: append new function pointers at the end of MineModApi; kModApiVersion is bumped when
// a release adds a surface a module must opt into (this release appended the runtime type registration
// fields and bumped to 2). struct_size is what lets a host tell how much of the struct a module knows
// about; it must never read past the size it was given.
#pragma once

#include <mine/registry.h> // ContentKind / ContentId, so a mod names them instead of guessing numbers

#include <mine/runtime/mod_component_api.h>

#include <t2d/core/types.h>

namespace mine {

using t2d::i64;
using t2d::u32;

/// Bumped when a field of MineModApi or MineModDesc changes shape or meaning. A module that was built
/// against another version is refused with a message instead of being called through a wrong layout.
/// Version 2 appended the runtime registration surface: custom components and tile types, their
/// lookups, and registration-time interface overrides. The prefix is unchanged; a module built for
/// version 1 is refused by abi_version before any new field is read.
inline constexpr u32 kModApiVersion = 2;

/// What the game hands to a native module.
///
/// $self is the pointer on_load() received: it identifies *which* mod is calling, so a function that
/// acts on a mod's behalf (registering content, reading its own data) does not need a global.
struct MineModApi {
    u32 abi_version;
    u32 struct_size;

    /// Writes one line to the game's log. level: 0 info, 1 warning, 2 error.
    void (*log)(void* self, u32 level, const char* message);
    /// Registers a piece of content and returns the id the running registry handed out, or
    /// kNoContent (0) when the name is already taken by another mod - which is reported, never merged.
    ContentId (*register_content)(void* self, u32 kind, const char* name);
    /// Looks a name up. kNoContent when nothing registered it.
    ContentId (*find_content)(void* self, u32 kind, const char* name);
    /// The name of an id, in a buffer the host owns: valid until the next API call, nullptr when the
    /// id is unknown.
    const char* (*content_name)(void* self, u32 kind, ContentId id);
    /// Reads a value out of *this mod's* mod.ecfg as text (numbers and booleans are rendered the way
    /// the file writes them). nullptr when the key does not exist.
    const char* (*mod_value)(void* self, const char* key);
    /// The same as an integer, with a fallback for "missing or not a number".
    i64 (*mod_int)(void* self, const char* key, i64 fallback);

    // --- version 2: runtime types and registration-time overrides ---------------------------------
    /// Looks a component type up by name. 0 when this runtime has no such component.
    MineFindComponentFn find_component;
    /// Looks a tile type up by name. 0 when this runtime has no such tile type.
    MineFindTileTypeFn find_tile_type;
    /// Registers a component type. 0 on success. Duplicate ids or names, a bad descriptor size, a
    /// foreign ABI hash and an invalid interface table are refused and reported.
    MineRegisterComponentFn register_component;
    /// Registers a tile type whose component ids are already registered. 0 on success.
    MineRegisterTileTypeFn register_tile_type;
    /// Replaces the active table of an original component interface. 0 on success; the replacement's
    /// previous table is written to \p previous for call chaining. Only valid while the module's
    /// on_load() is running: the host restores the registration tables before unloading it.
    MineOverrideInterfaceFn override_component_interface;
};

/// What a native module exports.
struct MineModDesc {
    u32 abi_version;
    u32 struct_size;
    /// Identity of the module. It has to match the id in the package's mod.ecfg: the manifest is what
    /// the host ordered and validated, and a library that claims to be something else is refused.
    const char* id;
    const char* name;
    const char* version;

    /// Called once after the library is loaded. Return 0 to accept the module; anything else makes the
    /// load fail (and the game keeps running without it). $self must be handed back to every API
    /// function that takes one.
    int (*on_load)(const MineModApi* api, void* self);
    /// Called before the library is unloaded, on shutdown or on a reload. May be null. Nothing the
    /// module allocated may outlive it, and the host must not be inside a module callback when this
    /// runs - which is why a reload is done between frames, never during one.
    void (*on_unload)(void* self);
};

#if defined(_WIN32)
#define MINE_MOD_EXPORT extern "C" __declspec(dllexport)
#else
#define MINE_MOD_EXPORT extern "C" __attribute__((visibility("default")))
#endif

/// The single symbol the host looks for. It returns a pointer to a static MineModDesc.
using MineModEntry = const MineModDesc* (*)();
inline constexpr const char* kModEntrySymbol = "mine_mod_entry";

} // namespace mine
