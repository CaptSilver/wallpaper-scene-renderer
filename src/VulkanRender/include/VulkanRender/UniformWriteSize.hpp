#pragma once
#include <cstddef>
#include <optional>

// SPIR-V reflection always reports a real UBO member's byte size once the
// shader compiles; a reflected size of 0 means reflection couldn't place the
// member, and there's nothing left to reconstruct a size from -- std140
// padding means even a component count wouldn't give the right byte size
// (a vec3 member sits in a 16-byte slot but has 3 components). Lives in its
// own header so the unit tests under src/Test can exercise the decision
// without pulling in the rest of the renderer (same reasoning as
// PassBlendFactors.hpp in this directory).
namespace wallpaper
{
namespace vulkan
{
inline std::optional<std::size_t> ResolveUniformMemberWriteSize(std::size_t reflectedSize) {
    if (reflectedSize == 0) return std::nullopt;
    return reflectedSize;
}

// How many bytes of a ShaderValue actually go into a UBO member: the value's
// own size, capped at the member's reflected size so a wider ShaderValue
// (e.g. a mat4 written into a mat3 slot) can't overflow into whatever
// follows it in the buffer. Same reasoning as ResolveUniformMemberWriteSize
// for living in its own header -- UpdateUniform's staging-buffer write needs
// a live Device, so this is the part of it a unit test can actually reach.
inline std::size_t ClampUniformWriteSize(std::size_t valueSize, std::size_t memberSize) {
    return valueSize > memberSize ? memberSize : valueSize;
}
} // namespace vulkan
} // namespace wallpaper
