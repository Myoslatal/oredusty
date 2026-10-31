// Mefemma - the first-run path field's model: UTF-8 text and a byte caret on a character boundary.
//
// The screen owns where the field is, which key is which and how the caret is drawn; this owns the string
// and where the next character goes. That split is what makes a typed CJK or emoji path testable without a
// window, and it is the same split the mine's name field uses - this is Mefemma's own copy, not a link to
// the mine.
#pragma once

#include <t2d/core/types.h>

#include <string>
#include <string_view>

namespace mefemma {

using t2d::usize;

/// What the first-run path field holds.
class PathInput {
public:
    /// A path field takes more than a save name: 512 characters is far more than a file system path needs
    /// and still bounds what one frame can hold.
    static constexpr usize kMaxCodepoints = 512;

    PathInput() = default;

    [[nodiscard]] std::string_view text() const { return text_; }
    /// Byte offset into text(); always the start of a character.
    [[nodiscard]] usize caret() const { return caret_; }
    /// Characters, not bytes.
    [[nodiscard]] usize length() const;
    [[nodiscard]] bool empty() const { return text_.empty(); }
    [[nodiscard]] static constexpr usize max_codepoints() { return kMaxCodepoints; }

    /// Empties the field; the caret goes to the start.
    void clear();
    /// Replaces the text: what would not be typed is dropped, the rest is truncated to the limit, and the
    /// caret goes to the end. A path read from somewhere else comes through this.
    void set_text(std::string_view utf8);

    /// Inserts what can be typed at the caret and returns how many characters went in. Malformed UTF-8, the
    /// C0 controls other than tab and DEL are skipped; characters past the limit are dropped.
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
};

} // namespace mefemma
