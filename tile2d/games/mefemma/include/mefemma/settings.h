// Mefemma - the settings file, and the fields the shell reads out of it.
//
// Settings are data, in the same .ecfg format the rest of the engine uses:
//
//     title:"Mefemma"
//     width:1280
//     height:720
//     vsync:true
//     language:"en"
//     seed:1
//
// A field the shell reads and cannot use is reported where the file is, never guessed at: a width of
// "wide" is not a window size, and reading it as the default would hide the typo. Fields the shell does
// not know are kept in the document and are the game's own - Settings::find() is how a game reads them.
#pragma once

#include <t2d/core/ecfg.h>
#include <t2d/core/types.h>

#include <optional>
#include <string>
#include <string_view>

namespace mefemma {

using t2d::u32;
using t2d::u64;

struct Settings {
    std::string title = "Mefemma";
    u32 width = 1280;
    u32 height = 720;
    bool vsync = true;
    /// The interface language, as the code t2d::parse_language accepts ("en", "zh-Hans", "zh-Hant").
    std::string language = "en";
    /// The seed the game's own random generator starts from. The shell only carries it; what a game draws
    /// from it is the game's business.
    u64 seed = 1;

    /// The file as parsed, so a game can read its own fields without this header having to know them. Empty
    /// when the game has no settings file.
    std::optional<t2d::EcfgDocument> document;

    /// The values a game has when it ships no settings file. A missing file is not an error: it is a game
    /// that kept the defaults.
    [[nodiscard]] static Settings defaults();

    /// Reads \p path. A missing file returns the defaults and leaves \p error alone; a file that does not
    /// parse, or a field the shell reads that is not what it has to be, fills \p error and returns what
    /// could still be read (the invalid field keeps its default).
    [[nodiscard]] static Settings load(const std::string& path, t2d::EcfgError* error = nullptr);

    /// Reads the shell's fields out of \p document. The document is copied into the result.
    [[nodiscard]] static Settings from_document(const t2d::EcfgDocument& document, t2d::EcfgError* error = nullptr);

    /// A top level field of the settings file, or nullptr when there is none (or no file). A game that
    /// wrote nested tables walks them with EcfgValue::find()/children() - the shell has no opinion about
    /// them.
    [[nodiscard]] const t2d::EcfgValue* find(std::string_view key) const;
};

} // namespace mefemma
