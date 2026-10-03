// Ore framework - descriptor set layouts, pools and bindings.
#pragma once

#include <ore/core/types.h>
#include <ore/rhi/device.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ore::rhi {

class Buffer;
class Texture;

struct DescriptorBinding {
    u32 binding = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    u32 count = 1;
    VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    /// e.g. VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT with descriptor indexing.
    VkDescriptorBindingFlags flags = 0;
};

class DescriptorSetLayout {
public:
    [[nodiscard]] static Scope<DescriptorSetLayout> create(Device& device, ConstSpan<DescriptorBinding> bindings,
                                                          std::string_view debug_name = {});
    ~DescriptorSetLayout();
    ORE_NON_MOVABLE(DescriptorSetLayout);

    [[nodiscard]] VkDescriptorSetLayout handle() const { return layout_; }
    [[nodiscard]] ConstSpan<DescriptorBinding> bindings() const { return bindings_; }
    [[nodiscard]] std::optional<DescriptorBinding> binding(u32 binding) const;
    /// Total descriptor count per type, used to size descriptor pools.
    [[nodiscard]] std::vector<VkDescriptorPoolSize> pool_sizes(u32 set_count) const;

private:
    DescriptorSetLayout() = default;
    Device* device_ = nullptr;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    std::vector<DescriptorBinding> bindings_;
};

class DescriptorPool {
public:
    [[nodiscard]] static Scope<DescriptorPool> create(Device& device, ConstSpan<VkDescriptorPoolSize> sizes,
                                                     u32 max_sets, bool free_individual = false);
    /// Derives the pool sizes from the layouts, reserving \p sets_per_layout sets for each.
    [[nodiscard]] static Scope<DescriptorPool> create_for_layouts(Device& device,
                                                                 ConstSpan<const DescriptorSetLayout*> layouts,
                                                                 u32 sets_per_layout);
    ~DescriptorPool();
    ORE_NON_MOVABLE(DescriptorPool);

    [[nodiscard]] VkDescriptorSet allocate(const DescriptorSetLayout& layout, std::string_view debug_name = {});
    void reset();
    [[nodiscard]] VkDescriptorPool handle() const { return pool_; }

private:
    DescriptorPool() = default;
    Device* device_ = nullptr;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
};

/// Non-owning handle to a descriptor set. Writes go straight to vkUpdateDescriptorSets;
/// the descriptor type is validated against the layout.
class DescriptorSet {
public:
    DescriptorSet() = default;
    DescriptorSet(Device& device, VkDescriptorSet set, const DescriptorSetLayout& layout);

    [[nodiscard]] VkDescriptorSet handle() const { return set_; }
    [[nodiscard]] bool valid() const { return set_ != VK_NULL_HANDLE; }
    [[nodiscard]] const DescriptorSetLayout& layout() const { return *layout_; }

    void write_buffer(u32 binding, const Buffer& buffer, u64 offset = 0, u64 range = VK_WHOLE_SIZE,
                      u32 array_index = 0);
    void write_buffer_raw(u32 binding, VkBuffer buffer, u64 offset, u64 range, VkDescriptorType type,
                          u32 array_index = 0);
    void write_texture(u32 binding, const Texture& texture, VkSampler sampler, u32 array_index = 0);
    void write_texture_raw(u32 binding, VkImageView view, VkSampler sampler,
                           VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, u32 array_index = 0);
    void write_storage_image(u32 binding, VkImageView view,
                             VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL, u32 array_index = 0);
    void write_sampler(u32 binding, VkSampler sampler, u32 array_index = 0);

private:
    [[nodiscard]] VkDescriptorType type_of(u32 binding) const;

    Device* device_ = nullptr;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    const DescriptorSetLayout* layout_ = nullptr;
};

} // namespace ore::rhi
