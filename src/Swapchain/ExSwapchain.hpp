#pragma once
#include "TripleSwapchain.hpp"
#include <cstdint>

namespace wallpaper
{

enum class TexTiling
{
    OPTIMAL,
    LINEAR
};

struct ExHandle {
    int         fd { -1 };
    int32_t     width { 0 };
    int32_t     height { 0 };
    std::size_t size { 0 };
    uint32_t    gl_format { 0x8058 }; // GL_RGBA8 default

    ExHandle() = default;
    ExHandle(int id): m_id(id) {};

    int32_t id() const { return m_id; }

private:
    int32_t m_id { 0 };
};

// class ExSwapchain : public TripleSwapchain<ExHandle> {};
using ExSwapchain = TripleSwapchain<ExHandle>;
} // namespace wallpaper
