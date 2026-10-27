// The command list a scene fills: what a rectangle, an outline, a picture and a line of text become, and
// that the order a scene asked for is the order the renderer gets.
#include <mefemma/draw_list.h>

#include <support/test_support.h>

using namespace mefemma;

T2D_TEST(a_new_list_is_empty_and_clear_empties_it) {
    DrawList list;
    T2D_CHECK(list.empty());
    T2D_CHECK_EQ(list.size(), 0u);
    list.rect(Aabb2{Vec2{0.0f, 0.0f}, Vec2{10.0f, 10.0f}}, rgba(1, 2, 3, 4));
    T2D_CHECK_FALSE(list.empty());
    list.clear();
    T2D_CHECK(list.empty());
    T2D_CHECK_EQ(list.rect_count(), 0u);
    T2D_CHECK_EQ(list.sprite_count(), 0u);
    T2D_CHECK_EQ(list.text_count(), 0u);
}

T2D_TEST(colours_are_packed_red_in_the_low_byte) {
    const u32 colour = rgba(0x11, 0x22, 0x33, 0x44);
    T2D_CHECK_EQ(colour, 0x44332211u);
    T2D_CHECK_EQ(kWhite, 0xFFFFFFFFu);
    const Aabb2 uv = full_uv();
    T2D_CHECK_EQ(uv.min.x, 0.0f);
    T2D_CHECK_EQ(uv.max.y, 1.0f);
}

T2D_TEST(a_rectangle_carries_its_box_and_its_colour) {
    DrawList list;
    const Aabb2 box{Vec2{1.0f, 2.0f}, Vec2{11.0f, 22.0f}};
    list.rect(box, rgba(10, 20, 30));
    T2D_REQUIRE(list.size() == 1u);
    const DrawCommand& command = list.commands()[0];
    T2D_CHECK(command.kind == DrawCommand::Kind::Rect);
    T2D_CHECK_EQ(command.rect, box);
    T2D_CHECK_EQ(command.color, rgba(10, 20, 30));
    T2D_CHECK_EQ(list.rect_count(), 1u);
}

T2D_TEST(an_outline_is_four_rectangles_and_a_thick_one_is_a_filled_box) {
    DrawList list;
    const Aabb2 box{Vec2{0.0f, 0.0f}, Vec2{100.0f, 60.0f}};
    list.rect_outline(box, 2.0f, rgba(200, 100, 50));
    T2D_REQUIRE(list.size() == 4u);
    T2D_CHECK_EQ(list.rect_count(), 4u);
    // Top: full width, 2 high, at the top edge.
    T2D_CHECK_EQ(list.commands()[0].rect.min.y, 0.0f);
    T2D_CHECK_EQ(list.commands()[0].rect.max.y, 2.0f);
    T2D_CHECK_EQ(list.commands()[0].rect.max.x, 100.0f);
    // Bottom: the last 2 pixels.
    T2D_CHECK_EQ(list.commands()[1].rect.min.y, 58.0f);
    T2D_CHECK_EQ(list.commands()[1].rect.max.y, 60.0f);
    // Left and right do not run under the top and bottom.
    T2D_CHECK_EQ(list.commands()[2].rect.min.y, 2.0f);
    T2D_CHECK_EQ(list.commands()[2].rect.max.y, 58.0f);
    T2D_CHECK_EQ(list.commands()[3].rect.min.x, 98.0f);

    DrawList thick;
    thick.rect_outline(box, 40.0f, rgba(200, 100, 50));
    T2D_REQUIRE(thick.size() == 1u);
    T2D_CHECK_EQ(thick.commands()[0].kind, DrawCommand::Kind::Rect);
    T2D_CHECK_EQ(thick.commands()[0].rect, box);

    DrawList nothing;
    nothing.rect_outline(box, 0.0f, kWhite);
    nothing.rect_outline(Aabb2{Vec2{0.0f, 0.0f}, Vec2{0.0f, 10.0f}}, 2.0f, kWhite);
    T2D_CHECK(nothing.empty());
}

T2D_TEST(a_picture_defaults_to_its_whole_texture_and_keeps_its_name) {
    DrawList list;
    const Aabb2 box{Vec2{10.0f, 10.0f}, Vec2{74.0f, 74.0f}};
    list.sprite("placeholder", box);
    T2D_REQUIRE(list.size() == 1u);
    const DrawCommand& command = list.commands()[0];
    T2D_CHECK(command.kind == DrawCommand::Kind::Sprite);
    T2D_CHECK_EQ(command.texture, std::string("placeholder"));
    T2D_CHECK_EQ(command.uv, full_uv());
    T2D_CHECK_EQ(command.color, kWhite);
    T2D_CHECK_EQ(list.sprite_count(), 1u);

    const Aabb2 part{Vec2{0.0f, 0.0f}, Vec2{0.5f, 0.5f}};
    list.sprite("atlas", box, part, rgba(255, 0, 0));
    T2D_REQUIRE(list.size() == 2u);
    T2D_CHECK_EQ(list.commands()[1].uv, part);
    T2D_CHECK_EQ(list.commands()[1].color, rgba(255, 0, 0));
}

T2D_TEST(a_line_of_text_carries_its_pen_size_colour_and_wrap_width) {
    DrawList list;
    list.text("hello", Vec2{12.0f, 34.0f}, 20, rgba(9, 8, 7), 300.0f);
    T2D_REQUIRE(list.size() == 1u);
    const DrawCommand& command = list.commands()[0];
    T2D_CHECK(command.kind == DrawCommand::Kind::Text);
    T2D_CHECK_EQ(command.text, std::string("hello"));
    T2D_CHECK_EQ(command.pen, (Vec2{12.0f, 34.0f}));
    T2D_CHECK_EQ(command.size_px, 20u);
    T2D_CHECK_EQ(command.color, rgba(9, 8, 7));
    T2D_CHECK_EQ(command.max_width, 300.0f);
    // An empty string is not a command: there is nothing to draw and nothing to measure.
    list.text("", Vec2{0.0f, 0.0f}, 16, kWhite);
    T2D_CHECK_EQ(list.size(), 1u);
    T2D_CHECK_EQ(list.text_count(), 1u);
}

T2D_TEST(the_submission_order_is_the_order_of_the_commands) {
    DrawList list;
    list.rect(Aabb2{Vec2{0.0f, 0.0f}, Vec2{1.0f, 1.0f}}, kWhite);
    list.sprite("a", Aabb2{Vec2{0.0f, 0.0f}, Vec2{2.0f, 2.0f}});
    list.text("b", Vec2{0.0f, 0.0f}, 12, kWhite);
    list.rect(Aabb2{Vec2{0.0f, 0.0f}, Vec2{3.0f, 3.0f}}, kWhite);
    T2D_REQUIRE(list.size() == 4u);
    T2D_CHECK_EQ(list.commands()[0].kind, DrawCommand::Kind::Rect);
    T2D_CHECK_EQ(list.commands()[1].kind, DrawCommand::Kind::Sprite);
    T2D_CHECK_EQ(list.commands()[2].kind, DrawCommand::Kind::Text);
    T2D_CHECK_EQ(list.commands()[3].kind, DrawCommand::Kind::Rect);
    T2D_CHECK_EQ(list.rect_count(), 2u);
    T2D_CHECK_EQ(list.sprite_count(), 1u);
    T2D_CHECK_EQ(list.text_count(), 1u);
}

T2D_TEST_MAIN
