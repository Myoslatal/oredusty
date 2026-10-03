// Mine - content packs: content the game loads out of plain .ecfg files.
//
// A pack is **one .ecfg file**. Its tables are content, exactly as in the game's own files, optionally
// prefixed by a "pack::" table that says who it is:
//
//     pack::                      # optional; without it the file name is the pack's identity
//         id:"example_pack"
//         name:"Example pack"
//         version:"1.0"
//         requires:["other_pack"]
//     item::                      # everything else is content
//         <name>::
//             <the designer's fields>
//
// No manifest file, no directory layout, no code: that is the whole point of a pack. A pack that needs
// to *run* something is a mod package instead (mod_package.h), which is the same thing plus a manifest
// and a shared library.
//
// Packs are found either by naming them (--pack <file>) or by pointing at a directory (--packs <dir>),
// in which case every *.ecfg directly inside it is a pack. Order is file name first, then whatever
// "requires" asks for.
#pragma once

#include <mine/content_loader.h>
#include <mine/mod_package.h>
#include <mine/registry.h>

#include <t2d/core/ecfg.h>

#include <optional>
#include <string>
#include <vector>

namespace mine {

/// One picture a pack ships, and what the engine made of it.
struct PackImage {
    ContentKind kind = ContentKind::Item;
    std::string content;   ///< the content entry it belongs to
    std::string path;      ///< as written in the pack ("art/wall.png")
    std::string resolved;  ///< absolute path, empty when it could not be resolved
    bool ok = false;
    std::string error;     ///< why not, when ok is false
};

/// One parsed pack.
struct ContentPack {
    std::string path;          ///< where it came from
    std::string id;            ///< pack::id, or the file's stem when it does not say
    std::string name;          ///< pack::name, or the id
    std::string version;       ///< pack::version, may be empty
    std::vector<std::string> requirements;   ///< pack::requires, by pack id
    bool declared = false;     ///< true when the file carries a pack:: table
    /// The document with the "pack::" table removed: what actually gets registered.
    t2d::EcfgDocument content;
    /// What this pack added to the registry (filled in by the pipeline).
    std::vector<ContentEntry> registered;
    /// The pictures it ships, resolved against the pack's own directory.
    std::vector<PackImage> images;
    bool ok = true;
    std::string error;

    /// Reads one pack. On failure returns nullopt and fills \p error with "path:line:column: why".
    [[nodiscard]] static std::optional<ContentPack> load(const std::string& path, std::string* error);
    /// Every *.ecfg file directly inside \p directory, sorted by name; missing directories and
    /// unreadable entries are reported through \p errors.
    [[nodiscard]] static std::vector<std::string> scan_directory(const std::string& directory,
                                                                 std::vector<std::string>* errors = nullptr);
};

/// What one ContentPipeline::load() did.
struct ContentPipelineReport {
    usize base_files = 0;        ///< the game's own content files that parsed
    usize base_registered = 0;   ///< names they added
    usize packs = 0;             ///< packs that loaded
    usize pack_content = 0;      ///< names the packs added
    usize pack_images = 0;       ///< pictures the packs referenced and that are there
    usize mods = 0;              ///< mods that loaded
    usize native_mods = 0;
    usize mod_content = 0;
    usize total_content = 0;     ///< everything in the registry afterwards
    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    [[nodiscard]] bool clean() const { return errors.empty(); }
    [[nodiscard]] std::string first_error() const { return errors.empty() ? std::string{} : errors.front(); }
};

/// The one place that decides what the game's content registry holds: the game's own files first, then
/// content packs (pure .ecfg), then mod packages (which may also carry code). Every stage registers
/// into the same registry, so a name two sources both declare is reported and keeps its first owner -
/// the game's own content can never be silently replaced by a pack.
///
/// Loading again is a reload: mods are unloaded first (on_unload, then the library is closed), and the
/// registry is cleared, so ids come out the same every time.
class ContentPipeline {
public:
    ContentPipeline() = default;
    T2D_NON_MOVABLE(ContentPipeline);   // it owns loaded libraries

    void set_base_files(std::vector<std::string> paths);
    void set_pack_files(std::vector<std::string> paths);
    void set_pack_directories(std::vector<std::string> directories);
    void set_mod_directories(std::vector<std::string> directories);

    [[nodiscard]] bool empty() const;
    [[nodiscard]] const std::vector<std::string>& base_files() const { return base_files_; }
    [[nodiscard]] const std::vector<ContentPack>& packs() const { return packs_; }
    [[nodiscard]] const ModHost& mods() const { return mods_; }
    [[nodiscard]] const ContentPipelineReport& report() const { return report_; }

    /// Clears \p registry and loads everything, in order.
    const ContentPipelineReport& load(ContentRegistry& registry);
    /// Calls every native module's on_unload and closes the libraries.
    void unload();

private:
    std::vector<std::string> base_files_;
    std::vector<std::string> pack_files_;
    std::vector<std::string> pack_directories_;
    std::vector<std::string> mod_directories_;
    std::vector<ContentPack> packs_;
    ModHost mods_;
    ContentPipelineReport report_;
};

} // namespace mine
