// Mine - the list of what the registry was filled from.
//
// Content arrives from three places, in a fixed order (docs/MODS.md): the game's own files, the content
// packs, and the mod packages. This is that list as data: one record per thing the pipeline was asked
// to load - **including the ones that did not load**, because a list that only shows successes is
// exactly the list a broken pack hides from.
//
// The screen is this model plus drawing. Nothing here touches a window, a GPU or a file, which is what
// makes the list itself testable in milliseconds.
#pragma once

#include <mine/registry.h>

#include <t2d/core/types.h>

#include <string>
#include <vector>

namespace mine {

using t2d::ConstSpan;
using t2d::i32;
using t2d::i64;
using t2d::u8;
using t2d::usize;

/// Where a piece of content came from, in load order.
enum class SourceKind : u8 { File = 0, Pack, Mod };
inline constexpr usize kSourceKindCount = 3;

/// One thing the registry was filled from, and what became of it.
///
/// A source that loaded can still carry an error: a pack whose second name was already taken loaded,
/// and lost a name doing it. That is what clean() is for: the row says PARTIAL rather than OK, and
/// the list keeps showing the message.
struct ContentSource {
    SourceKind kind = SourceKind::File;
    std::string path;      ///< the file or directory it came from
    std::string id;        ///< pack/mod id; for one of the game's own files, the file's stem
    std::string name;      ///< pack::name or the manifest's name; defaults to the id
    std::string version;   ///< may be empty
    std::vector<std::string> requirements;   ///< pack ids / mod ids this one needs first
    /// What it added to the registry, in registration order: the ids a save would store, and the
    /// names they belong to.
    std::vector<ContentEntry> entries;
    usize images = 0;         ///< pictures it references that the engine found (packs)
    usize images_failed = 0;  ///< pictures it references that it cannot have
    bool native = false;      ///< a mod package with a shared library
    bool ok = true;           ///< false: it did not load at all
    std::string error;        ///< why not, or what it lost while loading
    /// The whole source: it loaded and lost nothing.
    [[nodiscard]] bool clean() const { return ok && error.empty(); }
};

/// One line of the list: a source, or - when that source is open - one of the entries it registered.
struct ContentListRow {
    usize source = 0;   ///< index into the model's sources
    i32 entry = -1;     ///< -1: the source itself; otherwise the entry inside it
    [[nodiscard]] bool is_entry() const { return entry >= 0; }
    [[nodiscard]] u8 depth() const { return entry >= 0 ? 1 : 0; }
};

/// The numbers the screen's summary line reports.
struct ContentListTotals {
    usize sources = 0;
    usize files = 0;
    usize packs = 0;
    usize mods = 0;
    usize native_mods = 0;
    usize entries = 0;    ///< everything the sources added to the registry
    usize images = 0;
    usize failed = 0;     ///< sources that did not load at all
    usize partial = 0;    ///< sources that loaded and lost something
    [[nodiscard]] bool clean() const { return failed == 0 && partial == 0; }
};

/// The label a source's kind is drawn with: a locale id, never text, because the badge has to read in
/// every language the interface ships.
[[nodiscard]] const char* source_kind_id(SourceKind kind);
/// How a source reads at a glance: it loaded, it loaded and lost something, or it did not load.
[[nodiscard]] const char* source_status_id(const ContentSource& source);
/// Every locale id the content list screen draws, so a test can walk them through the string table the
/// game actually ships - the same guard the start screen's labels have. A missing id would otherwise
/// be printed on screen as the id itself.
[[nodiscard]] ConstSpan<const char*> content_list_locale_ids();

/// One message from the load itself, which no single source owns: a directory that is not there, a
/// table that is not a content kind. A message a source already carries on its own line is not
/// repeated here - the screen would be saying it twice.
struct ContentListIssue {
    bool error = true;
    std::string text;
};

/// The list screen as data: what is loaded, what is selected, what is open, how far it is scrolled.
///
/// A source is opened to show the content it registered. Only a source that registered something can
/// be opened: an empty fold-out says nothing that its own line does not already say.
class ContentListModel {
public:
    ContentListModel() = default;

    /// Replaces the list. This is a reload: the selection goes back to the first line and every fold
    /// is closed, because the list a reload produces may have nothing in common with the old one.
    ///
    /// \p errors and \p warnings are the load's own report lines; the ones a source already carries
    /// on its own line are dropped, and the rest become issues().
    void set_sources(std::vector<ContentSource> sources, std::vector<std::string> errors = {},
                     std::vector<std::string> warnings = {});

    [[nodiscard]] ConstSpan<ContentSource> sources() const {
        return ConstSpan<ContentSource>(sources_.data(), sources_.size());
    }
    [[nodiscard]] usize source_count() const { return sources_.size(); }
    [[nodiscard]] const ContentSource& source(usize index) const;
    [[nodiscard]] ContentListTotals totals() const;
    /// What the load reported and no source owns, errors first, then warnings.
    [[nodiscard]] ConstSpan<ContentListIssue> issues() const {
        return ConstSpan<ContentListIssue>(issues_.data(), issues_.size());
    }

    /// The lines the screen draws: every source, plus the entries of the open ones.
    [[nodiscard]] usize row_count() const { return rows_.size(); }
    [[nodiscard]] const ContentListRow& row(usize index) const;
    /// The selected line's source.
    [[nodiscard]] usize selected_source() const;

    [[nodiscard]] usize selected() const { return selected_; }
    /// Moves the selection to p index, clamped to the list, and scrolls it into view.
    void select(usize index);
    /// Moves the selection by p delta lines, clamped: the ends of the list do not wrap, so holding a
    /// key cannot spin the list.
    void move(i32 delta);
    void select_first() { select(0); }
    void select_last();

    [[nodiscard]] bool is_open(usize source) const;
    /// Opens or closes p source. Returns whether it is open afterwards; a source with nothing inside
    /// stays closed.
    bool set_open(usize source, bool open);
    /// Opens or closes the selected source.
    bool toggle();
    /// Opens every source that has something to show, or closes them all.
    void open_all();
    void close_all();

    /// How many lines the list has room for; the selection is kept inside them.
    void set_visible_rows(usize rows);
    [[nodiscard]] usize visible_rows() const { return visible_; }
    /// The first line the screen draws.
    [[nodiscard]] usize first_visible() const { return first_; }

private:
    void rebuild();
    void clamp_scroll();

    std::vector<ContentSource> sources_;
    std::vector<ContentListIssue> issues_;
    std::vector<ContentListRow> rows_;
    std::vector<bool> open_;
    usize selected_ = 0;
    usize first_ = 0;
    usize visible_ = 0;
};

} // namespace mine
