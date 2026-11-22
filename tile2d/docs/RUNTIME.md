# Runtime content model

Mine's plots are **data**. A plot's identity, footprint and layer are core fields; everything it can do
comes from **components** stored in one contiguous `ObjectData` block. The implementation lives in
function tables owned by the component **type**, never in the object. There is no `Warehouse`,
`Drill`, `SceneTile` or `EntityTile` class anymore, and no RTTI/concrete cast is needed to reach a
plot's abilities.

## Objects and types

```
Tile
  core fields: kind, id, layer, anchor, footprint, mirrored, definition
  runtime::ObjectData  ->  bytes of the components named by the tile type
  runtime::TileType*   ->  component id list + tile functions

ObjectData
  Archetype*           ->  component offsets, total size
  shared definitions   ->  one per content entry (capacity, image, ...)
  instance bytes       ->  one per plot
```

- **Shared definition**: content-level, read-only values: warehouse capacity, drill power, preview
  parts, building cost/time. One object per content entry; every plot of that content points at it.
- **Instance data**: per-plot state: container slots, click state, ticker phase, drill produced count,
  under-construction flag.
- **Save-shared state**: per-save state shared by all plots of a content (building unlocks, research)
  remains in the save attributes (`UNLK`/`RSCH`), not in a plot.
- **Runtime-only data**: caches and transient values, not serialized.

See `include/mine/runtime/component.h`, `archetype.h`, `object_data.h`, `tile_type.h`.

## Function tables

Every component type carries opaque interface tables (`const void*`) keyed by a stable
`InterfaceId`. The built-in content defines C-compatible tables in
`include/mine/runtime/content/components.h`:

