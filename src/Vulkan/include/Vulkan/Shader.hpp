#pragma once
#include "Instance.hpp"
#include "ShaderComp.hpp"
#include <glslang/Include/BaseTypes.h>
#include <string>
#include <string_view>
#include <unordered_map>

namespace wallpaper
{
namespace vulkan
{

// Transparent string hash + equality so unordered_map<std::string, V>
// supports heterogeneous find(std::string_view) without constructing a
// std::string per probe.  Previously member_map was std::map<..., std::less<>>,
// which already accepted std::string_view via the transparent comparator;
// preserving that no-allocation lookup is the whole point of the swap.
struct TransparentStringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view> {}(sv);
    }
    std::size_t operator()(const std::string& s) const noexcept {
        return std::hash<std::string_view> {}(s);
    }
    std::size_t operator()(const char* p) const noexcept {
        return std::hash<std::string_view> {}(std::string_view { p });
    }
};

VkFormat ToVkType(glslang::TBasicType, size_t);

struct ShaderReflected {
    struct BlockedUniform {
        int    block_index;
        uint   offset;
        size_t size { 0 };
        size_t num { 1 }; // for array,vector,matrix
    };
    struct Block {
        int         index;
        uint        size;
        std::string name;

        // Per-uniform name lookup runs on EVERY uniform written every frame
        // (CustomShaderPass::UpdateUniform → member_map.find(name)).  The
        // keys carry no ordering requirement — they came from glslang /
        // SPIRV-Reflect in declaration order and consumers do point lookups
        // (find/exists), never range walks where order is observable.  An
        // unordered_map gives O(1) average lookup vs std::map's O(log N · K)
        // RB-tree probe (K inflated by shared "g_Model*" string prefixes).
        //
        // Transparent hash + equal_to<> are required so find(string_view)
        // does NOT construct a std::string per probe — that would re-introduce
        // an allocation in the hot path the swap was meant to remove.
        //
        // Forward hook: a future per-light uniform split (lights/ambient)
        // can hang per-light data off the same hash table without
        // restructuring the lookup contract.
        std::unordered_map<std::string, BlockedUniform, TransparentStringHash, std::equal_to<>>
            member_map;
    };
    std::vector<Block> blocks;

    Map<std::string, VkDescriptorSetLayoutBinding> binding_map;

    struct Input {
        uint     location;
        VkFormat format;
    };
    Map<std::string, Input> input_location_map;
};

bool GenReflect(std::span<const std::vector<uint>> codes, std::vector<Uni_ShaderSpv>& spvs,
                ShaderReflected& ref);
} // namespace vulkan
} // namespace wallpaper
