#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <functional>
#include <string_view>

#include "Instance.hpp" // wallpaper::vulkan::Extension definition

namespace wallpaper
{
namespace vulkan
{

// VK_EXT_debug_utils is OPTIONAL.  It's only useful when validation layers
// are loaded (the callback only fires through the layer chain), and a
// surprising number of minimal Vulkan stacks (RDP sessions, embedded
// builds, older Mesa builds on hardware Vulkan-1.0 drivers) don't expose
// it.  Marking it required=true used to fail Instance::Create outright on
// those stacks — plasmashell saw the dlopen surface fail.  Flip to
// required=false: the gate in shouldEnableDebugCallback() decides whether
// we actually try to install the messenger.
inline constexpr std::array kDebugUtilsExt { Extension { /*required=*/false,
                                                         VK_EXT_DEBUG_UTILS_EXTENSION_NAME } };

// Returns the messageSeverity mask the debug-utils messenger subscribes to.
// Only WARNING + ERROR — INFO + VERBOSE are dropped at the source so the
// validation layer doesn't pay per-event formatting cost (severity tag,
// label-stack walk, source-file resolution) for messages the callback
// would have to threshold-check out.
inline VkDebugUtilsMessageSeverityFlagsEXT debugCallbackMessageSeverity() noexcept {
    return VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
}

// Decide whether to set up the debug-utils messenger.
//   env_getter      reads an env var by name (mirrors std::getenv signature);
//                   pass a closure in tests, std::getenv in production.
//   has_valid_layer true if VK_LAYER_KHRONOS_validation was successfully
//                   resolved into the instance's layer list (sceneviewer's
//                   --valid-layer / QML SceneBackend::m_enable_valid).
//
// Enabled when either:
//   - WEKDE_VULKAN_VALIDATION is set in the environment (any non-empty value),
//     OR
//   - the validation layer is already enabled on the instance.
//
// In production (validation layer off, no env var) the messenger is skipped
// entirely — no extension required, no callback object allocated, no
// validation-layer formatting work.
inline bool shouldEnableDebugCallback(const std::function<const char*(const char*)>& env_getter,
                                      bool has_valid_layer) noexcept {
    if (has_valid_layer) return true;
    if (env_getter) {
        const char* v = env_getter("WEKDE_VULKAN_VALIDATION");
        if (v && v[0] != '\0') return true;
    }
    return false;
}

} // namespace vulkan
} // namespace wallpaper
