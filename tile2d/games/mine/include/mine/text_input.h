// Mine - the text field's model: UTF-8 text and a byte caret that is always on a character boundary.
//
// The screen owns focus, the keyboard layout and the caret's pixels; this owns the string and where the
// next character goes, so a name typed with CJK, emoji or backspace is testable without a window
// (docs/GAME_DESIGN.md section 1.48).
#pragma once

#include <t2d/core/types.h>

#include <string>
#include <string_view>

namespace mine {

using t2d::usize;

/// What a text field holds.
class TextInput {
public:
    /// The longest name a field accepts, in characters: long enough for a sentence, short enough to be a
    /// directory name (SaveLibrary's limit is the same number of characters).
    static constexpr usize kDefaultMaxCodepoints = 48;

    explicit TextInput(usize max_codepoints = kDefaultMaxCodepoints);

    [[nodiscard]] std::string_view text() const { return text_; }
    /// Byte offset of the caret into text(); always the start of a character.
    [[nodiscard]] usize caret() const { return caret_; }
    /// Characters, not bytes.
    [[nodiscard]] usize length() const;
    [[nodiscard]] bool empty() const { return text_.empty(); }
    [[nodiscard]] usize max_codepoints() const { return max_codepoints_; }

    /// Empties the field; the caret goes to the start.
    void clear();
    /// Replaces the text: what would not be typed is dropped and the rest is truncated to the limit, with
    /// the caret at the end. A name read from somewhere else comes through this.
    void set_text(std::string_view utf8);

    /// Inserts what can be typed at the caret and returns how many characters went in. Malformed bytes,
    /// newline, the C0 controls other than tab and DEL are skipped - a field holds a name, not a
    /// document; a tab is a character a name may hold - and characters past the limit are dropped.
    [[nodiscard]] usize insert(std::string_view utf8);

    /// Deletes the character before the caret / after it. Returns whether anything was removed.
    bool backspace();
    bool erase();

    void move_left();
    void move_right();
    void home();
    void end();

private:
    /// The character boundary before \p at, or 0 at the start of the text.
    [[nodiscard]] usize previous_boundary(usize at) const;

    std::string text_;
    usize caret_ = 0;
    usize max_codepoints_ = kDefaultMaxCodepoints;
};

} // namespace mine
