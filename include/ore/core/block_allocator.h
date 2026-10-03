// Ore framework - best-fit free-list allocator over one contiguous byte range.
//
// This is the host-side algorithm that backs ore::GpuAllocator: every Vulkan memory block
// is sub-divided with one of these. It has no dependencies on Vulkan so it is unit tested
// directly (see tests/test_block_allocator.cpp).
#pragma once

#include <ore/core/types.h>

#include <optional>
#include <vector>

namespace ore {

class BlockAllocator {
public:
    struct Allocation {
        usize offset = 0;
        usize size = 0;
        [[nodiscard]] bool valid() const { return size > 0; }
        friend bool operator==(const Allocation&, const Allocation&) = default;
    };

    struct Region {
        usize offset = 0;
        usize size = 0;
        bool free = true;
    };

    BlockAllocator() = default;
    explicit BlockAllocator(usize size, usize alignment = 16) { reset(size, alignment); }

    /// Discards all bookkeeping and starts over with a single free region.
    void reset(usize size, usize alignment = 16);

    /// Best-fit allocation. \p alignment of 0 means "use the default alignment".
    [[nodiscard]] std::optional<Allocation> allocate(usize size, usize alignment = 0);
    void free(Allocation allocation);

    [[nodiscard]] usize capacity() const { return capacity_; }
    [[nodiscard]] usize used() const { return used_; }
    [[nodiscard]] usize available() const { return capacity_ - used_; }
    [[nodiscard]] usize largest_free_block() const;
    [[nodiscard]] u32 allocation_count() const { return allocation_count_; }
    [[nodiscard]] u32 free_region_count() const;
    /// True when nothing is allocated.
    [[nodiscard]] bool empty() const { return allocation_count_ == 0; }

    [[nodiscard]] std::vector<Region> regions() const;
    /// Internal consistency check used by the tests: contiguous, ordered, coalesced.
    [[nodiscard]] bool validate() const;

private:
    struct Node {
        usize offset = 0;
        usize size = 0;
        bool free = true;
    };

    std::vector<Node> nodes_;
    usize capacity_ = 0;
    usize used_ = 0;
    usize default_alignment_ = 16;
    u32 allocation_count_ = 0;
};

} // namespace ore
