// The start screen, as a state machine: selection, mode/seed editing and what each row starts.
#include <mine/menu.h>

#include <support/test_support.h>

#include <string>

using namespace mine;

namespace {

[[nodiscard]] std::string id_of(const MenuModel& menu, usize index) { return menu.row(index).id; }

[[nodiscard]] usize action_row(const MenuModel& menu, MenuAction action, Role role) {
    for (usize index = 0; index < menu.row_count(); ++index) {
        const MenuRow& row = menu.row(index);
        if (row.action == action && row.role == role) return index;
    }
    return menu.row_count();
}

} // namespace

T2D_TEST(the_story_screen_offers_the_actions_and_no_seed_row) {
    MenuModel menu;
    T2D_CHECK_EQ(menu.mode(), Mode::Story);
    T2D_CHECK_FALSE(menu.seed_visible());
    T2D_CHECK_EQ(menu.row_count(), 6u); // world, single, host, join, quit
    T2D_CHECK_EQ(id_of(menu, 0), std::string("row.world"));
    T2D_CHECK_EQ(menu.selected(), 0u);

    T2D_CHECK(action_row(menu, MenuAction::StartSession, Role::Single) < menu.row_count());
    T2D_CHECK(action_row(menu, MenuAction::StartSession, Role::Host) < menu.row_count());
    T2D_CHECK(action_row(menu, MenuAction::StartSession, Role::Join) < menu.row_count());
    T2D_CHECK(action_row(menu, MenuAction::Quit, Role::Single) < menu.row_count());
}

T2D_TEST(selecting_endless_adds_the_seed_row_and_leaving_it_removes_it) {
    MenuModel menu;
    menu.select(0);
    (void)menu.handle(MenuKey::Right);
    T2D_CHECK_EQ(menu.mode(), Mode::Endless);
    T2D_CHECK(menu.seed_visible());
    T2D_CHECK_EQ(menu.row_count(), 7u); // world, language, seed, single, host, join, quit
    T2D_CHECK_EQ(id_of(menu, 1), std::string("row.language"));
    T2D_CHECK_EQ(id_of(menu, 2), std::string("row.seed"));

    // Left/Right on the world row switches back, and the seed row goes away with it.
    menu.select(0);
    (void)menu.handle(MenuKey::Left);
    T2D_CHECK_EQ(menu.mode(), Mode::Story);
    T2D_CHECK_FALSE(menu.seed_visible());
    T2D_CHECK_EQ(menu.row_count(), 6u);

    // Focus the seed row, then leave endless mode through the model: the focus must land on an
    // action, not on whatever slid into the seed row's place.
    menu.set_mode(Mode::Endless);
    menu.select(2);
    T2D_CHECK_EQ(menu.row(menu.selected()).kind, RowKind::Seed);
    menu.set_mode(Mode::Story);
    T2D_CHECK_FALSE(menu.seed_visible());
    T2D_CHECK_EQ(menu.row(menu.selected()).kind, RowKind::Action);
}

T2D_TEST(navigation_wraps_in_both_directions) {
    MenuModel menu;
    menu.select(0);
    (void)menu.handle(MenuKey::Up);
    T2D_CHECK_EQ(menu.selected(), menu.row_count() - 1);
    (void)menu.handle(MenuKey::Down);
    T2D_CHECK_EQ(menu.selected(), 0u);

    menu.select(menu.row_count() - 1);
    (void)menu.handle(MenuKey::Down);
    T2D_CHECK_EQ(menu.selected(), 0u);
}

