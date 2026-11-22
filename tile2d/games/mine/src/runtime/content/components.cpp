#include <mine/runtime/content/builtin.h>

#include <cmath>
#include <limits>

namespace mine::runtime {
namespace content {
namespace {

constexpr u32 kMaxU32 = std::numeric_limits<u32>::max();
constexpr f32 kTau = 6.283185307179586f;
/// One call accounts for at most this many drops. A frame longer than this is a broken clock, not a
/// drill; the cap keeps a bad definition from turning one call into an unbounded loop.
constexpr u32 kMaxDrillDropsPerTick = 1024;

[[nodiscard]] u32 used_slots(const ContainerData& container) {
    return container.count < kMaxContainerSlots ? container.count : kMaxContainerSlots;
}

[[nodiscard]] ContainerSlot* find_slot(ContainerData& container, u32 kind, u32 id) {
    for (u32 index = 0; index < used_slots(container); ++index) {
        ContainerSlot& slot = container.slots[index];
        if (slot.kind == kind && slot.id == id) return &slot;
    }
    return nullptr;
}

u32 container_total(const void*, void* data) {
    const ContainerData* container = static_cast<const ContainerData*>(data);
    if (container == nullptr) return 0;
    u64 total = 0;
    for (u32 index = 0; index < used_slots(*container); ++index)
        total += container->slots[index].count;
    return total > kMaxU32 ? kMaxU32 : static_cast<u32>(total);
}

u32 container_room(const void* shared, void* data) {
    const ContainerShared* definition = static_cast<const ContainerShared*>(shared);
    if (definition == nullptr) return 0;
    const u32 total = container_total(shared, data);
    return definition->capacity > total ? definition->capacity - total : 0;
}

u32 container_count(const void*, void* data) {
    const ContainerData* container = static_cast<const ContainerData*>(data);
    return container != nullptr ? used_slots(*container) : 0;
}

u32 container_add(const void* shared, void* data, u32 kind, u32 id, u32 amount) {
    const ContainerShared* definition = static_cast<const ContainerShared*>(shared);
    ContainerData* container = static_cast<ContainerData*>(data);
    if (definition == nullptr || container == nullptr || amount == 0 || id == kNoContentId) return 0;
    const u32 room = container_room(shared, data);
    if (room == 0) return 0;
    u32 accepted = amount < room ? amount : room;
    if (ContainerSlot* slot = find_slot(*container, kind, id); slot != nullptr) {
        if (slot->count > kMaxU32 - accepted) accepted = kMaxU32 - slot->count;
        slot->count += accepted;
        return accepted;
    }
    if (used_slots(*container) >= kMaxContainerSlots) return 0;
    container->slots[container->count] = ContainerSlot{kind, id, accepted};
    ++container->count;
    return accepted;
}

u32 container_take(const void*, void* data, u32 kind, u32 id, u32 amount) {
    ContainerData* container = static_cast<ContainerData*>(data);
    if (container == nullptr || amount == 0) return 0;
    const u32 slots = used_slots(*container);
    for (u32 index = 0; index < slots; ++index) {
        ContainerSlot& slot = container->slots[index];
        if (slot.kind != kind || slot.id != id) continue;
        const u32 given = slot.count < amount ? slot.count : amount;
        slot.count -= given;
        if (slot.count == 0) {
            for (u32 next = index; next + 1 < slots; ++next)
                container->slots[next] = container->slots[next + 1];
            --container->count;
            container->slots[container->count] = ContainerSlot{0, 0, 0};
        }
        return given;
    }
    return 0;
}

const ContainerSlot* container_slots(const void*, void* data) {
    const ContainerData* container = static_cast<const ContainerData*>(data);
    return container != nullptr ? container->slots : nullptr;
}

// --- task ----------------------------------------------------------------------------------------
u32 task_count(const void* shared, void*) {
    const TaskShared* definition = static_cast<const TaskShared*>(shared);
    if (definition == nullptr) return 0;
    return definition->count < kMaxTaskSteps ? definition->count : kMaxTaskSteps;
}

u32 task_current(const void* shared, void* data) {
    const TaskData* task = static_cast<const TaskData*>(data);
    if (task == nullptr) return 0;
    const u32 total = task_count(shared, data);
    // Clamped here, not only in the reader: a save, a mod or an old content table may leave the state
    // past the chain, and "the current step" must still be a step that exists.
    return task->completed < total ? task->completed : total;
}

u8 task_step(const void* shared, void* data, u32 index, TaskStep* out) {
    const TaskShared* definition = static_cast<const TaskShared*>(shared);
    if (definition == nullptr || out == nullptr) return 0;
    if (index >= task_count(shared, data)) return 0;
    *out = definition->steps[index];
    return 1;
}

void task_advance(const void* shared, void* data) {
    TaskData* task = static_cast<TaskData*>(data);
    if (task == nullptr) return;
    if (task->completed < task_count(shared, data)) ++task->completed;
    // The next step starts measuring from now: what fed the step that just finished is spent with it,
    // and a hold on it cannot count toward the next step (docs/GAME_DESIGN.md section 1.54).
    task->entries = 0;
    task->held = 0.0f;
}

/// Drops the deliveries that slid out of the window ending at \p now, keeping arrival order.
void task_prune(TaskData& task, f32 window, f32 now) {
    u32 kept = 0;
    for (u32 index = 0; index < task.entries; ++index) {
        const TaskDelivery& delivery = task.recent[index];
        if (delivery.at <= now - window) continue;
        task.recent[kept++] = delivery;
    }
    for (u32 index = kept; index < task.entries; ++index) task.recent[index] = TaskDelivery{0.0f, 0};
    task.entries = kept;
}

u32 task_window_progress(const void* shared, void* data, f32 now) {
    TaskData* task = static_cast<TaskData*>(data);
    if (task == nullptr) return 0;
    TaskStep step{};
    const u32 current = task_current(shared, data);
    if (task_step(shared, data, current, &step) == 0 || step.type != kTaskTypeRate) return 0;
    task_prune(*task, step.window > 0.0f ? step.window : 1.0f, now);
    u64 total = 0;
    for (u32 index = 0; index < task->entries; ++index) total += task->recent[index].amount;
    return total > kMaxU32 ? kMaxU32 : static_cast<u32>(total);
}

void task_record(const void* shared, void* data, u32 kind, u32 id, u32 amount, f32 now) {
    TaskData* task = static_cast<TaskData*>(data);
    if (task == nullptr || amount == 0) return;
    const u32 current = task_current(shared, data);
    TaskStep step{};
    if (task_step(shared, data, current, &step) == 0) return;
    // Only the current step's own item, and only while that step measures a rate: a deliver step's
    // progress is what its container holds, not a window history (docs/GAME_DESIGN.md section 1.53).
    if (step.type != kTaskTypeRate || kind != step.kind || id != step.id) return;
    task_prune(*task, step.window > 0.0f ? step.window : 1.0f, now);
    if (task->entries >= kMaxTaskWindowEntries) {
        // A full history keeps its newest entry and adds to it: dropping a delivery would make the
        // measured rate wrong for a reason the player cannot see.
        TaskDelivery& newest = task->recent[task->entries - 1u];
        newest.amount = newest.amount > kMaxU32 - amount ? kMaxU32 : newest.amount + amount;
        return;
    }
    task->recent[task->entries++] = TaskDelivery{now, amount};
}

f32 task_held(const void*, void* data) {
    const TaskData* task = static_cast<const TaskData*>(data);
    return task != nullptr ? task->held : 0.0f;
}

void task_tick(const void* shared, void* data, f32 now, f32 delta_seconds) {
    TaskData* task = static_cast<TaskData*>(data);
    if (task == nullptr) return;
    TaskStep step{};
    const u32 current = task_current(shared, data);
    if (task_step(shared, data, current, &step) == 0 || step.type != kTaskTypeRate || step.hold <= 0.0f) {
        task->held = 0.0f;
        return;
    }
    // The hold is **continuous**: a frame in which the window no longer holds what the step asks for
    // resets it to zero, and only a fresh run from zero can complete the step.
    if (task_window_progress(shared, data, now) < step.count) {
        task->held = 0.0f;
        return;
    }
    task->held += delta_seconds > 0.0f ? delta_seconds : 0.0f;
}

u8 click_receive(const void*, void* data, u8 on_it) {
    ClickData* click = static_cast<ClickData*>(data);
    if (click == nullptr) return 0;
    if (on_it == 0) {
        click->open = 0;
        return 0;
    }
    ++click->clicks;
    click->open = click->open != 0 ? 0 : 1;
    return 1;
}

u8 click_open(const void*, void* data) {
    const ClickData* click = static_cast<const ClickData*>(data);
    return click != nullptr && click->open != 0 ? 1 : 0;
}

u8 ticker_advance(const void*, void* data, f32 delta_seconds) {
    TickerData* ticker = static_cast<TickerData*>(data);
    if (ticker == nullptr) return 0;
    if (!(ticker->period > 0.0f)) return 1; // no period: every tick
    if (!(delta_seconds > 0.0f)) return 0;
    ticker->elapsed += delta_seconds;
    if (ticker->elapsed < ticker->period) return 0;
    // Keep only what is left of a whole period: a long frame runs the plot **once** and carries the
    // rest, so the accumulator can never hold a full period again (types/update_cadence.h).
    ticker->elapsed = std::fmod(ticker->elapsed, ticker->period);
    return 1;
}

f32 ticker_period(const void*, void* data) {
    const TickerData* ticker = static_cast<const TickerData*>(data);
    return ticker != nullptr && ticker->period > 0.0f ? ticker->period : 0.0f;
}

TilePreview preview_get(const void* shared, void*, f32 seconds) {
    TilePreview preview{};
    const PreviewShared* definition = static_cast<const PreviewShared*>(shared);
    if (definition == nullptr) return preview;
    if (definition->image == nullptr || definition->image[0] == '\0') return preview;
    PreviewPart& base = preview.parts[preview.count];
    base.part = "base";
    base.image = definition->image;
    base.mirrored = definition->mirrored;
    base.radians = 0.0f;
    base.pivot = Vec2f{0.5f, 0.5f};
    ++preview.count;
    if (definition->top_image == nullptr || definition->top_image[0] == '\0') return preview;
    f32 radians = 0.0f;
    if (definition->spin_seconds > 0.0f && seconds > 0.0f) {
        const f32 turns = seconds / definition->spin_seconds;
        radians = (turns - std::floor(turns)) * kTau;
    }
    PreviewPart& top = preview.parts[preview.count];
    top.part = "top";
    top.image = definition->top_image;
    top.mirrored = definition->mirrored;
    top.radians = radians;
    top.pivot = definition->spin_origin;
    ++preview.count;
    return preview;
}

u8 building_under_construction(const void*, void* data) {
    const BuildingData* building = static_cast<const BuildingData*>(data);
    return building != nullptr && building->under_construction != 0 ? 1 : 0;
}

void building_set_under_construction(const void*, void* data, u8 value) {
    auto* building = static_cast<BuildingData*>(data);
    if (building != nullptr) building->under_construction = value != 0 ? 1 : 0;
}

[[nodiscard]] f32 drill_period(const void* shared, const DrillData& drill) {
    if (drill.job.period_seconds > 0.0f) return drill.job.period_seconds;
    const DrillShared* definition = static_cast<const DrillShared*>(shared);
    if (definition == nullptr || !(definition->dig_power > 0.0f) || !(drill.job.hardness > 0.0f)) return 0.0f;
    return drill.job.hardness / definition->dig_power;
}

u32 drill_tick(const void* shared, void* data, f32 elapsed_seconds) {
    DrillData* drill = static_cast<DrillData*>(data);
    if (drill == nullptr || drill->job.drop_id == kNoContentId) return 0;
    if (!(elapsed_seconds > 0.0f)) return 0;
    const f32 period = drill_period(shared, *drill);
    if (!(period > 0.0f)) return 0;
    const f32 due = elapsed_seconds / period;
    u32 count = 1;
    if (due >= static_cast<f32>(kMaxDrillDropsPerTick)) {
        count = kMaxDrillDropsPerTick;
    } else if (due >= 1.0f) {
        count = static_cast<u32>(due);
    }
    if (drill->produced > kMaxU32 - count) count = kMaxU32 - drill->produced;
    for (u32 index = 0; index < count; ++index) {
        drill->rng = drill->rng * 6364136223846793005ull + 1442695040888963407ull;
        drill->side = static_cast<u32>((drill->rng >> 33u) & 3ull);
        ++drill->produced;
    }
    return count;
}

u32 drill_produced(const void*, void* data) {
    const DrillData* drill = static_cast<const DrillData*>(data);
    return drill != nullptr ? drill->produced : 0;
}

u8 drag_accepts(const void*, void* data, u32 kind, u32 id, u32 amount) {
    const DragData* drag = static_cast<const DragData*>(data);
    if (drag == nullptr || drag->container == nullptr || drag->container_functions == nullptr) return 0;
    if (amount == 0 || id == kNoContentId) return 0;
    return drag->container_functions->room(drag->container_shared, drag->container) >= amount ? 1 : 0;
}

u8 drag_receive(const void*, void* data, u32 kind, u32 id, u32 amount) {
    DragData* drag = static_cast<DragData*>(data);
    if (drag == nullptr || drag->container == nullptr || drag->container_functions == nullptr) return 0;
    if (!drag_accepts(nullptr, data, kind, id, amount)) return 0;
    const u32 accepted =
        drag->container_functions->add(drag->container_shared, drag->container, kind, id, amount);
    if (accepted == amount) return 1;
    if (accepted != 0) {
        (void)drag->container_functions->take(drag->container_shared, drag->container, kind, id, accepted);
    }
    return 0;
}

const char* floor_role() {
    return "floor";
}

const char* ore_role() {
    return "ore";
}

const char* warehouse_role() {
    return "warehouse";
}

const char* drill_role() {
    return "drill";
}

const char* task_block_role() {
    return "task_block";
}

} // namespace

const ContainerFunctions kContainerFunctions = {
    {static_cast<u32>(sizeof(ContainerFunctions)), 1},
    &container_total,
    &container_room,
    &container_count,
    &container_add,
    &container_take,
    &container_slots,
};
const ClickFunctions kClickFunctions = {
    {static_cast<u32>(sizeof(ClickFunctions)), 1}, &click_receive, &click_open};
const DragFunctions kDragFunctions = {
    {static_cast<u32>(sizeof(DragFunctions)), 1}, &drag_accepts, &drag_receive};
const TickerFunctions kTickerFunctions = {
    {static_cast<u32>(sizeof(TickerFunctions)), 1}, &ticker_advance, &ticker_period};
const PreviewFunctions kPreviewFunctions = {{static_cast<u32>(sizeof(PreviewFunctions)), 1}, &preview_get};
const BuildingFunctions kBuildingFunctions = {{static_cast<u32>(sizeof(BuildingFunctions)), 1},
                                              &building_under_construction,
                                              &building_set_under_construction};
const DrillFunctions kDrillFunctions = {
    {static_cast<u32>(sizeof(DrillFunctions)), 1}, &drill_tick, &drill_produced};

const TaskFunctions kTaskFunctions = {
    {static_cast<u32>(sizeof(TaskFunctions)), 1}, &task_count, &task_current,          &task_step,
    &task_advance,                                &task_window_progress, &task_record, &task_held,
    &task_tick};
const BuiltinTileFunctions kFloorTileFunctions = {{static_cast<u32>(sizeof(BuiltinTileFunctions)), 1},
                                                  &floor_role};
const BuiltinTileFunctions kOreTileFunctions = {{static_cast<u32>(sizeof(BuiltinTileFunctions)), 1},
                                                &ore_role};
const BuiltinTileFunctions kWarehouseTileFunctions = {{static_cast<u32>(sizeof(BuiltinTileFunctions)), 1},
                                                      &warehouse_role};
const BuiltinTileFunctions kDrillTileFunctions = {{static_cast<u32>(sizeof(BuiltinTileFunctions)), 1},
                                                  &drill_role};
const BuiltinTileFunctions kTaskBlockTileFunctions = {
    {static_cast<u32>(sizeof(BuiltinTileFunctions)), 1}, &task_block_role};

} // namespace content
} // namespace mine::runtime
