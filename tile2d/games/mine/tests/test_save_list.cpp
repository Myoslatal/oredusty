// The save list: a row per slot, the slot's most recent automatic save under it when it is open, and a
// selection that stays on a row that exists (mine/save_list.h, docs/GAME_DESIGN.md section 1.48).
#include <mine/save_list.h>

#include <support/test_support.h>

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace mine;

namespace {

[[nodiscard]] SaveSlot make_slot(std::string name, bool has_autosave) {
    SaveSlot slot;
    slot.name = std::move(name);
    slot.directory = "/saves/" + slot.name;
    slot.autosave_path = slot.directory + "/auto.t2dsave";
    slot.has_autosave = has_autosave;
    return slot;
}

[[nodiscard]] std::vector<SaveSlot> numbered_slots(usize count, bool has_autosave = true) {
    std::vector<SaveSlot> slots;
    slots.reserve(count);
    for (usize index = 0; index < count; ++index) {
        slots.push_back(make_slot(std::format("slot {}", index), has_autosave));
    }
    return slots;
}

} // namespace

T2D_TEST(slots_are_rows_and_an_open_one_shows_its_autosave) {
    SaveListModel list;
    list.set_slots({make_slot("alpha", true), make_slot("beta", false), make_slot("gamma", true)});
    T2D_CHECK_EQ(list.row_count(), 3u);
    T2D_CHECK_EQ(list.row(0).slot, 0u);
    T2D_CHECK_FALSE(list.row(0).is_autosave);
    T2D_CHECK_FALSE(list.is_open(0));

    list.set_open(0, true);
    T2D_CHECK(list.is_open(0));
    T2D_CHECK_EQ(list.row_count(), 4u);
    T2D_CHECK_EQ(list.row(1).slot, 0u);
    T2D_CHECK(list.row(1).is_autosave);
    T2D_CHECK_EQ(list.row(2).slot, 1u);
    T2D_CHECK_FALSE(list.row(2).is_autosave);
    T2D_CHECK_EQ(list.row(3).slot, 2u);

    list.set_open(0, false);
    T2D_CHECK_FALSE(list.is_open(0));
    T2D_CHECK_EQ(list.row_count(), 3u);

    // A row index past the end is clamped rather than read past the list.
    T2D_CHECK_EQ(list.row(99).slot, 2u);
}

T2D_TEST(a_slot_without_an_autosave_cannot_open) {
    SaveListModel list;
    list.set_slots({make_slot("alpha", true), make_slot("beta", false)});
    list.toggle(1);
    T2D_CHECK_FALSE(list.is_open(1));
    T2D_CHECK_EQ(list.row_count(), 2u);
    list.set_open(1, true);
    T2D_CHECK_FALSE(list.is_open(1));
    T2D_CHECK_EQ(list.row_count(), 2u);

    // Slots that are not there are ignored too.
    list.toggle(9);
    list.set_open(9, true);
    T2D_CHECK_EQ(list.row_count(), 2u);
}

T2D_TEST(the_selection_stops_at_the_ends_and_never_wraps) {
    SaveListModel list;
    list.set_slots(numbered_slots(5));
    T2D_CHECK_EQ(list.selected(), 0u);
    list.move(-1);
    T2D_CHECK_EQ(list.selected(), 0u);
    list.move(2);
    T2D_CHECK_EQ(list.selected(), 2u);
    list.move(100);
    T2D_CHECK_EQ(list.selected(), 4u);
    list.move(1);
    T2D_CHECK_EQ(list.selected(), 4u);
    list.select(1);
    T2D_CHECK_EQ(list.selected(), 1u);
    list.select(99);
    T2D_CHECK_EQ(list.selected(), 4u);
}

