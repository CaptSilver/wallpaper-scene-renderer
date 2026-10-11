#pragma once
#include <span>
#include <mutex>
#include "Spv.hpp"
#include <glslang/Public/ShaderLang.h>

namespace wallpaper
{
namespace vulkan
{

extern const TBuiltInResource DefaultTBuiltInResource;

// glslang's compiler keeps process-wide state and is not thread-safe. Every
// caller that compiles shaders -- the scene parser on the loader thread and
// the renderer's own built-in shaders -- holds this.
std::mutex& GlslangSerialiseMutex();

struct ShaderCompUnit {
    EShLanguage stage;
    std::string src;
};

struct ShaderCompOpt {
    glslang::EShTargetClientVersion client_ver;

    bool hlsl { false };
    bool auto_map_locations { false };
    bool auto_map_bindings { false };
    // suppress GLSL warnings, except as required by "#extension : warn"
    bool suppress_warnings_glsl { false };
    // relaxed GLSL semantic error-checking mode
    bool relaxed_errors_glsl { false };
    // allowing the use of default uniforms, atomic_uints, and gl_VertexID and gl_InstanceID
    // keywords
    bool relaxed_rules_vulkan { false };
    // for global unifom block
    uint global_uniform_binding { 0 };
};

bool CompileAndLinkShaderUnits(std::span<const ShaderCompUnit> compUnits, const ShaderCompOpt& opt,
                               std::vector<Uni_ShaderSpv>& spvs);
} // namespace vulkan
} // namespace wallpaper
