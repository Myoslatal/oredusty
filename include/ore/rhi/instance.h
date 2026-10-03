// Ore framework - VkInstance ownership, validation layers and the debug messenger.
#pragma once

#include <ore/core/types.h>
#include <ore/rhi/vulkan.h>

#include <string>
#include <string_view>
#include <vector>

namespace ore::rhi {

struct InstanceDesc {
    std::string application_name = "Ore Application";
    u32 application_version = VK_MAKE_API_VERSION(0, ORE_VERSION_MAJOR, ORE_VERSION_MINOR, ORE_VERSION_PATCH);
    std::string engine_name = "Ore";
    /// Enables VK_LAYER_KHRONOS_validation and VK_EXT_debug_utils when available.
    bool enable_validation = ORE_ENABLE_VALIDATION != 0;
    /// Extra instance extensions (surface extensions are added by the caller / window).
    std::vector<std::string> extensions;
};

class Instance {
public:
    [[nodiscard]] static Scope<Instance> create(const InstanceDesc& desc = {});
    ~Instance();

    ORE_NON_MOVABLE(Instance);

    [[nodiscard]] VkInstance handle() const { return instance_; }
    [[nodiscard]] const InstanceDesc& desc() const { return desc_; }
    [[nodiscard]] bool validation_enabled() const { return validation_enabled_; }
    [[nodiscard]] bool extension_enabled(std::string_view name) const;
    [[nodiscard]] const std::vector<std::string>& enabled_extensions() const { return enabled_extensions_; }

    [[nodiscard]] static std::vector<std::string> available_extensions();
    [[nodiscard]] static std::vector<std::string> available_layers();
    [[nodiscard]] static bool validation_layer_available();

private:
    Instance() = default;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    InstanceDesc desc_{};
    std::vector<std::string> enabled_extensions_;
    bool validation_enabled_ = false;
};

} // namespace ore::rhi