- `ContainerFunctions`: total/room/count/add/take/slots
- `ClickFunctions`: receive/open
- `DragFunctions`: accepts/receive (a parked drag is linked to its container by `link_drag`)
- `TickerFunctions`: advance/period
- `PreviewFunctions`: complete preview parts
- `BuildingFunctions`: under-construction flag
- `DrillFunctions`: tick/produced
- `TaskFunctions`: count/current/step/advance over `TaskShared` (the chain: up to `kMaxTaskSteps`
  steps, each `type`/`kind`/`id`/`count`, plus a **rate** step's `rate`/`window`/`hold`) and `TaskData`
  (`completed`, a reserved word, the live **held** seconds, and the **live** delivery window: up to
  `kMaxTaskWindowEntries` `TaskDelivery{at, amount}` entries). Two more functions carry that window:
  `window_progress(now)` (how many of the current step's item arrived within the window ending at `now`)
  and `record(kind, id, amount, now)` (a delivery arrived; only a matching rate step keeps one). A hold
  adds `held()` (how long the rate condition has been continuously true) and `tick(now, delta)` (a
  satisfied step's held time grows, an unsatisfied one resets to 0 at once). `advance` clears both the
  window and the held time, so the next step starts measuring from zero. `settle_tasks(ObjectData&,
  f32 now)` is the one place the table runs: deliver counts the container; a rate step with a hold asks
  `held() >= hold`, one without asks `window_progress(now) >= count`; both consume exactly what the step
  asks for (docs/GAME_DESIGN.md sections 1.52–1.55). `MineLayer::clock_seconds()` is the `now`:
  `tick(dt)` advances it (negative spans clamped) without counting as a plot's turn. The layer keeps a
  list of task plots and, every tick, hands each one `tick(dt)` and then settles it — again without
  counting them in what `tick()` returns, because a task block is scenery. The window and the held time
  travel in the TASK chunk's version 2 as **remaining seconds** + amount per delivery (docs/SAVE.md,
  docs/GAME_DESIGN.md section 1.55): the save is written at `layer.clock_seconds()`, but the load's clock
  starts at 0, so `remaining` keeps its exact life and no clock is stored. A load restores the container
  first, then the task counts and live state, then settles every task plot - a save taken with the hold
  just satisfied completes on load, consuming its items as it would have in play.

All component functions take `(const void* shared, void* data, ...)`: `data` is the instance bytes and
is the function's self. No function table crosses an ABI with STL types; the structures are
standard-layout and trivially copyable.

The game calls a component through the type's **active** table:

```cpp
const auto* fns = static_cast<const ContainerFunctions*>(
    object.interface(type.type_id, content::kContainerInterface));
if (fns != nullptr) fns->add(shared, data, item, count);
```

## regist and link timing

- **codetab merge/link happens before game code.** The launcher merges tables, resolves symbols and
  relocations, and only then calls `mine_game_main`. Function tables in data sections already point at
  the winning implementation.
- **regist** is the game's startup step: built-in components/tile types are registered in the shared
  `RuntimeContext`; mod `on_load` runs before content definitions are built, registers custom types or
  overrides active interface tables.
- **link_drag** is the per-object link: it points a drag component's data at its own container
  component. Call it after construction and again after an override that changed the container table.
- **load** is the save/content step: build archetypes from the registered types and the per-content
  shared values, allocate `ObjectData`, then restore instance state.

## Custom content and mods

`TileDefinition::tile_type` selects a registered tile type by name. Empty means the built-in mapping
from `ContentKind`. A tile type names its components; custom components are zero-initialized and then
constructed by their registered callback. A tile type may carry one shared pointer per component
(`TileTypeDesc::shared`), so a mod can register variants with different immutable parameters.

Native modules use the version-2 `MineModApi` (`include/mine/mod_api.h`) and the C descriptors in
`include/mine/runtime/mod_component_api.h`:

- `register_component` / `find_component`
- `register_tile_type` / `find_tile_type`
- `override_component_interface(type_id, interface_id, expected_version, replacement, previous)`

Overrides replace the **active table** only. The previous table is returned for chaining. They must
happen during `on_load` (regist); all existing and future objects see the active table because objects
never cache a function pointer. Reload restores the built-in tables, unloads the module, and lets the new
load re-register/override.

Custom component state is saved in the optional `CMPT` chunk: plot serial, component type id, schema
version, size and the component's own `save` bytes; `load` consumes the bytes after the plot is
restored. Old saves without `CMPT` load unchanged; missing/wrong-version/incompatible custom state is
refused instead of guessed.

## Built-in tiles

```
floor      Preview + Building
ore        Preview + Building
warehouse  Container + Click + Drag + Preview + Building
drill      Ticker + Preview + Building + Drill
task_block Task + Container + Click + Drag + Preview + Building
structure  Preview + Building
machine    Ticker + Preview + Building
```

The task block's chain is content data (`tasks::`, docs/MODS.md): the runtime resolves each step's item
name to an id once, and `settle_tasks()` consumes a satisfied step's items out of the block's own
container and advances the chain. A **rate** step is settled the same way, once its window's measured
deliveries reach what the step needs; the runtime reports at build time a step whose count no container
of that content could ever hold, so it never silently passes (docs/GAME_DESIGN.md sections 1.52 and
1.53). Its click is what opens the task panel. A content entry (the game's own or a pack's) selects one
of these by name with `tile_type:"task_block"` (docs/MODS.md).

The built-in tables are registered by `runtime::content::register_builtin_content`; the game's
`TileRuntimeLibrary` maps content definitions to shared records and archetypes. A custom tile can reuse
any built-in/custom component and add its own function table.

## Files

- `include/mine/runtime/`: component registry, archetypes, object data, tile types, runtime context.
- `include/mine/runtime/content/`: stable ids, built-in C data/function tables, registration.
- `include/mine/runtime/mod_component_api.h`: the C surface for native mods.
- `src/types/tile_runtime.cpp`: content definition -> shared -> archetype/tile construction.
- `src/types/tile.cpp`: plot core data + object data.
- `src/game_save.cpp`: PLOT/ENTT/BUIL/UNLK/RSCH + optional CMPT.