T2D_TEST(the_window_follows_the_selection) {
    SaveListModel list;
    list.set_slots(numbered_slots(10));
    list.set_visible_rows(3);
    T2D_CHECK_EQ(list.visible_rows(), 3u);
    T2D_CHECK_EQ(list.first_visible(), 0u);

    list.move(2);
    T2D_CHECK_EQ(list.first_visible(), 0u);
    list.move(1);
    T2D_CHECK_EQ(list.first_visible(), 1u);
    list.select(9);
    T2D_CHECK_EQ(list.first_visible(), 7u);
    list.move(-5);
    T2D_CHECK_EQ(list.selected(), 4u);
    T2D_CHECK_EQ(list.first_visible(), 4u);
    list.move(-100);
    T2D_CHECK_EQ(list.selected(), 0u);
    T2D_CHECK_EQ(list.first_visible(), 0u);

    // A taller window shows the same selection with less scrolling.
    list.set_visible_rows(5);
    list.select(9);
    T2D_CHECK_EQ(list.first_visible(), 5u);
    // Until the screen says how many rows fit, nothing scrolls.
    list.set_visible_rows(0);
    T2D_CHECK_EQ(list.first_visible(), 0u);
}

T2D_TEST(the_autosave_row_belongs_to_its_slot) {
    SaveListModel list;
    list.set_slots({make_slot("alpha", true), make_slot("beta", true)});
    list.set_open(1, true);
    T2D_CHECK_EQ(list.row_count(), 3u);   // alpha, beta, beta's autosave

    list.select(2);
    const std::optional<usize> slot = list.selected_slot();
    T2D_REQUIRE(slot.has_value());
    T2D_CHECK_EQ(*slot, 1u);
    T2D_CHECK_EQ(list.selected_autosave_path(), std::string("/saves/beta/auto.t2dsave"));

    // A slot's header is the slot, not a file: there is nothing for a load to open from it.
    list.select(1);
    T2D_REQUIRE(list.selected_slot().has_value());
    T2D_CHECK_EQ(*list.selected_slot(), 1u);
    T2D_CHECK_EQ(list.selected_autosave_path(), std::string(""));
}

T2D_TEST(setting_new_slots_closes_what_was_open) {
    SaveListModel list;
    list.set_slots({make_slot("alpha", true), make_slot("beta", false)});
    list.set_open(0, true);
    list.move(1);
    list.set_visible_rows(2);
    list.set_slots({make_slot("fresh", true)});
    T2D_CHECK_EQ(list.row_count(), 1u);
    T2D_CHECK_EQ(list.selected(), 0u);
    T2D_CHECK_EQ(list.first_visible(), 0u);
    T2D_CHECK_FALSE(list.is_open(0));
}

T2D_TEST(an_empty_list_has_nothing_to_select) {
    SaveListModel list;
    T2D_CHECK_EQ(list.row_count(), 0u);
    T2D_CHECK_FALSE(list.selected_slot().has_value());
    T2D_CHECK_EQ(list.selected_autosave_path(), std::string(""));
    list.move(3);
    list.select(2);
    list.toggle(0);
    list.set_visible_rows(5);
    T2D_CHECK_EQ(list.row_count(), 0u);
    T2D_CHECK_EQ(list.selected(), 0u);
}

T2D_TEST(the_locale_ids_are_named_but_not_worded) {
    for (const std::string_view id : kSaveListIds) {
        T2D_CHECK(!id.empty());
        T2D_CHECK_EQ(id.substr(0, 5), std::string_view("save."));
    }
    T2D_CHECK_EQ(std::string_view(save_list_title_id()), std::string_view(kSaveListTitleId));
    T2D_CHECK_EQ(std::string_view(save_list_empty_id()), std::string_view(kSaveListEmptyId));
    T2D_CHECK_EQ(std::string_view(save_list_latest_id()), std::string_view(kSaveListLatestId));
    T2D_CHECK_EQ(std::string_view(save_list_no_autosave_id()), std::string_view(kSaveListNoAutosaveId));
    T2D_CHECK_EQ(std::string_view(save_list_hint_id()), std::string_view(kSaveListHintId));
}

T2D_TEST_MAIN
