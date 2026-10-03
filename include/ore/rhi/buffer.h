// Ore framework - GPU buffers with allocator-backed memory.
#pragma once

#include <ore/core/types.h>
#include <ore/rhi/allocator.h>
#include <ore/rhi/device.h>

#include <string>
#include <string_view>

namespace ore::rhi {

enum class BufferUsage : u32 {
    None = 0,
    Vertex = 1u << 0,
    Index = 1u << 1,
    Uniform = 1u << 2,
    Storage = 1u << 3,
    Indirect = 1u << 4,
    TransferSrc = 1u << 5,
    TransferDst = 1u << 6,
};

[[nodiscard]] constexpr BufferUsage operator|(BufferUsage a, BufferUsage b) {
    return static_cast<BufferUsage>(static_cast<u32>(a) | static_cast<u32>(b));
}
[[nodiscard]] constexpr bool has_flag(BufferUsage value, BufferUsage flag) {
    return (static_cast<u32>(value) & static_cast<u32>(flag)) != 0u;
}
[[nodiscard]] VkBufferUsageFlags to_vk_usage(BufferUsage usage);

struct BufferDesc {
    u64 size = 0;
    BufferUsage usage = BufferUsage::Vertex;
    /// Host visible memory (mappable). Use for per-frame uniform data and staging.
    bool host_visible = false;
    std::string debug_name;
};

class Buffer {
public:
    [[nodiscard]] static Scope<Buffer> create(GpuAllocator& allocator, const BufferDesc& desc);
    /// Convenience: a host visible buffer holding \p count elements of T.
    template <class T>
    [[nodiscard]] static Scope<Buffer> create_host(GpuAllocator& allocator, u64 count, BufferUsage usage,
                                                   std::string_view debug_name = {}) {
        BufferDesc desc;
        desc.size = static_cast<u64>(sizeof(T)) * count;
        desc.usage = usage;
        desc.host_visible = true;
        desc.debug_name = std::string(debug_name);
        return create(allocator, desc);
    }
    template <class T>
    [[nodiscard]] static Scope<Buffer> create_device(GpuAllocator& allocator, u64 count, BufferUsage usage,
                                                     std::string_view debug_name = {}) {
        BufferDesc desc;
        desc.size = static_cast<u64>(sizeof(T)) * count;
        desc.usage = usage;
        desc.host_visible = false;
        desc.debug_name = std::string(debug_name);
        return create(allocator, desc);
    }

    ~Buffer();
    ORE_NON_MOVABLE(Buffer);

    [[nodiscard]] VkBuffer handle() const { return buffer_; }
    [[nodiscard]] u64 size() const { return size_; }
    [[nodiscard]] BufferUsage usage() const { return usage_; }
    [[nodiscard]] bool host_visible() const { return host_visible_; }
    [[nodiscard]] bool mapped() const { return allocation_.mapped != nullptr; }
    [[nodiscard]] void* mapped_data() const { return allocation_.mapped_at(); }
    [[nodiscard]] const Allocation& allocation() const { return allocation_; }

    /// Copies bytes into host visible memory. Aborts when the buffer is not mappable.
    void write(ConstSpan<u8> data, u64 offset = 0) const;
    template <class T>
    void write(const T& value, u64 offset = 0) const {
        write(ConstSpan<u8>(reinterpret_cast<const u8*>(&value), sizeof(T)), offset);
    }
    template <class T>
    void write_array(ConstSpan<T> values, u64 offset = 0) const {
        write(ConstSpan<u8>(reinterpret_cast<const u8*>(values.data()), values.size() * sizeof(T)), offset);
    }
    /// Flushes a non-coherent range to the device. No-op for coherent memory.
    void flush(u64 offset = 0, u64 size = VK_WHOLE_SIZE) const;
    /// Invalidates a non-coherent range before host reads.
    void invalidate(u64 offset = 0, u64 size = VK_WHOLE_SIZE) const;

    [[nodiscard]] VkDescriptorBufferInfo descriptor_info(u64 offset = 0, u64 range = VK_WHOLE_SIZE) const;
    [[nodiscard]] VkDeviceAddress device_address() const;

private:
    Buffer() = default;

    GpuAllocator* allocator_ = nullptr;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    Allocation allocation_{};
    u64 size_ = 0;
    BufferUsage usage_ = BufferUsage::None;
    bool host_visible_ = false;
    bool coherent_ = true;
    std::string name_;
};

} // namespace ore::rhi
