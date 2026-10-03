#include <ore/rhi/allocator.h>

#include <ore/core/assert.h>
#include <ore/core/log.h>

#include <algorithm>
#include <format>

namespace ore::rhi {
namespace {

constexpr VkDeviceSize kSuballocationAlignment = 16;

[[nodiscard]] bool is_host_visible(VkMemoryPropertyFlags properties) {
    return (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0u;
}

} // namespace

GpuAllocator::GpuAllocator(Device& device, const GpuAllocatorDesc& desc) : device_(&device), desc_(desc) {}

GpuAllocator::~GpuAllocator() { release_all(); }

VkDeviceSize GpuAllocator::buffer_image_granularity() const {
    return std::max<VkDeviceSize>(device_->properties().limits.bufferImageGranularity, 1);
}

GpuAllocator::Block* GpuAllocator::create_block(u32 memory_type, VkMemoryPropertyFlags properties, VkDeviceSize size,
                                               MemoryKind kind, const void* dedicated_next) {
    auto block = make_scope<Block>();
    block->memory_type = memory_type;
    block->properties = properties;
    block->size = size;
    block->kind = kind;
    block->index = next_block_index_++;
    block->dedicated = dedicated_next != nullptr;
    block->allocator.reset(static_cast<usize>(size), static_cast<usize>(kSuballocationAlignment));

    VkMemoryAllocateInfo allocate_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate_info.pNext = dedicated_next;
    allocate_info.allocationSize = size;
    allocate_info.memoryTypeIndex = memory_type;

    const VkResult result = vkAllocateMemory(device_->handle(), &allocate_info, nullptr, &block->memory);
    if (result != VK_SUCCESS) {
        ORE_ERROR("vkAllocateMemory failed ({} bytes, type {}): {}", size, memory_type, result_string(result));
        return nullptr;
    }

    if (is_host_visible(properties)) {
        void* mapped = nullptr;
        if (vkMapMemory(device_->handle(), block->memory, 0, size, 0, &mapped) == VK_SUCCESS) {
            block->mapped = mapped;
        } else {
            ORE_WARN("vkMapMemory failed for a host visible block; falling back to per-copy staging");
        }
    }

    Block* raw = block.get();
    blocks_.push_back(std::move(block));
    ORE_DEBUG("allocator: reserved {} KiB block #{} (memory type {}, {})", size / 1024, raw->index, memory_type,
              kind == MemoryKind::Linear ? "linear" : "non-linear");
    return raw;
}

GpuAllocator::Block* GpuAllocator::find_block(VkDeviceMemory memory) {
    for (const Scope<Block>& block : blocks_) {
        if (block->memory == memory) return block.get();
    }
    return nullptr;
}

Allocation GpuAllocator::allocate(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags properties,
                                  MemoryKind kind, bool force_dedicated, const void* dedicated_next) {
    const auto memory_type = device_->find_memory_type(requirements.memoryTypeBits, properties);
    if (!memory_type.has_value()) {
        ORE_ERROR("no memory type satisfies properties 0x{:x} (requirements 0x{:x})", properties,
                  requirements.memoryTypeBits);
        return {};
    }

    const VkDeviceSize alignment = std::max<VkDeviceSize>(requirements.alignment, kSuballocationAlignment);
    const bool dedicated = force_dedicated || dedicated_next != nullptr ||
                           requirements.size >= desc_.dedicated_threshold;

    // Without buffer/image granularity restrictions a single pool per memory type is safe.
    const bool split_kinds = buffer_image_granularity() > 1;
    const MemoryKind pool_kind = split_kinds ? kind : MemoryKind::Linear;

    // A request for host visible memory must never be served from a block that was created for
    // device local use: on integrated GPUs both requests resolve to the same memory type, but only
    // the block created for the host visible request was mapped. Serving the unmapped block makes
    // Buffer::write() fail (or, worse, write to no mapping at all).
    const bool needs_mapping = is_host_visible(properties);

    auto try_block = [&](Block* block) -> Allocation {
        const auto allocation = block->allocator.allocate(static_cast<usize>(requirements.size),
                                                          static_cast<usize>(alignment));
        if (!allocation.has_value()) return {};
        Allocation out;
        out.memory = block->memory;
        out.offset = allocation->offset;
        out.size = allocation->size;
        out.mapped = block->mapped;
        out.block_index = static_cast<u32>(block->index);
        out.memory_type = block->memory_type;
        return out;
    };

    if (!dedicated) {
        Block* best = nullptr;
        for (const Scope<Block>& block : blocks_) {
            if (block->memory_type != *memory_type || block->kind != pool_kind || block->dedicated) continue;
            if (needs_mapping && block->mapped == nullptr) continue;
            if (block->allocator.largest_free_block() < requirements.size) continue;
            if (best == nullptr || block->allocator.available() > best->allocator.available()) best = block.get();
        }
        if (best != nullptr) {
            Allocation allocation = try_block(best);
            if (allocation.valid()) return allocation;
        }
    }

    const VkDeviceSize base_block_size =
        is_host_visible(properties) ? desc_.host_block_size : desc_.block_size;
    const VkDeviceSize block_size = std::max(base_block_size, align_up(requirements.size + alignment, 4096));
    Block* block = create_block(*memory_type, properties, block_size, pool_kind, dedicated ? dedicated_next : nullptr);
    if (block == nullptr) return {};

    Allocation allocation = try_block(block);
    if (!allocation.valid()) {
        ORE_ERROR("freshly created block could not satisfy a {} byte allocation", requirements.size);
        return allocation;
    }
    if (needs_mapping && allocation.mapped == nullptr) {
        // Never hand out an unmappable allocation for a host visible request: report it instead of
        // letting the caller write into a null mapping.
        ORE_ERROR("allocator: host visible allocation of {} bytes could not be mapped "
                  "(memory type {}, properties 0x{:x})",
                  requirements.size, block->memory_type, properties);
        block->allocator.free(BlockAllocator::Allocation{static_cast<usize>(allocation.offset),
                                                         static_cast<usize>(allocation.size)});
        return {};
    }
    return allocation;
}

void GpuAllocator::free(const Allocation& allocation) {
    if (!allocation.valid()) return;
    Block* block = find_block(allocation.memory);
    if (block == nullptr) {
        ORE_ERROR("allocator: freeing memory from an unknown block");
        return;
    }
    block->allocator.free(BlockAllocator::Allocation{static_cast<usize>(allocation.offset),
                                                     static_cast<usize>(allocation.size)});
    if (block->allocator.empty()) {
        ORE_DEBUG("allocator: block #{} is empty ({} KiB)", block->index, block->size / 1024);
    }
}

GpuAllocator::Stats GpuAllocator::stats() const {
    Stats stats;
    for (const Scope<Block>& block : blocks_) {
        stats.reserved_bytes += block->size;
        stats.used_bytes += block->allocator.used();
        if (block->dedicated) stats.dedicated_bytes += block->size;
    }
    stats.block_count = static_cast<u32>(blocks_.size());
    for (const Scope<Block>& block : blocks_) stats.live_allocations += block->allocator.allocation_count();
    return stats;
}

std::string GpuAllocator::dump_stats() const {
    const Stats s = stats();
    std::string out = std::format("gpu allocator: {} block(s), reserved {:.2f} MiB, used {:.2f} MiB, "
                                  "{} live allocation(s)",
                                  s.block_count, static_cast<f64>(s.reserved_bytes) / (1024.0 * 1024.0),
                                  static_cast<f64>(s.used_bytes) / (1024.0 * 1024.0), s.live_allocations);
    for (const Scope<Block>& block : blocks_) {
        std::format_to(std::back_inserter(out), "\n  block #{}: {:.2f} MiB, used {:.2f} MiB, {} region(s), type {}", block->index,
                       static_cast<f64>(block->size) / (1024.0 * 1024.0),
                       static_cast<f64>(block->allocator.used()) / (1024.0 * 1024.0),
                       block->allocator.free_region_count(), block->memory_type);
    }
    return out;
}

void GpuAllocator::trim() {
    for (auto it = blocks_.begin(); it != blocks_.end();) {
        Block* block = it->get();
        if (block->allocator.empty()) {
            if (block->mapped != nullptr) vkUnmapMemory(device_->handle(), block->memory);
            vkFreeMemory(device_->handle(), block->memory, nullptr);
            ORE_DEBUG("allocator: released empty block #{}", block->index);
            it = blocks_.erase(it);
        } else {
            ++it;
        }
    }
}

void GpuAllocator::release_all() {
    for (const Scope<Block>& block : blocks_) {
        if (block->mapped != nullptr) vkUnmapMemory(device_->handle(), block->memory);
        vkFreeMemory(device_->handle(), block->memory, nullptr);
    }
    blocks_.clear();
}

} // namespace ore::rhi
