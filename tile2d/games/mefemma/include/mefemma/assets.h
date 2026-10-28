// Mefemma - the files a game draws with: fonts, the string table, and pictures.
//
// Assets belong to the shell, not to a scene: a scene names a picture ("start_screen") and a string id, and
// never owns a texture or a font. That is what keeps a scene testable - it only ever sees names - and what
// makes loading, language switching and lifetime one decision in one place.
//
// Pictures are loaded on first use and cached by name. One picture is one texture: the framework does not
// pack art into an atlas, so a scene draws a 16x16 picture and a 512x512 picture the same way, and the
// filenames under assets/textures are the names a scene uses.
#pragma once

#include <mefemma/data_paths.h>

#include <t2d/core/types.h>
#include <t2d/text/font.h>
#include <t2d/text/locale.h>
#include <t2d/text/text_layout.h>

#include <ore/rhi/context.h>
#include <ore/rhi/texture.h>

#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mefemma {

using t2d::Scope;
using t2d::u32;
using t2d::usize;

/// Fonts, strings and pictures, loaded from the game's assets directory.
class Assets {
public:
    /// \p context is where textures are made. \p overrides are file paths a command line named; empty
    /// means "the first system font found", which is what a game with no font of its own uses.
    Assets(ore::rhi::GraphicsContext& context, DataPaths paths, std::string latin_font_override = {},
           std::string cjk_font_override = {});
    ~Assets();
    T2D_NON_MOVABLE(Assets);

    /// Reads assets/text/ui.ecfg. A missing file is reported and leaves an empty table: every label then
    /// shows its id, which is a visible failure rather than a crash.
    bool load_text();
    /// Loads the fonts the active language needs. A CJK collection gets the face that matches the language.
    bool load_fonts();
    /// Switches the string table and reloads the fonts the language needs. The caller re-hands fonts() to
    /// the renderer afterwards.
    void set_language(t2d::Language language);

    [[nodiscard]] const t2d::Locale& locale() const { return locale_; }
    [[nodiscard]] const t2d::FontSet& fonts() const { return fonts_; }
    [[nodiscard]] const DataPaths& paths() const { return paths_; }

    /// The picture \p name, loaded from assets/textures/<name>.png on first use. Nullptr when the file is
    /// missing or cannot be decoded; the reason is logged once, and the renderer draws a visible stand-in so
    /// the frame does not silently lose content.
    [[nodiscard]] ore::rhi::Texture* texture(std::string_view name);
    /// True once a picture named \p name has been found and decoded.
    [[nodiscard]] bool has_texture(std::string_view name) const;
    /// The pictures that failed, in the order they were asked for: what a debug overlay or a log line
    /// reports.
    [[nodiscard]] const std::vector<std::string>& texture_errors() const { return texture_errors_; }

private:
    ore::rhi::GraphicsContext* context_ = nullptr;
    DataPaths paths_;
    std::string latin_font_override_;
    std::string cjk_font_override_;

    t2d::Locale locale_{};
    /// The Latin face is shared by every language; a CJK collection gets the face that matches the
    /// language, and **the faces that were loaded stay loaded**. A glyph is cached by the font's address,
    /// so a font destroyed on a language switch and reallocated somewhere else could hand back another
    /// font's glyphs. Keeping them is the cheap half of that problem.
    Scope<t2d::Font> latin_font_;
    std::array<Scope<t2d::Font>, t2d::kLanguageCount> cjk_fonts_{};
    t2d::FontSet fonts_{};

    std::unordered_map<std::string, Scope<ore::rhi::Texture>> textures_;
    std::unordered_map<std::string, bool> texture_failed_;
    std::vector<std::string> texture_errors_;
};

} // namespace mefemma
