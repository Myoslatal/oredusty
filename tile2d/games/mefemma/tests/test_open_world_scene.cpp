// The open-world test scene, driven by a fake GameServices: two test tiles in two test chunks, drawn
// through the DrawList, edited, saved into a real ChunkStore file, reopened, recentred and drawn again.
//
// Everything happens in a directory under the system temporary directory that this run alone owns; the
// store is a real file because "the bytes came back" is the property under test. No window, no GPU and
// no sleeps: a scene never needs any of them.
#include <mefemma/scenes/open_world_scene.h>

#include <support/test_support.h>

#include <t2d/sim/open_world.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace mefemma;

namespace {

namespace fs = std::filesystem;

/// A fake shell: what a scene asks of GameServices, without a window, a GPU, a font or a clock.
class FakeServices final : public GameServices {
public:
    [[nodiscard]] const InputState& input() const override { return input_; }
    [[nodiscard]] InputState& input_mutable() { return input_; }
    [[nodiscard]] const Settings& settings() const override { return settings_; }
    [[nodiscard]] const t2d::Locale& locale() const override { return locale_; }
    [[nodiscard]] std::string_view text(std::string_view id) const override { return id; }
    [[nodiscard]] t2d::TextMetrics measure_text(std::string_view value, t2d::u16 size_px) const override {
        t2d::TextMetrics metrics;
        metrics.width = static_cast<t2d::f32>(value.size()) * static_cast<t2d::f32>(size_px) * 0.5f;
        metrics.height = static_cast<t2d::f32>(size_px) * 1.2f;
        metrics.ascent = static_cast<t2d::f32>(size_px);
        metrics.descent = static_cast<t2d::f32>(size_px) * 0.2f;
        metrics.lines = 1;
        metrics.characters = value.size();
        return metrics;
    }
    [[nodiscard]] t2d::Language language() const override { return language_; }
    void set_language(t2d::Language language) override { language_ = language; }
    [[nodiscard]] t2d::Rng& random() override { return rng_; }
    [[nodiscard]] t2d::f32 elapsed_seconds() const override { return 0.0f; }
    [[nodiscard]] t2d::u64 frame() const override { return 0; }
    [[nodiscard]] t2d::Vec2 viewport() const override { return t2d::Vec2{1280.0f, 720.0f}; }

    void push_scene(std::unique_ptr<Scene> scene) override {
        pushed.push_back(scene != nullptr ? scene->name() : "");
    }
    void pop_scene() override { ++pops; }
    void replace_scene(std::unique_ptr<Scene> scene) override {
        replaced = scene != nullptr ? scene->name() : "";
    }
    void quit() override { quit_called = true; }

    std::vector<std::string> pushed;
    std::string replaced;
    int pops = 0;
    bool quit_called = false;

private:
    InputState input_{};
    Settings settings_{};
    t2d::Locale locale_{};
    t2d::Rng rng_{1};
    t2d::Language language_ = t2d::Language::English;
};

/// A directory under the system temporary directory that this run alone uses, removed when the test ends.
class ScratchDir {
public:
    ScratchDir() {
        static std::atomic<unsigned> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() /
                ("t2d_mefemma_openworld_" + std::to_string(stamp) + "_" +
                 std::to_string(counter.fetch_add(1)));
        std::error_code code;
        fs::create_directories(path_, code);
    }
    ~ScratchDir() {
        std::error_code code;
        fs::remove_all(path_, code);
    }
    T2D_NON_MOVABLE(ScratchDir);

