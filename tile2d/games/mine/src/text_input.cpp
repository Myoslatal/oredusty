#include <mine/text_input.h>

#include <t2d/text/utf8.h>

namespace mine {
namespace {

/// True for a character the field will not hold: the C0 controls other than tab (a name may hold a tab;
/// the screen inserts one for the Tab key) and DEL.
[[nodiscard]] bool is_untypeable(t2d::u32 codepoint) {
    return (codepoint < 0x20u && codepoint != 0x09u) || codepoint == 0x7Fu;
}

} // namespace

TextInput::TextInput(usize max_codepoints) : max_codepoints_(max_codepoints) {}

usize TextInput::length() const { return t2d::utf8_length(text_); }

void TextInput::clear() {
    text_.clear();
    caret_ = 0;
}

void TextInput::set_text(std::string_view utf8) {
    clear();
    (void)insert(utf8);
}

usize TextInput::insert(std::string_view utf8) {
    std::string accepted;
    usize added = 0;
    const usize total = length();
    usize index = 0;
    while (index < utf8.size()) {
        const t2d::Codepoint point = t2d::utf8_decode(utf8, index);
        if (point.size == 0) break;
        index += point.size;
        // A malformed sequence decodes to U+FFFD with size 1; a real U+FFFD is three bytes and typed.
        if (point.value == 0xFFFDu && point.size == 1u) continue;
        if (is_untypeable(point.value)) continue;
        if (total + added >= max_codepoints_) break;
        t2d::utf8_encode(accepted, point.value);
        ++added;
    }
    if (accepted.empty()) return 0;
    text_.insert(caret_, accepted);
    // The caret was on a boundary and the insert was whole characters, so it still is one.
    caret_ += accepted.size();
    return added;
}

usize TextInput::previous_boundary(usize at) const {
    if (at == 0) return 0;
    usize index = 0;
    usize previous = 0;
    while (index < at && index < text_.size()) {
        previous = index;
        const t2d::Codepoint point = t2d::utf8_decode(text_, index);
        index += point.size == 0 ? 1 : point.size;
    }
    return previous;
}

bool TextInput::backspace() {
    if (caret_ == 0) return false;
    const usize start = previous_boundary(caret_);
    text_.erase(start, caret_ - start);
    caret_ = start;
    return true;
}

bool TextInput::erase() {
    if (caret_ >= text_.size()) return false;
    const t2d::Codepoint point = t2d::utf8_decode(text_, caret_);
    text_.erase(caret_, point.size == 0 ? 1 : point.size);
    return true;
}

void TextInput::move_left() { caret_ = previous_boundary(caret_); }

void TextInput::move_right() {
    if (caret_ >= text_.size()) return;
    const t2d::Codepoint point = t2d::utf8_decode(text_, caret_);
    caret_ += point.size == 0 ? 1 : point.size;
}

void TextInput::home() { caret_ = 0; }

void TextInput::end() { caret_ = text_.size(); }

} // namespace mine
