// Mine - the save library: one directory per slot, one auto.t2dsave inside it, and the names a player
// types for them (docs/GAME_DESIGN.md section 1.48).
//
// It is game logic and not screen code: a slot is a directory, a name is checked the way a file system
// checks one, and the newest automatic save is a fact about timestamps. The application gives the root
// (saves/ under the working directory); a test gives a scratch directory.
#pragma once

#include <t2d/core/types.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mine {

using t2d::usize;

/// The name every slot's most recent automatic save has: a slot keeps one, not a history (section 1.48).
inline constexpr std::string_view kAutosaveFileName = "auto.t2dsave";
/// What a slot name may be: characters and bytes. The two limits differ because one CJK character is
/// three bytes: a name can be too long without having too many characters.
inline constexpr usize kMaxSaveNameCodepoints = 48;
inline constexpr usize kMaxSaveNameBytes = 128;

/// A slot as the library sees it: its directory, the file a write goes to, and whether one is there yet.
struct SaveSlot {
    std::string name;          ///< the directory name, exactly as typed (UTF-8)
    std::string directory;     ///< <root>/<name>
    std::string autosave_path; ///< <root>/<name>/auto.t2dsave
    std::filesystem::file_time_type modified{}; ///< when auto.t2dsave was written; default when absent
    bool has_autosave = false;
};

/// The locale id (never the text) of what is wrong with \p name, or "" when a slot may be made from it.
/// A name the file system will not have is save.name.invalid, one that is too long save.name.long,
/// an empty one save.name.empty; save.name.exists and save.name.failed come from
/// SaveLibrary::create(), which is the only place that touches the disk. A tab is a character a name
/// may hold - the field inserts one for the Tab key - every other control character is not.
[[nodiscard]] const char* save_name_error_id(std::string_view name);

/// Every id a refused name can produce, so a string table can be checked for all of them at once.
inline constexpr std::string_view kSaveNameErrorIds[] = {
    "save.name.empty", "save.name.invalid", "save.name.long", "save.name.exists", "save.name.failed",
};

/// The slots a save library holds, in the order the save list shows them.
class SaveLibrary {
public:
    explicit SaveLibrary(std::string root);

    [[nodiscard]] const std::string& root() const { return root_; }
    /// The directory for \p name: <root>/<name>. It neither checks the name nor touches the disk.
    [[nodiscard]] std::string slot_directory(std::string_view name) const;
    /// The file a slot's automatic save is written to: <root>/<name>/auto.t2dsave.
    [[nodiscard]] std::string autosave_path(std::string_view name) const;

    /// Every slot: directories only. The ones with an automatic save come newest first; the ones without
    /// come after them by name - a slot that was just made is at the bottom until it holds something.
    [[nodiscard]] std::vector<SaveSlot> slots() const;
    /// The newest automatic save in the whole library, or nothing when no slot has one.
    [[nodiscard]] std::optional<SaveSlot> latest_autosave() const;
    /// The slot named exactly \p name, or nothing when there is no such directory.
    [[nodiscard]] std::optional<SaveSlot> find(std::string_view name) const;

    /// Makes the directory for \p name. On failure returns false and points \p error_id at the locale id
    /// that says why (save.name.exists for a name already taken, save.name.failed when the file system
    /// refused); on success \p error_id is "" and \p out is the new slot. The auto.t2dsave itself is
    /// written later, by the first save the player's next action makes.
    bool create(std::string_view name, const char*& error_id, SaveSlot& out);

private:
    [[nodiscard]] std::optional<SaveSlot> slot_at(std::string_view name) const;

    std::string root_;
};

} // namespace mine
