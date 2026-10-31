#include <mefemma/scenes/open_world_scene.h>

#include <t2d/core/log.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <string>
#include <utility>

namespace mefemma {
namespace {

constexpr u32 kBackground = rgba(14, 16, 22);
constexpr u32 kChunkEdge = rgba(58, 66, 84);
constexpr u32 kChunkLabel = rgba(120, 130, 150);
constexpr u32 kAccent = rgba(255, 196, 84);
constexpr u32 kText = rgba(232, 234, 240);
constexpr u32 kDim = rgba(148, 154, 170);
constexpr u32 kBad = rgba(240, 120, 110);
constexpr u32 kPanel = rgba(14, 16, 21, 210);
constexpr u32 kPanelEdge = rgba(56, 62, 76);
constexpr u32 kPointer = rgba(255, 216, 120);

/// The two test chunks are 32 cells square; one chunk is this many world units across.
constexpr f32 kChunkWorld = static_cast<f32>(t2d::WorldChunk::kSize) * OpenWorldScene::kTileSize;

/// One game cell's uv rectangle in assets/textures/test_tiles.png (two 16x16 cells side by side).
[[nodiscard]] constexpr Aabb2 tile_uv(t2d::TileId id) {
    const f32 x = id == OpenWorldScene::kTileB ? 0.5f : 0.0f;
    return Aabb2{Vec2{x, 0.0f}, Vec2{x + 0.5f, 1.0f}};
}

/// The working directory the game was started in: the default home of the open-world store.
[[nodiscard]] std::string default_world_file() {
    std::error_code code;
    const std::filesystem::path current = std::filesystem::current_path(code);
    if (code) return "mefemma_openworld.t2dk";
    return (current / "mefemma_openworld.t2dk").string();
}

template <class... Args>
[[nodiscard]] std::string format_localized(std::string_view pattern, Args&&... args) {
    return std::vformat(pattern, std::make_format_args(args...));
}

} // namespace

std::unique_ptr<Scene> make_open_world_scene(std::string world_file, t2d::ChunkKey content_origin) {
    return std::make_unique<OpenWorldScene>(std::move(world_file), content_origin);
}

OpenWorldScene::OpenWorldScene(std::string world_file, t2d::ChunkKey content_origin)
    : world_file_(world_file.empty() ? default_world_file() : std::move(world_file)),
      content_origin_(content_origin),
      world_(make_config()) {
    // The generator is content and needs the content origin; the world only ever calls it with an
    // ABSOLUTE key, which is what makes the same bytes come back at any origin.
    world_.set_generator([this](const t2d::ChunkKey& key, t2d::WorldChunk& chunk) { generate_chunk(key, chunk); });
    // Put the relative zero on the content before anything streams: the two test chunks are then at
    // relative (0,0) and (1,0) even when the absolute origin is near the limit, while generation and
    // the store keep the absolute keys. The shift is huge in absolute cells but no chunk is resident
    // yet, so nothing moves or reloads.
    (void)world_.set_origin(content_origin_);
    // One cell is 16 pixels; a chunk is 512. Zoom out only far enough that a 4K window still shows a
    // manageable number of cells, zoom in far enough to inspect one tile.
    camera_.set_zoom_limits(0.5f, 4.0f);
    camera_.look_at(t2d::Vec2{kChunkWorld * 0.5f, kChunkWorld * 0.5f});
}

t2d::OpenWorldConfig OpenWorldScene::make_config() {
    t2d::OpenWorldConfig config;
    config.tile_size = kTileSize;
    config.layers = 1;
    config.seed = 0xC0FFEEull;   // fixed: the test content must be identical on every run
    config.load_margin_chunks = 1;
    config.unload_margin_chunks = 2;
    // No soft cap: two test chunks are the content, and the scene is about streaming, not budgeting.
    config.max_loaded_chunks = 0;
    return config;
}

void OpenWorldScene::generate_chunk(const t2d::ChunkKey& key, t2d::WorldChunk& chunk) const {
    t2d::Span<t2d::TileId> cells = chunk.tiles(0);
    if (cells.size() != t2d::WorldChunk::kCellCount) return;
    constexpr i32 kSize = t2d::WorldChunk::kSize;
    const t2d::ChunkKey right_of_content{content_origin_.x + 1, content_origin_.y};

    if (key == content_origin_) {
        // The content origin chunk: tile A with a B diagonal every eight cells - one texture with the
        // other visible through it, and a pattern a streamed seam or a lost chunk would make obvious.
        for (i32 y = 0; y < kSize; ++y) {
            for (i32 x = 0; x < kSize; ++x) {
                const bool accent = ((x + y) % 8) == 0;
                cells[static_cast<usize>(y) * static_cast<usize>(kSize) + static_cast<usize>(x)] =
                    accent ? kTileB : kTileA;
            }
        }
    } else if (key == right_of_content) {
        // The chunk to its right: tile B with 2x2 blocks of A: the other texture, and a pattern that
        // tests the atlas' right-hand cell rather than the left one by accident.
        for (i32 y = 0; y < kSize; ++y) {
            for (i32 x = 0; x < kSize; ++x) {
                const bool accent = (((x / 2) + (y / 2)) % 2) == 0;
                cells[static_cast<usize>(y) * static_cast<usize>(kSize) + static_cast<usize>(x)] =
                    accent ? kTileA : kTileB;
            }
        }
    }
    // Every other absolute chunk is left empty: the generator says what the world is, not the scene.
}

void OpenWorldScene::on_enter(GameServices& services) {
    camera_.set_viewport(services.viewport());
    if (!framed_) {
        frame_camera();
        framed_ = true;
    }
    if (!store_attempted_) {
        store_attempted_ = true;
        open_store();
    }
    if (store_.has_value()) world_.set_store(store_->loader(), store_->saver());
    world_.update(camera_);
    T2D_INFO("scene: openworld ({} test chunk(s) at absolute chunk {},{}, {} loaded, store '{}')", kTestChunks,
             content_origin_.x, content_origin_.y, loaded_chunk_count(), world_file_);
}

void OpenWorldScene::on_exit(GameServices& services) {
    (void)services;
    (void)save();   // leaving the scene saves; a failure is reported by the HUD/log, never by a crash
}

void OpenWorldScene::open_store() {
    std::string error;
    std::optional<t2d::ChunkStore> opened = t2d::ChunkStore::open(world_file_, &error);
    if (!opened.has_value()) {
        store_error_ = error.empty() ? std::string("the store file could not be opened") : std::move(error);
        T2D_WARN("scene: openworld store '{}': {}", world_file_, store_error_);
        return;
    }
    store_.emplace(std::move(*opened));
    T2D_INFO("scene: openworld store '{}' ({} chunk(s))", world_file_, store_->chunk_count());
}

void OpenWorldScene::frame_camera() {
    const t2d::Vec2 view = camera_.viewport();
    // Both chunks side by side: 1024 x 512 world units, with a readable margin on every side.
    constexpr f32 kWorldWidth = 2.0f * kChunkWorld;
    constexpr f32 kWorldHeight = kChunkWorld;
    constexpr f32 kMargin = 24.0f;
    const f32 fit_x = (view.x - 2.0f * kMargin) / kWorldWidth;
    const f32 fit_y = (view.y - 2.0f * kMargin) / kWorldHeight;
    const f32 fit = std::min(fit_x, fit_y);
    camera_.set_zoom(fit > 0.0f ? fit : 1.0f);
    camera_.look_at(t2d::Vec2{kWorldWidth * 0.5f, kWorldHeight * 0.5f});
}

void OpenWorldScene::recenter_camera() {
    // A floating origin: the world moves its zero to the chunk the camera is in and says how far it
    // moved, so the camera follows by exactly that shift and the screen does not jump.
    const t2d::OpenWorld::OriginShift shift = world_.recenter(camera_);
    camera_.move(shift.position_delta(kTileSize));
    T2D_INFO("scene: openworld origin -> absolute chunk {},{} (shift {},{})", world_.origin().x, world_.origin().y,
             shift.x, shift.y);
}

void OpenWorldScene::move_camera(GameServices& services, f32 delta_seconds) {
    const InputState& input = services.input();
    constexpr f32 kPanPixelsPerSecond = 720.0f;
    // Arrow keys and W/A/D pan. S is the save key here, so downward pan is the Down arrow only.
    t2d::Vec2 look{};
    if (input.key_down(Key::Left) || input.key_down(Key::A)) look.x -= 1.0f;
    if (input.key_down(Key::Right) || input.key_down(Key::D)) look.x += 1.0f;
    if (input.key_down(Key::Up) || input.key_down(Key::W)) look.y -= 1.0f;
    if (input.key_down(Key::Down)) look.y += 1.0f;
    if (look.x != 0.0f || look.y != 0.0f) {
        const f32 step = kPanPixelsPerSecond * delta_seconds;
        camera_.pan(t2d::Vec2{-look.x * step, -look.y * step});
    }
    if (input.mouse_down(MouseButton::Middle)) {
        camera_.pan(t2d::Vec2{input.mouse_delta_x(), input.mouse_delta_y()});
    }
    if (input.scroll_y() != 0.0f) {
        camera_.zoom_at(t2d::Vec2{input.mouse_x(), input.mouse_y()},
                        input.scroll_y() > 0.0f ? 1.15f : (1.0f / 1.15f));
    }
    if (input.key_pressed(Key::Equal) || input.key_pressed(Key::KpAdd)) {
        camera_.zoom_at(services.viewport() * 0.5f, 1.25f);
    }
    if (input.key_pressed(Key::Minus) || input.key_pressed(Key::KpSubtract)) {
        camera_.zoom_at(services.viewport() * 0.5f, 0.8f);
    }
}

void OpenWorldScene::update(GameServices& services, f32 delta_seconds) {
    camera_.set_viewport(services.viewport());
    move_camera(services, delta_seconds);
    const InputState& input = services.input();
    if (input.key_pressed(Key::C)) recenter_camera();

    // Always after the camera has moved and before anything reads the world: the view defines what is
    // resident this frame.
    world_.update(camera_);

    pointer_inside_ = input.cursor_inside_window();
    pointer_world_ = camera_.world_of(t2d::Vec2{input.mouse_x(), input.mouse_y()});
    pointer_cell_x_ = t2d::floor_to_i32(pointer_world_.x / kTileSize);
    pointer_cell_y_ = t2d::floor_to_i32(pointer_world_.y / kTileSize);

    if (pointer_inside_) {
        if (input.mouse_pressed(MouseButton::Left)) cycle_pointer_cell();
        if (input.key_pressed(Key::D1) || input.key_pressed(Key::Kp1)) {
            (void)place_tile(pointer_cell_x_, pointer_cell_y_, kTileA);
        } else if (input.key_pressed(Key::D2) || input.key_pressed(Key::Kp2)) {
            (void)place_tile(pointer_cell_x_, pointer_cell_y_, kTileB);
        } else if (input.key_pressed(Key::D0) || input.key_pressed(Key::Kp0)) {
            (void)place_tile(pointer_cell_x_, pointer_cell_y_, t2d::kEmptyTile);
        }
    }

    if (input.key_pressed(Key::S)) (void)save();
    if (input.key_pressed(Key::Escape)) {
        (void)save();
        services.pop_scene();
    }
}

void OpenWorldScene::cycle_pointer_cell() {
    const t2d::TileId current = tile_at(pointer_cell_x_, pointer_cell_y_);
    const t2d::TileId next = (current == kTileA) ? kTileB : ((current == kTileB) ? t2d::kEmptyTile : kTileA);
    (void)place_tile(pointer_cell_x_, pointer_cell_y_, next);
}

t2d::TileId OpenWorldScene::tile_at(i32 cell_x, i32 cell_y) const { return world_.at(cell_x, cell_y); }

bool OpenWorldScene::place_tile(i32 cell_x, i32 cell_y, t2d::TileId id) {
    if (!world_.set(cell_x, cell_y, id)) return false;
    // The new content is not in the store yet; the HUD says so until S (or leaving) saves it.
    save_state_ = SaveState::Never;
    save_error_.clear();
    return true;
}

bool OpenWorldScene::save() {
    if (!store_.has_value()) {
        save_state_ = SaveState::Failed;
        save_error_ = store_error_.empty() ? std::string("no store was opened") : store_error_;
        T2D_WARN("scene: openworld save failed: {}", save_error_);
        return false;
    }
    std::string error;
    const bool world_ok = world_.flush();
    const bool store_ok = store_->flush(true, &error);
    if (!world_ok || !store_ok) {
        save_state_ = SaveState::Failed;
        save_error_ = error.empty() ? std::string("a dirty chunk could not be written") : std::move(error);
        T2D_WARN("scene: openworld save failed: {}", save_error_);
        return false;
    }
    save_state_ = SaveState::Saved;
    save_error_.clear();
    T2D_INFO("scene: openworld saved ({} chunk(s) in '{}')", store_->chunk_count(), world_file_);
    return true;
}

t2d::ChunkKey OpenWorldScene::content_origin() const { return content_origin_; }
t2d::ChunkKey OpenWorldScene::origin() const { return world_.origin(); }
t2d::usize OpenWorldScene::loaded_chunk_count() const { return world_.resident_chunk_count(); }
t2d::usize OpenWorldScene::dirty_chunk_count() const { return world_.dirty_chunk_count(); }
t2d::usize OpenWorldScene::store_chunk_count() const { return store_.has_value() ? store_->chunk_count() : 0; }
bool OpenWorldScene::store_open() const { return store_.has_value(); }
const std::string& OpenWorldScene::store_error() const { return store_error_; }
const std::string& OpenWorldScene::world_file() const { return world_file_; }
const t2d::Camera2D& OpenWorldScene::camera() const { return camera_; }

void OpenWorldScene::draw(GameServices& services, DrawList& out) const {
    const t2d::Vec2 view = services.viewport();
    out.rect(Aabb2{Vec2{0.0f, 0.0f}, view}, kBackground);
    draw_cells(out);
    draw_chunks(services, out);
    draw_pointer(out);
    draw_hud(services, out);
}

void OpenWorldScene::draw_cells(DrawList& out) const {
    const t2d::Aabb2 view = camera_.visible_world();
    const f32 width = view.max.x - view.min.x;
    const f32 height = view.max.y - view.min.y;
    if (!(width > 0.0f) || !(height > 0.0f)) return;

    const i32 first_x = t2d::floor_to_i32(view.min.x / kTileSize);
    const i32 last_x = t2d::floor_to_i32(view.max.x / kTileSize);
    const i32 first_y = t2d::floor_to_i32(view.min.y / kTileSize);
    const i32 last_y = t2d::floor_to_i32(view.max.y / kTileSize);
    // The zoom limits already bound this; the guard is for a camera a caller built by hand.
    constexpr i32 kMaxCellsPerAxis = 1024;
    if (last_x - first_x + 1 > kMaxCellsPerAxis || last_y - first_y + 1 > kMaxCellsPerAxis) return;

    for (i32 y = first_y; y <= last_y; ++y) {
        for (i32 x = first_x; x <= last_x; ++x) {
            const t2d::TileId id = world_.at(x, y);
            if (id == t2d::kEmptyTile) continue;
            const t2d::Vec2 min = camera_.screen_of(
                t2d::Vec2{static_cast<f32>(x) * kTileSize, static_cast<f32>(y) * kTileSize});
            const t2d::Vec2 max = camera_.screen_of(
                t2d::Vec2{static_cast<f32>(x + 1) * kTileSize, static_cast<f32>(y + 1) * kTileSize});
            out.sprite("test_tiles", Aabb2{min, max}, tile_uv(id));
        }
    }
}

void OpenWorldScene::draw_chunks(GameServices& services, DrawList& out) const {
    const t2d::Aabb2 view = camera_.visible_world();
    for (const t2d::ChunkCoord& coord : world_.chunks_in(view, 0)) {
        if (!world_.is_loaded(coord)) continue;
        const t2d::Vec2 min =
            camera_.screen_of(t2d::Vec2{static_cast<f32>(coord.x) * kChunkWorld,
                                        static_cast<f32>(coord.y) * kChunkWorld});
        const t2d::Vec2 max =
            camera_.screen_of(t2d::Vec2{static_cast<f32>(coord.x + 1) * kChunkWorld,
                                        static_cast<f32>(coord.y + 1) * kChunkWorld});
        out.rect_outline(Aabb2{min, max}, 1.0f, kChunkEdge);
        // The label is a relative chunk coordinate: origin() is on the status line, so the absolute
        // identity is always derivable without putting an absolute number into f32 geometry.
        out.text(format_localized(services.text("ow.chunk"), coord.x, coord.y),
                 t2d::Vec2{min.x + 6.0f, min.y + 4.0f}, 11, kChunkLabel);
    }
}

void OpenWorldScene::draw_pointer(DrawList& out) const {
    if (!pointer_inside_) return;
    const t2d::Vec2 min = camera_.screen_of(
        t2d::Vec2{static_cast<f32>(pointer_cell_x_) * kTileSize, static_cast<f32>(pointer_cell_y_) * kTileSize});
    const t2d::Vec2 max = camera_.screen_of(t2d::Vec2{static_cast<f32>(pointer_cell_x_ + 1) * kTileSize,
                                                      static_cast<f32>(pointer_cell_y_ + 1) * kTileSize});
    out.rect_outline(Aabb2{min, max}, 2.0f, kPointer);
}

void OpenWorldScene::draw_hud(GameServices& services, DrawList& out) const {
    const t2d::Vec2 view = services.viewport();

    // The scene's chrome: what this place is, the two test tiles, and the keys it answers.
    const Aabb2 panel{t2d::Vec2{16.0f, 16.0f}, t2d::Vec2{16.0f + 820.0f, 16.0f + 124.0f}};
    out.rect(panel, kPanel);
    out.rect_outline(panel, 1.0f, kPanelEdge);
    out.text(std::string(services.text("ow.title")), t2d::Vec2{28.0f, 24.0f}, 22, kAccent);
    out.text(std::string(services.text("ow.note")), t2d::Vec2{28.0f, 54.0f}, 15, kDim);
    out.text(std::string(services.text("ow.hint")), t2d::Vec2{28.0f, 82.0f}, 13, kText);

    // The state lines along the bottom left: the cell under the pointer, the streaming numbers and the
    // floating origin, then the store and whether the last save reached it. They sit on their own panel:
    // the test pattern is deliberately loud, and a status line that disappears into it is not a status.
    const Aabb2 status{Vec2{16.0f, view.y - 116.0f}, Vec2{16.0f + 820.0f, view.y - 16.0f}};
    out.rect(status, kPanel);
    out.rect_outline(status, 1.0f, kPanelEdge);
    std::string cell_line;
    if (pointer_inside_) {
        const t2d::TileId id = tile_at(pointer_cell_x_, pointer_cell_y_);
        std::string_view name = services.text("ow.tile.empty");
        if (id == kTileA) name = services.text("ow.tile.a");
        else if (id == kTileB) name = services.text("ow.tile.b");
        cell_line = format_localized(services.text("ow.cell"), pointer_cell_x_, pointer_cell_y_, name);
    } else {
        cell_line = std::string(services.text("ow.pointer.outside"));
    }
    out.text(cell_line, t2d::Vec2{28.0f, view.y - 104.0f}, 13, kText);

    const std::string chunk_line = format_localized(services.text("ow.chunks"), loaded_chunk_count(),
                                                    dirty_chunk_count(), origin().x, origin().y);
    out.text(chunk_line, t2d::Vec2{28.0f, view.y - 82.0f}, 13, kDim);

    const std::string store_line = store_.has_value()
                                       ? format_localized(services.text("ow.store"), world_file_)
                                       : format_localized(services.text("ow.store.error"), store_error_);
    out.text(store_line, t2d::Vec2{28.0f, view.y - 60.0f}, 13, store_.has_value() ? kDim : kBad);

    std::string save_line;
    switch (save_state_) {
        case SaveState::Never:
            save_line = std::string(services.text("ow.save.none"));
            break;
        case SaveState::Saved:
            save_line = format_localized(services.text("ow.save.ok"), store_chunk_count());
            break;
        case SaveState::Failed:
            save_line = save_error_.empty()
                            ? std::string(services.text("ow.save.failed"))
                            : format_localized(services.text("ow.save.failed.reason"), save_error_);
            break;
    }
    out.text(save_line, t2d::Vec2{28.0f, view.y - 38.0f}, 13, save_state_ == SaveState::Failed ? kBad : kDim);
}

} // namespace mefemma