    [[nodiscard]] const fs::path& path() const { return path_; }
    [[nodiscard]] std::string file(const std::string& name) const { return (path_ / name).string(); }

private:
    fs::path path_;
};

[[nodiscard]] bool has_text(const DrawList& list, std::string_view text) {
    for (const DrawCommand& command : list.commands()) {
        if (command.kind == DrawCommand::Kind::Text && command.text == text) return true;
    }
    return false;
}

[[nodiscard]] bool has_uv(const DrawList& list, const t2d::Aabb2& uv) {
    for (const DrawCommand& command : list.commands()) {
        if (command.kind != DrawCommand::Kind::Sprite || command.texture != "test_tiles") continue;
        if (command.uv == uv) return true;
    }
    return false;
}

[[nodiscard]] bool same_uv(const t2d::Aabb2& a, const t2d::Aabb2& b) {
    constexpr t2d::f32 kEpsilon = 1.0e-6f;
    return t2d::abs_f32(a.min.x - b.min.x) < kEpsilon && t2d::abs_f32(a.min.y - b.min.y) < kEpsilon &&
           t2d::abs_f32(a.max.x - b.max.x) < kEpsilon && t2d::abs_f32(a.max.y - b.max.y) < kEpsilon;
}

[[nodiscard]] constexpr t2d::Aabb2 uv_a() {
    return t2d::Aabb2{t2d::Vec2{0.0f, 0.0f}, t2d::Vec2{0.5f, 1.0f}};
}

[[nodiscard]] constexpr t2d::Aabb2 uv_b() {
    return t2d::Aabb2{t2d::Vec2{0.5f, 0.0f}, t2d::Vec2{1.0f, 1.0f}};
}

/// One frame with an empty input state: what a test that only wants the scene to stream looks like.
void step(OpenWorldScene& scene, FakeServices& services, t2d::f32 delta_seconds = 1.0f / 60.0f) {
    services.input_mutable().begin_frame();
    scene.update(services, delta_seconds);
}

void enter(OpenWorldScene& scene, FakeServices& services) {
    scene.on_enter(services);
    step(scene, services);
}

/// A key held for p frames, with a step a frame: panning a distance a test can predict.
void hold_key(OpenWorldScene& scene, FakeServices& services, Key key, int frames, t2d::f32 delta_seconds) {
    for (int index = 0; index < frames; ++index) {
        services.input_mutable().begin_frame();
        services.input_mutable().set_key(key, true);
        scene.update(services, delta_seconds);
        services.input_mutable().set_key(key, false);
    }
}

/// One tap of p key on a frame of its own.
void press_key(OpenWorldScene& scene, FakeServices& services, Key key) {
    services.input_mutable().begin_frame();
    services.input_mutable().set_key(key, true);
    scene.update(services, 1.0f / 60.0f);
    services.input_mutable().set_key(key, false);
}

/// A left click at p screen. The pointer stays inside for the frame the click happens in, which is what
/// a real pointer over the window looks like to the scene.
void click_at(OpenWorldScene& scene, FakeServices& services, t2d::Vec2 screen) {
    services.input_mutable().begin_frame();
    services.input_mutable().set_cursor_inside(true);
    services.input_mutable().set_mouse_position(screen.x, screen.y);
    services.input_mutable().set_mouse_button(MouseButton::Left, true);
    scene.update(services, 1.0f / 60.0f);
    services.input_mutable().set_mouse_button(MouseButton::Left, false);
}

[[nodiscard]] t2d::Vec2 cell_centre_screen(const OpenWorldScene& scene, t2d::i32 cell_x, t2d::i32 cell_y) {
    return scene.camera().screen_of(
        t2d::Vec2{(static_cast<t2d::f32>(cell_x) + 0.5f) * OpenWorldScene::kTileSize,
                  (static_cast<t2d::f32>(cell_y) + 0.5f) * OpenWorldScene::kTileSize});
}

/// The cells the scene's own visible range covers: the test counts what a correct draw_cells() must draw.
[[nodiscard]] std::vector<std::pair<t2d::i32, t2d::i32>> visible_cells(const OpenWorldScene& scene) {
    std::vector<std::pair<t2d::i32, t2d::i32>> cells;
    const t2d::Aabb2 view = scene.camera().visible_world();
    const t2d::f32 tile = OpenWorldScene::kTileSize;
    const t2d::i32 first_x = t2d::floor_to_i32(view.min.x / tile);
    const t2d::i32 last_x = t2d::floor_to_i32(view.max.x / tile);
    const t2d::i32 first_y = t2d::floor_to_i32(view.min.y / tile);
    const t2d::i32 last_y = t2d::floor_to_i32(view.max.y / tile);
    for (t2d::i32 y = first_y; y <= last_y; ++y) {
        for (t2d::i32 x = first_x; x <= last_x; ++x) {
            if (scene.tile_at(x, y) != t2d::kEmptyTile) cells.emplace_back(x, y);
        }
    }
    return cells;
}

} // namespace

