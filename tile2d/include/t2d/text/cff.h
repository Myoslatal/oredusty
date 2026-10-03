// Tile2D - CFF (Type 2 charstrings) outlines, the format the CJK families ship in.
//
// The CFF table is parsed once into a context; glyph outlines are then produced on demand. This is the
// only part of the font engine that has to interpret a program (Type 2 charstrings are a small stack
// language), so it is kept behind this narrow interface: give it the "CFF " table bytes, ask for a
// glyph id, get a GlyphPath back.
#pragma once

#include <t2d/core/types.h>
#include <t2d/text/font.h>

#include <memory>
#include <optional>
#include <string>

namespace t2d {

class CffFont {
public:
    /// Parses the "CFF " table. Returns nullopt (with a reason in \p error) for anything it cannot
    /// read completely: bad header, missing CharStrings, an INDEX whose offsets run past the table.
    [[nodiscard]] static std::shared_ptr<const CffFont> parse(ConstSpan<const u8> cff_table,
                                                              std::string* error = nullptr);

    [[nodiscard]] u32 glyph_count() const { return glyph_count_; }
    /// Outline of \p glyph in font units (y up, baseline at 0). Returns false when the charstring
    /// cannot be interpreted; a blank glyph (for example the space) yields an empty path and true.
    [[nodiscard]] bool glyph_path(u32 glyph, GlyphPath& out) const;
    /// Width of \p glyph in font units, as the charstring declares it (nominalWidthX + delta).
    [[nodiscard]] f32 advance(u32 glyph) const;
    /// True when the font is CID keyed (which is what the CJK families are).
    [[nodiscard]] bool is_cid_keyed() const { return is_cid_keyed_; }

private:
    CffFont() = default;
    struct Impl;
    std::shared_ptr<const Impl> impl_;
    u32 glyph_count_ = 0;
    bool is_cid_keyed_ = false;
};

} // namespace t2d
