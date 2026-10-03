// Ore framework - Vulkan device memory allocator with block suballocation.
//
// One VkDeviceMemory block is carved up with ore::BlockAllocator. Blocks are grouped by
// (memory type, linear/non-linear) so that buffer and optimal-tiling image suballocations never
// share a region when the device reports bufferImageGranularity > 1 (Vulkan spec requirement).
#pragma once

#include <ore/core/block_allocator.h>
#include <ore/core/types.h>
#include <ore/rhi/device.h>

#include <string>
#include <vector>

namespace ore::rhi {

/// Where a suballocation lives inside its block.
struct Allocation {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    /// Host pointer for the whole block, already offset by nothing (use mapped_at()).
    void* mapped = nullptr;
    u32 block_index = kInvalidIndex;
    /// Memory type the block was allocated from (index into VkPhysicalDeviceMemoryProperties).
    u32 memory_type = kInvalidIndex;

    [[nodiscard]] bool valid() const { return memory != VK_NULL_HANDLE; }
    [[nodiscard]] void* mapped_at(VkDeviceSize extra = 0) const {
        return mapped != nullptr ? static_cast<u8*>(mapped) + offset + extra : nullptr;
    }
};

/// Distinguishes buffer-style (linear) from image-style (optimal tiling) allocations.
enum class MemoryKind : u8 { Linear, NonLinear };

/// A host flush / invalidate range that satisfies VUID-VkMappedMemoryRange-size-01390:
/// the offset must be a multiple of nonCoherentAtomSize and size either VK_WHOLE_SIZE or a
/// multiple of it, clamped to the allocation. Graphics drivers used to be forgiving about this;
/// non-coherent memory (common on integrated GPUs) is not.
struct FlushRange {
    VkDeviceSize offset = 0;
    VkDeviceSize size = VK_WHOLE_SIZE;
};

[[nodiscard]] constexpr FlushRange align_flush_range(VkDeviceSize offset, VkDeviceSize size,
                                                     VkDeviceSize allocation_size,
                                                     VkDeviceSize atom_size) noexcept {
    if (size == VK_WHOLE_SIZE) return FlushRange{0, VK_WHOLE_SIZE};
    if (atom_size == 0 || allocation_size == 0) return FlushRange{offset, size};

    const VkDeviceSize start = align_down(offset < allocation_size ? offset : allocation_size, atom_size);
    VkDeviceSize end = align_up(offset + size > allocation_size ? allocation_size : offset + size, atom_size);
    if (end > allocation_size) end = allocation_size;
    if (end <= start) return FlushRange{start, 0};
    return FlushRange{start, end - start};
}

struct GpuAllocatorDesc {
    VkDeviceSize block_size = 16ull * 1024 * 1024;
    VkDeviceSize host_block_size = 4ull * 1024 * 1024;
    /// Allocations bigger than this get a dedicated VkDeviceMemory.
    VkDeviceSize dedicated_threshold = 32ull * 1024 * 1024;
    bool allow_dedicated_allocation = true;
};

class GpuAllocator {
public:
    explicit GpuAllocator(Device& device, const GpuAllocatorDesc& desc = {});
    ~GpuAllocator();

    ORE_NON_MOVABLE(GpuAllocator);

    /// Suballocates device memory matching \p requirements and \p properties.
    [[nodiscard]] Allocation allocate(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags properties,
                                      MemoryKind kind = MemoryKind::Linear, bool force_dedicated = false,
                                      const void* dedicated_next = nullptr);
    void free(const Allocation& allocation);

    struct Stats {
        u64 reserved_bytes = 0;   ///< device memory actually allocated from the driver
        u64 used_bytes = 0;       ///< bytes handed out
        u64 dedicated_bytes = 0;
        u32 block_count = 0;
        u32 live_allocations = 0;
    };
    [[nodiscard]] Stats stats() const;
    [[nodiscard]] std::string dump_stats() const;

    /// Releases empty blocks back to the driver. Call after a level unload, for example.
    void trim();
    /// Frees every block; every outstanding Allocation becomes invalid.
    void release_all();

    [[nodiscard]] Device& device() const { return *device_; }
    [[nodiscard]] VkDeviceSize buffer_image_granularity() const;

private:
    struct Block {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        u32 memory_type = 0;
        VkMemoryPropertyFlags properties = 0;
        VkDeviceSize size = 0;
        BlockAllocator allocator;
        void* mapped = nullptr;
        bool dedicated = false;
        MemoryKind kind = MemoryKind::Linear;
        u64 index = 0;
    };

    [[nodiscard]] Block* find_block(VkDeviceMemory memory);
    [[nodiscard]] Block* create_block(u32 memory_type, VkMemoryPropertyFlags properties, VkDeviceSize size,
                                      MemoryKind kind, const void* dedicated_next);

    Device* device_ = nullptr;
    GpuAllocatorDesc desc_{};
    std::vector<Scope<Block>> blocks_;
    u64 next_block_index_ = 0;
};

} // namespace ore::rhi
