#include <ore/core/block_allocator.h>

#include <support/test_support.h>

using ore::BlockAllocator;

ORE_TEST(block_allocator_fresh_state) {
    BlockAllocator allocator(1024, 16);
    ORE_CHECK_EQ(allocator.capacity(), 1024u);
    ORE_CHECK_EQ(allocator.used(), 0u);
    ORE_CHECK_EQ(allocator.available(), 1024u);
    ORE_CHECK_EQ(allocator.allocation_count(), 0u);
    ORE_CHECK(allocator.empty());
    ORE_CHECK(allocator.validate());
    ORE_CHECK_EQ(allocator.largest_free_block(), 1024u);
}

ORE_TEST(block_allocator_allocates_and_frees) {
    BlockAllocator allocator(1024, 16);
    const auto a = allocator.allocate(100);
    const auto b = allocator.allocate(200);
    ORE_REQUIRE(a.has_value());
    ORE_REQUIRE(b.has_value());
    ORE_CHECK_EQ(a->offset, 0u);
    ORE_CHECK_EQ(a->size, 100u);
    ORE_CHECK_EQ(b->offset, 112u); // 100 rounded up to the 16 byte default alignment
    ORE_CHECK_EQ(allocator.used(), 300u);
    ORE_CHECK_EQ(allocator.allocation_count(), 2u);
    ORE_CHECK(allocator.validate());

    allocator.free(*a);
    ORE_CHECK_EQ(allocator.used(), 200u);
    ORE_CHECK_EQ(allocator.allocation_count(), 1u);
    ORE_CHECK(allocator.validate());
    allocator.free(*b);
    ORE_CHECK(allocator.empty());
    ORE_CHECK_EQ(allocator.available(), 1024u);
    ORE_CHECK_EQ(allocator.free_region_count(), 1u);
}

ORE_TEST(block_allocator_coalesces_neighbours) {
    BlockAllocator allocator(4096);
    auto a = allocator.allocate(1000);
    auto b = allocator.allocate(1000, 256);
    auto c = allocator.allocate(1000);
    ORE_REQUIRE(a && b && c);
    ORE_CHECK(allocator.validate());

    allocator.free(*a);
    allocator.free(*c);
    // 'a' is a hole of its own; 'c' merges with the free tail.
    ORE_CHECK_EQ(allocator.free_region_count(), 2u);
    allocator.free(*b);
    ORE_CHECK_EQ(allocator.free_region_count(), 1u);
    ORE_CHECK_EQ(allocator.largest_free_block(), 4096u);
    ORE_CHECK(allocator.validate());
}

ORE_TEST(block_allocator_honours_alignment) {
    BlockAllocator allocator(8192, 4);
    auto a = allocator.allocate(3);
    auto b = allocator.allocate(64, 256);
    ORE_REQUIRE(a && b);
    ORE_CHECK_EQ(b->offset % 256u, 0u);
    auto c = allocator.allocate(4096, 4096);
    ORE_REQUIRE(c.has_value());
    ORE_CHECK_EQ(c->offset % 4096u, 0u);
    ORE_CHECK(allocator.validate());
}

ORE_TEST(block_allocator_fails_when_full) {
    BlockAllocator allocator(256, 1);
    auto a = allocator.allocate(200);
    ORE_REQUIRE(a.has_value());
    const auto too_big = allocator.allocate(100);
    ORE_CHECK_FALSE(too_big.has_value());
    // Fragmentation: 56 bytes are free, but a 64 byte allocation cannot be satisfied contiguously.
    const auto fragmented = allocator.allocate(64);
    ORE_CHECK_FALSE(fragmented.has_value());
    const auto exact = allocator.allocate(56);
    ORE_CHECK(exact.has_value());
    ORE_CHECK_FALSE(allocator.allocate(1).has_value());
    ORE_CHECK_EQ(allocator.available(), 0u);
}

ORE_TEST(block_allocator_reuses_freed_space) {
    BlockAllocator allocator(1024, 16);
    std::vector<BlockAllocator::Allocation> live;
    for (int i = 0; i < 8; ++i) {
        const auto allocation = allocator.allocate(64);
        ORE_REQUIRE(allocation.has_value());
        live.push_back(*allocation);
    }
    ORE_CHECK_EQ(allocator.used(), 512u);
    for (const auto& allocation : live) allocator.free(allocation);
    ORE_CHECK(allocator.empty());

    const auto reused = allocator.allocate(512);
    ORE_REQUIRE(reused.has_value());
    ORE_CHECK_EQ(reused->offset, 0u);
    ORE_CHECK_EQ(allocator.available(), 512u);
    ORE_CHECK(allocator.validate());
}

ORE_TEST(block_allocator_reset_and_degenerate_sizes) {
    BlockAllocator allocator(1024, 16);
    ORE_CHECK(allocator.allocate(0).has_value());
    ORE_CHECK_EQ(allocator.used(), 0u);
    auto a = allocator.allocate(1024);
    ORE_REQUIRE(a.has_value());
    ORE_CHECK(allocator.allocate(1).has_value() == false);
    allocator.reset(64, 8);
    ORE_CHECK_EQ(allocator.capacity(), 64u);
    ORE_CHECK(allocator.empty());
    ORE_CHECK(allocator.validate());

    BlockAllocator empty_allocator(0);
    ORE_CHECK_FALSE(empty_allocator.allocate(1).has_value());
    ORE_CHECK(empty_allocator.validate());
}

ORE_TEST_MAIN
