// Tile2D - the font engine: TrueType and CFF outlines, metrics and character mapping.
//
// A font file is a container of tables; the outlines inside are either TrueType quadratic contours
// ("glyf", sfnt version 0x00010000) or CFF Type 2 charstrings ("CFF ", sfnt version "OTTO"). Both are
// supported because the fonts a game actually needs are split that way: Latin text usually ships as
// TrueType, the CJK families (Noto Sans CJK and friends) ship as CID keyed CFF. .ttc collections are
// handled too, so one file can carry several faces.
//
// Coordinates are font units with y pointing up and the origin on the baseline; the caller scales by
// pixel_size / units_per_em. Nothing here touches the GPU: rasterising a glyph is a separate step
// (raster.h) and caching it in a texture is another one (glyph_atlas.h).
#pragma once

#include <t2d/core/types.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace t2d {

class CffFont;

/// A point in font units.
struct FontPoint {
    f32 x = 0.0f;
    f32 y = 0.0f;
};

/// Outline commands. Curves are kept as curves: the rasteriser flattens them at the pixel size it
/// actually draws at, which is where the accuracy matters.
enum class PathVerb : u8 { Move, Line, Quad, Cubic, Close };

struct PathCommand {
    PathVerb verb = PathVerb::Close;
    FontPoint points[3]{}; ///< Move/Line: 1 point, Quad: control+end, Cubic: two controls+end
    u8 count = 0;
};

/// One closed contour is a Move followed by lines/curves and a Close.
class GlyphPath {
public:
    void move_to(f32 x, f32 y);
    void line_to(f32 x, f32 y);
    void quad_to(f32 cx, f32 cy, f32 x, f32 y);
    void cubic_to(f32 c1x, f32 c1y, f32 c2x, f32 c2y, f32 x, f32 y);
    void close();
    void clear() { commands_.clear(); }

    [[nodiscard]] ConstSpan<PathCommand> commands() const {
        return ConstSpan<PathCommand>(commands_.data(), commands_.size());
    }
    [[nodiscard]] bool empty() const { return commands_.empty(); }
    /// Control point bounds; exact enough to size an atlas slot before rasterising.
    [[nodiscard]] bool bounds(FontPoint& min, FontPoint& max) const;

private:
    friend class Font; ///< composite glyphs append transformed component outlines

    std::vector<PathCommand> commands_;
};

struct FontMetrics {
    f32 units_per_em = 1000.0f;
    f32 ascender = 0.0f;   ///< hhea ascent, font units above the baseline
    f32 descender = 0.0f;  ///< hhea descent, negative
    f32 line_gap = 0.0f;
    f32 underline_position = 0.0f;
    f32 underline_thickness = 0.0f;
    u32 glyph_count = 0;
    u32 face_index = 0;
    u32 face_count = 1;
};

/// Tables a font may carry. Real fonts have around twenty (Liberation Sans has nineteen), and the
/// directory is only capped to keep the font object small.
inline constexpr u32 kMaxFontTables = 32;

/// A loaded font. Immutable once parsed; glyphs are looked up by code point and rasterised on demand.
class Font {
public:
    [[nodiscard]] static std::optional<Font> load(const std::string& path, u32 face_index = 0,
                                                  std::string* error = nullptr);
    [[nodiscard]] static std::optional<Font> load_from_memory(std::vector<u8> data, u32 face_index = 0,
                                                              std::string* error = nullptr);
    /// Family and style of every face in a file, read from the table directories and the name tables
    /// only. Picking a face out of a collection (Noto Sans CJK carries ten, and Simplified and
    /// Traditional Chinese want different ones) must not cost ten outline parses.
    [[nodiscard]] static std::vector<std::pair<std::string, std::string>> face_names(const std::string& path);

    [[nodiscard]] const FontMetrics& metrics() const { return metrics_; }
    [[nodiscard]] const char* outline_kind() const { return cff_ ? "cff" : "truetype"; }
    [[nodiscard]] std::string_view family_name() const { return family_name_; }
    [[nodiscard]] std::string_view style_name() const { return style_name_; }

    /// Glyph index for a code point (0 = .notdef when the font has no such character).
    [[nodiscard]] u32 glyph_index(u32 codepoint) const;
    /// Advance width in font units.
    [[nodiscard]] f32 advance(u32 glyph) const;
    /// Left side bearing in font units (xMin of the outline).
    [[nodiscard]] f32 bearing(u32 glyph) const;
    /// Outline of \p glyph in font units. Empty for a blank glyph (space) or a glyph without an
    /// outline; false when the glyph cannot be decoded at all.
    [[nodiscard]] bool glyph_path(u32 glyph, GlyphPath& out) const;

private:
    Font() = default;

    struct Table {
        u32 tag = 0;
        usize offset = 0;
        usize length = 0;
        bool present = false;
    };
    [[nodiscard]] const Table& table(u32 tag) const;
    [[nodiscard]] ConstSpan<const u8> table_bytes(u32 tag) const;
    [[nodiscard]] bool parse_glyf_glyph(u32 glyph, GlyphPath& out, u32 depth) const;

    std::vector<u8> data_;
    Table tables_[kMaxFontTables]{};
    u32 table_count_ = 0;
    FontMetrics metrics_{};
    std::string family_name_;
    std::string style_name_;

    // cmap
    usize cmap_offset_ = 0;
    usize cmap_length_ = 0;
    u16 cmap_format_ = 0;

    // glyf / loca
    usize glyf_offset_ = 0;
    usize loca_offset_ = 0;
    bool long_loca_ = false;

    // CFF outlines, when the font is "OTTO" (see cff.h)
    std::shared_ptr<const CffFont> cff_;
};

} // namespace t2d
