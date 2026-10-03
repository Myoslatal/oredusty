// The map grid: packed cells, lazily allocated layers, O(1) fill counters. The type the sandbox paints
// on and the one the game's world will be made of, so what it promises is checked here rather than in
// the middle of a screen: four bytes a cell, and a layer nobody wrote to costs nothing.
#include <mine/content_grid.h>

#include <support/test_support.h>

#include <vector>

using namespace mine;

namespace {

constexpr ContentRef kStructure{ContentKind::Structure, 3, false};
constexpr ContentRef kMachine{ContentKind::Machine, 1, false};

} // namespace

T2D_TEST(a_cell_packs_into_four_bytes_and_comes_back_unchanged) {
    T2D_CHECK_EQ(ContentRef{}.packed(), 0u);
    T2D_CHECK_EQ(ContentRef::unpack(0u), ContentRef{});

    const ContentRef refs[] = {
        kStructure,
        kMachine,
        ContentRef{ContentKind::Item, 1, false},
        ContentRef{ContentKind::Channel, kMaxContentId - 1u, false},
        ContentRef{ContentKind::Structure, 7, true},   // a cell whose content is gone
        ContentRef{ContentKind::Count, kNoContent, false},
    };
    for (const ContentRef& ref : refs) {
        const ContentRef back = ContentRef::unpack(ref.packed());
        T2D_CHECK_EQ(back.kind, ref.kind);
        T2D_CHECK_EQ(back.id, ref.id);
        T2D_CHECK_EQ(back.stale, ref.stale);
        T2D_CHECK_EQ(back.empty(), ref.empty());
    }
    // The flag is what tells "gone" from "here", and an empty cell never claims to be either.
    const ContentRef gone = ContentRef::unpack(ContentRef{ContentKind::Structure, 7, true}.packed());
    T2D_CHECK(gone.missing());
    T2D_CHECK_FALSE(kStructure.missing());
    T2D_CHECK_FALSE(ContentRef{}.missing());
    T2D_CHECK(ContentRef{}.empty());
}

T2D_TEST(a_layer_nobody_wrote_to_costs_nothing) {
    ContentGrid grid(512, 512, 8);
    T2D_CHECK_EQ(grid.cell_count(), 512u * 512u);
    T2D_CHECK_EQ(grid.bytes(), 0u);
    T2D_CHECK_EQ(grid.filled(), 0u);
    for (i32 layer = 0; layer < grid.layer_count(); ++layer) {
        T2D_CHECK_FALSE(grid.allocated(layer));
        T2D_CHECK_EQ(grid.filled(layer), 0u);
        T2D_CHECK(grid.at(layer, GridPos{10, 10}).empty());
    }

    // One cell written allocates that layer and no other: 512x512x4 bytes is 1 MiB, which is what a
    // map of this size costs per layer that is actually used.
    grid.set(3, GridPos{511, 511}, kMachine);
    T2D_CHECK(grid.allocated(3));
    T2D_CHECK_EQ(grid.bytes(), 1024u * 1024u);
    T2D_CHECK_EQ(grid.filled(), 1u);
    T2D_CHECK_EQ(grid.filled(3), 1u);
    T2D_CHECK_EQ(grid.filled(0), 0u);
    T2D_CHECK_EQ(grid.at(3, GridPos{511, 511}), kMachine);
    T2D_CHECK(grid.at(0, GridPos{511, 511}).empty());

    // Clearing releases the storage again: an emptied layer is as cheap as one never written.
    grid.clear_layer(3);
    T2D_CHECK_EQ(grid.bytes(), 0u);
    T2D_CHECK_EQ(grid.filled(), 0u);
    T2D_CHECK_FALSE(grid.allocated(3));
    grid.set(3, GridPos{0, 0}, kStructure);
    grid.set(3, GridPos{1, 0}, kStructure);
    grid.clear();
    T2D_CHECK_EQ(grid.bytes(), 0u);
    T2D_CHECK_EQ(grid.filled(), 0u);
}

