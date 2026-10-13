#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <vulkan/vulkan.h>

namespace wallpaper
{
namespace vulkan
{

// Bytes per pixel for the formats our render graph and swapchains emit.
// Returns 0 for formats we don't know how to read back; callers skip those
// rather than writing garbage.  RGBA8/BGRA8 = 4bpp; RGBA16F (HDR) = 8bpp.
inline uint32_t BytesPerPixelFor(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB: return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
    default: return 0;
    }
}

// Half-to-float decode.  Standard IEEE 754-2008 binary16 layout; handles
// normals, subnormals, and inf/NaN.
inline float Half2Float(uint16_t h) {
    uint32_t sign = (h >> 15) & 0x1u;
    uint32_t exp  = (h >> 10) & 0x1fu;
    uint32_t mant = h & 0x3ffu;
    uint32_t f;
    if (exp == 0) {
        if (mant == 0) {
            f = sign << 31;
        } else {
            while ((mant & 0x400u) == 0) {
                mant <<= 1;
                exp--;
            }
            exp++;
            mant &= 0x3ffu;
            f = (sign << 31) | ((exp + (127 - 15)) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        f = (sign << 31) | 0x7f800000u | (mant << 13);
    } else {
        f = (sign << 31) | ((exp + (127 - 15)) << 23) | (mant << 13);
    }
    float out;
    std::memcpy(&out, &f, 4);
    return out;
}

// IEC 61966-2-1 sRGB encode of a linear value, clamped to [0,1] first: the
// same mapping FinPass's SDR shader applies before presenting.
inline float SrgbEncode(float linear) {
    const float v = std::clamp(linear, 0.0f, 1.0f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

// Converts one row of a swapchain image to packed 8-bit RGB for a PPM.
// An HDR swapchain holds FinPass's unbounded linear output, so it gets the SDR
// final pass's clamp and sRGB encode; that keeps an HDR capture comparable to
// an SDR one, with highlights above 1.0 clipped.  Returns false, writing
// nothing, for a format BytesPerPixelFor does not know.
inline bool SwapchainRowToRGB8(const uint8_t* src, VkFormat format, uint32_t width, uint8_t* dst) {
    switch (BytesPerPixelFor(format)) {
    case 4: {
        const bool bgr = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
        for (uint32_t x = 0; x < width; x++) {
            dst[x * 3 + 0] = src[x * 4 + (bgr ? 2 : 0)];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + (bgr ? 0 : 2)];
        }
        return true;
    }
    case 8:
        for (uint32_t x = 0; x < width; x++) {
            for (uint32_t c = 0; c < 3; c++) {
                uint16_t h;
                std::memcpy(&h, src + x * 8 + c * 2, 2);
                dst[x * 3 + c] =
                    static_cast<uint8_t>(std::lround(SrgbEncode(Half2Float(h)) * 255.0f));
            }
        }
        return true;
    default: return false;
    }
}

} // namespace vulkan
} // namespace wallpaper
