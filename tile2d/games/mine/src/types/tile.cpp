#include <mine/types/tile.h>

#include <mine/world.h>

#include <mine/types/building.h>
#include <mine/types/tile_definition.h>
#include <mine/types/tile_runtime.h>

#include <mine/runtime/content/builtin.h>
#include <mine/runtime/object_data.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <utility>

namespace mine::types {
namespace {

using t2d::u32;
using t2d::u64;

[[nodiscard]] u32 as_u32(usize value) {
    return static_cast<u32>(std::min<usize>(value, std::numeric_limits<u32>::max()));
}

constexpr f32 kTau = 6.283185307179586f;

} // namespace

Tile::Tile(std::shared_ptr<const TileRuntimeLibrary> library,
           std::shared_ptr<const RuntimeTileDefinition> runtime_definition, ContentId id, i32 layer,
           GridPos anchor, i32 width, i32 height)
    : library_(std::move(library)), runtime_definition_(std::move(runtime_definition)),
      tile_type_(runtime_definition_->tile_type), kind_(runtime_definition_->definition.kind), id_(id),
      layer_(layer),
      cells_(footprint(anchor, width, height)),
      random_reverse_(runtime_definition_->definition.random_reverse) {
    if (runtime_definition_->archetype != nullptr) {
        object_ = std::make_unique<runtime::ObjectData>(*runtime_definition_->archetype);
    }
    // The drill's own dice, seeded from what it is and where it stands: a layer built from the same data
    // throws the same way, and a drill's ticks never draw from the layer's stream.
    if (runtime::content::DrillData* drill = static_cast<runtime::content::DrillData*>(
            const_cast<void*>(instance(runtime::content::kDrillComponent)));
        drill != nullptr) {
        const u64 where = static_cast<u64>(static_cast<u32>(anchor.x)) << 32u |
                          static_cast<u64>(static_cast<u32>(anchor.y));
        drill->rng = where ^ (static_cast<u64>(id) * 0x9E3779B97F4A7C15ull);
    }
}

Tile::~Tile() = default;

std::string Tile::name(const ContentRegistry& registry) const {
    const ContentEntry* entry = registry.find(kind_, id_);
    if (entry == nullptr) return std::format("#{}", id_);
    return entry->name;
}

const TileDefinition* Tile::definition() const {
    return runtime_definition_ != nullptr ? &runtime_definition_->definition : nullptr;
}

const Building* Tile::building() const {
    const TileDefinition* record = definition();
    if (record == nullptr || record->building.empty()) return nullptr;
    return &record->building;
}

bool Tile::overlaps(const Tile& other) const {
    if (layer_ != other.layer_) return false;
    return cells_.x < other.cells_.right() && other.cells_.x < cells_.right() &&
           cells_.y < other.cells_.bottom() && other.cells_.y < cells_.bottom();
}

bool Tile::within(i32 map_width, i32 map_height) const {
    return cells_.x >= 0 && cells_.y >= 0 && cells_.right() <= map_width && cells_.bottom() <= map_height;
}

void Tile::randomise_mirror(t2d::Rng& rng) {
    mirrored_ = random_reverse_ && rng.chance(0.5f);
}

bool Tile::has_component(runtime::ComponentTypeId type_id) const {
    return object_ != nullptr && object_->archetype().find(type_id) != nullptr;
}

const void* Tile::instance(runtime::ComponentTypeId type_id) const {
    return object_ != nullptr ? object_->instance(type_id) : nullptr;
}

const void* Tile::shared(runtime::ComponentTypeId type_id) const {
    return object_ != nullptr ? object_->shared(type_id) : nullptr;
}

const void* Tile::interface(runtime::ComponentTypeId type_id, runtime::InterfaceId interface_id) const {
    return object_ != nullptr ? object_->interface(type_id, interface_id) : nullptr;
}

const runtime::content::ContainerFunctions* Tile::container_functions() const {
    return static_cast<const runtime::content::ContainerFunctions*>(
        interface(runtime::content::kContainerComponent, runtime::content::kContainerInterface));
}

const runtime::content::PreviewFunctions* Tile::preview_functions() const {
    return static_cast<const runtime::content::PreviewFunctions*>(
        interface(runtime::content::kPreviewComponent, runtime::content::kPreviewInterface));
}

const runtime::content::ClickFunctions* Tile::click_functions() const {
    return static_cast<const runtime::content::ClickFunctions*>(
        interface(runtime::content::kClickComponent, runtime::content::kClickInterface));
}

const runtime::content::DragFunctions* Tile::drag_functions() const {
    return static_cast<const runtime::content::DragFunctions*>(
        interface(runtime::content::kDragComponent, runtime::content::kDragInterface));
}

const runtime::content::TickerFunctions* Tile::ticker_functions() const {
    return static_cast<const runtime::content::TickerFunctions*>(
        interface(runtime::content::kTickerComponent, runtime::content::kTickerInterface));
}

const runtime::content::BuildingFunctions* Tile::building_functions() const {
    return static_cast<const runtime::content::BuildingFunctions*>(
        interface(runtime::content::kBuildingComponent, runtime::content::kBuildingInterface));
}

const runtime::content::DrillFunctions* Tile::drill_functions() const {
    return static_cast<const runtime::content::DrillFunctions*>(
        interface(runtime::content::kDrillComponent, runtime::content::kDrillInterface));
}

const runtime::content::TaskFunctions* Tile::task_functions() const {
    return static_cast<const runtime::content::TaskFunctions*>(
        interface(runtime::content::kTaskComponent, runtime::content::kTaskInterface));
}

bool Tile::has_container() const {
    return container_functions() != nullptr;
}
bool Tile::has_click() const {
    return click_functions() != nullptr;
}
bool Tile::has_drag() const {
    return drag_functions() != nullptr;
}
bool Tile::is_ticking() const {
    return ticker_functions() != nullptr;
}
bool Tile::is_drill() const {
    return has_component(runtime::content::kDrillComponent);
}

Container Tile::container() const {
    return Container(const_cast<void*>(instance(runtime::content::kContainerComponent)),
                     shared(runtime::content::kContainerComponent), container_functions());
}

usize Tile::capacity() const {
    return container().capacity();
}

Click Tile::click() const {
    return Click(const_cast<void*>(instance(runtime::content::kClickComponent)), click_functions());
}

bool Tile::on_click(GridPos cell) {
    if (!has_click()) return false;
    Click view = click();
    if (under_construction()) return view.receive(false);
    // A click on the plot opens (or closes) what it has to show. The state it shows is settled first: a
    // panel that opened a step behind what its container already holds would be a lie
    // (docs/GAME_DESIGN.md section 1.52).
    if (covers(cell)) run_tasks();
    return view.receive(covers(cell));
}

bool Tile::drag_accepts(ContentKind kind, ContentId id, usize count) const {
    const runtime::content::DragFunctions* functions = drag_functions();
    if (functions == nullptr || count == 0) return false;
    return functions->accepts(nullptr, const_cast<void*>(instance(runtime::content::kDragComponent)),
                              static_cast<u32>(kind), id, as_u32(count)) != 0;
}

bool Tile::drag_receive(ContentKind kind, ContentId id, usize count) {
    const runtime::content::DragFunctions* functions = drag_functions();
    if (functions == nullptr || count == 0) return false;
    const bool received =
        functions->receive(nullptr, const_cast<void*>(instance(runtime::content::kDragComponent)),
                           static_cast<u32>(kind), id, as_u32(count)) != 0;
    if (received) {
        // The window first, then the settle: a rate step measures what arrived, so the delivery has to
        // be on record before "is it enough?" is asked. Both at the layer's own clock, so a drag and the
        // tick that follows it agree about when the items came in (docs/GAME_DESIGN.md section 1.53).
        note_delivery(kind, id, count);
        run_tasks();
    }
    return received;
}

bool Tile::has_task() const {
    return task_functions() != nullptr;
}

Task Tile::task() const {
    return Task(const_cast<void*>(instance(runtime::content::kTaskComponent)),
                shared(runtime::content::kTaskComponent), task_functions());
}

void Tile::task_tick(f32 delta_seconds) {
    const runtime::content::TaskFunctions* functions = task_functions();
    if (functions == nullptr) return;
    functions->tick(shared(runtime::content::kTaskComponent),
                    const_cast<void*>(instance(runtime::content::kTaskComponent)), now_seconds(),
                    delta_seconds);
}

f32 Tile::now_seconds() const {
    return owner_ != nullptr ? owner_->clock_seconds() : 0.0f;
}

void Tile::note_delivery(ContentKind kind, ContentId id, usize count) {
    const runtime::content::TaskFunctions* functions = task_functions();
    if (functions == nullptr || count == 0) return;
    const auto amount = static_cast<u32>(std::min<usize>(count, std::numeric_limits<u32>::max()));
    functions->record(shared(runtime::content::kTaskComponent),
                      const_cast<void*>(instance(runtime::content::kTaskComponent)),
                      static_cast<u32>(kind), id, amount, now_seconds());
}

usize Tile::run_tasks() {
    if (object_ == nullptr || !has_task()) return 0;
    return runtime::content::settle_tasks(*object_, now_seconds());
}

bool Tile::set_task_completed(usize completed) {
    auto* data = static_cast<runtime::content::TaskData*>(
        const_cast<void*>(instance(runtime::content::kTaskComponent)));
    const runtime::content::TaskFunctions* functions = task_functions();
    if (data == nullptr || functions == nullptr) return false;
    const u32 total = functions->count(shared(runtime::content::kTaskComponent), data);
    data->completed = static_cast<u32>(std::min<usize>(completed, total));
    return true;
}

usize Tile::task_completed() const {
    const auto* data =
        static_cast<const runtime::content::TaskData*>(instance(runtime::content::kTaskComponent));
    return data != nullptr ? data->completed : 0;
}

bool Tile::refresh() {
    if (!dirty_ || under_construction()) return false;
    dirty_ = false;
    return true;
}

bool Tile::under_construction() const {
    const runtime::content::BuildingFunctions* functions = building_functions();
    if (functions == nullptr) return false;
    return functions->under_construction(shared(runtime::content::kBuildingComponent),
                                         const_cast<void*>(instance(runtime::content::kBuildingComponent))) !=
           0;
}

void Tile::set_under_construction(bool value) {
    const runtime::content::BuildingFunctions* functions = building_functions();
    if (functions == nullptr) return;
    functions->set_under_construction(shared(runtime::content::kBuildingComponent),
                                      const_cast<void*>(instance(runtime::content::kBuildingComponent)),
                                      value ? 1 : 0);
}

f32 Tile::period_seconds() const {
    const runtime::content::TickerFunctions* functions = ticker_functions();
    if (functions == nullptr) return 0.0f;
    return functions->period(nullptr, const_cast<void*>(instance(runtime::content::kTickerComponent)));
}

void Tile::set_period_seconds(f32 seconds) {
    auto* ticker = static_cast<runtime::content::TickerData*>(
        const_cast<void*>(instance(runtime::content::kTickerComponent)));
    if (ticker != nullptr) ticker->period = seconds > 0.0f ? seconds : 0.0f;
}

f32 Tile::since_last_update() const {
    const auto* ticker =
        static_cast<const runtime::content::TickerData*>(instance(runtime::content::kTickerComponent));
    return ticker != nullptr ? ticker->elapsed : 0.0f;
}

void Tile::set_since_last_update(f32 seconds) {
    auto* ticker = static_cast<runtime::content::TickerData*>(
        const_cast<void*>(instance(runtime::content::kTickerComponent)));
    if (ticker != nullptr) ticker->elapsed = seconds > 0.0f ? seconds : 0.0f;
}

std::optional<f32> Tile::advance(f32 delta_seconds) {
    const runtime::content::TickerFunctions* functions = ticker_functions();
    auto* ticker = static_cast<runtime::content::TickerData*>(
        const_cast<void*>(instance(runtime::content::kTickerComponent)));
    if (functions == nullptr || ticker == nullptr || under_construction()) return std::nullopt;
    const f32 before = ticker->elapsed;
    if (functions->advance(nullptr, ticker, delta_seconds) == 0) return std::nullopt;
    if (ticker->period > 0.0f) return before + delta_seconds - ticker->elapsed;
    ticker->elapsed = 0.0f;
    return delta_seconds;
}

bool Tile::ready() const {
    if (!is_drill()) return false;
    const runtime::content::DrillJob job_value = job();
    return job_value.drop_id != runtime::content::kNoContentId &&
           (job_value.period_seconds > 0.0f || (job_value.hardness > 0.0f && dig_power() > 0.0f));
}

f32 Tile::dig_power() const {
    const auto* shared_drill =
        static_cast<const runtime::content::DrillShared*>(shared(runtime::content::kDrillComponent));
    return shared_drill != nullptr ? shared_drill->dig_power : 0.0f;
}

runtime::content::DrillJob Tile::job() const {
    const auto* drill =
        static_cast<const runtime::content::DrillData*>(instance(runtime::content::kDrillComponent));
    return drill != nullptr ? drill->job : runtime::content::DrillJob{};
}

void Tile::set_job(runtime::content::DrillJob job_value) {
    auto* drill = static_cast<runtime::content::DrillData*>(
        const_cast<void*>(instance(runtime::content::kDrillComponent)));
    if (drill == nullptr) return;
    drill->job = job_value;
    set_period_seconds(job_value.period_seconds);
}

usize Tile::produced() const {
    const auto* drill =
        static_cast<const runtime::content::DrillData*>(instance(runtime::content::kDrillComponent));
    return drill != nullptr ? drill->produced : 0;
}

void Tile::set_produced(usize count) {
    auto* drill = static_cast<runtime::content::DrillData*>(
        const_cast<void*>(instance(runtime::content::kDrillComponent)));
    if (drill != nullptr) drill->produced = as_u32(count);
}

f32 Tile::spin_seconds() const {
    const TileDefinition* record = definition();
    return record != nullptr ? record->spin : 0.0f;
}

f32 Tile::spin_angle(f32 seconds) const {
    const f32 turn = spin_seconds();
    if (!(turn > 0.0f) || !(seconds > 0.0f)) return 0.0f;
    const f32 turns = seconds / turn;
    return (turns - std::floor(turns)) * kTau;
}

t2d::Vec2 Tile::spin_origin() const {
    const TileDefinition* record = definition();
    return record != nullptr ? record->spin_origin : t2d::Vec2{0.5f, 0.5f};
}

usize Tile::drill_tick(f32 elapsed_seconds) {
    const runtime::content::DrillFunctions* functions = drill_functions();
    if (functions == nullptr) return 0;
    return functions->tick(shared(runtime::content::kDrillComponent),
                           const_cast<void*>(instance(runtime::content::kDrillComponent)), elapsed_seconds);
}

u64 Tile::drill_rng() const {
    const auto* drill =
        static_cast<const runtime::content::DrillData*>(instance(runtime::content::kDrillComponent));
    return drill != nullptr ? drill->rng : 0;
}

u32 Tile::drill_side() const {
    const auto* drill =
        static_cast<const runtime::content::DrillData*>(instance(runtime::content::kDrillComponent));
    return drill != nullptr ? drill->side : 0;
}

TilePreview Tile::get_preview(f32 seconds) const {
    TilePreview preview;
    const runtime::content::PreviewFunctions* functions = preview_functions();
    const auto* shared_preview =
        static_cast<const runtime::content::PreviewShared*>(shared(runtime::content::kPreviewComponent));
    if (functions == nullptr || shared_preview == nullptr) return preview;
    // Mirroring is per plot, while the rest of the picture is a shared definition: hand the function a
    // copy with this plot's flag, so the table stays read-only.
    runtime::content::PreviewShared definition = *shared_preview;
    definition.mirrored = mirrored_ ? 1 : 0;
    const runtime::content::TilePreview raw = functions->get_preview(
        &definition, const_cast<void*>(instance(runtime::content::kPreviewComponent)), seconds);
    preview.images.reserve(raw.count);
    for (u32 index = 0; index < raw.count; ++index) {
        const runtime::content::PreviewPart& part = raw.parts[index];
        PreviewImage image;
        image.part = part.part != nullptr && std::string_view(part.part) == "top" ? "top" : "";
        image.mirrored = part.mirrored != 0;
        image.radians = part.radians;
        image.pivot = t2d::Vec2{part.pivot.x, part.pivot.y};
        preview.images.push_back(std::move(image));
    }
    return preview;
}

} // namespace mine::types
