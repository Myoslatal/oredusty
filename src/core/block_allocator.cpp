#include <ore/core/block_allocator.h>

#include <ore/core/assert.h>

#include <algorithm>

namespace ore {

void BlockAllocator::reset(usize size, usize alignment) {
    nodes_.clear();
    capacity_ = size;
    used_ = 0;
    allocation_count_ = 0;
    default_alignment_ = alignment == 0 ? 1 : alignment;
    if (size > 0) {
        nodes_.push_back(Node{0, size, true});
    }
}

std::optional<BlockAllocator::Allocation> BlockAllocator::allocate(usize size, usize alignment) {
    if (size == 0) return Allocation{0, 0};
    const usize align = std::max(alignment == 0 ? default_alignment_ : alignment, static_cast<usize>(1));

    usize best_index = nodes_.size();
    usize best_slack = 0;
    usize best_offset = 0;
    for (usize i = 0; i < nodes_.size(); ++i) {
        const Node& node = nodes_[i];
        if (!node.free) continue;
        const usize offset = align_up(node.offset, align);
        const usize padding = offset - node.offset;
        if (node.size < padding + size) continue;
        const usize slack = node.size - padding - size;
        if (best_index == nodes_.size() || slack < best_slack) {
            best_index = i;
            best_slack = slack;
            best_offset = offset;
        }
    }
    if (best_index == nodes_.size()) return std::nullopt;

    // Splitting inserts into nodes_ and therefore invalidates references: work with indices only.
    const usize padding = best_offset - nodes_[best_index].offset;
    if (padding > 0) {
        nodes_[best_index].offset = best_offset;
        nodes_[best_index].size -= padding;
        nodes_.insert(nodes_.begin() + static_cast<isize>(best_index), Node{best_offset - padding, padding, true});
        ++best_index;
    }
    if (nodes_[best_index].size > size) {
        const usize remainder = nodes_[best_index].size - size;
        const usize start = nodes_[best_index].offset;
        nodes_[best_index].size = size;
        nodes_.insert(nodes_.begin() + static_cast<isize>(best_index) + 1, Node{start + size, remainder, true});
    }
    nodes_[best_index].free = false;
    used_ += size;
    ++allocation_count_;
    return Allocation{nodes_[best_index].offset, nodes_[best_index].size};
}

void BlockAllocator::free(Allocation allocation) {
    if (!allocation.valid()) return;
    for (usize i = 0; i < nodes_.size(); ++i) {
        Node& node = nodes_[i];
        if (node.free || node.offset != allocation.offset || node.size != allocation.size) continue;
        // Erasing invalidates references, so keep the size in a local before merging.
        usize freed_size = node.size;
        node.free = true;
        used_ -= freed_size;
        if (allocation_count_ > 0) --allocation_count_;
        // Coalesce with the following region first, then with the previous one.
        if (i + 1 < nodes_.size() && nodes_[i + 1].free) {
            nodes_[i].size += nodes_[i + 1].size;
            freed_size = nodes_[i].size;
            nodes_.erase(nodes_.begin() + static_cast<isize>(i) + 1);
        }
        if (i > 0 && nodes_[i - 1].free) {
            nodes_[i - 1].size += freed_size;
            nodes_.erase(nodes_.begin() + static_cast<isize>(i));
        }
        return;
    }
    ORE_ASSERT_MSG(false, "BlockAllocator::free: unknown allocation (offset={}, size={})", allocation.offset,
                   allocation.size);
}

usize BlockAllocator::largest_free_block() const {
    usize largest = 0;
    for (const Node& node : nodes_) {
        if (node.free) largest = std::max(largest, node.size);
    }
    return largest;
}

u32 BlockAllocator::free_region_count() const {
    u32 count = 0;
    for (const Node& node : nodes_) {
        if (node.free) ++count;
    }
    return count;
}

std::vector<BlockAllocator::Region> BlockAllocator::regions() const {
    std::vector<Region> out;
    out.reserve(nodes_.size());
    for (const Node& node : nodes_) out.push_back(Region{node.offset, node.size, node.free});
    return out;
}

bool BlockAllocator::validate() const {
    usize offset = 0;
    for (usize i = 0; i < nodes_.size(); ++i) {
        const Node& node = nodes_[i];
        if (node.offset != offset) return false;
        if (node.size == 0) return false;
        if (i > 0 && nodes_[i - 1].free && node.free) return false;
        offset += node.size;
    }
    return offset == capacity_;
}

} // namespace ore
