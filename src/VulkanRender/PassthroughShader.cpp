#include "PassthroughShader.hpp"

#include <array>
#include <mutex>
#include <string>
#include <string_view>

#include "Vulkan/ShaderComp.hpp"
#include "Utils/Logging.h"

using namespace wallpaper;
using namespace wallpaper::vulkan;

namespace
{

// Hand-written passthrough pair for a hideable pass (in the spirit
// of FinPass.cpp's own hand-rolled quad shaders).  The real material's
// translated-HLSL vertex shader gives no guarantee about which varyings it
// exposes or at which locations, so the passthrough can't reuse it -- it
// draws the hideable pass's own mesh with its own tiny shader pair instead,
// sampling the chain's already-resolved input 1:1.  a_Position/a_TexCoord
// match every mesh's vertex-attribute names (WPSceneParser's WE_IN_POSITION/
// WE_IN_TEXCOORD), so the same mesh vertex buffers bind unchanged.
constexpr std::string_view kPassthroughVertCode = R"(#version 320 es
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;
layout(location = 0) out vec2 v_Texcoord;

layout(binding = 0) uniform PassthroughBlock {
    mat4 g_ModelViewProjectionMatrix;
};

void main()
{
    v_Texcoord = a_TexCoord;
    gl_Position = g_ModelViewProjectionMatrix * vec4(a_Position, 1.0);
}
)";

// No color transform: passing the sampled texel straight through IS "the
// layer's own image" for a single-effect chain, or "the last visible
// effect's output" for a longer one -- see CustomShaderPass.hpp's
// DecideHideablePass and the design note above `hideable`.
constexpr std::string_view kPassthroughFragCode = R"(#version 320 es
precision highp float;
layout(location = 0) in vec2 v_Texcoord;
layout(location = 0) out vec4 out_FragColor;

layout(binding = 1) uniform sampler2D g_Texture0;

void main()
{
    out_FragColor = texture(g_Texture0, v_Texcoord);
}
)";

std::mutex                     s_cacheMutex;
bool                           s_tried { false };
bool                           s_ok { false };
std::vector<std::vector<uint>> s_spirv;
std::size_t                    s_compiles { 0 };

} // namespace

bool wallpaper::vulkan::PassthroughSpirv(std::vector<std::vector<uint>>& out) {
    std::lock_guard<std::mutex> cache(s_cacheMutex);
    if (! s_tried) {
        s_tried = true;
        s_compiles++;
        ShaderCompOpt opt;
        opt.client_ver             = glslang::EShTargetVulkan_1_1;
        opt.relaxed_errors_glsl    = true;
        opt.relaxed_rules_vulkan   = true;
        opt.suppress_warnings_glsl = true;
        std::array<ShaderCompUnit, 2> units {
            ShaderCompUnit { EShLangVertex, std::string(kPassthroughVertCode) },
            ShaderCompUnit { EShLangFragment, std::string(kPassthroughFragCode) },
        };
        std::vector<Uni_ShaderSpv> compiled;
        {
            std::lock_guard<std::mutex> glslangLock(GlslangSerialiseMutex());
            s_ok = CompileAndLinkShaderUnits(units, opt, compiled);
        }
        if (s_ok)
            for (auto& spv : compiled) s_spirv.push_back(spv->spirv);
        else
            LOG_ERROR("passthrough shader compile failed");
    }
    if (s_ok) out = s_spirv;
    return s_ok;
}

std::size_t wallpaper::vulkan::PassthroughSpirvCompileCountForTesting() {
    std::lock_guard<std::mutex> cache(s_cacheMutex);
    return s_compiles;
}
