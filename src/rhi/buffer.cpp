#include <ore/rhi/buffer.h>

#include <ore/core/assert.h>
#include <ore/core/log.h>

#include <cstring>

namespace ore::rhi {

VkBufferUsageFlags to_vk_usage(BufferUsage usage) {
    VkBufferUsageFlags flags = 0;
    if (has_flag(usage, BufferUsage::Vertex)) flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (has_flag(usage, BufferUsage::Index)) flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (has_flag(usage, BufferUsage::Uniform)) flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (has_flag(usage, BufferUsage::Storage)) flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (has_flag(usage, BufferUsage::Indirect)) flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (has_flag(usage, BufferUsage::TransferSrc)) flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (has_flag(usage, BufferUsage::TransferDst)) flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (flags == 0) flags = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    // Shader device addresses are cheap to enable and let users opt into BDA later.
    flags |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    return flags;
}

Scope<Buffer> Buffer::create(GpuAllocator& allocator, const BufferDesc& desc) {
    if (desc.size == 0) {
        ORE_ERROR("Buffer::create: size must not be zero ({})", desc.debug_name);
        return nullptr;
    }

    Scope<Buffer> buffer(new Buffer());
    buffer->allocator_ = &allocator;
    buffer->size_ = desc.size;
    buffer->usage_ = desc.usage;
    buffer->host_visible_ = desc.host_visible;
    buffer->name_ = desc.debug_name;

    VkBufferCreateInfo create_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    create_info.size = desc.size;
    create_info.usage = to_vk_usage(desc.usage);
    create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ORE_VK_CHECK(vkCreateBuffer(allocator.device().handle(), &create_info, nullptr, &buffer->buffer_));

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(allocator.device().handle(), buffer->buffer_, &requirements);

    const VkMemoryPropertyFlags properties =
        desc.host_visible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                          : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    buffer->allocation_ = allocator.allocate(requirements, properties, MemoryKind::Linear);
    if (!buffer->allocation_.valid()) {
        ORE_ERROR("Buffer::create: out of memory ({} bytes)", desc.size);
        vkDestroyBuffer(allocator.device().handle(), buffer->buffer_, nullptr);
        return nullptr;
    }

    ORE_VK_CHECK(vkBindBufferMemory(allocator.device().handle(), buffer->buffer_, buffer->allocation_.memory,
                                    buffer->allocation_.offset));

    // Tag the allocation as coherent when the chosen memory type allows skipping flushes.
    const auto& memory_properties = allocator.device().memory_properties();
    for (u32 i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) != 0u &&
            memory_properties.memoryTypes[i].propertyFlags == properties) {
            buffer->coherent_ = (properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u;
            break;
        }
    }

    if (!desc.debug_name.empty()) {
        allocator.device().set_debug_name(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<u64>(buffer->buffer_),
                                          desc.debug_name);
    }
    return buffer;
}

Buffer::~Buffer() {
    if (buffer_ != VK_NULL_HANDLE && allocator_ != nullptr) {
        vkDestroyBuffer(allocator_->device().handle(), buffer_, nullptr);
        allocator_->free(allocation_);
    }
}

void Buffer::write(ConstSpan<u8> data, u64 offset) const {
    ORE_ASSERT_MSG(mapped(), "Buffer::write requires host visible memory ({})", name_);
    ORE_ASSERT_MSG(offset + data.size() <= size_, "Buffer::write out of range ({})", name_);
    std::memcpy(static_cast<u8*>(mapped_data()) + offset, data.data(), data.size());
    if (!coherent_) flush(offset, data.size());
}

void Buffer::flush(u64 offset, u64 size) const {
    if (coherent_ || !mapped()) return;
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = allocation_.memory;
    range.offset = allocation_.offset + offset;
    range.size = size == VK_WHOLE_SIZE ? VK_WHOLE_SIZE : size;
    ORE_VK_CHECK(vkFlushMappedMemoryRanges(allocator_->device().handle(), 1, &range));
}

void Buffer::invalidate(u64 offset, u64 size) const {
    if (coherent_ || !mapped()) return;
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = allocation_.memory;
    range.offset = allocation_.offset + offset;
    range.size = size == VK_WHOLE_SIZE ? VK_WHOLE_SIZE : size;
    ORE_VK_CHECK(vkInvalidateMappedMemoryRanges(allocator_->device().handle(), 1, &range));
}

VkDescriptorBufferInfo Buffer::descriptor_info(u64 offset, u64 range) const {
    VkDescriptorBufferInfo info{};
    info.buffer = buffer_;
    info.offset = offset;
    info.range = range;
    return info;
}

VkDeviceAddress Buffer::device_address() const {
    VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = buffer_;
    return vkGetBufferDeviceAddress(allocator_->device().handle(), &info);
}

} // namespace ore::rhi
