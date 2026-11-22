// The task chain of a task block: what the content data declares, how the items in its container become
// progress, and what a save brings back (docs/GAME_DESIGN.md section 1.52).
//
// The content in here is a **test double**: "stone" and "copper" are the fixture's items, not the
// game's - the game names no content at all (docs/GAME_DESIGN.md section 7).
#include <mine/content_loader.h>
#include <mine/runtime/content/content_ids.h>
#include <mine/types/task.h>
#include <mine/types/types.h>
#include <mine/world.h>

#include <support/test_support.h>

#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace mine;
using namespace mine::types;

namespace {

/// The one plot definition a document declares, or nothing with the reader's first error in \p error.
[[nodiscard]] std::optional<TileDefinition> read_one(std::string_view text, std::string* error = nullptr) {
    const std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::parse(text);
    if (!document.has_value()) return std::nullopt;
    std::vector<std::string> errors;
    const std::vector<TileDefinition> definitions = tile_definitions(*document, &errors);
    if (error != nullptr) *error = errors.empty() ? std::string{} : errors.front();
    if (!errors.empty() || definitions.size() != 1u) return std::nullopt;
    return definitions.front();
}

[[nodiscard]] TaskSpec deliver(std::string name, std::string item, usize count) {
    TaskSpec spec;
    spec.name = std::move(name);
    spec.type = TaskType::Deliver;
    spec.item = std::move(item);
    spec.count = count;
    return spec;
}

/// A rate step: how many items per second, over how many seconds. What it needs in the window is the
/// runtime's to compute (ceil(rate * window)); the spec only carries the designer's two numbers.
[[nodiscard]] TaskSpec rate(std::string name, std::string item, f32 rate_per_second, f32 window_seconds,
                            f32 hold_seconds = 0.0f) {
    TaskSpec spec;
    spec.name = std::move(name);
    spec.type = TaskType::Rate;
    spec.item = std::move(item);
    spec.rate = rate_per_second;
    spec.window = window_seconds;
    spec.hold = hold_seconds;
    return spec;
}

/// A registry, the definitions that go with it, and the task-block content a test asks for: the same
/// shape a content file produces, built in memory.
struct Fixture {
    ContentRegistry registry;
    ContentDefinitions definitions;

    ContentId add_item(std::string_view name) { return registry.register_content(ContentKind::Item, name); }

    /// A 2x1 task block: the built-in "task_block" tile type for its components, and the chain the data
    /// declares (a definition attribute, like the footprint and the capacity).
    ContentId add_block(std::string_view name, usize capacity, std::vector<TaskSpec> tasks) {
        const ContentId id = registry.register_content(ContentKind::Structure, name);
        TileDefinition definition;
        definition.kind = ContentKind::Structure;
        definition.name = std::string(name);
        definition.tile_type = "task_block";
        definition.capacity = capacity;
        definition.width = 2;
        definition.height = 1;
        definition.tasks = std::move(tasks);
        definitions.add(std::move(definition));
        return id;
    }

    [[nodiscard]] std::unique_ptr<Tile> make(ContentKind kind, std::string_view name, ContentId id,
                                             GridPos anchor = GridPos{0, 0}) {
        const TileDefinition* stored = definitions.find_tile(kind, name);
        if (stored == nullptr) return nullptr;
        return make_tile(definitions, *stored, registry, id, 0, anchor);
    }
};

/// Places the task block p name on tile layer 0 of p layer: the layer gives the plot its clock, which
/// a rate window is measured against.
[[nodiscard]] Tile* place_block(MineLayer& layer, const Fixture& content, std::string_view name) {
    PlacedContent where;
    where.kind = ContentKind::Structure;
    where.name = std::string(name);
    where.layer = 0;
    where.anchor = GridPos{1, 1};
    return layer.place(content.registry, content.definitions, where);
}

} // namespace

