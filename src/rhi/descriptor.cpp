#include <ore/rhi/descriptor.h>

#include <ore/core/assert.h>
#include <ore/core/log.h>
#include <ore/rhi/buffer.h>
#include <ore/rhi/texture.h>

#include <algorithm>
#include <format>

namespace ore::rhi {

Scope<DescriptorSetLayout> DescriptorSetLayout::create(Device& device, ConstSpan<DescriptorBinding> bindings,
                                                       std::string_view debug_name) {
    Scope<DescriptorSetLayout> layout(new DescriptorSetLayout());
    layout->device_ = &device;
    layout->bindings_.assign(bindings.begin(), bindings.end());

    std::vector<VkDescriptorSetLayoutBinding> vk_bindings;
    vk_bindings.reserve(bindings.size());
    std::vector<VkDescriptorBindingFlags> vk_flags;
    vk_flags.reserve(bindings.size());
    bool uses_flags = false;
    for (const DescriptorBinding& binding : bindings) {
        VkDescriptorSetLayoutBinding vk_binding{};
        vk_binding.binding = binding.binding;
        vk_binding.descriptorType = binding.type;
        vk_binding.descriptorCount = binding.count;
        vk_binding.stageFlags = binding.stages;
        vk_bindings.push_back(vk_binding);
        vk_flags.push_back(binding.flags);
        if (binding.flags != 0) uses_flags = true;
    }

    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    if (uses_flags) {
        flags_info.bindingCount = static_cast<u32>(vk_flags.size());
        flags_info.pBindingFlags = vk_flags.data();
        info.pNext = &flags_info;
    }
    info.bindingCount = static_cast<u32>(vk_bindings.size());
    info.pBindings = vk_bindings.empty() ? nullptr : vk_bindings.data();
    ORE_VK_CHECK(vkCreateDescriptorSetLayout(device.handle(), &info, nullptr, &layout->layout_));
    if (!debug_name.empty()) {
        device.set_debug_name(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, reinterpret_cast<u64>(layout->layout_), debug_name);
    }
    return layout;
}

DescriptorSetLayout::~DescriptorSetLayout() {
    if (layout_ != VK_NULL_HANDLE && device_ != nullptr) {
        vkDestroyDescriptorSetLayout(device_->handle(), layout_, nullptr);
    }
}

std::optional<DescriptorBinding> DescriptorSetLayout::binding(u32 index) const {
    for (const DescriptorBinding& binding : bindings_) {
        if (binding.binding == index) return binding;
    }
    return std::nullopt;
}

std::vector<VkDescriptorPoolSize> DescriptorSetLayout::pool_sizes(u32 set_count) const {
    std::vector<VkDescriptorPoolSize> sizes;
    for (const DescriptorBinding& binding : bindings_) {
        const u32 needed = binding.count * set_count;
        const auto it = std::find_if(sizes.begin(), sizes.end(),
                                     [&binding](const VkDescriptorPoolSize& size) { return size.type == binding.type; });
        if (it != sizes.end()) {
            it->descriptorCount += needed;
        } else {
            sizes.push_back(VkDescriptorPoolSize{binding.type, needed});
        }
    }
    return sizes;
}

Scope<DescriptorPool> DescriptorPool::create(Device& device, ConstSpan<VkDescriptorPoolSize> sizes, u32 max_sets,
                                             bool free_individual) {
    Scope<DescriptorPool> pool(new DescriptorPool());
    pool->device_ = &device;

    VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    if (free_individual) info.flags |= VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    info.maxSets = max_sets;
    info.poolSizeCount = static_cast<u32>(sizes.size());
    info.pPoolSizes = sizes.empty() ? nullptr : sizes.data();
    ORE_VK_CHECK(vkCreateDescriptorPool(device.handle(), &info, nullptr, &pool->pool_));
    return pool;
}

Scope<DescriptorPool> DescriptorPool::create_for_layouts(Device& device,
                                                         ConstSpan<const DescriptorSetLayout*> layouts,
                                                         u32 sets_per_layout) {
    std::vector<VkDescriptorPoolSize> sizes;
    for (const DescriptorSetLayout* layout : layouts) {
        for (const VkDescriptorPoolSize& size : layout->pool_sizes(sets_per_layout)) {
            const auto it = std::find_if(sizes.begin(), sizes.end(),
                                         [&size](const VkDescriptorPoolSize& entry) { return entry.type == size.type; });
            if (it != sizes.end()) {
                it->descriptorCount += size.descriptorCount;
            } else {
                sizes.push_back(size);
            }
        }
    }
    return create(device, sizes, static_cast<u32>(layouts.size()) * sets_per_layout);
}

DescriptorPool::~DescriptorPool() {
    if (pool_ != VK_NULL_HANDLE && device_ != nullptr) vkDestroyDescriptorPool(device_->handle(), pool_, nullptr);
}

VkDescriptorSet DescriptorPool::allocate(const DescriptorSetLayout& layout, std::string_view debug_name) {
    VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    info.descriptorPool = pool_;
    info.descriptorSetCount = 1;
    const VkDescriptorSetLayout handle = layout.handle();
    info.pSetLayouts = &handle;
    VkDescriptorSet set = VK_NULL_HANDLE;
    const VkResult result = vkAllocateDescriptorSets(device_->handle(), &info, &set);
    if (result != VK_SUCCESS) {
        ORE_ERROR("vkAllocateDescriptorSets failed: {}", result_string(result));
        return VK_NULL_HANDLE;
    }
    if (!debug_name.empty()) {
        device_->set_debug_name(VK_OBJECT_TYPE_DESCRIPTOR_SET, reinterpret_cast<u64>(set), debug_name);
    }
    return set;
}

void DescriptorPool::reset() { ORE_VK_CHECK(vkResetDescriptorPool(device_->handle(), pool_, 0)); }

DescriptorSet::DescriptorSet(Device& device, VkDescriptorSet set, const DescriptorSetLayout& layout)
    : device_(&device), set_(set), layout_(&layout) {}

VkDescriptorType DescriptorSet::type_of(u32 binding) const {
    ORE_ASSERT(layout_ != nullptr);
    const auto info = layout_->binding(binding);
    ORE_ASSERT_MSG(info.has_value(), "descriptor binding {} is not part of the layout", binding);
    return info->type;
}

void DescriptorSet::write_buffer(u32 binding, const Buffer& buffer, u64 offset, u64 range, u32 array_index) {
    write_buffer_raw(binding, buffer.handle(), offset, range, type_of(binding), array_index);
}

void DescriptorSet::write_buffer_raw(u32 binding, VkBuffer buffer, u64 offset, u64 range, VkDescriptorType type,
                                     u32 array_index) {
    ORE_ASSERT(device_ != nullptr && set_ != VK_NULL_HANDLE);
    VkDescriptorBufferInfo buffer_info{};
    buffer_info.buffer = buffer;
    buffer_info.offset = offset;
    buffer_info.range = range;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set_;
    write.dstBinding = binding;
    write.dstArrayElement = array_index;
    write.descriptorCount = 1;
    write.descriptorType = type;
    write.pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(device_->handle(), 1, &write, 0, nullptr);
}

void DescriptorSet::write_texture(u32 binding, const Texture& texture, VkSampler sampler, u32 array_index) {
    write_texture_raw(binding, texture.view(), sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, array_index);
}

void DescriptorSet::write_texture_raw(u32 binding, VkImageView view, VkSampler sampler, VkImageLayout layout,
                                      u32 array_index) {
    ORE_ASSERT(device_ != nullptr && set_ != VK_NULL_HANDLE);
    VkDescriptorImageInfo image_info{};
    image_info.sampler = sampler;
    image_info.imageView = view;
    image_info.imageLayout = layout;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set_;
    write.dstBinding = binding;
    write.dstArrayElement = array_index;
    write.descriptorCount = 1;
    write.descriptorType = type_of(binding);
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(device_->handle(), 1, &write, 0, nullptr);
}

void DescriptorSet::write_storage_image(u32 binding, VkImageView view, VkImageLayout layout, u32 array_index) {
    write_texture_raw(binding, view, VK_NULL_HANDLE, layout, array_index);
}

void DescriptorSet::write_sampler(u32 binding, VkSampler sampler, u32 array_index) {
    ORE_ASSERT(device_ != nullptr && set_ != VK_NULL_HANDLE);
    VkDescriptorImageInfo image_info{};
    image_info.sampler = sampler;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set_;
    write.dstBinding = binding;
    write.dstArrayElement = array_index;
    write.descriptorCount = 1;
    write.descriptorType = type_of(binding);
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(device_->handle(), 1, &write, 0, nullptr);
}

} // namespace ore::rhi
