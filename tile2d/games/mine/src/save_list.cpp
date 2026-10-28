#include <mine/save_list.h>

namespace mine {
namespace {

/// The row an empty list answers with, so row() never reads past the end of what it holds.
constexpr SaveListRow kNoRow{};

} // namespace

const char* save_list_title_id() { return kSaveListTitleId; }
const char* save_list_empty_id() { return kSaveListEmptyId; }
const char* save_list_latest_id() { return kSaveListLatestId; }
const char* save_list_no_autosave_id() { return kSaveListNoAutosaveId; }
const char* save_list_hint_id() { return kSaveListHintId; }

void SaveListModel::set_slots(std::vector<SaveSlot> slots) {
    slots_ = std::move(slots);
    open_.assign(slots_.size(), false);
    selected_ = 0;
    first_visible_ = 0;
    rebuild_rows();
    clamp_selection();
}

void SaveListModel::rebuild_rows() {
    rows_.clear();
    rows_.reserve(slots_.size() * 2);
    for (usize slot = 0; slot < slots_.size(); ++slot) {
        rows_.push_back(SaveListRow{slot, false});
        if (is_open(slot)) rows_.push_back(SaveListRow{slot, true});
    }
}

const SaveListRow& SaveListModel::row(usize index) const {
    if (rows_.empty()) return kNoRow;
    return rows_[index < rows_.size() ? index : rows_.size() - 1];
}

void SaveListModel::select(usize index) {
    if (rows_.empty()) {
        selected_ = 0;
        first_visible_ = 0;
        return;
    }
    selected_ = index < rows_.size() ? index : rows_.size() - 1;
    scroll_to_selection();
}

void SaveListModel::move(i32 delta) {
    if (rows_.empty()) return;
    const i32 last = static_cast<i32>(rows_.size()) - 1;
    i32 index = static_cast<i32>(selected_) + delta;
    if (index < 0) index = 0;
    if (index > last) index = last;
    select(static_cast<usize>(index));
}

void SaveListModel::set_visible_rows(usize rows) {
    visible_rows_ = rows; // 0 means the screen has not said yet, and nothing scrolls
    if (visible_rows_ == 0) {
        first_visible_ = 0;
        return;
    }
    scroll_to_selection();
}

void SaveListModel::scroll_to_selection() {
    if (visible_rows_ == 0 || rows_.empty()) return;
    if (selected_ < first_visible_) first_visible_ = selected_;
    if (selected_ >= first_visible_ + visible_rows_) first_visible_ = selected_ + 1 - visible_rows_;
    const usize last_start = rows_.size() > visible_rows_ ? rows_.size() - visible_rows_ : 0;
    if (first_visible_ > last_start) first_visible_ = last_start;
}

void SaveListModel::clamp_selection() {
    if (rows_.empty()) {
        selected_ = 0;
        first_visible_ = 0;
        return;
    }
    if (selected_ >= rows_.size()) selected_ = rows_.size() - 1;
    scroll_to_selection();
}

bool SaveListModel::is_open(usize slot) const {
    if (slot >= slots_.size() || slot >= open_.size()) return false;
    return open_[slot] && slots_[slot].has_autosave;
}

void SaveListModel::toggle(usize slot) {
    if (!is_open(slot)) {
        if (slot >= slots_.size() || !slots_[slot].has_autosave) return;
        set_open(slot, true);
        return;
    }
    set_open(slot, false);
}

void SaveListModel::set_open(usize slot, bool open) {
    if (slot >= slots_.size() || !slots_[slot].has_autosave) return;
    if (open_[slot] == open) return;
    open_[slot] = open;
    rebuild_rows();
    clamp_selection();
}

std::optional<usize> SaveListModel::selected_slot() const {
    if (rows_.empty() || selected_ >= rows_.size()) return std::nullopt;
    return rows_[selected_].slot;
}

std::string SaveListModel::selected_autosave_path() const {
    if (rows_.empty() || selected_ >= rows_.size()) return {};
    const SaveListRow& current = rows_[selected_];
    if (!current.is_autosave || current.slot >= slots_.size()) return {};
    return slots_[current.slot].autosave_path;
}

} // namespace mine
