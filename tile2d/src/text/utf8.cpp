#include <t2d/text/utf8.h>

namespace t2d {
namespace {

constexpr u32 kReplacement = 0xFFFDu;

/// Ranges that need a CJK font and take a full width cell.
[[nodiscard]] bool in_cjk_range(u32 code) {
    return (code >= 0x1100u && code <= 0x11FFu) ||   // Hangul Jamo
           (code >= 0x2E80u && code <= 0x2EFFu) ||   // CJK radicals
           (code >= 0x3000u && code <= 0x303Fu) ||   // CJK punctuation
           (code >= 0x3040u && code <= 0x30FFu) ||   // kana
           (code >= 0x3100u && code <= 0x312Fu) ||   // bopomofo
           (code >= 0x3130u && code <= 0x318Fu) ||   // Hangul compatibility jamo
           (code >= 0x31C0u && code <= 0x31EFu) ||   // CJK strokes
           (code >= 0x3200u && code <= 0x32FFu) ||   // enclosed CJK
           (code >= 0x3300u && code <= 0x33FFu) ||   // CJK compatibility
           (code >= 0x3400u && code <= 0x4DBFu) ||   // CJK extension A
           (code >= 0x4E00u && code <= 0x9FFFu) ||   // CJK unified ideographs
           (code >= 0xA000u && code <= 0xA4CFu) ||   // Yi
           (code >= 0xAC00u && code <= 0xD7AFu) ||   // Hangul syllables
           (code >= 0xF900u && code <= 0xFAFFu) ||   // CJK compatibility ideographs
           (code >= 0xFE30u && code <= 0xFE4Fu) ||   // CJK compatibility forms
           (code >= 0xFF00u && code <= 0xFF60u) ||   // full width forms
           (code >= 0xFFE0u && code <= 0xFFE6u) ||   // full width signs
           (code >= 0x20000u && code <= 0x3FFFDu);   // CJK extensions B and beyond
}

} // namespace

Codepoint utf8_decode(std::string_view text, usize index) {
    if (index >= text.size()) return Codepoint{0u, 0u};
    const u8 first = static_cast<u8>(text[index]);
    if (first < 0x80u) return Codepoint{first, 1u};

    u32 value = 0;
    u8 length = 0;
    u32 minimum = 0;
    if ((first & 0xE0u) == 0xC0u) {
        value = first & 0x1Fu;
        length = 2;
        minimum = 0x80u;
    } else if ((first & 0xF0u) == 0xE0u) {
        value = first & 0x0Fu;
        length = 3;
        minimum = 0x800u;
    } else if ((first & 0xF8u) == 0xF0u) {
        value = first & 0x07u;
        length = 4;
        minimum = 0x10000u;
    } else {
        return Codepoint{kReplacement, 1u}; // a continuation byte or an invalid lead
    }
    if (index + length > text.size()) return Codepoint{kReplacement, 1u};
    for (u8 offset = 1; offset < length; ++offset) {
        const u8 next = static_cast<u8>(text[index + offset]);
        if ((next & 0xC0u) != 0x80u) return Codepoint{kReplacement, 1u};
        value = (value << 6) | (next & 0x3Fu);
    }
    // Overlong encodings, surrogates and out of range values are not characters.
    if (value < minimum || value > 0x10FFFFu || (value >= 0xD800u && value <= 0xDFFFu)) {
        return Codepoint{kReplacement, 1u};
    }
    return Codepoint{value, length};
}

void utf8_encode(std::string& out, u32 codepoint) {
    if (codepoint > 0x10FFFFu || (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) codepoint = kReplacement;
    if (codepoint < 0x80u) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else if (codepoint < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    }
}

std::string utf8_from_codepoint(u32 codepoint) {
    std::string out;
    utf8_encode(out, codepoint);
    return out;
}

usize utf8_length(std::string_view text) {
    usize count = 0;
    for (usize index = 0; index < text.size();) {
        const Codepoint point = utf8_decode(text, index);
        index += point.size == 0 ? 1 : point.size;
        ++count;
    }
    return count;
}

std::vector<u32> utf8_codepoints(std::string_view text) {
    std::vector<u32> out;
    out.reserve(text.size());
    for (usize index = 0; index < text.size();) {
        const Codepoint point = utf8_decode(text, index);
        index += point.size == 0 ? 1 : point.size;
        out.push_back(point.value);
    }
    return out;
}

bool is_cjk(u32 codepoint) { return in_cjk_range(codepoint); }

bool is_closing_punctuation(u32 codepoint) {
    switch (codepoint) {
        case 0x3001u: // 、
        case 0x3002u: // 。
        case 0xFF0Cu: // ，
        case 0xFF0Eu: // ．
        case 0xFF01u: // ！
        case 0xFF1Fu: // ？
        case 0xFF1Au: // ：
        case 0xFF1Bu: // ；
        case 0xFF09u: // ）
        case 0x300Du: // 」
        case 0x300Fu: // 』
        case 0x3011u: // 】
        case 0x2026u: // …
        case 0x201Du: // ”
        case 0x2019u: // ’
        case 0x002Cu: // ,
        case 0x002Eu: // .
        case 0x003Au: // :
        case 0x003Bu: // ;
        case 0x0021u: // !
        case 0x003Fu: // ?
        case 0x0029u: // )
        case 0x005Du: // ]
        case 0x007Du: // }
            return true;
        default: return false;
    }
}

bool is_opening_punctuation(u32 codepoint) {
    switch (codepoint) {
        case 0xFF08u: // （
        case 0x300Cu: // 「
        case 0x300Eu: // 『
        case 0x3010u: // 【
        case 0x201Cu: // “
        case 0x2018u: // ‘
        case 0x0028u: // (
        case 0x005Bu: // [
        case 0x007Bu: // {
            return true;
        default: return false;
    }
}

bool is_breaking_space(u32 codepoint) {
    return codepoint == 0x20u || codepoint == 0x09u || codepoint == 0x3000u /* ideographic space */;
}

bool can_break_between(u32 previous, u32 next) {
    if (next == 0u || previous == 0u) return false;
    if (next == 0x0Au || next == 0x0Du) return true;             // an explicit newline always breaks
    if (is_breaking_space(previous)) return true;                 // after a space
    if (is_opening_punctuation(previous)) return false;           // never after an opening bracket
    if (is_closing_punctuation(next)) return false;               // never before a closing one
    if (is_cjk(previous) || is_cjk(next)) return true;            // CJK breaks anywhere
    return false;                                                 // inside a Latin word: no
}

} // namespace t2d
