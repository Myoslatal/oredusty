// Mine - content packs: content the game loads out of a directory of .ecfg files.
//
// A pack is **a directory**: the .ecfg files that describe its content, the resources they name (art,
// notes, anything that is not .ecfg), and optionally a "pack.ecfg" header that says who it is:
//
//     packs/my_pack/
//         pack.ecfg               # optional: id, name, version, requires
//         items.ecfg              # content files: any number, in any subdirectory
//         structures.ecfg
//         art/wall.png            # resources, named relative to the file that declares them
//
// A pack that needs to *run* something is a mod package instead (mod_package.h): the same directory,
// plus a manifest that lists its content files and an optional shared library.
//
// Packs are found by pointing at a directory (--packs <dir>): the directory itself when it is a pack,
// otherwise every pack directly inside it - which makes one directory a workspace of packs. Without it,
// a run looks in the packs directory beside its executable and in the working directory's own
// (content_search.h).
#pragma once

#include <mine/content_list.h>
#include <mine/content_loader.h>
#include <mine/mod_package.h>
#include <mine/registry.h>

#include <t2d/core/ecfg.h>

#include <optional>
#include <string>
#include <vector>

namespace mine {

/// The header a pack may carry: a file of its own at the top of the pack, with the same keys a mod's
/// manifest has. It is not content - it says who the pack is, so that another pack can require it.
inline constexpr const char* kPackHeaderName = "pack.ecfg";

/// One content pack: a directory, the files it holds, and what it added to the registry.
struct ContentPack {
    std::string directory;     ///< where it lives
    std::string id;            ///< the header's id, or the directory's name
    std::string name;          ///< the header's name, or the id
    std::string version;       ///< the header's version, may be empty
    std::vector<std::string> requirements;   ///< the header's requires, by pack id
    bool declared = false;     ///< true when the directory carries a pack.ecfg
    /// The content files, in load order: every *.ecfg under the directory except the header, sorted by
    /// path so the order does not depend on what the filesystem returns first.
    std::vector<std::string> files;
    /// The parsed files, in the same order as \c files. A file that does not parse fails the pack, so
    /// the two always have the same size.
    std::vector<t2d::EcfgDocument> documents;
    /// What this pack added to the registry, and what its pictures cost (filled in by the pipeline).
    std::vector<ContentEntry> registered;
    usize images = 0;         ///< pictures it referenced and that are there
    usize images_failed = 0;  ///< pictures it referenced and that are not
    bool ok = true;
    std::string error;

    /// Reads one pack: its header, then every content file it holds. Returns nothing and fills
    /// \p error when the directory is not a pack, when the header is not what a header has to be, or
    /// when a content file does not parse - a pack that is half read is refused rather than half
    /// loaded.
    [[nodiscard]] static std::optional<ContentPack> load(const std::string& directory, std::string* error);

    /// True when \p directory looks like a pack: it carries a header, or it holds at least one .ecfg
    /// file. A mod package directory is never a pack - its content files belong to the mod host
    /// (mod_package.h), which loads them in the order its manifest gives.
    [[nodiscard]] static bool is_pack(const std::string& directory);

    /// The packs in \p directory, sorted by path. The directory itself is the pack when no pack sits
    /// directly inside it; otherwise it is a **workspace** and its packs are the ones directly inside
    /// it, and a loose *.ecfg file at the top of a workspace is reported - a pack is a directory now,
    /// and silently dropping a file is how content disappears. A directory that does not exist is
    /// reported too (a path a caller named and that is not there is an error, unlike a default).
    [[nodiscard]] static std::vector<std::string> find_packs(const std::string& directory,
                                                             std::vector<std::string>* errors = nullptr);

