// The text field a save name is typed into: UTF-8 text, a caret on character boundaries, CJK and emoji as
// one character each, backspace and delete by character, and a limit that counts characters rather than
// bytes (mine/text_input.h, docs/GAME_DESIGN.md section 1.48).
#include <mine/text_input.h>

#include <support/test_support.h>

#include <t2d/text/utf8.h>

#include <string>
#include <string_view>

using namespace mine;

T2D_TEST(the_caret_is_a_place_in_the_character_stream) {
    TextInput input;
    T2D_CHECK(input.empty());
    T2D_CHECK_EQ(input.length(), 0u);
    T2D_CHECK_EQ(input.max_codepoints(), TextInput::kDefaultMaxCodepoints);

    T2D_CHECK_EQ(input.insert("abc"), 3u);
    T2D_CHECK_EQ(input.text(), std::string_view("abc"));
    T2D_CHECK_EQ(input.caret(), 3u);
    T2D_CHECK_EQ(input.length(), 3u);

    // The caret is where the next character goes, so inserting mid text grows around it.
    input.home();
    T2D_CHECK_EQ(input.caret(), 0u);
    T2D_CHECK_EQ(input.insert("X"), 1u);
    T2D_CHECK_EQ(input.text(), std::string_view("Xabc"));
    T2D_CHECK_EQ(input.caret(), 1u);

    input.end();
    T2D_CHECK_EQ(input.caret(), 4u);
    input.move_left();
    input.move_left();
    T2D_CHECK_EQ(input.caret(), 2u);
    T2D_CHECK_EQ(input.insert("Y"), 1u);
    T2D_CHECK_EQ(input.text(), std::string_view("XaYbc"));
    T2D_CHECK_EQ(input.caret(), 3u);
}

T2D_TEST(cjk_and_emoji_are_single_characters) {
    TextInput input;
    T2D_CHECK_EQ(input.insert("\xE4\xBD\xA0\xE5\xA5\xBD"), 2u);   // 你好: two characters, six bytes
    T2D_CHECK_EQ(input.length(), 2u);
    T2D_CHECK_EQ(input.caret(), 6u);
    T2D_CHECK_EQ(input.insert("\xF0\x9F\x99\x82"), 1u);   // 🙂: one character, four bytes
    T2D_CHECK_EQ(input.length(), 3u);
    T2D_CHECK_EQ(input.caret(), 10u);

    // A move is by character: a three byte one is no more than one step.
    input.home();
    input.move_right();
    T2D_CHECK_EQ(input.caret(), 3u);
    T2D_CHECK(input.backspace());
    T2D_CHECK_EQ(input.text(), std::string_view("\xE5\xA5\xBD\xF0\x9F\x99\x82"));   // 好🙂
    T2D_CHECK_EQ(input.caret(), 0u);
    T2D_CHECK(input.erase());
    T2D_CHECK_EQ(input.text(), std::string_view("\xF0\x9F\x99\x82"));
    T2D_CHECK_EQ(input.length(), 1u);

    // The ends are places too, and moving past them does nothing.
    input.end();
    input.move_right();
    T2D_CHECK_EQ(input.caret(), 4u);
    input.home();
    input.move_left();
    T2D_CHECK_EQ(input.caret(), 0u);
}

T2D_TEST(the_limit_counts_characters_not_bytes) {
    TextInput input(4);
    T2D_CHECK_EQ(input.insert("abcdef"), 4u);
    T2D_CHECK_EQ(input.text(), std::string_view("abcd"));
    T2D_CHECK_EQ(input.insert("e"), 0u);
    T2D_CHECK_EQ(input.length(), 4u);

    // Four CJK characters are twelve bytes and still fit: the limit is characters.
    input.set_text("\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C\xE5\x95\x8A");   // 你好世界啊
    T2D_CHECK_EQ(input.length(), 4u);
    T2D_CHECK_EQ(input.text(), std::string_view("\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C"));
    T2D_CHECK_EQ(input.caret(), 12u);

    input.set_text("ab");
    T2D_CHECK_EQ(input.caret(), 2u);
    input.clear();
    T2D_CHECK(input.empty());
    T2D_CHECK_EQ(input.caret(), 0u);

    // The default limit is the save name limit: a full name goes in, one more character does not.
    TextInput wide;
    const std::string full(TextInput::kDefaultMaxCodepoints, 'a');
    T2D_CHECK_EQ(wide.insert(full), TextInput::kDefaultMaxCodepoints);
    T2D_CHECK_EQ(wide.insert("b"), 0u);
    T2D_CHECK_EQ(wide.length(), TextInput::kDefaultMaxCodepoints);
}

