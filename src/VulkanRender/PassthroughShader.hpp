#pragma once
#include <cstddef>
#include <vector>
#include <sys/types.h> // uint

namespace wallpaper::vulkan
{
// SPIR-V for the copy shader a hidden hideable pass draws (vertex then
// fragment). Compiled on first use under GlslangSerialiseMutex and cached;
// the words are returned by value, so each call copies them into `out`. False if it failed to
// compile.
bool        PassthroughSpirv(std::vector<std::vector<uint>>& out);
std::size_t PassthroughSpirvCompileCountForTesting();
} // namespace wallpaper::vulkan
