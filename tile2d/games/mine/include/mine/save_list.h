// Mine - the save list as rows: one per slot, and the slot's most recent automatic save under it when
// the slot is open (docs/GAME_DESIGN.md section 1.48).
//
// The screen moves a selection and draws it; this decides what the rows are and keeps the selection on a
// row that exists, so a list of saves is testable without pixels.
#pragma once

#include <mine/save_library.h>

#include <t2d/core/types.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mine {

using t2d::i32;
using t2d::usize;

/// The ids the save list needs from the string table (assets/text/ui.ecfg, task-16). The list is model
/// code, so it names them and never holds the words.
inline constexpr const char* kSaveListTitleId = "save.list.title";
inline constexpr const char* kSaveListEmptyId = "save.list.empty";
inline constexpr const char* kSaveListLatestId = "save.list.recent";
inline constexpr const char* kSaveListNoAutosaveId = "save.list.no_autosave";
inline constexpr const char* kSaveListHintId = "save.list.hint";
inline constexpr std::string_view kSaveListIds[] = {
    kSaveListTitleId, kSaveListEmptyId, kSaveListLatestId, kSaveListNoAutosaveId, kSaveListHintId,
};

[[nodiscard]] const char* save_list_title_id();
[[nodiscard]] const char* save_list_empty_id();
[[nodiscard]] const char* save_list_latest_id();
[[nodiscard]] const char* save_list_no_autosave_id();
[[nodiscard]] const char* save_list_hint_id();

/// One line of the save list: a slot's header, or the most recent automatic save inside that slot.
struct SaveListRow {
    usize slot = 0; ///< into the slots the model was given
    bool is_autosave = false;
};

class SaveListModel {
public:
    /// Takes the slots to show, closes every open slot and selects the first row.
    void set_slots(std::vector<SaveSlot> slots);

    [[nodiscard]] const std::vector<SaveSlot>& slots() const { return slots_; }
    [[nodiscard]] usize row_count() const { return rows_.size(); }
    /// The row at \p index, clamped to what exists: a screen that asks about the last row cannot read
    /// past the list.
    [[nodiscard]] const SaveListRow& row(usize index) const;
    [[nodiscard]] usize selected() const { return selected_; }
    void select(usize index);
    /// Moves the selection by \p delta rows, stopping at the first and the last: the list does not wrap.
    void move(i32 delta);

    /// How many rows fit on screen; the screen sets this when it knows. Until then nothing scrolls.
    void set_visible_rows(usize rows);
    [[nodiscard]] usize visible_rows() const { return visible_rows_; }
    /// The first row the window shows; a call to select()/move() keeps the selection inside the window.
    [[nodiscard]] usize first_visible() const { return first_visible_; }

    [[nodiscard]] bool is_open(usize slot) const;
    /// Opens or closes a slot's automatic save row. A slot with no automatic save has nothing to show, so
    /// it stays closed.
    void toggle(usize slot);
    void set_open(usize slot, bool open);

    /// The slot the selected row belongs to, or nothing when there are no rows.
    [[nodiscard]] std::optional<usize> selected_slot() const;
    /// The automatic save the selected row stands for: a sub row's file, and "" for a slot's header.
    [[nodiscard]] std::string selected_autosave_path() const;

private:
    void rebuild_rows();
    void clamp_selection();
    void scroll_to_selection();

    std::vector<SaveSlot> slots_;
    std::vector<SaveListRow> rows_;
    std::vector<bool> open_;
    usize selected_ = 0;
    usize visible_rows_ = 0;
    usize first_visible_ = 0;
};

} // namespace mine
