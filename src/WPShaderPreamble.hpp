#pragma once
// Shader preamble injected before WE-authored shader source by
// `WPShaderParser::PreShaderHeader`.  Defined in this header so unit tests can
// verify content (e.g. that required `#extension` lines are present) without
// having to link the full Vulkan + glslang shader-compile chain that lives in
// WPShaderParser.cpp.
//
// Single source of truth: WPShaderParser.cpp pulls these constants in directly
// rather than re-declaring them, so a test reading the header value also reads
// what the renderer actually emits.

namespace wallpaper {

// Common preamble — applied to every shader regardless of stage.  Stage-
// specific suffixes follow as separate constants.  Macro helpers (frac, atan2,
// pow→_wep, max→_wemx etc.) keep HLSL-flavoured WE shader sources compiling
// against GLSL semantics; see the body for per-helper rationale.
inline constexpr const char* kPreShaderCodeCommon = R"(#version 330
// Enable spec-constant aggregate constructors so glslang stops emitting the
// `extension GL_EXT_spec_constant_composites is being used for spec constant
// aggregate constructor` warning storm.  WE shaders routinely declare
// const-initialised vec4 colour / coefficient tables at file scope; glslang
// classifies those as candidate spec-constant aggregates and warns until the
// extension is enabled.  Discovered via the 2026-05-15 mass-audit log noise
// (44+ hits across 8+ wallpapers).  Without enable the warning is harmless
// to compile success but pollutes logs and trips error-classifying scripts.
#extension GL_EXT_spec_constant_composites : enable
#define GLSL 1
#define HLSL 0
#define highp

#define CAST2(x) (vec2(x))
#define CAST3(x) (vec3(x))
#define CAST4(x) (vec4(x))
#define CAST3X3(x) (mat3(x))

#define texSample2D texture
#define texSample2DLod textureLod
#define mul(x, y) ((y) * (x))
#define frac fract
#define atan2 atan
#define fmod(x, y) (x-y*trunc(x/y))
#define ddx dFdx
#define ddy(x) dFdy(-(x))
#define saturate(x) (clamp(x, 0.0, 1.0))
#define log10(x) (log(x) / log(10.0))

// HLSL built-ins broadcast scalar to vector; GLSL requires matching genType.
// Overloads must be defined BEFORE the #define so their bodies call the
// real built-in, while all subsequent shader code gets redirected.
//
// Additionally: GLSL says pow(x, y) is UNDEFINED for x < 0 (spec §8.2).
// HLSL's pow(x, 2) with negative x just does x*x and returns a positive
// result, so wallpapers routinely write `sign(x) * pow(x, 2.0)` meaning
// "signed square".  Under RADV (and other GLSL drivers) the undefined
// branch can return large or NaN values that then multiply into g_Time
// in motion shaders (scroll, cloudmotion) — clouds visibly blur / alias /
// "disappear" after minutes as v_Scroll drifts far out of [0,1].
// Wrap x with abs() so negative inputs take the positive-definite path;
// for the even-exponent case (the common `pow(x, 2.0)`) this yields the
// same magnitude HLSL does, and callers who need the sign keep their
// own `sign(x) *` prefix intact.
float _wep(float x, float y) { return pow(abs(x), y); }
vec2 _wep(vec2 x, float y) { return pow(abs(x), vec2(y)); }
vec3 _wep(vec3 x, float y) { return pow(abs(x), vec3(y)); }
vec4 _wep(vec4 x, float y) { return pow(abs(x), vec4(y)); }
vec2 _wep(float x, vec2 y) { return pow(vec2(abs(x)), y); }
vec3 _wep(float x, vec3 y) { return pow(vec3(abs(x)), y); }
vec4 _wep(float x, vec4 y) { return pow(vec4(abs(x)), y); }
vec2 _wep(vec2 x, vec2 y) { return pow(abs(x), y); }
vec3 _wep(vec3 x, vec3 y) { return pow(abs(x), y); }
vec4 _wep(vec4 x, vec4 y) { return pow(abs(x), y); }
#define pow _wep
vec2 _wemx(float x, vec2 y) { return max(vec2(x), y); }
vec3 _wemx(float x, vec3 y) { return max(vec3(x), y); }
vec4 _wemx(float x, vec4 y) { return max(vec4(x), y); }
vec2 _wemx(vec2 x, float y) { return max(x, vec2(y)); }
vec3 _wemx(vec3 x, float y) { return max(x, vec3(y)); }
vec4 _wemx(vec4 x, float y) { return max(x, vec4(y)); }
float _wemx(float x, float y) { return max(x, y); }
vec2 _wemx(vec2 x, vec2 y) { return max(x, y); }
vec3 _wemx(vec3 x, vec3 y) { return max(x, y); }
vec4 _wemx(vec4 x, vec4 y) { return max(x, y); }
#define max _wemx
vec2 _wemn(float x, vec2 y) { return min(vec2(x), y); }
vec3 _wemn(float x, vec3 y) { return min(vec3(x), y); }
vec4 _wemn(float x, vec4 y) { return min(vec4(x), y); }
vec2 _wemn(vec2 x, float y) { return min(x, vec2(y)); }
vec3 _wemn(vec3 x, float y) { return min(x, vec3(y)); }
vec4 _wemn(vec4 x, float y) { return min(x, vec4(y)); }
float _wemn(float x, float y) { return min(x, y); }
vec2 _wemn(vec2 x, vec2 y) { return min(x, y); }
vec3 _wemn(vec3 x, vec3 y) { return min(x, y); }
vec4 _wemn(vec4 x, vec4 y) { return min(x, y); }
#define min _wemn
float _wedot(vec4 x, vec3 y) { return dot(x.xyz, y); }
float _wedot(vec3 x, vec4 y) { return dot(x, y.xyz); }
float _wedot(vec4 x, vec2 y) { return dot(x.xy, y); }
float _wedot(vec2 x, vec4 y) { return dot(x, y.xy); }
float _wedot(vec3 x, vec2 y) { return dot(x.xy, y); }
float _wedot(vec2 x, vec3 y) { return dot(x, y.xy); }
float _wedot(vec2 x, vec2 y) { return dot(x, y); }
float _wedot(vec3 x, vec3 y) { return dot(x, y); }
float _wedot(vec4 x, vec4 y) { return dot(x, y); }
#define dot _wedot
// GLSL has no `step(vec edge, float x)` overload (HLSL broadcasts x).
// `_westep` adds the missing combinations.  Driver: Cybering (2326102392)
// ships `step(uv, 1)` where `uv` is vec2.
float _westep(float e, float x) { return step(e, x); }
vec2  _westep(float e, vec2  x) { return step(e, x); }
vec3  _westep(float e, vec3  x) { return step(e, x); }
vec4  _westep(float e, vec4  x) { return step(e, x); }
vec2  _westep(vec2  e, vec2  x) { return step(e, x); }
vec3  _westep(vec3  e, vec3  x) { return step(e, x); }
vec4  _westep(vec4  e, vec4  x) { return step(e, x); }
vec2  _westep(vec2  e, float x) { return step(e, vec2(x)); }
vec3  _westep(vec3  e, float x) { return step(e, vec3(x)); }
vec4  _westep(vec4  e, float x) { return step(e, vec4(x)); }
#define step _westep

#define float1 float
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define lerp mix

__SHADER_PLACEHOLD__

)";