T2D_TEST(the_fill_count_follows_every_write_and_ignores_the_ones_that_change_nothing) {
    ContentGrid grid(8, 4, 2);
    T2D_CHECK_EQ(grid.filled(), 0u);
    grid.set(0, GridPos{1, 1}, kStructure);
    T2D_CHECK_EQ(grid.filled(), 1u);
    grid.set(0, GridPos{1, 1}, kStructure);          // the same cell, the same content
    T2D_CHECK_EQ(grid.filled(), 1u);
    grid.set(0, GridPos{1, 1}, kMachine);            // a different content still fills one cell
    T2D_CHECK_EQ(grid.filled(), 1u);
    grid.set(0, GridPos{1, 1}, ContentRef{});        // erased
    T2D_CHECK_EQ(grid.filled(), 0u);
    grid.set(0, GridPos{1, 1}, ContentRef{});        // erasing an empty cell is not a negative count
    T2D_CHECK_EQ(grid.filled(), 0u);

    grid.set(1, GridPos{7, 3}, kMachine);
    grid.set(1, GridPos{0, 0}, kMachine);
    T2D_CHECK_EQ(grid.filled(1), 2u);
    T2D_CHECK_EQ(grid.filled(), 2u);

    // Anything outside the grid or the layer range is ignored, not redirected.
    grid.set(0, GridPos{-1, 0}, kStructure);
    grid.set(0, GridPos{8, 0}, kStructure);
    grid.set(0, GridPos{0, 4}, kStructure);
    grid.set(9, GridPos{0, 0}, kStructure);
    grid.set(-1, GridPos{0, 0}, kStructure);
    T2D_CHECK_EQ(grid.filled(), 2u);
    T2D_CHECK(grid.at(0, GridPos{-1, 0}).empty());
    T2D_CHECK(grid.at(9, GridPos{0, 0}).empty());

    // A zero sized grid is clamped rather than allocated empty.
    const ContentGrid clamped(0, 0, 0);
    T2D_CHECK_EQ(clamped.width(), 1u);
    T2D_CHECK_EQ(clamped.height(), 1u);
    T2D_CHECK_EQ(clamped.layer_count(), 1);
    T2D_CHECK_EQ(ContentGrid(1, 1, 99).layer_count(), ContentGrid::kMaxLayers);
    T2D_CHECK_EQ(ContentGrid(99999, 1, 1).width(), ContentGrid::kMaxDimension);
}

T2D_TEST(resizing_keeps_the_cells_that_still_fit_and_recounts_them) {
    ContentGrid grid(4, 4, 2);
    grid.set(0, GridPos{0, 0}, kStructure);
    grid.set(0, GridPos{3, 3}, kStructure);
    grid.set(1, GridPos{3, 3}, kMachine);

    grid.resize(2, 2);
    T2D_CHECK_EQ(grid.width(), 2u);
    T2D_CHECK_EQ(grid.height(), 2u);
    T2D_CHECK_EQ(grid.filled(), 1u);          // the two cells at 3,3 fell outside
    T2D_CHECK_EQ(grid.at(0, GridPos{0, 0}), kStructure);
    T2D_CHECK_EQ(grid.filled(1), 0u);
    T2D_CHECK_FALSE(grid.allocated(1));       // an emptied layer releases its storage

    // Growing adds empty cells and keeps what was there.
    grid.resize(6, 6);
    T2D_CHECK_EQ(grid.width(), 6u);
    T2D_CHECK_EQ(grid.filled(), 1u);
    T2D_CHECK_EQ(grid.at(0, GridPos{0, 0}), kStructure);
    T2D_CHECK(grid.at(0, GridPos{5, 5}).empty());

    // A resize to the same size changes nothing at all, counters included.
    const usize bytes = grid.bytes();
    grid.resize(6, 6);
    T2D_CHECK_EQ(grid.bytes(), bytes);
    T2D_CHECK_EQ(grid.filled(), 1u);
}

T2D_TEST(walking_a_layer_visits_every_cell_of_it_and_nothing_of_the_others) {
    ContentGrid grid(4, 3, 2);
    grid.set(1, GridPos{2, 1}, kMachine);

    std::vector<GridPos> visited;
    grid.for_each(1, [&](GridPos pos, ContentRef& ref) {
        visited.push_back(pos);
        if (!ref.empty()) ref.stale = true;   // the visitor writes through the reference
    });
    T2D_CHECK_EQ(visited.size(), 12u);
    T2D_CHECK_EQ(visited.front(), (GridPos{0, 0}));
    T2D_CHECK_EQ(visited.at(1), (GridPos{1, 0}));   // row major
    T2D_CHECK_EQ(visited.back(), (GridPos{3, 2}));
    T2D_CHECK(grid.at(1, GridPos{2, 1}).stale);
    T2D_CHECK_EQ(grid.filled(1), 1u);               // the flag does not change what is filled

    // A layer nobody wrote to has nothing to walk, and a layer that does not exist is not an error.
    usize empty_visits = 0;
    grid.for_each(0, [&](GridPos, ContentRef&) { ++empty_visits; });
    grid.for_each(7, [&](GridPos, ContentRef&) { ++empty_visits; });
    grid.for_each(-1, [&](GridPos, ContentRef&) { ++empty_visits; });
    T2D_CHECK_EQ(empty_visits, 0u);

    // The const walk sees the same cells and cannot change them.
    const ContentGrid& read_only = grid;
    usize seen = 0;
    read_only.for_each(1, [&](GridPos, const ContentRef& ref) {
        if (!ref.empty()) ++seen;
    });
    T2D_CHECK_EQ(seen, 1u);
}

T2D_TEST_MAIN
