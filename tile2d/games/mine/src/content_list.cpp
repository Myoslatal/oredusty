#include <mine/content_list.h>

#include <algorithm>
#include <iterator>

namespace mine {

const char* source_kind_id(SourceKind kind) {
    switch (kind) {
        case SourceKind::Pack: return "content.kind.pack";
        case SourceKind::Mod: return "content.kind.mod";
        case SourceKind::File: break;
    }
    return "content.kind.file";
}

const char* source_status_id(const ContentSource& source) {
    if (!source.ok) return "content.status.failed";
    if (!source.error.empty()) return "content.status.partial";
    return "content.status.ok";
}

ConstSpan<const char*> content_list_locale_ids() {
    static const char* const kIds[] = {
        "content.title",    "content.summary",     "content.count",  "content.art",
        "content.native",   "content.path",        "content.name",   "content.requires",
        "content.error",    "content.empty",       "content.messages", "content.messages.more",
        "content.issue.error", "content.issue.warn", "content.hint.keys",
        // The badges and the status column come from the two functions above.
        "content.kind.file", "content.kind.pack", "content.kind.mod",
        "content.status.ok", "content.status.partial", "content.status.failed",
    };
    return ConstSpan<const char*>(kIds, std::size(kIds));
}

const ContentSource& ContentListModel::source(usize index) const {
    static const ContentSource kEmpty{};
    return index < sources_.size() ? sources_[index] : kEmpty;
}

ContentListTotals ContentListModel::totals() const {
    ContentListTotals totals;
    totals.sources = sources_.size();
    for (const ContentSource& source : sources_) {
        switch (source.kind) {
            case SourceKind::File: ++totals.files; break;
            case SourceKind::Pack: ++totals.packs; break;
            case SourceKind::Mod:
                ++totals.mods;
                if (source.native) ++totals.native_mods;
                break;
        }
        totals.entries += source.entries.size();
        totals.images += source.images;
        if (!source.ok) ++totals.failed;
        else if (!source.error.empty()) ++totals.partial;
    }
    return totals;
}

namespace {

/// True when some source already says this. The screen shows a source's message on the source's own
/// line, so repeating it in the message block would report one failure twice - and the load's own
/// report usually repeats it with the source named in front ("mod 'x': the file is missing"), which is
/// why a message that ends with a source's own error counts as the same message.
[[nodiscard]] bool already_reported(const std::vector<ContentSource>& sources, const std::string& message) {
    for (const ContentSource& source : sources) {
        if (source.error.empty() || source.error.size() > message.size()) continue;
        if (message.compare(message.size() - source.error.size(), source.error.size(), source.error) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

void ContentListModel::set_sources(std::vector<ContentSource> sources, std::vector<std::string> errors,
                                   std::vector<std::string> warnings) {
    sources_ = std::move(sources);
    // Fold state belongs to the list that was just replaced, not to whatever lands in its place.
    open_.assign(sources_.size(), false);
    selected_ = 0;
    first_ = 0;

    issues_.clear();
    for (std::string& error : errors) {
        if (already_reported(sources_, error)) continue;
        issues_.push_back(ContentListIssue{true, std::move(error)});
    }
    for (std::string& warning : warnings) {
        if (already_reported(sources_, warning)) continue;
        issues_.push_back(ContentListIssue{false, std::move(warning)});
    }

    rebuild();
    clamp_scroll();
}

const ContentListRow& ContentListModel::row(usize index) const {
    static const ContentListRow kEmpty{};
    return index < rows_.size() ? rows_[index] : kEmpty;
}

usize ContentListModel::selected_source() const {
    return rows_.empty() ? 0 : row(selected_).source;
}

bool ContentListModel::is_open(usize source) const {
    return source < open_.size() && open_[source];
}

void ContentListModel::rebuild() {
    rows_.clear();
    for (usize index = 0; index < sources_.size(); ++index) {
        rows_.push_back(ContentListRow{index, -1});
        if (!is_open(index)) continue;
        for (usize entry = 0; entry < sources_[index].entries.size(); ++entry) {
            rows_.push_back(ContentListRow{index, static_cast<i32>(entry)});
        }
    }
    if (rows_.empty()) {
        selected_ = 0;
        first_ = 0;
        return;
    }
    selected_ = std::min(selected_, rows_.size() - 1);
}

void ContentListModel::clamp_scroll() {
    if (rows_.empty()) {
        first_ = 0;
        return;
    }
    if (visible_ == 0) {
        // Nothing is drawn, so there is nothing to scroll: when lines come back, the selection is at
        // the top of them rather than somewhere off screen.
        first_ = selected_;
        return;
    }
    if (selected_ < first_) first_ = selected_;
    if (selected_ >= first_ + visible_) first_ = selected_ + 1 - visible_;
    // The list never scrolls past its own end: a panel with four empty lines under the last entry
    // looks like four entries that failed to load.
    if (rows_.size() <= visible_) first_ = 0;
    else first_ = std::min(first_, rows_.size() - visible_);
}

void ContentListModel::select(usize index) {
    if (rows_.empty()) return;
    selected_ = std::min(index, rows_.size() - 1);
    clamp_scroll();
}

void ContentListModel::move(i32 delta) {
    if (rows_.empty()) return;
    const i64 moved = static_cast<i64>(selected_) + delta;
    selected_ = static_cast<usize>(std::clamp<i64>(moved, 0, static_cast<i64>(rows_.size()) - 1));
    clamp_scroll();
}

void ContentListModel::select_last() {
    if (rows_.empty()) return;
    select(rows_.size() - 1);
}

bool ContentListModel::set_open(usize source, bool open) {
    if (source >= sources_.size()) return false;
    // A source with nothing inside cannot be opened: there is nothing a fold-out would show.
    const bool wanted = open && !sources_[source].entries.empty();
    if (open_[source] == wanted) return wanted;
    open_[source] = wanted;
    rebuild();
    clamp_scroll();
    return wanted;
}

bool ContentListModel::toggle() {
    if (rows_.empty()) return false;
    const usize source = row(selected_).source;
    return set_open(source, !is_open(source));
}

void ContentListModel::open_all() {
    for (usize index = 0; index < sources_.size(); ++index) {
        open_[index] = !sources_[index].entries.empty();
    }
    rebuild();
    clamp_scroll();
}

void ContentListModel::close_all() {
    std::fill(open_.begin(), open_.end(), false);
    rebuild();
    clamp_scroll();
}

void ContentListModel::set_visible_rows(usize rows) {
    visible_ = rows;
    clamp_scroll();
}

} // namespace mine