T2D_TEST(the_data_says_which_tasks_a_block_carries) {
    // The task table: the order the entries are written in is the order they are done in, and the key
    // is the task's name (for reports; the panel says what the type says).
    const std::optional<TileDefinition> two = read_one(R"(structure::
    block::
        tasks::
            first_delivery::
                type:"deliver"
                item:"stone"
                count:10
            second_delivery::
                type:"deliver"
                item:"stone"
                count:25
)");
    T2D_REQUIRE(two.has_value());
    T2D_REQUIRE(two->tasks.size() == 2u);
    T2D_CHECK_EQ(two->tasks[0].name, std::string("first_delivery"));
    T2D_CHECK(two->tasks[0].type == TaskType::Deliver);
    T2D_CHECK_EQ(two->tasks[0].item, std::string("stone"));
    T2D_CHECK_EQ(two->tasks[0].count, 10u);
    T2D_CHECK_EQ(two->tasks[1].name, std::string("second_delivery"));
    T2D_CHECK_EQ(two->tasks[1].count, 25u);

    // Content that asks nothing of the player has no tasks; an empty tasks:: table says the same thing
    // and is allowed.
    const std::optional<TileDefinition> quiet = read_one("structure::\n    plain::\n");
    T2D_REQUIRE(quiet.has_value());
    T2D_CHECK(quiet->tasks.empty());
    const std::optional<TileDefinition> empty = read_one("structure::\n    plain::\n        tasks::\n");
    T2D_REQUIRE(empty.has_value());
    T2D_CHECK(empty->tasks.empty());

    // A task the engine reads and cannot use is refused where the file is, with the task named.
    const auto refused = [](std::string_view text, std::string_view expected) {
        std::string error;
        const std::optional<TileDefinition> definition = read_one(text, &error);
        return !definition.has_value() && error.find(expected) != std::string::npos;
    };
    T2D_CHECK(refused(R"(structure::
    block::
        tasks::
            odd::
                type:"explode"
                item:"stone"
                count:1
)", "names task type 'explode'"));
    T2D_CHECK(refused(R"(structure::
    block::
        tasks::
            odd::
                type:"deliver"
                item:"stone"
                count:1
                reward:"gold"
)", "has no field 'reward'"));
    T2D_CHECK(refused(R"(structure::
    block::
        tasks::
            odd::
                item:"stone"
                count:1
)", "does not say its type"));
    T2D_CHECK(refused(R"(structure::
    block::
        tasks::
            odd::
                type:"deliver"
                count:1
)", "does not say what to deliver"));
    T2D_CHECK(refused(R"(structure::
    block::
        tasks::
            odd::
                type:"deliver"
                item:""
                count:1
)", "takes the quoted name of the item"));
    T2D_CHECK(refused(R"(structure::
    block::
        tasks::
            odd::
                type:"deliver"
                item:"stone"
)", "does not say how many"));
    T2D_CHECK(refused(R"(structure::
    block::
        tasks::
            odd::
                type:"deliver"
                item:"stone"
                count:0
)", "asks for at least one item"));
    T2D_CHECK(refused(R"(structure::
    block::
        tasks::
            odd::
                type:"deliver"
                item:"stone"
                count:2.5
)", "takes a whole number of items"));
    T2D_CHECK(refused("structure::\n    block::\n        tasks:7\n", "tasks must be a table"));
    T2D_CHECK(refused("structure::\n    block::\n        tasks::\n            odd:7\n",
                      "must be a table (type, item, count)"));

    // At most eight steps: a chain longer than the runtime can hold is refused, not trimmed.
    std::string nine = "structure::\n    block::\n        tasks::\n";
    for (int index = 0; index < 9; ++index) {
        nine += std::format("            task_{}::\n                type:\"deliver\"\n                "
                            "item:\"stone\"\n                count:1\n",
                            index);
    }
    T2D_CHECK(refused(nine, "more than 8 entries"));
}

T2D_TEST(a_task_block_knows_its_chain) {
    Fixture content;
    const ContentId stone = content.add_item("stone");
    const ContentId block = content.add_block(
        "block", 100,
        {deliver("first_delivery", "stone", 10), deliver("second_delivery", "stone", 25)});
    const std::unique_ptr<Tile> plot = content.make(ContentKind::Structure, "block", block);
    T2D_REQUIRE(plot != nullptr);

    // The tile type is the one the content selected, and it carries the click that opens the panel as
    // well as the container and the task chain.
    T2D_CHECK(plot->has_task());
    T2D_CHECK(plot->has_container());
    T2D_CHECK(plot->has_click());
    T2D_CHECK(plot->has_drag());

    const Task task = plot->task();
    T2D_CHECK(task.valid());
    T2D_CHECK_EQ(task.count(), 2u);
    T2D_CHECK_EQ(task.current(), 0u);
    T2D_CHECK_FALSE(task.complete());
    const std::optional<TaskStepView> first = task.step(0);
    T2D_REQUIRE(first.has_value());
    T2D_CHECK(first->type == TaskType::Deliver);
    T2D_CHECK(first->kind == ContentKind::Item);
    T2D_CHECK_EQ(first->id, stone);
    T2D_CHECK_EQ(first->count, 10u);
    const std::optional<TaskStepView> second = task.step(1);
    T2D_REQUIRE(second.has_value());
    T2D_CHECK_EQ(second->id, stone);
    T2D_CHECK_EQ(second->count, 25u);
    T2D_CHECK_FALSE(task.step(2).has_value());

    // A chain that names an item this build does not have resolves to "no content": the step can never
    // be satisfied, and it is not silently skipped for the next one.
    const ContentId ghost = content.add_block("ghost_block", 10, {deliver("ghost", "ghost_item", 1)});
    const std::unique_ptr<Tile> ghost_plot = content.make(ContentKind::Structure, "ghost_block", ghost);
    T2D_REQUIRE(ghost_plot != nullptr);
    T2D_REQUIRE(ghost_plot->task().step(0).has_value());
    T2D_CHECK_EQ(ghost_plot->task().step(0)->id, kNoContent);
    T2D_CHECK_EQ(ghost_plot->task().current(), 0u);
}

T2D_TEST(delivering_through_the_container_advances_one_step_at_a_time) {
    Fixture content;
    const ContentId stone = content.add_item("stone");
    const ContentId block = content.add_block(
        "block", 100,
        {deliver("first_delivery", "stone", 10), deliver("second_delivery", "stone", 25)});
    const std::unique_ptr<Tile> plot = content.make(ContentKind::Structure, "block", block);
    T2D_REQUIRE(plot != nullptr);

    // Six of ten: the step is not done, and what was delivered stays in the container as the progress the
    // screen shows (6/10). Nothing is consumed before the step is complete.
    T2D_CHECK(plot->drag_accepts(ContentKind::Item, stone, 6));
    T2D_CHECK(plot->drag_receive(ContentKind::Item, stone, 6));
    T2D_CHECK_EQ(plot->task().current(), 0u);
    T2D_CHECK_EQ(plot->container().count_of(ContentKind::Item, stone), 6u);

    // Four more: the step is complete, and exactly its ten items are consumed.
    T2D_CHECK(plot->drag_receive(ContentKind::Item, stone, 4));
    T2D_CHECK_EQ(plot->task().current(), 1u);
    T2D_CHECK_EQ(plot->container().total(), 0u);

    // Twenty-five for the second step: done, consumed, and the chain is complete.
    T2D_CHECK(plot->drag_receive(ContentKind::Item, stone, 25));
    T2D_CHECK_EQ(plot->task().current(), 2u);
    T2D_CHECK(plot->task().complete());
    T2D_CHECK_EQ(plot->container().total(), 0u);

    // Once complete, items stay: there is no step left for them to feed.
    T2D_CHECK(plot->drag_receive(ContentKind::Item, stone, 7));
    T2D_CHECK_EQ(plot->task().current(), 2u);
    T2D_CHECK_EQ(plot->container().count_of(ContentKind::Item, stone), 7u);
    T2D_CHECK_EQ(plot->run_tasks(), 0u);
}

T2D_TEST(a_chain_is_done_in_order_and_nothing_is_taken_early) {
    Fixture content;
    const ContentId stone = content.add_item("stone");
    const ContentId copper = content.add_item("copper");
    const ContentId block = content.add_block(
        "block", 100,
        {deliver("first_delivery", "stone", 10), deliver("second_delivery", "copper", 25)});
    const std::unique_ptr<Tile> plot = content.make(ContentKind::Structure, "block", block);
    T2D_REQUIRE(plot != nullptr);

    // The second step's item, before the first step is done: it sits in the container and the chain does
    // not move - a task block works through its tasks in order.
    T2D_CHECK(plot->drag_receive(ContentKind::Item, copper, 25));
    T2D_CHECK_EQ(plot->task().current(), 0u);
    T2D_CHECK_EQ(plot->container().count_of(ContentKind::Item, copper), 25u);

    // The first step's ten stone completes it; the copper is still there and now feeds the second step,
    // which is already satisfied, so delivering it completes the chain in the same call.
    T2D_CHECK(plot->drag_receive(ContentKind::Item, stone, 10));
    T2D_CHECK_EQ(plot->task().current(), 2u);
    T2D_CHECK(plot->task().complete());
    T2D_CHECK_EQ(plot->container().total(), 0u);
}

T2D_TEST(a_click_settles_the_container_before_it_opens) {
    Fixture content;
    const ContentId stone = content.add_item("stone");
    const ContentId block = content.add_block("block", 100, {deliver("first_delivery", "stone", 10)});
    const std::unique_ptr<Tile> plot = content.make(ContentKind::Structure, "block", block, GridPos{2, 3});
    T2D_REQUIRE(plot != nullptr);

    // Ten stone put straight in (a save, a placement's starting stock): the click is what turns them into
    // progress, and it does so before the panel opens.
    T2D_CHECK_EQ(plot->container().add(ContentKind::Item, stone, 10), 10u);
    T2D_CHECK_EQ(plot->task().current(), 0u);
    T2D_CHECK(plot->on_click(GridPos{2, 3}));
    T2D_CHECK_EQ(plot->task().current(), 1u);
    T2D_CHECK(plot->click().open());
    T2D_CHECK_EQ(plot->container().total(), 0u);

    // A click that landed somewhere else neither settles nor counts; a plot still going up does not
    // settle either (it is not something the player has yet). The chain is complete by now, so the stock
    // stays where it is: there is no step left to consume it.
    T2D_CHECK_EQ(plot->container().add(ContentKind::Item, stone, 10), 10u);
    T2D_CHECK_FALSE(plot->on_click(GridPos{9, 9}));
    T2D_CHECK_EQ(plot->task().current(), 1u);
    T2D_CHECK_EQ(plot->container().total(), 10u);
    plot->set_under_construction(true);
    T2D_CHECK_FALSE(plot->on_click(GridPos{2, 3}));
    T2D_CHECK_EQ(plot->task().current(), 1u);
    T2D_CHECK_EQ(plot->container().total(), 10u);
    plot->set_under_construction(false);
    T2D_CHECK(plot->on_click(GridPos{2, 3}));
    T2D_CHECK_EQ(plot->task().current(), 1u);
    T2D_CHECK_EQ(plot->container().total(), 10u);
    T2D_CHECK(plot->task().complete()); // the chain was already complete before the click
}

T2D_TEST(a_placement_that_starts_with_its_items_has_already_delivered_them) {
    // What a layer's place_block fills in is delivered while the layer is built: the same settle runs
    // there as after a drag, so the panel never opens showing stock the task should already have taken.
    Fixture content;
    const ContentId stone = content.add_item("stone");
    (void)content.add_block("block", 100, {deliver("first_delivery", "stone", 10)});
    T2D_CHECK_NE(stone, kNoContent);

    LayerSpec spec;
    spec.shape = LayerShape{4, 4, 1};
    PlacedContent placed;
    placed.kind = ContentKind::Structure;
    placed.name = "block";
    placed.layer = 0;
    placed.anchor = GridPos{1, 1};
    placed.holds.push_back(ContentStack{"stone", 10});
    spec.content.push_back(placed);

    MineLayer layer(0, spec.shape, 1);
    LayerBuildReport report;
    layer.build(spec, content.registry, content.definitions, &report);
    T2D_CHECK_MSG(report.clean(), "{}", report.errors.empty() ? "" : report.errors.front());
    const Tile* plot = layer.plot_at(0, GridPos{1, 1});
    T2D_REQUIRE(plot != nullptr);
    T2D_REQUIRE(plot->has_task());
    T2D_CHECK_EQ(plot->task().current(), 1u);
    T2D_CHECK(plot->task().complete());
    T2D_CHECK_EQ(plot->container().total(), 0u);
}

T2D_TEST(a_save_restores_how_far_the_chain_had_got) {
    Fixture content;
    const ContentId stone = content.add_item("stone");
    const ContentId block = content.add_block(
        "block", 100,
        {deliver("first_delivery", "stone", 10), deliver("second_delivery", "stone", 25)});
    const std::unique_ptr<Tile> plot = content.make(ContentKind::Structure, "block", block);
    T2D_REQUIRE(plot != nullptr);
    T2D_CHECK_EQ(stone, plot->task().step(0)->id);

    // The saved count is restored by index; a value past the chain is clamped to its length, because the
    // designer may have shortened the tasks between two builds (docs/SAVE.md).
    T2D_CHECK(plot->set_task_completed(1));
    T2D_CHECK_EQ(plot->task().current(), 1u);
    T2D_CHECK_EQ(plot->task_completed(), 1u);
    T2D_CHECK(plot->set_task_completed(99));
    T2D_CHECK_EQ(plot->task().current(), 2u);
    T2D_CHECK_EQ(plot->task_completed(), 2u);
    T2D_CHECK(plot->task().complete());

    // A plot without the task component answers no, and an empty chain is a chain whose work is done.
    Fixture other;
    const ContentId wall = other.registry.register_content(ContentKind::Structure, "wall");
    TileDefinition wall_definition;
    wall_definition.kind = ContentKind::Structure;
    wall_definition.name = "wall";
    other.definitions.add(std::move(wall_definition));
    const std::unique_ptr<Tile> scenery = other.make(ContentKind::Structure, "wall", wall);
    T2D_REQUIRE(scenery != nullptr);
    T2D_CHECK_FALSE(scenery->has_task());
    T2D_CHECK_FALSE(scenery->task().valid());
    T2D_CHECK_EQ(scenery->task().count(), 0u);
    T2D_CHECK_EQ(scenery->run_tasks(), 0u);
    T2D_CHECK_FALSE(scenery->set_task_completed(1));

    const ContentId empty_block = content.add_block("empty_block", 10, {});
    const std::unique_ptr<Tile> empty = content.make(ContentKind::Structure, "empty_block", empty_block);
    T2D_REQUIRE(empty != nullptr);
    T2D_CHECK(empty->has_task());
    T2D_CHECK_EQ(empty->task().count(), 0u);
    T2D_CHECK(empty->task().complete());
    T2D_CHECK_EQ(empty->run_tasks(), 0u);
    T2D_CHECK(empty->set_task_completed(3)); // clamped to 0: there is nothing to have done
    T2D_CHECK_EQ(empty->task().current(), 0u);
}

T2D_TEST(an_unresolvable_task_step_is_never_satisfied) {
    // A step whose item the build does not have resolves to kNoContent: nothing in any container can
    // match it, so the chain stops there instead of skipping ahead.
    Fixture content;
    const ContentId stone = content.add_item("stone");
    const ContentId block = content.add_block(
        "block", 100, {deliver("missing", "ghost_item", 1), deliver("later", "stone", 1)});
    const std::unique_ptr<Tile> plot = content.make(ContentKind::Structure, "block", block);
    T2D_REQUIRE(plot != nullptr);
    const std::optional<TaskStepView> step = plot->task().step(0);
    T2D_REQUIRE(step.has_value());
    T2D_CHECK_EQ(step->id, kNoContent);
    T2D_CHECK(plot->drag_receive(ContentKind::Item, stone, 1));
    T2D_CHECK_EQ(plot->task().current(), 0u);
    T2D_CHECK_EQ(plot->container().count_of(ContentKind::Item, stone), 1u);
}

T2D_TEST(the_data_says_a_rate_step_and_how_many_items_it_needs) {
    // A rate step: the item and the speed it has to arrive at, over the window that is measured.
    const std::optional<TileDefinition> speed = read_one(R"(structure::
    pump::
        tasks::
            feed::
                type:"rate"
                item:"stone"
                rate:3
                window:5
)");
    T2D_REQUIRE(speed.has_value());
    T2D_REQUIRE(speed->tasks.size() == 1u);
    T2D_CHECK_EQ(speed->tasks[0].name, std::string("feed"));
    T2D_CHECK(speed->tasks[0].type == TaskType::Rate);
    T2D_CHECK_EQ(speed->tasks[0].item, std::string("stone"));
    T2D_CHECK_EQ(speed->tasks[0].rate, 3.0f);
    T2D_CHECK_EQ(speed->tasks[0].window, 5.0f);
    // What the window has to hold is ceil(rate * window): 3/s over 5s is 15 items.
    T2D_CHECK_EQ(speed->tasks[0].count, 15u);

    // window is optional and means one second of measurement.
    const std::optional<TileDefinition> quick = read_one(R"(structure::
    pump::
        tasks::
            feed::
                type:"rate"
                item:"stone"
                rate:2
)");
    T2D_REQUIRE(quick.has_value());
    T2D_CHECK_EQ(quick->tasks[0].window, 1.0f);
    T2D_CHECK_EQ(quick->tasks[0].count, 2u);

    // A task the engine reads and cannot use is refused where the file is, naming what its type allows.
    const auto refused = [](std::string_view text, std::string_view expected) {
        std::string error;
        const std::optional<TileDefinition> definition = read_one(text, &error);
        return !definition.has_value() && error.find(expected) != std::string::npos;
    };
    T2D_CHECK(refused(R"(structure::
    pump::
        tasks::
            feed::
                type:"rate"
                item:"stone"
)", "does not say its rate"));
    T2D_CHECK(refused(R"(structure::
    pump::
        tasks::
            feed::
                type:"rate"
                item:"stone"
                rate:3
                count:9
)", "takes no count"));
    T2D_CHECK(refused(R"(structure::
    pump::
        tasks::
            feed::
                type:"rate"
                item:"stone"
                rate:0
)", "must be greater than zero"));
    T2D_CHECK(refused(R"(structure::
    pump::
        tasks::
            feed::
                type:"rate"
                item:"stone"
                rate:3
                window:0
)", "must be greater than zero"));
    T2D_CHECK(refused(R"(structure::
    pump::
        tasks::
            feed::
                type:"rate"
                item:"stone"
                rate:"fast"
)", "takes a number"));
    T2D_CHECK(refused(R"(structure::
    pump::
        tasks::
            feed::
                type:"rate"
                item:"stone"
                rate:3
                reward:"gold"
)", "has no field 'reward' (type, item, rate, window, hold)"));
    // A deliver step counts the container: it has no rate or window.
    T2D_CHECK(refused(R"(structure::
    pump::
        tasks::
            load::
                type:"deliver"
                item:"stone"
                count:5
                rate:1
)", "takes no rate, window or hold"));
    T2D_CHECK(refused(R"(structure::
    pump::
        tasks::
            load::
                type:"deliver"
                item:"stone"
                count:5
                window:2
)", "takes no rate, window or hold"));
}

T2D_TEST(a_rate_step_reads_its_window_and_needs_the_ceil) {
    Fixture content;
    const ContentId stone = content.add_item("stone");
    const ContentId block = content.add_block("pump", 100, {rate("feed", "stone", 3.0f, 5.0f)});
    const std::unique_ptr<Tile> plot = content.make(ContentKind::Structure, "pump", block);
    T2D_REQUIRE(plot != nullptr);
    T2D_CHECK(plot->has_task());
    const std::optional<TaskStepView> step = plot->task().step(0);
    T2D_REQUIRE(step.has_value());
    T2D_CHECK(step->type == TaskType::Rate);
    T2D_CHECK(step->kind == ContentKind::Item);
    T2D_CHECK_EQ(step->id, stone);
    T2D_CHECK_EQ(step->rate, 3.0f);
    T2D_CHECK_EQ(step->window, 5.0f);
    T2D_CHECK_EQ(step->count, 15u);
    // With no deliveries the window is empty, not complete.
    T2D_CHECK_EQ(plot->task().window_progress(0.0f), 0u);
    T2D_CHECK_EQ(plot->task().current(), 0u);
}

T2D_TEST(a_rate_window_measures_deliveries_over_time) {
    Fixture content;
    const ContentId stone = content.add_item("stone");
    content.add_block("pump", 100, {rate("feed", "stone", 3.0f, 1.0f)});
    MineLayer layer(0, LayerShape{4, 4, 1}, 1);
    Tile* plot = place_block(layer, content, "pump");
    T2D_REQUIRE(plot != nullptr);

    // Two of the three the one-second window needs: the step is not done and the two wait in the box.
    T2D_CHECK(plot->drag_receive(ContentKind::Item, stone, 2));
    T2D_CHECK_EQ(plot->task().window_progress(0.0f), 2u);
    T2D_CHECK_EQ(plot->task().held(), 0.0f); // no hold on this step, and nothing held yet either way
    T2D_CHECK_EQ(plot->task().current(), 0u);
    T2D_CHECK_EQ(plot->container().count_of(ContentKind::Item, stone), 2u);

    // 1.5 seconds later those deliveries have slid out of the window - and the layer ticked no plot,
    // so its count of turns did not change while its clock did.
    T2D_CHECK_EQ(layer.tick(1.5f), 0u);
    T2D_CHECK_EQ(layer.clock_seconds(), 1.5f);
    T2D_CHECK_EQ(plot->task().window_progress(1.5f), 0u);
    T2D_CHECK_EQ(plot->task().current(), 0u);

    // Three at once now satisfy the window: exactly those three are consumed, the two from before stay.
    T2D_CHECK(plot->drag_receive(ContentKind::Item, stone, 3));
    T2D_CHECK_EQ(plot->task().current(), 1u);
    T2D_CHECK_EQ(plot->container().count_of(ContentKind::Item, stone), 2u);
    T2D_CHECK_EQ(plot->task().window_progress(1.5f), 0u); // advancing cleared the window

    // The chain is done: more items just stay where they are.
    T2D_CHECK(plot->drag_receive(ContentKind::Item, stone, 3));
    T2D_CHECK_EQ(plot->task().current(), 1u);
    T2D_CHECK_EQ(plot->container().count_of(ContentKind::Item, stone), 5u);
}

T2D_TEST(a_rate_window_can_be_wider_than_one_second) {
    // rate 2/s over 2 seconds: four items within the span, not two in a second.
    Fixture content;
    const ContentId stone = content.add_item("stone");
    content.add_block("slow_pump", 100, {rate("feed", "stone", 2.0f, 2.0f)});
    MineLayer layer(0, LayerShape{4, 4, 1}, 1);
    Tile* plot = place_block(layer, content, "slow_pump");
    T2D_REQUIRE(plot != nullptr);
    T2D_CHECK(plot->drag_receive(ContentKind::Item, stone, 2));
    T2D_CHECK_EQ(layer.tick(1.5f), 0u);
    T2D_CHECK(plot->drag_receive(ContentKind::Item, stone, 2));
    T2D_CHECK_EQ(plot->task().current(), 1u);
    T2D_CHECK_EQ(plot->container().total(), 0u);

    // Two items and then a 2.5 second gap is not a rate of one every two seconds.
    Fixture other;
    const ContentId other_stone = other.add_item("stone");
    other.add_block("slow_pump", 100, {rate("feed", "stone", 2.0f, 2.0f)});
    MineLayer other_layer(0, LayerShape{4, 4, 1}, 1);
    Tile* quiet = place_block(other_layer, other, "slow_pump");
    T2D_REQUIRE(quiet != nullptr);
    T2D_CHECK(quiet->drag_receive(ContentKind::Item, other_stone, 2));
    T2D_CHECK_EQ(other_layer.tick(2.5f), 0u);
    T2D_CHECK_EQ(quiet->task().window_progress(2.5f), 0u);
    T2D_CHECK_EQ(quiet->task().current(), 0u);
    T2D_CHECK_EQ(quiet->container().count_of(ContentKind::Item, other_stone), 2u);
}

T2D_TEST(a_chain_mixes_deliver_and_rate_steps_in_order) {
    Fixture content;
    const ContentId stone = content.add_item("stone");
    content.add_block("mixer", 100,
                      {deliver("load", "stone", 10), rate("feed", "stone", 3.0f, 1.0f),
                       deliver("pack", "stone", 5)});
    MineLayer layer(0, LayerShape{4, 4, 1}, 1);
    Tile* plot = place_block(layer, content, "mixer");
    T2D_REQUIRE(plot != nullptr);

    // The first deliver step: ten at once, consumed, and the rate step begins at zero.
    T2D_CHECK(plot->drag_receive(ContentKind::Item, stone, 10));
    T2D_CHECK_EQ(plot->task().current(), 1u);
    T2D_CHECK_EQ(plot->container().total(), 0u);

    // Two toward the rate step: not enough, and the last deliver step is not started early.
    T2D_CHECK(plot->drag_receive(ContentKind::Item, stone, 2));
    T2D_CHECK_EQ(plot->task().current(), 1u);
    T2D_CHECK_EQ(plot->task().window_progress(0.0f), 2u);
    T2D_CHECK_EQ(plot->container().count_of(ContentKind::Item, stone), 2u);

    // The first window slid away; three more satisfy the rate step and exactly three are consumed.
    T2D_CHECK_EQ(layer.tick(1.5f), 0u);
    T2D_CHECK(plot->drag_receive(ContentKind::Item, stone, 3));
    T2D_CHECK_EQ(plot->task().current(), 2u);
    T2D_CHECK_EQ(plot->container().count_of(ContentKind::Item, stone), 2u);

    // The last deliver step: three more make five, and they are consumed.
    T2D_CHECK(plot->drag_receive(ContentKind::Item, stone, 3));
    T2D_CHECK_EQ(plot->task().current(), 3u);
    T2D_CHECK(plot->task().complete());
    T2D_CHECK_EQ(plot->container().total(), 0u);
}

T2D_TEST(the_layer_clock_moves_with_tick_and_never_backwards) {
    MineLayer layer(0, LayerShape{4, 4, 1}, 1);
    T2D_CHECK_EQ(layer.clock_seconds(), 0.0f);
    // Nothing in the layer runs, so no plot takes a turn - and time still passes for task windows.
    T2D_CHECK_EQ(layer.tick(0.5f), 0u);
    T2D_CHECK_EQ(layer.clock_seconds(), 0.5f);
    T2D_CHECK_EQ(layer.tick(-2.0f), 0u); // a caller's mistake never makes the clock run backwards
    T2D_CHECK_EQ(layer.clock_seconds(), 0.5f);
    T2D_CHECK_EQ(layer.tick(1.0f), 0u);
    T2D_CHECK_EQ(layer.clock_seconds(), 1.5f);
}

T2D_TEST(a_rate_window_is_live_state_and_a_fresh_plot_starts_it_empty) {
    // A load builds every plot again: completed comes from the save, the window does not (docs/SAVE.md).
    Fixture content;
    const ContentId stone = content.add_item("stone");
    content.add_block("pump", 100, {rate("feed", "stone", 3.0f, 1.0f)});
    MineLayer layer(0, LayerShape{4, 4, 1}, 1);
    Tile* plot = place_block(layer, content, "pump");
    T2D_REQUIRE(plot != nullptr);

    // Items restored straight into the container (a load fills it, it does not deliver) are not a
    // delivery the window has seen: the step does not complete just because the box holds enough.
    T2D_CHECK_EQ(plot->container().add(ContentKind::Item, stone, 3), 3u);
    T2D_CHECK_EQ(plot->run_tasks(), 0u);
    T2D_CHECK_EQ(plot->task().current(), 0u);
    T2D_CHECK_EQ(plot->task().held(), 0.0f); // held is live state: a fresh plot starts it at zero
    T2D_CHECK_EQ(plot->container().count_of(ContentKind::Item, stone), 3u);
    // The same items delivered - recorded on the clock - complete the step.
    plot->note_delivery(ContentKind::Item, stone, 3);
    T2D_CHECK_EQ(plot->run_tasks(), 1u);
    T2D_CHECK_EQ(plot->task().current(), 1u);
    T2D_CHECK_EQ(plot->container().total(), 0u);
}

T2D_TEST(a_rate_step_no_container_can_hold_never_completes) {
    // rate 3/s over a second needs three and the box holds two: the runtime reports it at build time,
    // and the step stops there instead of silently skipping (docs/GAME_DESIGN.md section 1.53).
    Fixture content;
    const ContentId stone = content.add_item("stone");
    content.add_block("tiny", 2, {rate("feed", "stone", 3.0f, 1.0f)});
    MineLayer layer(0, LayerShape{4, 4, 1}, 1);
    Tile* plot = place_block(layer, content, "tiny");
    T2D_REQUIRE(plot != nullptr);
    const std::optional<TaskStepView> step = plot->task().step(0);
    T2D_REQUIRE(step.has_value());
    T2D_CHECK_EQ(step->count, 3u);
    T2D_CHECK(plot->drag_receive(ContentKind::Item, stone, 2));
    T2D_CHECK_EQ(plot->task().window_progress(0.0f), 2u);
    T2D_CHECK_EQ(plot->task().current(), 0u);
    T2D_CHECK_FALSE(plot->drag_accepts(ContentKind::Item, stone, 1)); // the box is full
    T2D_CHECK_EQ(plot->task().current(), 0u);
}

T2D_TEST(the_data_says_how_long_a_rate_condition_has_to_hold) {
    // hold: the rate condition has to keep being true for this many seconds before the step is done.
    const std::optional<TileDefinition> held = read_one(R"(structure::
    pump::
        tasks::
            feed::
                type:"rate"
                item:"stone"
                rate:3
                window:1
                hold:10
)");
    T2D_REQUIRE(held.has_value());
    T2D_REQUIRE(held->tasks.size() == 1u);
    T2D_CHECK(held->tasks[0].type == TaskType::Rate);
    T2D_CHECK_EQ(held->tasks[0].hold, 10.0f);
    T2D_CHECK_EQ(held->tasks[0].count, 3u); // ceil(3 * 1)

    // hold is optional and means zero: the window being satisfied right now is enough (the §1.53 rule).
    const std::optional<TileDefinition> immediate = read_one(R"(structure::
    pump::
        tasks::
            feed::
                type:"rate"
                item:"stone"
                rate:2
                window:2
)");
    T2D_REQUIRE(immediate.has_value());
    T2D_CHECK_EQ(immediate->tasks[0].hold, 0.0f);
    // A hold of zero is as good as not writing it.
    const std::optional<TileDefinition> zero = read_one(R"(structure::
    pump::
        tasks::
            feed::
                type:"rate"
                item:"stone"
                rate:2
                window:2
                hold:0
)");
    T2D_REQUIRE(zero.has_value());
    T2D_CHECK_EQ(zero->tasks[0].hold, 0.0f);

    // A hold the engine reads and cannot use is refused where the file is, naming what rate allows.
    const auto refused = [](std::string_view text, std::string_view expected) {
        std::string error;
        const std::optional<TileDefinition> definition = read_one(text, &error);
        return !definition.has_value() && error.find(expected) != std::string::npos;
    };
    T2D_CHECK(refused(R"(structure::
    pump::
        tasks::
            feed::
                type:"rate"
                item:"stone"
                rate:3
                hold:-1
)", "a hold cannot be negative"));
    T2D_CHECK(refused(R"(structure::
    pump::
        tasks::
            feed::
                type:"rate"
                item:"stone"
                rate:3
                hold:"long"
)", "takes a number of seconds"));
    // A deliver step counts the container: it has no rate, window or hold.
    T2D_CHECK(refused(R"(structure::
    pump::
        tasks::
            load::
                type:"deliver"
                item:"stone"
                count:5
                hold:2
)", "takes no rate, window or hold"));
}

T2D_TEST(a_rate_step_with_a_hold_reports_how_long_it_has_held) {
    Fixture content;
    const ContentId stone = content.add_item("stone");
    content.add_block("pump", 100, {rate("feed", "stone", 3.0f, 1.0f, 2.0f)});
    MineLayer layer(0, LayerShape{4, 4, 1}, 1);
    Tile* plot = place_block(layer, content, "pump");
    T2D_REQUIRE(plot != nullptr);
    const std::optional<TaskStepView> step = plot->task().step(0);
    T2D_REQUIRE(step.has_value());
    T2D_CHECK(step->type == TaskType::Rate);
    T2D_CHECK_EQ(step->hold, 2.0f);
    T2D_CHECK_EQ(step->count, 3u);
    T2D_CHECK_EQ(plot->task().held(), 0.0f);

    // The condition is true (three in the window), so the tick pass starts counting the hold - though
    // no plot took a turn, so tick() still reports zero.
    T2D_CHECK(plot->drag_receive(ContentKind::Item, stone, 3));
    T2D_CHECK_EQ(plot->task().held(), 0.0f);
    T2D_CHECK_EQ(layer.tick(0.5f), 0u);
    T2D_CHECK_EQ(layer.clock_seconds(), 0.5f);
    T2D_CHECK_EQ(plot->task().held(), 0.5f);
    T2D_CHECK_EQ(plot->task().current(), 0u);
}

T2D_TEST(a_rate_hold_has_to_keep_holding_to_complete_the_step) {
    Fixture content;
    const ContentId stone = content.add_item("stone");
    content.add_block("pump", 100, {rate("feed", "stone", 3.0f, 1.0f, 2.0f)});
    MineLayer layer(0, LayerShape{4, 4, 1}, 1);
    Tile* plot = place_block(layer, content, "pump");
    T2D_REQUIRE(plot != nullptr);

    // Deliver three every half second: the one-second window stays satisfied, so the hold keeps growing.
    T2D_CHECK(plot->drag_receive(ContentKind::Item, stone, 3));
    T2D_CHECK_EQ(plot->task().held(), 0.0f);
    T2D_CHECK_EQ(layer.tick(0.5f), 0u);
    T2D_CHECK_EQ(plot->task().held(), 0.5f);
    T2D_CHECK_EQ(plot->task().current(), 0u);
    T2D_CHECK(plot->drag_receive(ContentKind::Item, stone, 3));
    T2D_CHECK_EQ(layer.tick(0.5f), 0u);
    T2D_CHECK_EQ(plot->task().held(), 1.0f);
    T2D_CHECK(plot->drag_receive(ContentKind::Item, stone, 3));
    T2D_CHECK_EQ(layer.tick(0.5f), 0u);
    T2D_CHECK_EQ(plot->task().held(), 1.5f);
    T2D_CHECK(plot->drag_receive(ContentKind::Item, stone, 3));
    // The fourth half second brings held to 2.0: the tick's settle consumes exactly the three the step
    // asks for and advances - twelve were delivered, so nine are left.
    T2D_CHECK_EQ(layer.tick(0.5f), 0u);
    T2D_CHECK_EQ(plot->task().current(), 1u);
    T2D_CHECK(plot->task().complete());
    T2D_CHECK_EQ(plot->task().held(), 0.0f); // advancing cleared the hold (and the window)
    T2D_CHECK_EQ(plot->task().window_progress(2.0f), 0u);
    T2D_CHECK_EQ(plot->container().count_of(ContentKind::Item, stone), 9u);
}

T2D_TEST(a_rate_hold_is_reset_the_moment_the_condition_breaks) {
    Fixture content;
    const ContentId stone = content.add_item("stone");
    content.add_block("pump", 100, {rate("feed", "stone", 3.0f, 1.0f, 2.0f)});
    MineLayer layer(0, LayerShape{4, 4, 1}, 1);
    Tile* plot = place_block(layer, content, "pump");
    T2D_REQUIRE(plot != nullptr);

    // Three at t=0, then a tick inside the window: the hold starts.
    T2D_CHECK(plot->drag_receive(ContentKind::Item, stone, 3));
    T2D_CHECK_EQ(layer.tick(0.5f), 0u);
    T2D_CHECK_EQ(plot->task().held(), 0.5f);
    // A tick past the window: the deliveries slid out, the condition is false, and the hold is gone.
    T2D_CHECK_EQ(layer.tick(1.0f), 0u);
    T2D_CHECK_EQ(plot->task().window_progress(1.5f), 0u);
    T2D_CHECK_EQ(plot->task().held(), 0.0f);
    T2D_CHECK_EQ(plot->task().current(), 0u);
    T2D_CHECK_EQ(plot->container().count_of(ContentKind::Item, stone), 3u); // nothing consumed
    // A fresh delivery starts the hold from zero again: it has to be continuous, not accumulated.
    T2D_CHECK(plot->drag_receive(ContentKind::Item, stone, 3));
    T2D_CHECK_EQ(layer.tick(0.5f), 0u);
    T2D_CHECK_EQ(plot->task().held(), 0.5f);
    T2D_CHECK_EQ(plot->task().current(), 0u);
}

T2D_TEST_MAIN
