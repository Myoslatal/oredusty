#include <mine/menu.h>

namespace mine {
namespace {

/// Labels are plain ASCII on purpose: the built-in bitmap font has no other glyphs.
// Locale ids (see assets/text/ui.ecfg), not display text.
constexpr const char* kWorldLabel = "row.world";
constexpr const char* kLanguageLabel = "row.language";
constexpr const char* kSeedLabel = "row.seed";
constexpr const char* kStartLabel = "row.start";
constexpr const char* kHostLabel = "row.host";
constexpr const char* kJoinLabel = "row.join";
constexpr const char* kQuitLabel = "row.quit";

/// Keeps a seed inside the readable range: values that already fit are untouched, anything else is
/// folded back in (so a seed typed on the command line can be any u32).
[[nodiscard]] u32 wrap_seed(u32 value) {
    if (value >= MenuModel::kMinSeed && value <= MenuModel::kMaxSeed) return value;
    const u32 span = MenuModel::kMaxSeed - MenuModel::kMinSeed + 1u;
    return MenuModel::kMinSeed + (value % span);
}

} // namespace

MenuModel::MenuModel() { rebuild_rows(); }

const MenuRow& MenuModel::row(usize index) const {
    const usize clamped = index < row_count_ ? index : (row_count_ > 0 ? row_count_ - 1 : 0);
    return rows_[clamped];
}

void MenuModel::select(usize index) {
    if (row_count_ == 0) {
        selected_ = 0;
        return;
    }
    selected_ = index % row_count_;
}

void MenuModel::rebuild_rows() {
    usize count = 0;
    rows_[count++] = MenuRow{RowKind::World, MenuAction::None, Role::Single, kWorldLabel};
    rows_[count++] = MenuRow{RowKind::Language, MenuAction::None, Role::Single, kLanguageLabel};
    if (seed_visible()) rows_[count++] = MenuRow{RowKind::Seed, MenuAction::None, Role::Single, kSeedLabel};
    rows_[count++] = MenuRow{RowKind::Action, MenuAction::StartSession, Role::Single, kStartLabel};
    rows_[count++] = MenuRow{RowKind::Action, MenuAction::StartSession, Role::Host, kHostLabel};
    rows_[count++] = MenuRow{RowKind::Action, MenuAction::StartSession, Role::Join, kJoinLabel};
    rows_[count++] = MenuRow{RowKind::Action, MenuAction::Quit, Role::Single, kQuitLabel};
    row_count_ = count;
    if (selected_ >= row_count_) selected_ = row_count_ - 1;
}

void MenuModel::set_mode(Mode mode) {
    if (mode_ == mode) return;
    const bool was_seed_row = row_count_ > 0 && rows_[selected_].kind == RowKind::Seed;
    mode_ = mode;
    rebuild_rows();
    // Leaving endless mode removes the seed row; land on the first action instead of on whatever
    // slid into its place.
    if (was_seed_row && !seed_visible()) select(row_count_ - 4);
}

void MenuModel::set_language(t2d::Language language) {
    const usize index = static_cast<usize>(language);
    if (index < t2d::kLanguageCount) language_ = language;
}

t2d::Language MenuModel::next_language(i32 delta) const {
    const i32 count = static_cast<i32>(t2d::kLanguageCount);
    i32 index = static_cast<i32>(language_) + delta;
    index = ((index % count) + count) % count; // wrap in both directions
    return static_cast<t2d::Language>(index);
}

void MenuModel::set_seed(u32 seed) { seed_ = wrap_seed(seed); }

void MenuModel::randomise_seed() { seed_ = wrap_seed(seed_ * 1664525u + 1013904223u); }

MenuAction MenuModel::handle(MenuKey key) {
    if (row_count_ == 0) return MenuAction::None;
    switch (key) {
        case MenuKey::Up:
            select(selected_ + row_count_ - 1);
            return MenuAction::None;
        case MenuKey::Down:
            select(selected_ + 1);
            return MenuAction::None;
        case MenuKey::Left:
        case MenuKey::Right: {
            const bool forward = key == MenuKey::Right;
            const MenuRow& current = rows_[selected_];
            if (current.kind == RowKind::World) {
                set_mode(forward ? Mode::Endless : Mode::Story);
            } else if (current.kind == RowKind::Language) {
                set_language(next_language(forward ? 1 : -1));
            } else if (current.kind == RowKind::Seed) {
                // Stepping wraps at the ends instead of clamping, so the row always responds.
                if (forward) set_seed(seed_ >= kMaxSeed ? kMinSeed : seed_ + 1u);
                else set_seed(seed_ <= kMinSeed ? kMaxSeed : seed_ - 1u);
            }
            return MenuAction::None;
        }
        case MenuKey::Randomise:
            if (seed_visible()) randomise_seed();
            return MenuAction::None;
        case MenuKey::Confirm: {
            const MenuRow& current = rows_[selected_];
            if (current.kind == RowKind::Action) return current.action;
            select(selected_ + 1); // step into the list instead of doing nothing
            return MenuAction::None;
        }
        case MenuKey::Back:
            return MenuAction::Quit;
    }
    return MenuAction::None;
}

SessionConfig MenuModel::session_config_for(usize index) const {
    SessionConfig config;
    config.mode = mode_;
    config.seed = seed_;
    config.role = row_count_ > 0 ? row(index).role : Role::Single;
    return config;
}

} // namespace mine
