// Ore framework - Vulkan helpers shared by the render hardware interface.
#pragma once

#include <ore/core/assert.h>
#include <ore/core/log.h>
#include <ore/core/types.h>

#include <vulkan/vulkan.h>

#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace ore::rhi {

[[nodiscard]] const char* result_string(VkResult result);
[[nodiscard]] std::string api_version_string(u32 version);
[[nodiscard]] std::string format_string(VkFormat format);
[[nodiscard]] bool is_depth_format(VkFormat format);
[[nodiscard]] bool is_srgb_format(VkFormat format);

/// Logs a fatal error when \p result is not VK_SUCCESS.
void check_result(VkResult result, const char* expression, const std::source_location& loc);

[[nodiscard]] bool has_extension(ConstSpan<VkExtensionProperties> extensions, std::string_view name);
[[nodiscard]] bool has_layer(ConstSpan<VkLayerProperties> layers, std::string_view name);

/// Queue a debug label / marker through VK_EXT_debug_utils when the extension is enabled.
struct DebugUtilsFunctions {
    PFN_vkSetDebugUtilsObjectNameEXT set_object_name = nullptr;
    PFN_vkCmdBeginDebugUtilsLabelEXT cmd_begin_label = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT cmd_end_label = nullptr;
    PFN_vkCmdInsertDebugUtilsLabelEXT cmd_insert_label = nullptr;

    [[nodiscard]] bool valid() const { return set_object_name != nullptr; }
};

} // namespace ore::rhi

/// Fails loudly (log + abort) when a Vulkan call does not return VK_SUCCESS.
#define ORE_VK_CHECK(expr) ::ore::rhi::check_result((expr), #expr, std::source_location::current())
