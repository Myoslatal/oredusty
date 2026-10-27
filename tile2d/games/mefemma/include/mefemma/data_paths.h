// Mefemma - where the files that travel with the game are.
//
// A game is run from three different places: a build tree (the executable next to the sources), an
// installed package (the executable next to its assets), and a developer's shell (wherever they were
// standing). This header is the one rule that makes all three work: the game looks for its assets
// **beside its own executable first**, and falls back to the source tree only when it is running out of a
// build tree that has no assets copied next to it.
//
// Nothing here knows what a texture, a string table or a setting is: it turns names into paths, and the
// modules that read files own the reading.
#pragma once

#include <string>
#include <string_view>

namespace mefemma {

/// The root of the game's data and the names inside it.
///
///     assets/
///         config/game.ecfg      Settings
///         text/ui.ecfg          the interface strings
///         textures/<name>.png   one picture per name
///         fonts/<name>          a font shipped with the game (optional; the system fonts are the default)
class DataPaths {
public:
    DataPaths() = default;
    explicit DataPaths(std::string root);

    /// The assets directory of the game at \p executable_path. \p source_root is the game's own source
    /// directory (games/mefemma), used only when no assets directory sits beside the executable - which is
    /// exactly the case of a freshly built tree. The result is never empty: the executable's own directory
    /// is the last answer, so a caller always has a path to report.
    [[nodiscard]] static DataPaths resolve(std::string_view executable_path, std::string_view source_root);

    [[nodiscard]] const std::string& root() const { return root_; }

    /// A path inside the root, joined with the platform's separator.
    [[nodiscard]] std::string file(std::string_view relative) const;
    /// "textures/<name>.png": a picture a DrawList sprite command can name.
    [[nodiscard]] std::string texture(std::string_view name) const;
    /// "text/<name>.ecfg": an interface string table.
    [[nodiscard]] std::string text(std::string_view name = "ui") const;
    /// "config/game.ecfg": the shell's settings.
    [[nodiscard]] std::string settings() const;
    /// "fonts/<name>": a font shipped with the game, for a game that does not want the system's.
    [[nodiscard]] std::string font(std::string_view name) const;

    /// Whether \p relative names an existing regular file inside the root.
    [[nodiscard]] bool has_file(std::string_view relative) const;

private:
    std::string root_;
};

} // namespace mefemma