T2D_TEST(confirm_starts_the_focused_row_and_back_quits) {
    MenuModel menu;
    menu.set_mode(Mode::Endless);
    const usize single = action_row(menu, MenuAction::StartSession, Role::Single);
    const usize host = action_row(menu, MenuAction::StartSession, Role::Host);
    const usize join = action_row(menu, MenuAction::StartSession, Role::Join);
    const usize quit_row = action_row(menu, MenuAction::Quit, Role::Single);
    T2D_REQUIRE(single < menu.row_count());
    T2D_REQUIRE(host < menu.row_count());
    T2D_REQUIRE(join < menu.row_count());
    T2D_REQUIRE(quit_row < menu.row_count());

    menu.select(single);
    T2D_CHECK_EQ(menu.handle(MenuKey::Confirm), MenuAction::StartSession);
    menu.select(host);
    T2D_CHECK_EQ(menu.handle(MenuKey::Confirm), MenuAction::StartSession);
    menu.select(join);
    T2D_CHECK_EQ(menu.handle(MenuKey::Confirm), MenuAction::StartSession);
    menu.select(quit_row);
    T2D_CHECK_EQ(menu.handle(MenuKey::Confirm), MenuAction::Quit);

    T2D_CHECK_EQ(menu.handle(MenuKey::Back), MenuAction::Quit);

    // Confirming an option row steps into the list instead of doing nothing.
    menu.select(0);
    T2D_CHECK_EQ(menu.handle(MenuKey::Confirm), MenuAction::None);
    T2D_CHECK_EQ(menu.selected(), 1u);
}

T2D_TEST(the_focused_row_decides_the_session) {
    MenuModel menu;
    menu.set_seed(4242);
    menu.set_mode(Mode::Endless);

    const usize single = action_row(menu, MenuAction::StartSession, Role::Single);
    const usize host = action_row(menu, MenuAction::StartSession, Role::Host);
    const usize join = action_row(menu, MenuAction::StartSession, Role::Join);

    menu.select(single);
    SessionConfig config = menu.session_config();
    T2D_CHECK_EQ(config.mode, Mode::Endless);
    T2D_CHECK_EQ(config.role, Role::Single);
    T2D_CHECK_EQ(config.seed, 4242u);

    menu.select(host);
    T2D_CHECK_EQ(menu.session_config().role, Role::Host);
    menu.select(join);
    T2D_CHECK_EQ(menu.session_config().role, Role::Join);

    // The world and seed rows are not actions: they report single player.
    menu.select(0);
    T2D_CHECK_EQ(menu.session_config().role, Role::Single);
    T2D_CHECK_EQ(menu.session_config().mode, Mode::Endless);
}

T2D_TEST(the_seed_stays_readable_and_wraps_at_the_ends) {
    MenuModel menu;
    menu.set_mode(Mode::Endless);
    menu.select(2); // the seed row (world, language, seed)
    T2D_CHECK_EQ(menu.row(menu.selected()).kind, RowKind::Seed);

    menu.set_seed(MenuModel::kMinSeed);
    (void)menu.handle(MenuKey::Left);
    T2D_CHECK_EQ(menu.seed(), MenuModel::kMaxSeed);

    menu.set_seed(MenuModel::kMaxSeed);
    (void)menu.handle(MenuKey::Right);
    T2D_CHECK_EQ(menu.seed(), MenuModel::kMinSeed);

    menu.set_seed(500);
    (void)menu.handle(MenuKey::Right);
    T2D_CHECK_EQ(menu.seed(), 501u);
    (void)menu.handle(MenuKey::Left);
    (void)menu.handle(MenuKey::Left);
    T2D_CHECK_EQ(menu.seed(), 499u);

    // Out of range seeds are pulled back into the readable range.
    menu.set_seed(0);
    T2D_CHECK_GE(menu.seed(), MenuModel::kMinSeed);
    T2D_CHECK_LT(menu.seed(), MenuModel::kMaxSeed + 1u);
}

T2D_TEST(randomising_the_seed_is_deterministic_and_changes_it) {
    MenuModel first;
    MenuModel second;
    first.set_mode(Mode::Endless);
    second.set_mode(Mode::Endless);
    first.set_seed(7);
    second.set_seed(7);

    const u32 before = first.seed();
    (void)first.handle(MenuKey::Randomise);
    T2D_CHECK_NE(first.seed(), before);

    // Same state, same sequence: the menu never consults a clock.
    (void)second.handle(MenuKey::Randomise);
    T2D_CHECK_EQ(first.seed(), second.seed());

    (void)first.handle(MenuKey::Randomise);
    (void)second.handle(MenuKey::Randomise);
    T2D_CHECK_EQ(first.seed(), second.seed());

    // In story mode there is no seed row, so the key does nothing.
    MenuModel story;
    const u32 story_seed = story.seed();
    (void)story.handle(MenuKey::Randomise);
    T2D_CHECK_EQ(story.seed(), story_seed);
}

T2D_TEST_MAIN