T2D_TEST(the_default_store_path_is_the_game_file_in_the_working_directory) {
    OpenWorldScene scene;
    T2D_CHECK(scene.world_file().find("mefemma_openworld.t2dk") != std::string::npos);
    T2D_CHECK_FALSE(scene.world_file().empty());
}

T2D_TEST(two_test_chunks_generate_two_tile_patterns_and_nothing_is_saved_yet) {
    ScratchDir dir;
    const std::string path = dir.file("world.t2dk");
    OpenWorldScene scene(path);
    FakeServices services;
    enter(scene, services);

    // The content: chunk {0,0} is A with a B diagonal, chunk {1,0} is B with 2x2 blocks of A.
    T2D_CHECK_EQ(scene.tile_at(0, 0), OpenWorldScene::kTileB);
    T2D_CHECK_EQ(scene.tile_at(1, 1), OpenWorldScene::kTileA);
    T2D_CHECK_EQ(scene.tile_at(31, 31), OpenWorldScene::kTileA);
    T2D_CHECK_EQ(scene.tile_at(32, 0), OpenWorldScene::kTileA);
    T2D_CHECK_EQ(scene.tile_at(34, 0), OpenWorldScene::kTileB);
    T2D_CHECK_EQ(scene.tile_at(33, 0), OpenWorldScene::kTileA);
    // Nothing outside the two test chunks: the generator's content is the two chunks.
    T2D_CHECK_EQ(scene.tile_at(-1, 0), t2d::kEmptyTile);
    T2D_CHECK_EQ(scene.tile_at(0, -1), t2d::kEmptyTile);
    T2D_CHECK_EQ(scene.tile_at(64, 32), t2d::kEmptyTile);

    T2D_CHECK_GE(scene.loaded_chunk_count(), 2u);
    T2D_CHECK_EQ(scene.dirty_chunk_count(), 0u);
    // Generating a chunk is not an edit: a first look writes nothing to the store.
    T2D_CHECK_EQ(scene.store_chunk_count(), 0u);
    T2D_CHECK(fs::exists(path));

    DrawList list;
    scene.draw(services, list);
    T2D_CHECK(has_uv(list, uv_a()));
    T2D_CHECK(has_uv(list, uv_b()));
    T2D_CHECK(has_text(list, "ow.title"));
    T2D_CHECK(has_text(list, "ow.note"));
    T2D_CHECK(has_text(list, "ow.hint"));
    T2D_CHECK(has_text(list, "ow.pointer.outside"));
    T2D_CHECK(has_text(list, "ow.chunks"));
    T2D_CHECK(has_text(list, "ow.store"));
    T2D_CHECK(has_text(list, "ow.save.none"));
    T2D_CHECK_GT(list.sprite_count(), 0u);

    // A generated tile is drawn through the atlas, never through an engine tile-map renderer.
    usize wrong_texture = 0;
    usize wrong_filter = 0;
    usize wrong_uv = 0;
    for (const DrawCommand& command : list.commands()) {
        if (command.kind != DrawCommand::Kind::Sprite) continue;
        if (command.texture != "test_tiles") ++wrong_texture;
        if (command.filter != TextureFilter::Nearest) ++wrong_filter;
        if (!same_uv(command.uv, uv_a()) && !same_uv(command.uv, uv_b())) ++wrong_uv;
    }
    T2D_CHECK_EQ(wrong_texture, 0u);
    T2D_CHECK_EQ(wrong_filter, 0u);
    T2D_CHECK_EQ(wrong_uv, 0u);
}

