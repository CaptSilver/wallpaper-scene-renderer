#pragma once
#include "WPJson.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <array>

namespace wallpaper
{
namespace fs
{
class VFS;
}

namespace wpscene
{

class WPModelObject {
public:
    bool                 FromJson(const nlohmann::json&, fs::VFS&);
    int32_t              id { 0 };
    int32_t              parent_id { -1 };
    std::string          name;
    std::string          model;
    bool                 visible { true };
    std::array<float, 3> origin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> scale { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> angles { 0.0f, 0.0f, 0.0f };
};

} // namespace wpscene
} // namespace wallpaper
