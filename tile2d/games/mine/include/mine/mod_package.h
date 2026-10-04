// Mine - mod packages: content and code that live outside the game.
//
// A package is a directory with a manifest:
//
//     mods/example/
//         mod.ecfg              id, name, version, api, requires, content, native
//         content/*.ecfg        the same content files the game itself reads (tables per kind)
//         libexample.so         optional: a shared library exporting mine_mod_entry()
//
// The game loads a package in two halves. Its **data** goes into the content registry exactly like the
// game's own content does, so ids, saves and the name -> id table work the same. Its **code**, when it
// has any, is a shared library the host opens, validates against the ABI version, calls on_load() on,
// and closes again on unload. Nothing else about a mod is special: it cannot add content kinds, it
// cannot touch the framework, and a mod that fails to load is reported and skipped while the game
// keeps running.
//
// Loading native code is not a sandbox. A module runs with the game's privileges and can do anything
// the game can do; the ABI check is about compatibility, not safety.
#pragma once

#include <mine/content_loader.h>
#include <mine/mod_api.h>
#include <mine/registry.h>

#include <t2d/core/ecfg.h>
#include <t2d/core/module.h>
#include <t2d/core/types.h>

#include <optional>
#include <string>
#include <vector>

namespace mine {

using t2d::usize;

/// What a package's mod.ecfg says.
struct ModManifest {
    std::string id;          ///< unique; what other mods list in "requires"
    std::string name;        ///< for humans; defaults to the id
    std::string version;
    std::string description;
    std::string directory;   ///< where the package lives
    std::string native;      ///< file name of the shared library, empty for a data only mod
    u32 api = 0;             ///< the ABI the library was built against (native mods only)
    std::vector<std::string> requirements;
    std::vector<std::string> content;   ///< content files, relative to the package, in load order

    /// Reads a manifest out of an already parsed mod.ecfg; \p directory is the package's folder, which
    /// the relative paths inside the manifest are resolved against. A missing or malformed field is
    /// refused rather than guessed at.
    [[nodiscard]] static std::optional<ModManifest> from_document(const t2d::EcfgDocument& document,
                                                                  const std::string& directory,
                                                                  std::string* error);
    /// Convenience for a caller that only wants the manifest, not the document behind it.
    [[nodiscard]] static std::optional<ModManifest> parse_file(const std::string& path, std::string* error);
};

/// One piece of content a mod added.
struct ModContentEntry {
    ContentKind kind = ContentKind::Item;
    ContentId id = kNoContent;
    std::string name;
};

/// One package the host looked at. A package it refused before it got as far as loading anything - a
/// manifest that does not parse, an id two packages claim - is in here too, with whatever the manifest
/// managed to say: "what did the host look at" and "what is running" are different questions, and the
/// content list (content_list.h) asks the first one.
struct LoadedMod {
    ModManifest manifest;
    std::vector<std::string> content_paths;   ///< absolute paths, in load order
    std::vector<ModContentEntry> registered;  ///< what it actually added to the registry
    /// The pictures its content files ship, resolved against the directory of the file that declares
    /// them - the same rule the game's own content and the packs follow.
    std::vector<ResolvedImage> images;
    /// The plot definitions its content files declare: the fields the engine reads (types/
    /// tile_definition.h), kept so that a layer can be built out of what a mod added.
    std::vector<types::TileDefinition> definitions;
    bool native_loaded = false;
    bool ok = true;                           ///< false: the report says why, the game runs without it
    std::string error;
};

/// What one load() did.
struct ModLoadReport {
    /// Every package that was found, in the order the host handled it: the ones it refused, the ones
    /// whose requirements were missing or cyclic, then the ones it loaded (dependency order).
    std::vector<LoadedMod> mods;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    usize content_registered = 0;
    usize native_modules = 0;

    [[nodiscard]] bool clean() const { return errors.empty(); }
    [[nodiscard]] bool empty() const { return mods.empty(); }
};

/// What the C ABI's "self" points at: the host, and which of its mods is calling. A mod treats it as
/// opaque and hands it back unchanged.
struct ModContext {
    void* host = nullptr;
    usize slot = 0;
};

/// Loads packages, owns the loaded libraries, and implements the C ABI they call.
///
/// It is deliberately single threaded: a reload happens between frames, never while a module is
/// running, and a module that is still on the stack is never unloaded. The host must outlive every
/// call into a module it loaded.
class ModHost {
public:
    ModHost();
    ~ModHost();
    T2D_NON_MOVABLE(ModHost);   // it owns loaded libraries: copying or moving one would double free

    /// Scans every directory in \p directories for packages, checks them against each other, orders
    /// them by what they require and loads them: data into \p registry first, then code. Whatever was
    /// loaded before is unloaded first, so calling this again is a reload. Names the registry already
    /// has are reported as collisions and left with their first owner.
    const ModLoadReport& load(const std::vector<std::string>& directories, ContentRegistry& registry);

    /// Calls every module's on_unload in reverse load order, then closes the libraries. Safe when
    /// nothing is loaded, and safe to call twice.
    void unload();

    /// How many mods are running: a package that failed to load is in the report but not here.
    [[nodiscard]] usize count() const;
    [[nodiscard]] usize native_count() const;
    [[nodiscard]] bool empty() const { return count() == 0; }
    [[nodiscard]] const ModLoadReport& report() const { return report_; }
    /// The API handed to every module. One instance, stable for the host's lifetime.
    [[nodiscard]] const MineModApi* api() const { return &api_; }

    // --- the C ABI, called through api(). Public because the ABI's thunks are plain functions; a mod
    // never calls these directly, and nothing else in the game should either. ---
    void api_log(void* self, u32 level, const char* message);
    ContentId api_register_content(void* self, u32 kind, const char* name);
    ContentId api_find_content(void* self, u32 kind, const char* name);
    const char* api_content_name(void* self, u32 kind, ContentId id);
    const char* api_mod_value(void* self, const char* key);
    i64 api_mod_int(void* self, const char* key, i64 fallback);

private:
    struct Slot {
        LoadedMod loaded;
        /// The mod's own manifest, kept alive so mod_value() can read it while the module runs.
        std::optional<t2d::EcfgDocument> document;
        std::optional<t2d::Module> module;
        const MineModDesc* desc = nullptr;
    };

    [[nodiscard]] Slot* slot_from(void* self);
    /// The value \p key names for \p slot: its own "data" table first, then the manifest's fields.
    [[nodiscard]] static const t2d::EcfgValue* mod_value_of(Slot& slot, const char* key);
    /// Renders \p value the way the file writes it, into the host's scratch buffer.
    [[nodiscard]] const char* render_value(const t2d::EcfgValue& value);

    std::vector<Slot> slots_;
    ModLoadReport report_;
    MineModApi api_{};
    ContentRegistry* registry_ = nullptr;
    /// Where content_name() and mod_value() write their answers: valid until the next API call.
    std::string scratch_;
};

} // namespace mine