T2D_TEST(the_draw_list_holds_exactly_the_visible_cells_and_both_atlas_cells) {
    ScratchDir dir;
    OpenWorldScene scene(dir.file("world.t2dk"));
    FakeServices services;
    enter(scene, services);

    const std::vector<std::pair<t2d::i32, t2d::i32>> cells = visible_cells(scene);
    T2D_REQUIRE(!cells.empty());

    DrawList list;
    scene.draw(services, list);

    // Exact: every visible non-empty cell once, and nothing else. A chunk renderer, an off-screen cell or
    // a duplicated seam would fail this count.
    T2D_CHECK_EQ(list.sprite_count(), cells.size());
    const t2d::f32 cell_size = OpenWorldScene::kTileSize * scene.camera().zoom();
    usize wrong_size = 0;
    usize off_screen = 0;
    for (const DrawCommand& command : list.commands()) {
        if (command.kind != DrawCommand::Kind::Sprite) continue;
        if (t2d::abs_f32(command.rect.width() - cell_size) > 0.01f ||
            t2d::abs_f32(command.rect.height() - cell_size) > 0.01f) {
            ++wrong_size;
        }
        if (!(command.rect.max.x > 0.0f && command.rect.min.x < services.viewport().x &&
              command.rect.max.y > 0.0f && command.rect.min.y < services.viewport().y)) {
            ++off_screen;
        }
    }
    T2D_CHECK_EQ(wrong_size, 0u);
    T2D_CHECK_EQ(off_screen, 0u);

    // The uv split is the atlas': left cell A, right cell B, one half each.
    usize expected_a = 0;
    usize expected_b = 0;
    for (const auto& [x, y] : cells) {
        if (scene.tile_at(x, y) == OpenWorldScene::kTileA) ++expected_a;
        else ++expected_b;
    }
    usize drawn_a = 0;
    usize drawn_b = 0;
    for (const DrawCommand& command : list.commands()) {
        if (command.kind != DrawCommand::Kind::Sprite) continue;
        if (same_uv(command.uv, uv_a())) ++drawn_a;
        else if (same_uv(command.uv, uv_b())) ++drawn_b;
    }
    T2D_CHECK_EQ(drawn_a, expected_a);
    T2D_CHECK_EQ(drawn_b, expected_b);
    T2D_CHECK_GT(expected_a, 0u);
    T2D_CHECK_GT(expected_b, 0u);

    // The two chunks are both on screen at the initial framing, and their bounds and labels are drawn.
    T2D_CHECK(has_text(list, "ow.chunk"));
    T2D_CHECK_GE(list.rect_count(), 8u);
}

T2D_TEST(the_pointer_cycles_a_b_empty_and_the_number_keys_place) {
    ScratchDir dir;
    OpenWorldScene scene(dir.file("world.t2dk"));
    FakeServices services;
    enter(scene, services);

    // (5,5) is a generated A (5+5 is not a multiple of 8).
    T2D_CHECK_EQ(scene.tile_at(5, 5), OpenWorldScene::kTileA);
    const t2d::Vec2 centre = cell_centre_screen(scene, 5, 5);
    click_at(scene, services, centre);
    T2D_CHECK_EQ(scene.tile_at(5, 5), OpenWorldScene::kTileB);
    click_at(scene, services, centre);
    T2D_CHECK_EQ(scene.tile_at(5, 5), t2d::kEmptyTile);
    click_at(scene, services, centre);
    T2D_CHECK_EQ(scene.tile_at(5, 5), OpenWorldScene::kTileA);
    T2D_CHECK_EQ(scene.dirty_chunk_count(), 1u);

    // The last click above was A; the HUD now names the cell and its tile.
    DrawList list;
    scene.draw(services, list);
    T2D_CHECK(has_text(list, "ow.cell"));
    T2D_CHECK(has_text(list, "ow.save.none"));

    // 1/2/0 place A/B/empty at the pointer without cycling.
    const t2d::Vec2 other = cell_centre_screen(scene, 9, 9);
    services.input_mutable().begin_frame();
    services.input_mutable().set_cursor_inside(true);
    services.input_mutable().set_mouse_position(other.x, other.y);
    services.input_mutable().set_key(Key::D2, true);
    scene.update(services, 1.0f / 60.0f);
    T2D_CHECK_EQ(scene.tile_at(9, 9), OpenWorldScene::kTileB);

    services.input_mutable().begin_frame();
    services.input_mutable().set_cursor_inside(true);
    services.input_mutable().set_mouse_position(other.x, other.y);
    services.input_mutable().set_key(Key::D0, true);
    scene.update(services, 1.0f / 60.0f);
    T2D_CHECK_EQ(scene.tile_at(9, 9), t2d::kEmptyTile);
}

