// Tile2D - text layout, the CPU half: which font draws which character, how wide a line is and
// where every glyph sits. No GPU and no atlas are involved, so a dedicated server can measure and
// place a string with the same code the renderer uses.
//
// The vertical contract is one sentence: **the pen is the top left corner of the first line box.**
// A line box is as tall as the font's own line (and never shorter than TextStyle::line_spacing times
// the size), its baseline sits LineBox::ascent below the pen, and every glyph of a line is drawn
// inside that line's box. A caller that wants a background behind a line therefore draws the box
// that the pen and the metrics describe, with no guessed offset: that is what makes a highlighted
// row and its text line up.
#pragma once

#include <t2d/core/types.h>
#include <t2d/text/font.h>

#include <string_view>
#include <vector>

namespace t2d {

/// The fonts a language needs. \p cjk may be null for a Latin only interface; \p latin is required.
struct FontSet {
    const Font* latin = nullptr;
    const Font* cjk = nullptr;

    /// The font that should draw \p codepoint: CJK characters go to the CJK font, everything else to
    /// the Latin one, and a character the chosen font lacks falls back to the other.
    [[nodiscard]] const Font* pick(u32 codepoint) const;
};

struct TextStyle {
    u16 size_px = 16;
    u32 color = 0xFFFFFFFFu;
    /// The line advance as a multiple of \c size_px. It is a floor, not a ceiling: a font whose own
    /// line box is taller than this wins, because a box shorter than its font would put one line's
    /// ink on top of the next line's.
    f32 line_spacing = 1.25f;
    f32 letter_spacing = 0.0f;
};

/// The vertical metrics of one line. The top of the box is where the pen is, the baseline is
/// \c ascent below it, and the box is \c height tall.
struct LineBox {
    f32 ascent = 0.0f;   ///< top of the box down to the baseline
    f32 descent = 0.0f;  ///< baseline down to the bottom of the box
    f32 height = 0.0f;   ///< never less than ascent + descent
};

/// The line box of \p style with \p fonts. It depends on the fonts and the size, never on the
/// string: a label and the value beside it share one baseline, and a row's background is the same
/// rectangle whatever text happens to be in it.
[[nodiscard]] LineBox line_box(const FontSet& fonts, const TextStyle& style);

struct TextMetrics {
    f32 width = 0.0f;   ///< widest line
    f32 height = 0.0f;  ///< every line box: the ink lies inside [pen.y, pen.y + height]
    f32 ascent = 0.0f;  ///< top of the first line box down to its baseline
    f32 descent = 0.0f; ///< first baseline down to the bottom of the first line box
    u32 lines = 0;
    usize characters = 0;
    /// Characters no font in the set had. Reported rather than silently skipped: a missing glyph is a
    /// content or font problem, not something to hide.
    usize missing = 0;
};

/// One glyph the layout placed: which font and glyph, and where its baseline starts.
struct PlacedGlyph {
    const Font* font = nullptr;
    u32 glyph = 0;
    f32 x = 0.0f;   ///< left edge of the advance, measured from the pen
    f32 y = 0.0f;   ///< baseline of the glyph's line, measured down from the pen
};

/// Lays \p text out: fills \p out with the position of every glyph and \p metrics with the box
/// those glyphs live in. Lines longer than \p max_width wrap where the script allows it. Nothing is
/// rasterised and nothing is drawn, so measuring costs no more than laying out.
void layout_text(std::string_view text, const FontSet& fonts, const TextStyle& style, f32 max_width,
                 std::vector<PlacedGlyph>& out, TextMetrics& metrics);

} // namespace t2d
