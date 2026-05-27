#include <doctest.h>

#include "Vulkan/DebugCallback.hpp" // includes Instance.hpp transitively

#include <cstring>
#include <span>
#include <string>
#include <vector>

using namespace wallpaper::vulkan;

namespace
{

// Stub env getter — simulates std::getenv() against a captured map of
// (name -> value) pairs without touching the real process environment.
struct StubEnv {
    std::vector<std::pair<std::string, std::string>> entries;

    const char* get(const char* name) const {
        for (const auto& [k, v] : entries) {
            if (k == name) return v.c_str();
        }
        return nullptr;
    }
};

} // namespace

TEST_SUITE("Vulkan Instance debug callback severity") {
    TEST_CASE("messageSeverity mask drops INFO + VERBOSE — only WARN + ERROR") {
        const auto mask = debugCallbackMessageSeverity();
        CHECK((mask & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0);
        CHECK((mask & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0);
        // INFO + VERBOSE used to OR in here; layer paid per-event formatting
        // cost for messages the callback body threshold-checked away.  Drop
        // them at the source.
        CHECK((mask & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) == 0u);
        CHECK((mask & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) == 0u);
    }

    TEST_CASE("gate UNSET + no validation layer => debug callback not enabled") {
        StubEnv env; // empty — no WEKDE_VULKAN_VALIDATION
        CHECK(shouldEnableDebugCallback(
                  [&](const char* n) {
                      return env.get(n);
                  },
                  /*has_valid_layer=*/false) == false);
    }

    TEST_CASE("gate SET via WEKDE_VULKAN_VALIDATION => debug callback enabled") {
        StubEnv env;
        env.entries.push_back({ "WEKDE_VULKAN_VALIDATION", "1" });
        CHECK(shouldEnableDebugCallback(
                  [&](const char* n) {
                      return env.get(n);
                  },
                  /*has_valid_layer=*/false) == true);
    }

    TEST_CASE("validation layer enabled (--valid-layer / QML m_enable_valid) => callback enabled") {
        StubEnv env; // env unset — but CLI flag already pulled the layer in
        CHECK(shouldEnableDebugCallback(
                  [&](const char* n) {
                      return env.get(n);
                  },
                  /*has_valid_layer=*/true) == true);
    }

    TEST_CASE("WEKDE_VULKAN_VALIDATION empty string treated as UNSET") {
        // Mirrors the WEKDE_DEBUG_UNIFORM / WEKDE_TIME_DIAG pattern: "set
        // but empty" is ambiguous, so we require a non-empty value to opt in.
        StubEnv env;
        env.entries.push_back({ "WEKDE_VULKAN_VALIDATION", "" });
        CHECK(shouldEnableDebugCallback(
                  [&](const char* n) {
                      return env.get(n);
                  },
                  /*has_valid_layer=*/false) == false);
    }

    TEST_CASE("null env_getter is tolerated (defensive) — falls back to layer flag") {
        // Production code passes std::getenv (non-null), but a defensive
        // null check keeps the helper safe if a future call site forgets.
        CHECK(shouldEnableDebugCallback(nullptr, /*has_valid_layer=*/false) == false);
        CHECK(shouldEnableDebugCallback(nullptr, /*has_valid_layer=*/true) == true);
    }
}

TEST_SUITE("Vulkan Instance debug-utils extension required-bit") {
    // The validation gate is moot if Instance::Create refuses to start on
    // drivers without VK_EXT_debug_utils.  Mark the base extension OPTIONAL
    // (required=false) so minimal Vulkan stacks (RDP sessions, older Mesa
    // builds, hardware Vulkan-1.0 paths) can still create an instance —
    // they just don't get a debug callback.  This locks the contract
    // against future audits silently flipping required back to true.

    TEST_CASE("kDebugUtilsExt is single-element with required=false") {
        CHECK(kDebugUtilsExt.size() == 1u);
        CHECK(kDebugUtilsExt[0].required == false);
        CHECK(std::strcmp(kDebugUtilsExt[0].name.data(), VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0);
    }
}