T2D_TEST(edits_are_saved_reopened_and_clean_cells_regenerate) {
    ScratchDir dir;
    const std::string path = dir.file("world.t2dk");
    {
        OpenWorldScene scene(path);
        FakeServices services;
        enter(scene, services);
        // One edit in each test chunk, both changing a generated tile to the other one.
        T2D_CHECK(scene.place_tile(1, 1, OpenWorldScene::kTileB));
        T2D_CHECK(scene.place_tile(34, 0, OpenWorldScene::kTileA));
        T2D_CHECK_EQ(scene.dirty_chunk_count(), 2u);
        T2D_CHECK(scene.save());
        T2D_CHECK_EQ(scene.store_chunk_count(), 2u);
        T2D_CHECK(fs::exists(path));
        T2D_CHECK_GT(fs::file_size(path), 0u);
    }
    {
        OpenWorldScene scene(path);
        FakeServices services;
        enter(scene, services);
        // Both edits came back, and both chunks are in the store by their absolute keys.
        T2D_CHECK_EQ(scene.tile_at(1, 1), OpenWorldScene::kTileB);
        T2D_CHECK_EQ(scene.tile_at(34, 0), OpenWorldScene::kTileA);
        T2D_CHECK_EQ(scene.store_chunk_count(), 2u);
        T2D_CHECK_EQ(scene.dirty_chunk_count(), 0u);
        // A cell that was never edited is the generator's, not stale store bytes.
        T2D_CHECK_EQ(scene.tile_at(2, 2), OpenWorldScene::kTileA);
        T2D_CHECK_EQ(scene.tile_at(35, 0), OpenWorldScene::kTileB);
        T2D_CHECK_EQ(scene.tile_at(0, 0), OpenWorldScene::kTileB);

        // The HUD names the store and no unsaved work.
        DrawList list;
        scene.draw(services, list);
        T2D_CHECK(has_text(list, "ow.store"));
        T2D_CHECK(has_text(list, "ow.save.none"));
    }
}

T2D_TEST(s_saves_and_leaving_saves_even_without_saving_by_hand) {
    ScratchDir dir;
    const std::string path = dir.file("world.t2dk");
    {
        OpenWorldScene scene(path);
        FakeServices services;
        enter(scene, services);
        T2D_CHECK(scene.place_tile(7, 7, OpenWorldScene::kTileB));   // generated A
        press_key(scene, services, Key::S);
        // S is the save key: the file has the chunk before the scene ever leaves.
        T2D_CHECK_EQ(scene.store_chunk_count(), 1u);
        T2D_CHECK(fs::exists(path));
        DrawList list;
        scene.draw(services, list);
        T2D_CHECK(has_text(list, "ow.save.ok"));
    }
    {
        OpenWorldScene scene(path);
        FakeServices services;
        enter(scene, services);
        T2D_CHECK_EQ(scene.tile_at(7, 7), OpenWorldScene::kTileB);
        // An edit not saved by hand is saved by leaving: ESC calls save() and pop_scene().
        T2D_CHECK(scene.place_tile(8, 8, OpenWorldScene::kTileB));   // generated A
        press_key(scene, services, Key::Escape);
        T2D_CHECK_EQ(services.pops, 1);
    }
    {
        OpenWorldScene scene(path);
        FakeServices services;
        enter(scene, services);
        T2D_CHECK_EQ(scene.tile_at(7, 7), OpenWorldScene::kTileB);
        T2D_CHECK_EQ(scene.tile_at(8, 8), OpenWorldScene::kTileB);
        T2D_CHECK_GT(scene.store_chunk_count(), 0u);
    }
}

