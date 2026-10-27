#include <mine/types/drill.h>

#include <mine/types/tile_definition.h>

#include <cmath>

#include <mine/world.h>

namespace mine::types {

f32 Drill::spin_seconds() const {
    const TileDefinition* record = definition();
    return record != nullptr ? record->spin : 0.0f;
}

t2d::Vec2 Drill::spin_origin() const {
    const TileDefinition* record = definition();
    return record != nullptr ? record->spin_origin : t2d::Vec2{0.5f, 0.5f};
}

f32 Drill::spin_angle(f32 seconds) const {
    const f32 turn = spin_seconds();
    if (!(turn > 0.0f) || !(seconds > 0.0f)) return 0.0f;
    constexpr f32 kTau = 6.283185307179586f;
    const f32 turns = seconds / turn;
    return (turns - std::floor(turns)) * kTau;
}


TilePreview Drill::get_preview(t2d::f32 seconds) const {
    // The base first, answered the way any scene plot answers it (types/scene_tile.cpp): the entry's own
    // "image", if it names one.
    TilePreview preview = SceneTile::get_preview(seconds);
    // The top comes over it only when there is a base for it to turn on. A drill whose entry names no
    // "image" is drawn as the fallback colour (app.cpp), so its top is not a part of any picture.
    if (preview.empty() || definition_ == nullptr || definition_->top_image.empty()) return preview;
    PreviewImage top;
    top.part = "top";
    top.mirrored = mirrored();
    top.radians = spin_angle(seconds);
    top.pivot = spin_origin();
    preview.images.push_back(top);
    return preview;
}

namespace {

/// The sides a drill throws to, in the order it uses them: right, down, left, up. Fixed, so the same
/// mine with the same clock produces the same picture twice.


} // namespace

Drill::Drill(ContentKind kind, ContentId id, i32 layer, GridPos anchor, i32 width, i32 height)
    : EntityTile(kind, id, layer, anchor, width, height) {
    // Its own dice, seeded from what it is and where it stands: the same map throws the same way every
    // time it is built, and two drills of the same entry on one layer do not throw alike.
    const u64 where = static_cast<u64>(static_cast<u32>(anchor.x)) << 32u |
                      static_cast<u64>(static_cast<u32>(anchor.y));
    rng_.seed(where ^ (static_cast<u64>(id) * 0x9E3779B97F4A7C15ull));
}

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
    // **Any** direction at all, not the four sides and not the next one round: the designer asked for a
    // random direction and then made it exact - 360 degrees - so a drop goes out at a random **angle**, one
    // cell away from the drill's middle, landing anywhere on the circle of neighbours rather than walking
    // clockwise. The distance is still one cell: what was asked to be random is the direction.
    //
    // The stream is the drill's own and is seeded from what it is and where it stands (drill.h), so the same
    // map throws the same way every time it is built - the dice of a layer are reproducible
    // (docs/GAME_DESIGN.md sections 1.21, 1.26) - and a drill does not draw from the layer's stream, where
    // its own ticks would move the map's dice.
    constexpr f32 kTau = 6.283185307179586f;
    const f32 angle = kTau * rng_.next_float();
    // Kept on the map when the drill stands at its edge: a drop is a thing lying on the ground, and there
    // is no ground outside the layer.
    const t2d::Vec2 where{
        t2d::clamp(static_cast<f32>(anchor().x) + 0.5f + std::cos(angle), 0.0f,
                   static_cast<f32>(layer->width())),
        t2d::clamp(static_cast<f32>(anchor().y) + 0.5f + std::sin(angle), 0.0f,
                   static_cast<f32>(layer->height())),
    };
    // The thing on the ground **is** the item its ore drops: same name, same picture, same id - made out
    // of the item rather than declared beside it (docs/GAME_DESIGN.md sections 1.23, 1.25). That is also
    // why carrying one into a warehouse stores the item, and why a price can be paid with it. It goes in
    // through the layer's drop(), so what lands next to a pile of the same item becomes part of it
    // (section 1.43).
    (void)layer->drop(ContentKind::Item, job_.drop_id, where);
    ++thrown_;
}

} // namespace mine::types