T2D_TEST(malformed_bytes_and_controls_are_skipped) {
    TextInput input;
    std::string mixed;
    mixed += 'a';
    mixed += '\n';
    mixed += 'b';
    mixed += '\t';
    mixed += 'c';
    mixed.push_back('\0');
    mixed += 'd';
    mixed.push_back(static_cast<char>(0x7F));
    mixed += 'e';
    // A tab is a character a name may hold; the newline, NUL and DEL in the same string are not.
    T2D_CHECK_EQ(input.insert(mixed), 6u);
    T2D_CHECK_EQ(input.text(), std::string_view("ab\tcde"));

    // A byte that is not UTF-8, and a lead byte whose continuation is not one, are skipped one at a time.
    std::string broken;
    broken += 'x';
    broken.push_back(static_cast<char>(0xFF));
    broken += 'y';
    broken.push_back(static_cast<char>(0xC3));
    broken += 'z';
    T2D_CHECK_EQ(input.insert(broken), 3u);
    T2D_CHECK_EQ(input.text(), std::string_view("ab\tcdexyz"));

    // A real replacement character is text the player may type; only the broken encoding is dropped.
    std::string replacement;
    t2d::utf8_encode(replacement, 0xFFFDu);
    T2D_CHECK_EQ(input.insert(replacement), 1u);
    T2D_CHECK_EQ(input.length(), 10u);
}

T2D_TEST(a_tab_is_a_character_the_field_keeps) {
    TextInput input;
    T2D_CHECK_EQ(input.insert("\t"), 1u);
    T2D_CHECK_EQ(input.text(), std::string_view("\t"));
    T2D_CHECK_EQ(input.length(), 1u);
    T2D_CHECK_EQ(input.caret(), 1u);

    // It moves and deletes like any other character, and the other controls are still not text.
    input.home();
    T2D_CHECK_EQ(input.insert("a"), 1u);
    T2D_CHECK_EQ(input.text(), std::string_view("a\t"));
    T2D_CHECK_EQ(input.insert("\n\r"), 0u);
    T2D_CHECK_EQ(input.length(), 2u);
    input.end();
    T2D_CHECK(input.backspace());
    T2D_CHECK_EQ(input.text(), std::string_view("a"));
}

T2D_TEST(backspace_and_erase_remove_whole_characters) {
    TextInput input;
    T2D_CHECK_FALSE(input.backspace());
    T2D_CHECK_FALSE(input.erase());
    T2D_CHECK_EQ(input.insert("a\xE4\xBD\xA0" "b"), 3u);   // a你b

    input.home();
    T2D_CHECK(input.erase());   // the 'a' after the caret
    T2D_CHECK_EQ(input.text(), std::string_view("\xE4\xBD\xA0" "b"));
    T2D_CHECK(input.erase());   // the CJK character, all three bytes
    T2D_CHECK_EQ(input.text(), std::string_view("b"));
    T2D_CHECK_EQ(input.caret(), 0u);
    T2D_CHECK_FALSE(input.backspace());

    input.end();
    T2D_CHECK(input.backspace());
    T2D_CHECK(input.empty());
    T2D_CHECK_FALSE(input.erase());
}

T2D_TEST(set_text_cleans_what_it_is_given_and_clear_empties) {
    TextInput input;
    T2D_CHECK_EQ(input.insert("old"), 3u);
    input.set_text("a\nb\tc");   // the newline goes, the tab stays where it was
    T2D_CHECK_EQ(input.text(), std::string_view("ab\tc"));
    T2D_CHECK_EQ(input.length(), 4u);
    T2D_CHECK_EQ(input.caret(), 4u);

    TextInput narrow(3);
    narrow.set_text("abcdef");
    T2D_CHECK_EQ(narrow.text(), std::string_view("abc"));
    T2D_CHECK_EQ(narrow.length(), 3u);

    input.clear();
    T2D_CHECK(input.empty());
    T2D_CHECK_EQ(input.caret(), 0u);
    T2D_CHECK_EQ(input.length(), 0u);
}

T2D_TEST_MAIN