inline constexpr const char* kPreShaderCodeVert = R"(
#define attribute in
#define varying out

)";

inline constexpr const char* kPreShaderCodeFrag = R"(
#define varying in
#define gl_FragColor glOutColor
out vec4 glOutColor;

// Sample _rt_sceneDepth at half-res destination coordinate, taking the
// near-most NDC z of the four full-res texels under each half-res pixel.
// Under our regular-z baseline (REVERSEDEPTH=0) smaller = nearer, so we
// take min; under reversed-z we take max.  Avoids the 1-pixel silhouette
// halo that nearest-sampling a single texel produces at half-res.
//
// The sampler is bound NEAREST/CLAMP — see TextureCache::GetOrCreateDepthSampler.
// Explicit textureLod(..., 0.0) reinforces the single-mip assumption.
float weSampleSceneDepthMinGather(sampler2D srcDepth, vec2 uv) {
    vec2 px = 1.0 / vec2(textureSize(srcDepth, 0));
    float a = textureLod(srcDepth, uv + vec2(-0.5,-0.5)*px, 0.0).r;
    float b = textureLod(srcDepth, uv + vec2( 0.5,-0.5)*px, 0.0).r;
    float c = textureLod(srcDepth, uv + vec2(-0.5, 0.5)*px, 0.0).r;
    float d = textureLod(srcDepth, uv + vec2( 0.5, 0.5)*px, 0.0).r;
#if REVERSEDEPTH
    return max(max(a,b), max(c,d));
#else
    return min(min(a,b), min(c,d));
#endif
}

)";

// Geometry shader: no attribute/varying defines needed.
// Layout declarations are injected by TranslateGeometryShader based on [maxvertexcount].
inline constexpr const char* kPreShaderCodeGeom = R"(
)";

} // namespace wallpaper