    /// Every *.ecfg file under \p directory except the header, sorted: what load() reads. Entries that
    /// start with a dot are skipped (which keeps .git and editor leftovers out of a load), and a mod
    /// package is not descended into.
    [[nodiscard]] static std::vector<std::string> content_files(const std::string& directory);
};

/// What one ContentPipeline::load() did.
struct ContentPipelineReport {
    /// The game's own content: the first stage of the load order, one or more packs. They are packs
    /// like any other - a directory of .ecfg files and art - and the first one is found beside the
    /// executable (content_search.h); what makes them the game's own is that they register first, so
    /// their ids are the ones that never move.
    usize base_packs = 0;        ///< base packs that loaded
    usize base_files = 0;        ///< content files they hold
    usize base_registered = 0;   ///< names they added
    usize packs = 0;             ///< packs that loaded
    usize pack_content = 0;      ///< names the packs added
    usize base_images = 0;       ///< pictures the game's own pack referenced and that are there
    usize pack_images = 0;       ///< pictures the packs referenced and that are there
    usize mod_images = 0;        ///< pictures the mods' content files referenced and that are there
    /// Every picture the load resolved, in load order, from all three stages: what the engine draws
    /// the content with. A picture that is not there is in here too, with ok == false and the reason.
    std::vector<ResolvedImage> images;
    /// Every plot definition the load read, from all three stages: what building a layer needs. The
    /// definitions are read while a source loads anyway - a "random_reverse" the engine cannot read is
    /// a data error, and it is reported where the file is - so this is that read, kept (content_loader.h).
    ContentDefinitions definitions;
    usize mods = 0;              ///< mods that loaded
    usize native_mods = 0;
    usize mod_content = 0;
    usize total_content = 0;     ///< everything in the registry afterwards
    /// One record per thing the pipeline was asked to load, in the order it handled them: the game's
    /// own pack, then the packs (the ones that could not even be read first, then the ones it loaded,
    /// in dependency order), then the mod packages. The failures are in here too - a pack that does not
    /// read is as much a part of "what is loaded" as one that does, and a list that only shows
    /// successes is exactly the list a broken pack hides from. The content list screen
    /// (content_list.h) is built from this.
    std::vector<ContentSource> sources;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    [[nodiscard]] bool clean() const { return errors.empty(); }
    [[nodiscard]] std::string first_error() const { return errors.empty() ? std::string{} : errors.front(); }
};

/// The one place that decides what the game's content registry holds: the game's own pack first, then
/// content packs, then mod packages (which may also carry code). Every stage registers into the same
/// registry, so a name two sources both declare is reported and keeps its first owner - the game's own
/// content can never be silently replaced by a pack.
///
/// Loading again is a reload: mods are unloaded first (on_unload, then the library is closed), and the
/// registry is cleared, so ids come out the same every time.
class ContentPipeline {
public:
    ContentPipeline() = default;
    T2D_NON_MOVABLE(ContentPipeline);   // it owns loaded libraries

    /// The directories of the game's own content (the first stage), in load order: the game's own pack
    /// first, then whatever a command line added to it. Empty means the game has none.
    void set_base_packs(std::vector<std::string> directories);
    /// Directories of packs: each is a pack itself, or a workspace holding packs.
    void set_pack_directories(std::vector<std::string> directories);
    void set_mod_directories(std::vector<std::string> directories);

    [[nodiscard]] bool empty() const;
    [[nodiscard]] const std::vector<std::string>& base_packs() const { return base_packs_; }
    [[nodiscard]] const std::vector<ContentPack>& packs() const { return packs_; }
    [[nodiscard]] const ModHost& mods() const { return mods_; }
    [[nodiscard]] const ContentPipelineReport& report() const { return report_; }

    /// Clears \p registry and loads everything, in order.
    const ContentPipelineReport& load(ContentRegistry& registry);
    /// Calls every native module's on_unload and closes the libraries.
    void unload();

private:
    std::vector<std::string> base_packs_;
    std::vector<std::string> pack_directories_;
    std::vector<std::string> mod_directories_;
    std::vector<ContentPack> packs_;
    ModHost mods_;
    ContentPipelineReport report_;
};

} // namespace mine