T2D_TEST(a_store_that_cannot_be_opened_is_reported_and_the_scene_still_runs) {
    ScratchDir dir;
    // A directory is not a store file: open() fails and the scene must keep running without persistence.
    OpenWorldScene scene(dir.path().string());
    FakeServices services;
    enter(scene, services);
    T2D_CHECK_FALSE(scene.store_open());
    T2D_CHECK_FALSE(scene.store_error().empty());
    T2D_CHECK_EQ(scene.store_chunk_count(), 0u);

    // Editing and streaming still work in memory; only the saving cannot.
    T2D_CHECK(scene.place_tile(1, 1, OpenWorldScene::kTileB));
    T2D_CHECK_EQ(scene.tile_at(1, 1), OpenWorldScene::kTileB);
    T2D_CHECK_EQ(scene.dirty_chunk_count(), 1u);
    T2D_CHECK_FALSE(scene.save());

    DrawList list;
    scene.draw(services, list);
    T2D_CHECK(has_text(list, "ow.store.error"));
    T2D_CHECK(has_text(list, "ow.save.failed.reason"));
    T2D_CHECK_GT(list.sprite_count(), 0u);

    // Leaving a scene with no store is a save that fails, not a crash.
    scene.on_exit(services);
}

T2D_TEST(the_floating_origin_moves_and_the_edited_chunk_comes_back_by_absolute_key) {
    ScratchDir dir;
    const std::string path = dir.file("world.t2dk");
    OpenWorldScene scene(path);
    FakeServices services;
    enter(scene, services);

    // An edit in chunk {0,0}: it has to survive the camera leaving, the origin moving and the camera
    // coming back.
    T2D_CHECK(scene.place_tile(1, 1, OpenWorldScene::kTileB));
    T2D_CHECK_EQ(scene.origin().x, 0);
    T2D_CHECK_EQ(scene.origin().y, 0);

    // Pan a little over twelve chunks to the right: the test chunk is evicted (and, being dirty, saved);
    // no origin move has happened yet.
    hold_key(scene, services, Key::D, 100, 0.1f);
    T2D_CHECK_EQ(scene.origin().x, 0);
    T2D_CHECK_GT(scene.camera().centre().x, 4096.0f);
    T2D_CHECK_EQ(scene.store_chunk_count(), 1u);
    T2D_CHECK_GT(scene.loaded_chunk_count(), 0u);

    // C recentres: the origin becomes the camera's absolute chunk and the camera is moved back by the
    // same shift, so the view does not jump.
    press_key(scene, services, Key::C);
    const t2d::ChunkKey new_origin = scene.origin();
    T2D_CHECK_GT(new_origin.x, 10);
    T2D_CHECK_EQ(new_origin.y, 0);
    T2D_CHECK_LT(scene.camera().centre().x, static_cast<t2d::f32>(t2d::WorldChunk::kSize) * OpenWorldScene::kTileSize);
    T2D_CHECK_GE(scene.camera().centre().x, 0.0f);

    // Pan back the same distance: the camera is over the same absolute place, and the edited chunk is
    // loaded from the store under its absolute key and re-addressed by the new origin.
    hold_key(scene, services, Key::Left, 100, 0.1f);
    const t2d::i32 relative_x = static_cast<t2d::i32>(1 - new_origin.x * t2d::WorldChunk::kSize);
    const t2d::i32 relative_y = static_cast<t2d::i32>(1 - new_origin.y * t2d::WorldChunk::kSize);
    T2D_CHECK_EQ(scene.tile_at(relative_x, relative_y), OpenWorldScene::kTileB);
}

T2D_TEST(escape_saves_and_pops) {
    ScratchDir dir;
    OpenWorldScene scene(dir.file("world.t2dk"));
    FakeServices services;
    enter(scene, services);
    T2D_CHECK(scene.place_tile(1, 1, OpenWorldScene::kTileB));

    press_key(scene, services, Key::Escape);
    T2D_CHECK_EQ(services.pops, 1);
    T2D_CHECK_GT(scene.store_chunk_count(), 0u);
}

