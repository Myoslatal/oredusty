#include <mine/types/drill.h>

#include <mine/types/drop.h>
#include <mine/world.h>

#include <memory>

namespace mine::types {

namespace {

/// The sides a drill throws to, in the order it uses them: right, down, left, up. Fixed, so the same
/// mine with the same clock produces the same picture twice.
constexpr t2d::Vec2 kThrowSides[4] = {{1.0f, 0.0f}, {0.0f, 1.0f}, {-1.0f, 0.0f}, {0.0f, -1.0f}};

} // namespace

Drill::Drill(ContentKind kind, ContentId id, i32 layer, GridPos anchor, i32 width, i32 height)
    : EntityTile(kind, id, layer, anchor, width, height) {}

void Drill::set_job(DrillJob job) {
    job_ = std::move(job);
    // The cadence *is* the job's period: what a drill is, is how long a drop takes it
    // (types/update_cadence.h).
    set_period_seconds(job_.period_seconds);
}

void Drill::on_update(f32 elapsed_seconds) {
    if (!ready() || owner() == nullptr) return;
    // The cadence ran it, so at least one drop is due; a frame that swallowed several periods is worth
    // several, because a hitch must not change what the mine produces (types/entity_tile.h).
    usize count = static_cast<usize>(elapsed_seconds / job_.period_seconds);
    if (count == 0) count = 1;
    for (usize index = 0; index < count; ++index) throw_one();
}

void Drill::throw_one() {
    MineLayer* layer = owner();
    if (layer == nullptr) return;
    const t2d::Vec2 side = kThrowSides[thrown_ % 4u];
    // The middle of the neighbouring cell: one cell away from the drill's own middle, and kept on the
    // map when the drill stands at its edge.
    const t2d::Vec2 where{
        t2d::clamp(static_cast<f32>(anchor().x) + 0.5f + side.x, 0.0f, static_cast<f32>(layer->width())),
        t2d::clamp(static_cast<f32>(anchor().y) + 0.5f + side.y, 0.0f, static_cast<f32>(layer->height())),
    };
    layer->adopt(std::make_unique<Drop>(ContentKind::Drop, job_.drop_id, where));
    ++thrown_;
}

} // namespace mine::types
