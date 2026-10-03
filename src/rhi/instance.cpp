#include <ore/rhi/instance.h>

#include <ore/core/assert.h>

#include <algorithm>
#include <cstring>

namespace ore::rhi {
namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT types,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void* user_data) {
    (void)user_data;
    const char* kind = "general";
    if ((types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0u) kind = "validation";
    else if ((types & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) != 0u) kind = "performance";

    const std::string_view message = data->pMessage != nullptr ? data->pMessage : "";
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0u) {
        ORE_ERROR("[vulkan {}] {}", kind, message);
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0u) {
        ORE_WARN("[vulkan {}] {}", kind, message);
    } else {
        ORE_DEBUG("[vulkan {}] {}", kind, message);
    }
    return VK_FALSE;
}

} // namespace

std::vector<std::string> Instance::available_extensions() {
    u32 count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) return {};
    std::vector<VkExtensionProperties> properties(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, properties.data()) != VK_SUCCESS) return {};
    std::vector<std::string> names;
    names.reserve(count);
    for (const auto& property : properties) names.emplace_back(property.extensionName);
    return names;
}

std::vector<std::string> Instance::available_layers() {
    u32 count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) return {};
    std::vector<VkLayerProperties> properties(count);
    if (vkEnumerateInstanceLayerProperties(&count, properties.data()) != VK_SUCCESS) return {};
    std::vector<std::string> names;
    names.reserve(count);
    for (const auto& property : properties) names.emplace_back(property.layerName);
    return names;
}

bool Instance::validation_layer_available() {
    static const bool available = [] {
        u32 count = 0;
        if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) return false;
        std::vector<VkLayerProperties> properties(count);
        if (vkEnumerateInstanceLayerProperties(&count, properties.data()) != VK_SUCCESS) return false;
        return has_layer(properties, "VK_LAYER_KHRONOS_validation");
    }();
    return available;
}

Scope<Instance> Instance::create(const InstanceDesc& desc) {
    Scope<Instance> instance(new Instance());
    instance->desc_ = desc;

    std::vector<const char*> extensions;
    for (const std::string& extension : desc.extensions) extensions.push_back(extension.c_str());

    const bool want_debug_utils = desc.enable_validation;
    if (want_debug_utils) {
        const auto available = available_extensions();
        if (std::find(available.begin(), available.end(), VK_EXT_DEBUG_UTILS_EXTENSION_NAME) != available.end()) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            instance->enabled_extensions_.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
    }

    std::vector<const char*> layers;
    if (desc.enable_validation) {
        if (validation_layer_available()) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            instance->validation_enabled_ = true;
        } else {
            ORE_WARN("Vulkan validation layers requested but not installed "
                     "(install vulkan-validation-layers)");
        }
    }

    VkApplicationInfo application_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application_info.pApplicationName = desc.application_name.c_str();
    application_info.applicationVersion = desc.application_version;
    application_info.pEngineName = desc.engine_name.c_str();
    application_info.engineVersion = VK_MAKE_API_VERSION(0, ORE_VERSION_MAJOR, ORE_VERSION_MINOR, ORE_VERSION_PATCH);
    application_info.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo create_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create_info.pApplicationInfo = &application_info;
    create_info.enabledExtensionCount = static_cast<u32>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
    create_info.enabledLayerCount = static_cast<u32>(layers.size());
    create_info.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();

    const VkResult result = vkCreateInstance(&create_info, nullptr, &instance->instance_);
    if (result == VK_ERROR_INCOMPATIBLE_DRIVER) {
        ORE_ERROR("vkCreateInstance failed: the loader reports no Vulkan 1.3 capable driver");
        return nullptr;
    }
    if (result != VK_SUCCESS) {
        ORE_ERROR("vkCreateInstance failed: {}", result_string(result));
        return nullptr;
    }

    if (instance->validation_enabled_) {
        auto* create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance->instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (create_messenger != nullptr) {
            VkDebugUtilsMessengerCreateInfoEXT messenger_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                             VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                         VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            messenger_info.pfnUserCallback = &debug_callback;
            if (create_messenger(instance->instance_, &messenger_info, nullptr, &instance->messenger_) != VK_SUCCESS) {
                ORE_WARN("failed to install the Vulkan debug messenger");
            }
        }
    }

    return instance;
}

Instance::~Instance() {
    if (messenger_ != VK_NULL_HANDLE) {
        auto* destroy_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy_messenger != nullptr) destroy_messenger(instance_, messenger_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
}

bool Instance::extension_enabled(std::string_view name) const {
    return std::find(enabled_extensions_.begin(), enabled_extensions_.end(), name) != enabled_extensions_.end();
}

} // namespace ore::rhi
