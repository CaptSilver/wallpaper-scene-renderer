#pragma once
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <unordered_map>
#include <optional>

#include "Utils/Logging.h"

namespace wallpaper
{

// Stores and provides access to user-configurable properties from project.json
class WPUserProperties {
public:
    WPUserProperties() = default;

    // Load properties from project.json content
    bool LoadFromProjectJson(const std::string& projectJsonContent) {
        try {
            auto json = nlohmann::json::parse(projectJsonContent);
            if (json.contains("general") && json["general"].contains("properties")) {
                const auto& props = json["general"]["properties"];
                for (auto it = props.begin(); it != props.end(); ++it) {
                    const std::string& name = it.key();
                    const auto& prop = it.value();
                    if (prop.contains("value")) {
                        m_properties[name] = prop["value"];
                        m_defaults[name]   = prop["value"];
                        if (prop.contains("type") && prop["type"].is_string())
                            m_types[name] = prop["type"].get<std::string>();
                        LOG_INFO("User property: %s = %s", name.c_str(),
                                 prop["value"].dump().c_str());
                    }
                }
                return true;
            }
        } catch (const nlohmann::json::exception& e) {
            LOG_ERROR("Failed to parse project.json for user properties: %s", e.what());
        }
        return false;
    }

    // Check if a property exists
    bool HasProperty(const std::string& name) const {
        return m_properties.count(name) > 0;
    }

    // Get property value as JSON (returns nullopt if not found)
    std::optional<nlohmann::json> GetProperty(const std::string& name) const {
        auto it = m_properties.find(name);
        if (it != m_properties.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    // Get property default value (original from project.json, never overridden)
    std::optional<nlohmann::json> GetDefault(const std::string& name) const {
        auto it = m_defaults.find(name);
        if (it != m_defaults.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    // Get property type ("bool", "combo", "slider", etc.)
    std::optional<std::string> GetType(const std::string& name) const {
        auto it = m_types.find(name);
        if (it != m_types.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    // Resolve a JSON value that may contain user property reference
    // Handles: {"user": "propname", "value": default}
    // Also handles: {"user": {"condition": "x", "name": "propname"}, "value": default}
    nlohmann::json ResolveValue(const nlohmann::json& json) const {
        if (!json.is_object()) {
            return json;
        }

        if (!json.contains("user")) {
            return json;
        }

        const auto& userField = json["user"];
        std::string propName;
        std::string condition;

        if (userField.is_string()) {
            // Simple case: {"user": "propname", "value": default}
            propName = userField.get<std::string>();
        } else if (userField.is_object() && userField.contains("name")) {
            // Conditional case: {"user": {"condition": "x", "name": "propname"}, "value": default}
            propName = userField["name"].get<std::string>();
            if (userField.contains("condition")) {
                condition = userField["condition"].get<std::string>();
            }
        } else {
            // Unknown format, return value or original
            if (json.contains("value")) {
                return json["value"];
            }
            return json;
        }

        // Get the user property value
        auto propValue = GetProperty(propName);
        if (!propValue.has_value()) {
            // Property not found, use default value
            if (json.contains("value")) {
                return json["value"];
            }
            return json;
        }

        // Simple user-string format with no condition.
        // If binding expects bool but property is non-bool (combo returning "6"),
        // use default-flip: visible = (current==default) ? visDefault : !visDefault
        if (condition.empty()) {
            if (json.contains("value") && json["value"].is_boolean() && !propValue->is_boolean()) {
                bool visDefault = json["value"].get<bool>();
                auto defaultVal = GetDefault(propName);
                if (defaultVal.has_value()) {
                    std::string currentStr =
                        propValue->is_string() ? propValue->get<std::string>() : propValue->dump();
                    std::string defaultStr =
                        defaultVal->is_string() ? defaultVal->get<std::string>()
                                                : defaultVal->dump();
                    return nlohmann::json((currentStr == defaultStr) ? visDefault : !visDefault);
                }
                return json["value"]; // no default available, use binding default
            }
        }

        // Handle condition checking for visibility
        if (!condition.empty()) {
            // For combo properties, check if value matches condition
            std::string propStr = propValue->is_string()
                ? propValue->get<std::string>()
                : propValue->dump();

            bool matches = (propStr == condition);

            // For bool visibility, return whether condition matches
            if (json.contains("value") && json["value"].is_boolean()) {
                return nlohmann::json(matches);
            }
        }

        return *propValue;
    }

    // Insert all property names into a set (for activeBindings)
    void InsertAllNames(std::set<std::string>& out) const {
        for (const auto& [name, _] : m_properties)
            out.insert(name);
    }

    // Check if empty
    bool Empty() const { return m_properties.empty(); }

    // Serialize all properties to JSON string: {"name": value, ...}
    std::string ToJson() const {
        nlohmann::json j;
        for (const auto& [name, value] : m_properties)
            j[name] = value;
        return j.dump();
    }

    // Set a single property value (used for overrides)
    void SetProperty(const std::string& name, const nlohmann::json& value) {
        m_properties[name] = value;
        LOG_INFO("User property override: %s = %s", name.c_str(), value.dump().c_str());
    }

    // Apply overrides from JSON string (format: {"propname": value, ...})
    bool ApplyOverrides(const std::string& jsonStr) {
        if (jsonStr.empty()) return true;
        try {
            auto json = nlohmann::json::parse(jsonStr);
            if (!json.is_object()) {
                LOG_ERROR("User properties override is not an object");
                return false;
            }
            for (auto it = json.begin(); it != json.end(); ++it) {
                SetProperty(it.key(), it.value());
            }
            return true;
        } catch (const nlohmann::json::exception& e) {
            LOG_ERROR("Failed to parse user properties override: %s", e.what());
            return false;
        }
    }

private:
    std::unordered_map<std::string, nlohmann::json> m_properties;
    std::unordered_map<std::string, nlohmann::json> m_defaults; // original from project.json
    std::unordered_map<std::string, std::string>    m_types;    // "bool", "combo", "slider", etc.
};

// Global thread-local pointer to current user properties context
// This is set during scene parsing and used by GetJsonValue
inline thread_local const WPUserProperties* g_currentUserProperties = nullptr;

class UserPropertiesScope {
public:
    explicit UserPropertiesScope(const WPUserProperties* props)
        : m_previous(g_currentUserProperties) {
        g_currentUserProperties = props;
    }
    ~UserPropertiesScope() {
        g_currentUserProperties = m_previous;
    }
private:
    const WPUserProperties* m_previous;
};

} // namespace wallpaper
