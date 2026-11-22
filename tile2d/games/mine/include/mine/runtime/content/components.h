// Mine - the data and function tables of the built-in content runtime.
//
// Every type in this header is C compatible on purpose: standard layout, fixed-width fields, no STL,
// no vtable, no pointer to a C++ object. An instance stores its own state in a *Data struct;
// the read-only definition shared by every instance of one content entry is a *Shared struct passed as
// the first argument to every function. The runtime core stores both pointers and knows neither shape.
//
// The function signature convention is
//
//     return_type fn(const void* shared, void* data, ...);
//
// with shared first and data second, both nullable. The instance parameter is writable in every table
// even when a particular function only reads it; the implementation casts to const itself. A table
// begins with InterfaceTableHeader so a consumer built with another compiler can check its version
// before calling it. Instances never hold an object pointer: drag is the one component that reaches
// another component of the same object, and it does so through the ContainerData pointer and
// ContainerFunctions table that link_drag() puts in it.
#pragma once

#include <mine/runtime/component.h>

#include <t2d/core/types.h>

#include <type_traits>

namespace mine::runtime {
namespace content {

using t2d::f32;
using t2d::u32;
using t2d::u64;
using t2d::u8;

// --- container -----------------------------------------------------------------------------------
/// Longest list of distinct stacks one container can hold. Capacity is counted in items, not stacks, so
/// this is only a bound on how many different kinds fit before a merge is the only way to add more.
inline constexpr u32 kMaxContainerSlots = 8;

/// One stack: what it is (a kind and an id in the caller's content vocabulary) and how many.
struct ContainerSlot {
    u32 kind;
    u32 id;
    u32 count;
};

/// The instance state: the stacks, and how many of the array's slots are used.
struct ContainerData {
    u32 count;
    ContainerSlot slots[kMaxContainerSlots];
};

/// The definition: how many items (all stacks together) fit. Capacity 0 takes nothing.
struct ContainerShared {
    u32 capacity;
};

struct ContainerFunctions {
    InterfaceTableHeader header;
    u32 (*total)(const void* shared, void* data);
    u32 (*room)(const void* shared, void* data);
    /// Number of used stacks.
    u32 (*count)(const void* shared, void* data);
    /// Puts up to \p amount in and returns how many it actually took (0 when full or no identity).
    u32 (*add)(const void* shared, void* data, u32 kind, u32 id, u32 amount);
    /// Takes up to \p amount out and returns how many it actually gave.
    u32 (*take)(const void* shared, void* data, u32 kind, u32 id, u32 amount);
    const ContainerSlot* (*slots)(const void* shared, void* data);
};

// --- click ---------------------------------------------------------------------------------------
/// A panel's open flag and how many clicks landed on it.
struct ClickData {
    u8 open;
    u32 clicks;
};

struct ClickFunctions {
    InterfaceTableHeader header;
    /// A click arrived; \p on_it says whether it landed on the thing. Returns 1 when it took the click.
    u8 (*receive)(const void* shared, void* data, u8 on_it);
    u8 (*open)(const void* shared, void* data);
};

// --- drag ----------------------------------------------------------------------------------------
/// The instance state of a thing a drop can be dragged into. The three pointers are filled by link_drag()
/// after the object exists: they are the same object's container, its shared definition and its active
/// container table. null means "nothing to receive into" and every drag call answers no.
struct DragData {
    void* container;
    const void* container_shared;
    const ContainerFunctions* container_functions;
};

struct DragFunctions {
    InterfaceTableHeader header;
    /// Whether the container would take \p amount right now.
    u8 (*accepts)(const void* shared, void* data, u32 kind, u32 id, u32 amount);
    /// Takes \p amount in whole or not at all; returns 1 only when all of it went in.
    u8 (*receive)(const void* shared, void* data, u32 kind, u32 id, u32 amount);
};

/// The name the interface goes by: DragTarget.
using DragTargetFunctions = DragFunctions;

// --- ticker --------------------------------------------------------------------------------------
/// A period in seconds (0 means every tick) and the time piled up since the last run.
struct TickerData {
    f32 period;
    f32 elapsed;
};

struct TickerFunctions {
    InterfaceTableHeader header;
    /// Hands the ticker the frame's time; returns 1 when its turn came. One call runs at most once and
    /// keeps the remainder, so a long frame does not replay what it missed.
    u8 (*advance)(const void* shared, void* data, f32 delta_seconds);
    f32 (*period)(const void* shared, void* data);
};

// --- preview -------------------------------------------------------------------------------------
/// A point in a picture's own coordinates, C compatible (0,0 bottom left, 1,1 top right).
struct Vec2f {
    f32 x;
    f32 y;
};

inline constexpr u32 kMaxPreviewParts = 4;

/// One picture of a preview: which part it is, the image name the caller resolves, how it is drawn.
struct PreviewPart {
    const char* part;
    const char* image;
    u8 mirrored;
    f32 radians;
    Vec2f pivot;
};

/// The complete preview, in drawing order. No STL: a fixed array and a count.
struct TilePreview {
    PreviewPart parts[kMaxPreviewParts];
    u32 count;
};

/// The definition of how a thing draws: its base image, an optional turning top, and the top's spin.
struct PreviewShared {
    const char* image;
    const char* top_image;
    u8 mirrored;
    f32 spin_seconds;
    Vec2f spin_origin;
};

/// The instance state of a preview. It has nothing to remember yet (the picture is a function of the
/// shared definition and the clock); the field keeps the component a real data block for the next round.
struct PreviewData {
    u32 reserved;
};

struct PreviewFunctions {
    InterfaceTableHeader header;
    /// The complete preview at \p seconds of the world clock. Returns count 0 when shared is null or
    /// names no image.
    TilePreview (*get_preview)(const void* shared, void* data, f32 seconds);
};

// --- building ------------------------------------------------------------------------------------
inline constexpr u32 kMaxBuildingMaterials = 4;

/// One price line: a kind, an id and how many are needed.
struct BuildingMaterial {
    u32 kind;
    u32 id;
    u32 count;
};

/// The definition of a building: how long it takes and what it costs.
struct BuildingShared {
    f32 seconds;
    u32 material_count;
    BuildingMaterial materials[kMaxBuildingMaterials];
};

/// The instance state: whether it is still going up, and how far the build has got.
struct BuildingData {
    u8 under_construction;
    f32 elapsed;
    u32 delivered;
};

struct BuildingFunctions {
    InterfaceTableHeader header;
    u8 (*under_construction)(const void* shared, void* data);
    /// Sets the sweep flag. The build model owns when it changes; the component only stores it.
    void (*set_under_construction)(const void* shared, void* data, u8 value);
};

// --- drill ---------------------------------------------------------------------------------------
/// What a drill was told to mine: the ore, the drop its ore yields, and the numbers a rate comes from.
/// period_seconds takes precedence when positive; otherwise hardness / shared dig_power is the period.
struct DrillJob {
    u32 ore_kind;
    u32 ore_id;
    u32 drop_id;
    f32 hardness;
    f32 period_seconds;
};

/// The definition of a drill: only how strong it is. The job and the state belong to each instance.
struct DrillShared {
    f32 dig_power;
};

/// The instance state of a drill: its job, how many drops it made, where the next one goes, and its dice.
struct DrillData {
    DrillJob job;
    u32 produced;
    u32 side;
    u64 rng;
};

struct DrillFunctions {
    InterfaceTableHeader header;
    /// Accounts for \p elapsed_seconds and returns how many drops it threw (0 when idle). A frame worth
    /// less than one period still produces one, the way a due cadence hands it a whole span; the dice
    /// advance once per drop so two drills with the same state throw the same way.
    u32 (*tick)(const void* shared, void* data, f32 elapsed_seconds);
    u32 (*produced)(const void* shared, void* data);
};

// --- task ----------------------------------------------------------------------------------------
/// Longest chain of tasks one task block carries. The content reader refuses a longer "tasks::" table
/// (docs/MODS.md).
inline constexpr u32 kMaxTaskSteps = 8;
/// The task types the engine has: deliver an item in a count (docs/GAME_DESIGN.md section 1.52), and
/// take an item at a rate for a span of time (section 1.53). The number is what TaskStep::type carries,
/// so a mod's own type can be told apart from the game's.
inline constexpr u32 kTaskTypeDeliver = 1;
inline constexpr u32 kTaskTypeRate = 2;
/// How many deliveries one task block remembers for a rate window. A window holds a handful of drags;
/// the cap keeps the state a fixed size, and a full history merges into its newest entry instead of
/// dropping a delivery - a rate task must not lose progress to bookkeeping (section 1.53).
inline constexpr u32 kMaxTaskWindowEntries = 32;

/// One step of a chain: what - by the caller's content vocabulary - and how much. A **deliver** step
/// uses count; a **rate** step uses count (how many in the window), rate (items per second) and window
/// (the measured seconds).
struct TaskStep {
    u32 type;
    u32 kind;
    u32 id;
    u32 count;
    f32 rate;
    f32 window;
    f32 hold;
};

/// One delivery a rate window remembers: when it arrived, and how many.
struct TaskDelivery {
    f32 at;
    u32 amount;
};

/// The definition of a task block: its chain of steps, in execution order.
struct TaskShared {
    u32 count;
    TaskStep steps[kMaxTaskSteps];
};

/// The instance state: how many steps are done, and the deliveries the current **rate** window has seen.
/// completed is also the index of the current step, so a value equal to TaskShared::count means the
/// whole chain is complete. The window history and the held time are **live** (they are measured in
/// seconds on the layer's clock), but a save keeps them: the TASK chunk's version 2 writes each delivery
/// as its remaining life plus amount, so a load continues the window and the hold where the save left
/// off (docs/SAVE.md, docs/GAME_DESIGN.md sections 1.53-1.55). The reserved word keeps the state layout
/// open for a future per-plot task field.
struct TaskData {
    u32 completed;
    u32 reserved;
    /// How long the current rate step's condition has been continuously true, in seconds. Measured on
    /// the layer's clock and saved with the window (TASK chunk version 2, docs/SAVE.md).
    f32 held;
    u32 entries;
    TaskDelivery recent[kMaxTaskWindowEntries];
};

struct TaskFunctions {
    InterfaceTableHeader header;
    /// How many steps the shared chain holds (clamped to kMaxTaskSteps).
    u32 (*count)(const void* shared, void* data);
    /// How many are done: the current step's index, clamped to count.
    u32 (*current)(const void* shared, void* data);
    /// Reads step \p index into \p out; returns 0 when the chain has no such step.
    u8 (*step)(const void* shared, void* data, u32 index, TaskStep* out);
    /// Marks one step done and clears the rate window - the next step starts measuring from zero. Does
    /// nothing when the chain is already complete.
    void (*advance)(const void* shared, void* data);
    /// How many of the current step's item arrived within its window ending at \p now: the measured
    /// progress of a **rate** step (0 for any other step). May prune the window.
    u32 (*window_progress)(const void* shared, void* data, f32 now);
    /// Remembers that \p amount of \p kind / \p id arrived at \p now. Only a matching **rate** step
    /// records - a deliver step keeps no window, its progress is what its container holds.
    void (*record)(const void* shared, void* data, u32 kind, u32 id, u32 amount, f32 now);
    /// How long the current rate step's condition has been continuously true, in seconds (0 for any
    /// other step, and for a step with no hold).
    f32 (*held)(const void* shared, void* data);
    /// Accounts for \p delta_seconds on the layer's clock \p now: a satisfied rate step's held time
    /// grows, an unsatisfied one's is reset to zero at once - a hold has to be continuous
    /// (docs/GAME_DESIGN.md section 1.54).
    void (*tick)(const void* shared, void* data, f32 now, f32 delta_seconds);
};

// --- the C surface stays a C surface -------------------------------------------------------------
static_assert(std::is_standard_layout_v<ContainerSlot> && std::is_trivially_copyable_v<ContainerSlot>);
static_assert(std::is_standard_layout_v<ContainerData> && std::is_trivially_copyable_v<ContainerData>);
static_assert(std::is_standard_layout_v<ContainerShared> && std::is_trivially_copyable_v<ContainerShared>);
static_assert(std::is_standard_layout_v<ContainerFunctions>);
static_assert(std::is_standard_layout_v<ClickData> && std::is_trivially_copyable_v<ClickData>);
static_assert(std::is_standard_layout_v<ClickFunctions>);
static_assert(std::is_standard_layout_v<DragData> && std::is_trivially_copyable_v<DragData>);
static_assert(std::is_standard_layout_v<DragFunctions>);
static_assert(std::is_standard_layout_v<TickerData> && std::is_trivially_copyable_v<TickerData>);
static_assert(std::is_standard_layout_v<TickerFunctions>);
static_assert(std::is_standard_layout_v<PreviewPart> && std::is_trivially_copyable_v<PreviewPart>);
static_assert(std::is_standard_layout_v<TilePreview> && std::is_trivially_copyable_v<TilePreview>);
static_assert(std::is_standard_layout_v<PreviewShared> && std::is_trivially_copyable_v<PreviewShared>);
static_assert(std::is_standard_layout_v<PreviewData> && std::is_trivially_copyable_v<PreviewData>);
static_assert(std::is_standard_layout_v<PreviewFunctions>);
static_assert(std::is_standard_layout_v<BuildingMaterial> && std::is_trivially_copyable_v<BuildingMaterial>);
static_assert(std::is_standard_layout_v<BuildingShared> && std::is_trivially_copyable_v<BuildingShared>);
static_assert(std::is_standard_layout_v<BuildingData> && std::is_trivially_copyable_v<BuildingData>);
static_assert(std::is_standard_layout_v<BuildingFunctions>);
static_assert(std::is_standard_layout_v<DrillJob> && std::is_trivially_copyable_v<DrillJob>);
static_assert(std::is_standard_layout_v<DrillShared> && std::is_trivially_copyable_v<DrillShared>);
static_assert(std::is_standard_layout_v<DrillData> && std::is_trivially_copyable_v<DrillData>);
static_assert(std::is_standard_layout_v<DrillFunctions>);
static_assert(std::is_standard_layout_v<TaskStep> && std::is_trivially_copyable_v<TaskStep>);
static_assert(std::is_standard_layout_v<TaskDelivery> && std::is_trivially_copyable_v<TaskDelivery>);
static_assert(std::is_standard_layout_v<TaskShared> && std::is_trivially_copyable_v<TaskShared>);
static_assert(std::is_standard_layout_v<TaskData> && std::is_trivially_copyable_v<TaskData>);
static_assert(std::is_standard_layout_v<TaskFunctions>);
static_assert(!std::is_polymorphic_v<ContainerFunctions>);
static_assert(!std::is_polymorphic_v<ClickFunctions>);
static_assert(!std::is_polymorphic_v<DragFunctions>);
static_assert(!std::is_polymorphic_v<TickerFunctions>);
static_assert(!std::is_polymorphic_v<PreviewFunctions>);
static_assert(!std::is_polymorphic_v<BuildingFunctions>);
static_assert(!std::is_polymorphic_v<DrillFunctions>);
static_assert(!std::is_polymorphic_v<TaskData>);
static_assert(!std::is_polymorphic_v<TaskFunctions>);

} // namespace content
} // namespace mine::runtime
