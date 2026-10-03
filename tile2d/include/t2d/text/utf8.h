// Tile2D - UTF-8, the only text encoding the engine accepts.
//
// Decoding is explicit about failure: a malformed sequence yields U+FFFD and advances by one byte, so
// a broken file can never make the layout loop spin or read past the end.
#pragma once

#include <t2d/core/types.h>

#include <string>
#include <string_view>
#include <vector>

namespace t2d {

/// One decoded character.
struct Codepoint {
    u32 value = 0;
    u8 size = 0; ///< bytes consumed
};

/// Decodes the character at \p index. Returns U+FFFD with size 1 for a malformed sequence, and
/// U+0000 with size 0 at the end of the text.
[[nodiscard]] Codepoint utf8_decode(std::string_view text, usize index);

/// Appends \p codepoint to \p out as UTF-8 (U+FFFD when it is out of range).
void utf8_encode(std::string& out, u32 codepoint);
[[nodiscard]] std::string utf8_from_codepoint(u32 codepoint);

/// Number of characters (not bytes). Malformed bytes count as one character each.
[[nodiscard]] usize utf8_length(std::string_view text);

/// Every character in order.
[[nodiscard]] std::vector<u32> utf8_codepoints(std::string_view text);

/// True for the scripts that need a CJK font and full width layout: CJK ideographs, kana, Hangul,
/// full width forms and the CJK punctuation blocks.
[[nodiscard]] bool is_cjk(u32 codepoint);
/// True for characters that must not start a line (。、，！？：；）」』】…).
[[nodiscard]] bool is_closing_punctuation(u32 codepoint);
/// True for characters that must not end a line (（「『【).
[[nodiscard]] bool is_opening_punctuation(u32 codepoint);
/// True for the spaces a line may break at.
[[nodiscard]] bool is_breaking_space(u32 codepoint);

/// Whether a line may be broken between two adjacent characters. This is the small subset of UAX #14
/// a game UI needs: break after a space, break between CJK characters, never break before closing
/// punctuation or after opening punctuation, and never inside a run of Latin letters.
[[nodiscard]] bool can_break_between(u32 previous, u32 next);

} // namespace t2d