T2D_TEST(a_near_limit_content_origin_keeps_the_scene_relative_and_restores_both_edits) {
    ScratchDir dir;
    const std::string path = dir.file("far.t2dk");
    // The content origin the shell's --world-origin names for the far run: four chunks inside the clamp,
    // about 1.1e12 cells from the default origin.
    const t2d::ChunkKey far{t2d::OpenWorld::kMaxAbsoluteChunk - 4, -(t2d::OpenWorld::kMaxAbsoluteChunk - 4)};
    T2D_CHECK(far.x == 34359738364LL);

    {
        OpenWorldScene scene(path, far);
        FakeServices services;
        enter(scene, services);
        // The scene is built at the far content origin, and the world's relative zero is on it.
        T2D_CHECK(scene.content_origin() == far);
        T2D_CHECK(scene.origin() == far);
        // The generator's two chunks are at relative (0,0) and (1,0), so every test cell is tiny.
        T2D_CHECK_EQ(scene.tile_at(0, 0), OpenWorldScene::kTileB);
        T2D_CHECK_EQ(scene.tile_at(1, 1), OpenWorldScene::kTileA);
        T2D_CHECK_EQ(scene.tile_at(32, 0), OpenWorldScene::kTileA);
        T2D_CHECK_EQ(scene.tile_at(34, 0), OpenWorldScene::kTileB);

        DrawList list;
        scene.draw(services, list);
        T2D_CHECK(has_uv(list, uv_a()));
        T2D_CHECK(has_uv(list, uv_b()));
        T2D_CHECK(has_text(list, "ow.chunks"));   // includes the absolute origin line
        T2D_CHECK(has_text(list, "ow.store"));

        // One edit in each far chunk, saved through the store under the absolute keys.
        T2D_CHECK(scene.place_tile(1, 1, OpenWorldScene::kTileB));
        T2D_CHECK(scene.place_tile(34, 0, OpenWorldScene::kTileA));
        T2D_CHECK_EQ(scene.dirty_chunk_count(), 2u);
        T2D_CHECK(scene.save());
        T2D_CHECK_EQ(scene.store_chunk_count(), 2u);
    }
    {
        OpenWorldScene scene(path, far);
        FakeServices services;
        enter(scene, services);
        T2D_CHECK(scene.content_origin() == far);
        T2D_CHECK_EQ(scene.store_chunk_count(), 2u);
        T2D_CHECK_EQ(scene.tile_at(1, 1), OpenWorldScene::kTileB);
        T2D_CHECK_EQ(scene.tile_at(34, 0), OpenWorldScene::kTileA);
        // Unedited cells are generated, not stale bytes; a chunk outside the two content chunks is empty
        // because it was never stored (the store holds exactly the two edited chunks).
        T2D_CHECK_EQ(scene.tile_at(2, 2), OpenWorldScene::kTileA);
        T2D_CHECK_EQ(scene.tile_at(35, 0), OpenWorldScene::kTileB);
        T2D_CHECK_EQ(scene.tile_at(64, 0), t2d::kEmptyTile);

        // C recentres: the origin becomes the camera's absolute chunk next to the far content and the
        // camera follows the shift; the content is re-addressed relatively and the edits are the same
        // absolute tiles.
        press_key(scene, services, Key::C);
        const t2d::ChunkKey after = scene.origin();
        T2D_CHECK(scene.content_origin() == far);
        T2D_CHECK(after == (t2d::ChunkKey{far.x + 1, far.y}));
        T2D_CHECK_GT(after.x, 34359738364LL);
        T2D_CHECK_EQ(after.y, far.y);
        T2D_CHECK_EQ(scene.tile_at(2, 0), OpenWorldScene::kTileA);    // edit at (34,0), chunk now relative 0
        T2D_CHECK_EQ(scene.tile_at(-31, 1), OpenWorldScene::kTileB);  // edit at (1,1), chunk now relative -1

        DrawList list;
        scene.draw(services, list);
        T2D_CHECK(has_uv(list, uv_a()));
        T2D_CHECK(has_uv(list, uv_b()));
        T2D_CHECK(scene.save());
        T2D_CHECK_EQ(scene.store_chunk_count(), 2u);
    }
}

T2D_TEST_MAIN
