#include <mefemma/path_input.h>

#include <t2d/text/utf8.h>

namespace mefemma {
namespace {

/// True for a character the field will not hold: the C0 controls other than tab and DEL. A path is one line;
/// a tab is still a byte a path could contain on POSIX, so it is allowed through like it is in the mine's
/// name field.
[[nodiscard]] bool is_untypeable(t2d::u32 codepoint) {
    return (codepoint < 0x20u && codepoint != 0x09u) || codepoint == 0x7Fu;
}

} // namespace

usize PathInput::length() const { return t2d::utf8_length(text_); }

void PathInput::clear() {
    text_.clear();
    caret_ = 0;
}

void PathInput::set_text(std::string_view utf8) {
    clear();
    (void)insert(utf8);
}

usize PathInput::insert(std::string_view utf8) {
    std::string accepted;
    usize added = 0;
    const usize total = length();
    usize index = 0;
    while (index < utf8.size()) {
        const t2d::Codepoint point = t2d::utf8_decode(utf8, index);
        if (point.size == 0) break;
        index += point.size;
        // A malformed sequence decodes to U+FFFD with size 1; a real U+FFFD is three bytes and is typed.
        if (point.value == 0xFFFDu && point.size == 1u) continue;
        if (is_untypeable(point.value)) continue;
        if (total + added >= kMaxCodepoints) break;
        t2d::utf8_encode(accepted, point.value);
        ++added;
    }
    if (accepted.empty()) return 0;
    text_.insert(caret_, accepted);
    // The caret was on a boundary and the insert was whole characters, so it still is one.
    caret_ += accepted.size();
    return added;
}

usize PathInput::previous_boundary(usize at) const {
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

bool PathInput::backspace() {
    if (caret_ == 0) return false;
    const usize start = previous_boundary(caret_);
    text_.erase(start, caret_ - start);
    caret_ = start;
    return true;
}

bool PathInput::erase() {
    if (caret_ >= text_.size()) return false;
    const t2d::Codepoint point = t2d::utf8_decode(text_, caret_);
    text_.erase(caret_, point.size == 0 ? 1 : point.size);
    return true;
}

void PathInput::move_left() { caret_ = previous_boundary(caret_); }

void PathInput::move_right() {
    if (caret_ >= text_.size()) return;
    const t2d::Codepoint point = t2d::utf8_decode(text_, caret_);
    caret_ += point.size == 0 ? 1 : point.size;
}

void PathInput::home() { caret_ = 0; }

void PathInput::end() { caret_ = text_.size(); }

} // namespace mefemma
