// Mefemma - the open-world test scene: two test tiles, two test chunks, and the whole facility
// exercised in the game.
//
// This is the worked example for Tile2D's optional streamed world (`t2d::OpenWorld`, docs/OPEN_WORLD.md)
// and its optional single-file store (`t2d::ChunkStore`). It is deliberately small: two 32x32 chunks at
// an absolute content origin (default {0,0}) and the chunk to its right hold two tiny atlas tiles, so the
// things a streamed world has to get right are all visible at once:
//
//   * streaming: only the chunks near the camera are resident; the camera can be panned and zoomed and
//     chunks outside the margins are dropped (and edited ones are saved before they go);
//   * editing: a left click cycles the cell under the pointer A -> B -> empty, and the edit survives a
//     camera move, a recentre and a restart;
//   * persistence: enter loads, S and leaving save through a ChunkStore at the path the shell named;
//     an unopenable store is reported on the HUD and the scene still runs without persistence;
//   * rendering: the scene draws its own cells through the DrawList with the test atlas' two uv
//     rectangles, in relative coordinates - the floating origin means the two test chunks never move
//     even when the absolute position is far away.
//
// The scene is *content*, not engine: it uses the same Camera2D/geometry rules the playground does, but
// the world model, the chunk store and the editing policy are the game's own decisions.
#pragma once

#include <mefemma/scene.h>

#include <t2d/core/camera2d.h>
#include <t2d/sim/chunk_store.h>
#include <t2d/sim/open_world.h>

#include <memory>
#include <optional>
#include <string>

namespace mefemma {

/// The open-world test scene: a streamed world of two hand-generated chunks, saveable through a file.
class OpenWorldScene final : public Scene {
public:
    /// The two test tiles: cells of assets/textures/test_tiles.png, left and right.
    static constexpr t2d::TileId kTileA = 1;
    static constexpr t2d::TileId kTileB = 2;
    /// One cell is 16 pixels; the atlas has two 16x16 cells side by side.
    static constexpr f32 kTileSize = 16.0f;
    /// Chunks {0,0} and {1,0} hold the test content; every other absolute chunk is empty.
    static constexpr t2d::i64 kTestChunks = 2;

    /// \p world_file is where the store lives. Empty means the scene's fallback:
    /// "mefemma_openworld.t2dk" in the process' working directory. The shell normally passes
    /// <save directory>/mefemma_openworld.t2dk or an explicit --world-file.
    /// \p content_origin is the ABSOLUTE chunk the two test chunks start at (default {0,0}). The world's
    /// origin is set there before streaming, so relative cells/geometry stay near zero however far that
    /// is, while generation and the store keep using the absolute keys. A test or a scripted run can put
    /// the content near the absolute limit (see --world-origin in the shell).
    explicit OpenWorldScene(std::string world_file = {}, t2d::ChunkKey content_origin = {});

    [[nodiscard]] const char* name() const override { return "openworld"; }

    void on_enter(GameServices& services) override;
    void on_exit(GameServices& services) override;
    void update(GameServices& services, f32 delta_seconds) override;
    void draw(GameServices& services, DrawList& out) const override;

    // --- the scene's own surface, also what the test drives (no test-only paths) ------------------
    /// The tile at a RELATIVE cell, or kEmptyTile. Reads resident chunks only, like OpenWorld::at().
    [[nodiscard]] t2d::TileId tile_at(i32 cell_x, i32 cell_y) const;
    /// Writes one relative cell (kEmptyTile clears it); true when the layer exists.
    bool place_tile(i32 cell_x, i32 cell_y, t2d::TileId id);
    /// world.flush() followed by a durable store flush. false when there is no store or a write failed.
    bool save();
    /// The absolute chunk the two test chunks were built at: fixed for the scene's lifetime.
    [[nodiscard]] t2d::ChunkKey content_origin() const;
    /// The current floating origin (the absolute chunk relative (0,0) maps to); equals content_origin()
    /// until C recentres, after which content is still addressed by the same absolute store keys.
    [[nodiscard]] t2d::ChunkKey origin() const;
    [[nodiscard]] t2d::usize loaded_chunk_count() const;
    [[nodiscard]] t2d::usize dirty_chunk_count() const;
    /// The live chunks the store reports; 0 when the store could not be opened.
    [[nodiscard]] t2d::usize store_chunk_count() const;
    [[nodiscard]] bool store_open() const;
    [[nodiscard]] const std::string& store_error() const;
    [[nodiscard]] const std::string& world_file() const;
    [[nodiscard]] const t2d::Camera2D& camera() const;

private:
    enum class SaveState : t2d::u8 { Never, Saved, Failed };

    [[nodiscard]] static t2d::OpenWorldConfig make_config();
    /// Deterministic content for the two test chunks (content_origin_ and content_origin_ + {1,0});
    /// every other chunk stays empty.
    void generate_chunk(const t2d::ChunkKey& key, t2d::WorldChunk& chunk) const;
    /// Opens the store once (on enter). A failure is reported and leaves the scene running without one.
    void open_store();
    /// Frames both test chunks: the camera's first view, and what C returns to.
    void frame_camera();
    /// world.recenter(camera) and the camera moved by the same shift, so the view does not jump.
    void recenter_camera();
    void cycle_pointer_cell();
    void move_camera(GameServices& services, f32 delta_seconds);
    void draw_cells(DrawList& out) const;
    void draw_chunks(GameServices& services, DrawList& out) const;
    void draw_pointer(DrawList& out) const;
    void draw_hud(GameServices& services, DrawList& out) const;

    // The store is declared before the world so the world (whose callbacks capture the store) is
    // destroyed first: the world never outlives the bytes it reads.
    std::string world_file_;
    std::optional<t2d::ChunkStore> store_;
    std::string store_error_;
    t2d::ChunkKey content_origin_{};
    t2d::OpenWorld world_;
    t2d::Camera2D camera_{};

    t2d::Vec2 pointer_world_{};
    i32 pointer_cell_x_ = 0;
    i32 pointer_cell_y_ = 0;
    bool pointer_inside_ = false;
    bool framed_ = false;
    bool store_attempted_ = false;
    SaveState save_state_ = SaveState::Never;
    std::string save_error_;
};

[[nodiscard]] std::unique_ptr<Scene> make_open_world_scene(std::string world_file = {},
                                                            t2d::ChunkKey content_origin = {});

} // namespace mefemma
