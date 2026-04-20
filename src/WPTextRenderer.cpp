#include "WPTextRenderer.hpp"
#include "Utils/Logging.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cstring>
#include <vector>

namespace wallpaper
{

static FT_Library s_ftLib = nullptr;

void WPTextRenderer::Init() {
    if (s_ftLib != nullptr) return;
    FT_Error err = FT_Init_FreeType(&s_ftLib);
    if (err) {
        LOG_ERROR("FT_Init_FreeType failed: %d", err);
        s_ftLib = nullptr;
    }
}

void WPTextRenderer::Shutdown() {
    if (s_ftLib) {
        FT_Done_FreeType(s_ftLib);
        s_ftLib = nullptr;
    }
}

// Decode one UTF-8 codepoint, advance ptr. Returns 0 on error/end.
static uint32_t DecodeUtf8(const char*& p, const char* end) {
    if (p >= end) return 0;
    auto b = static_cast<uint8_t>(*p);
    uint32_t cp = 0;
    int      n  = 0;
    if (b < 0x80) {
        cp = b;
        n  = 0;
    } else if ((b & 0xE0) == 0xC0) {
        cp = b & 0x1F;
        n  = 1;
    } else if ((b & 0xF0) == 0xE0) {
        cp = b & 0x0F;
        n  = 2;
    } else if ((b & 0xF8) == 0xF0) {
        cp = b & 0x07;
        n  = 3;
    } else {
        ++p;
        return 0xFFFD; // replacement char
    }
    ++p;
    for (int i = 0; i < n; ++i) {
        if (p >= end) return 0xFFFD;
        b = static_cast<uint8_t>(*p);
        if ((b & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (b & 0x3F);
        ++p;
    }
    return cp;
}

// Split text into lines by '\n'
static std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string              cur;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    lines.push_back(cur);
    return lines;
}

// Compute effective advance for a glyph — max of typographic advance and visual right edge.
// Decorative/3D fonts often have bitmaps wider than their advance width.
static int EffectiveAdvance(FT_GlyphSlot g) {
    int adv        = static_cast<int>(g->advance.x >> 6);
    int bearingX   = static_cast<int>(g->metrics.horiBearingX >> 6);
    int glyphWidth = static_cast<int>(g->metrics.width >> 6);
    int visualEnd  = bearingX + glyphWidth;
    return std::max(adv, visualEnd);
}

// Measure the pixel width of a single line
static int MeasureLineWidth(FT_Face face, const std::string& line) {
    int         pen_x = 0;
    const char* p     = line.data();
    const char* end   = p + line.size();
    while (p < end) {
        uint32_t cp = DecodeUtf8(p, end);
        if (cp == 0) break;
        if (FT_Load_Char(face, cp, FT_LOAD_DEFAULT) != 0) continue;
        pen_x += EffectiveAdvance(face->glyph);
    }
    return pen_x;
}

std::shared_ptr<Image> WPTextRenderer::RenderText(const std::string& fontData, float pointsize,
                                                   const std::string& text, i32 width, i32 height,
                                                   const std::string& halign,
                                                   const std::string& valign, i32 padding) {
    if (!s_ftLib) {
        // Lazy init — scene parser shuts down after Parse(), but render thread
        // needs FreeType for dynamic text re-rasterization
        Init();
        if (!s_ftLib) {
            LOG_ERROR("WPTextRenderer: FreeType init failed");
            return nullptr;
        }
    }
    if (fontData.empty()) {
        LOG_ERROR("WPTextRenderer: empty font data");
        return nullptr;
    }
    if (width <= 0 || height <= 0) {
        LOG_ERROR("WPTextRenderer: invalid dimensions %dx%d", width, height);
        return nullptr;
    }

    // Empty text: return a fully-transparent canvas of the requested size.
    // Scripts clearing a label (e.g. wallpaper 2866203962 playervolume
    // cursorUp does `percentageLayer.text = ""`) rely on this to actually
    // blank the texture — without it the old bitmap (e.g. "100%") persists.
    if (text.empty()) {
        usize bufSize = static_cast<usize>(width) * static_cast<usize>(height) * 4;
        auto img_ptr = std::make_shared<Image>();
        auto& img = *img_ptr;
        img.header.width = img.header.mapWidth = width;
        img.header.height = img.header.mapHeight = height;
        img.header.format = TextureFormat::RGBA8;
        img.header.count = 1;
        Image::Slot slot;
        slot.width = width;
        slot.height = height;
        ImageData mipmap;
        mipmap.width = width;
        mipmap.height = height;
        mipmap.size = static_cast<isize>(bufSize);
        auto* rawBuf = new uint8_t[bufSize]();   // zero-initialised → RGBA=(0,0,0,0)
        mipmap.data = ImageDataPtr(rawBuf, [](uint8_t* p) { delete[] p; });
        slot.mipmaps.push_back(std::move(mipmap));
        img.slots.push_back(std::move(slot));
        return img_ptr;
    }

    FT_Face  face = nullptr;
    FT_Error err  = FT_New_Memory_Face(s_ftLib, reinterpret_cast<const FT_Byte*>(fontData.data()),
                                       static_cast<FT_Long>(fontData.size()), 0, &face);
    if (err || !face) {
        LOG_ERROR("FT_New_Memory_Face failed: %d", err);
        return nullptr;
    }

    // Convert pointsize from typographic points to pixels.
    // WE authors a scene against a 3840x2160 virtual ortho — pointsize is
    // intended to render at the native full-resolution scale, not at 96 DPI
    // of the VIEWER.  At the 2x implied "Retina"-style DPI the glyphs end
    // up the correct relative size for the reference look (comparing against
    // the YouTube capture of wallpaper 2866203962's VHS Time/Date label).
    // Net formula: points * 96/72 * 2 = points * 8/3 ≈ 2.67 × pointsize.
    // ParseTextObj applies the same kRasterDpiScale to the texture canvas
    // so glyphs don't clip at the edges of an authored 1× size.
    i32 pixelSize = static_cast<i32>(pointsize * 96.0f / 72.0f * kRasterDpiScale + 0.5f);
    if (pixelSize < 4) pixelSize = 4;
    FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixelSize));

    i32 usableW = width - padding * 2;
    i32 usableH = height - padding * 2;
    if (usableW < 1) usableW = 1;
    if (usableH < 1) usableH = 1;

    // Allocate RGBA buffer (white text, alpha = coverage)
    usize              bufSize = static_cast<usize>(width) * static_cast<usize>(height) * 4;
    std::vector<uint8_t> buf(bufSize, 0);

    i32 ascender  = static_cast<i32>(face->size->metrics.ascender >> 6);
    i32 descender = static_cast<i32>(face->size->metrics.descender >> 6); // negative
    i32 lineH     = ascender - descender;

    auto lines    = SplitLines(text);
    i32  totalH   = lineH * static_cast<i32>(lines.size());

    // Vertical start position (baseline of first line)
    i32 startY;
    if (valign == "top") {
        startY = padding + ascender;
    } else if (valign == "bottom") {
        startY = height - padding - totalH + ascender;
    } else { // center
        startY = padding + (usableH - totalH) / 2 + ascender;
    }

    for (usize li = 0; li < lines.size(); ++li) {
        const auto& line = lines[li];
        if (line.empty()) continue;

        i32 lineWidth = MeasureLineWidth(face, line);

        // Horizontal offset
        i32 startX;
        if (halign == "left") {
            startX = padding;
        } else if (halign == "right") {
            startX = padding + usableW - lineWidth;
        } else { // center
            startX = padding + (usableW - lineWidth) / 2;
        }

        i32 baselineY = startY + static_cast<i32>(li) * lineH;
        i32 pen_x     = startX;

        const char* p   = line.data();
        const char* end = p + line.size();
        while (p < end) {
            uint32_t cp = DecodeUtf8(p, end);
            if (cp == 0) break;
            if (FT_Load_Char(face, cp, FT_LOAD_RENDER) != 0) continue;

            FT_GlyphSlot g  = face->glyph;
            i32          bx = pen_x + g->bitmap_left;
            i32          by = baselineY - g->bitmap_top;

            for (u32 row = 0; row < g->bitmap.rows; ++row) {
                for (u32 col = 0; col < g->bitmap.width; ++col) {
                    i32 px = bx + static_cast<i32>(col);
                    i32 py = by + static_cast<i32>(row);
                    if (px < 0 || px >= width || py < 0 || py >= height) continue;

                    uint8_t alpha = g->bitmap.buffer[row * g->bitmap.pitch + col];
                    if (alpha == 0) continue;

                    usize idx    = (static_cast<usize>(py) * static_cast<usize>(width) +
                                 static_cast<usize>(px)) * 4;
                    // White text, alpha = glyph coverage (max blend for overlapping glyphs)
                    buf[idx + 0] = 255;
                    buf[idx + 1] = 255;
                    buf[idx + 2] = 255;
                    buf[idx + 3] = std::max(buf[idx + 3], alpha);
                }
            }
            pen_x += EffectiveAdvance(g);
        }
    }

    FT_Done_Face(face);

    // Build Image
    auto  img_ptr = std::make_shared<Image>();
    auto& img     = *img_ptr;
    img.header.width     = width;
    img.header.height    = height;
    img.header.mapWidth  = width;
    img.header.mapHeight = height;
    img.header.format    = TextureFormat::RGBA8;
    img.header.count     = 1;

    Image::Slot slot;
    slot.width  = width;
    slot.height = height;

    ImageData mipmap;
    mipmap.width  = width;
    mipmap.height = height;
    mipmap.size   = static_cast<isize>(bufSize);

    auto* rawBuf = new uint8_t[bufSize];
    std::memcpy(rawBuf, buf.data(), bufSize);
    mipmap.data = ImageDataPtr(rawBuf, [](uint8_t* p) { delete[] p; });

    slot.mipmaps.push_back(std::move(mipmap));
    img.slots.push_back(std::move(slot));

    LOG_INFO("WPTextRenderer: rasterized %dx%d, %zu lines, pointsize=%.0f",
             width, height, lines.size(), pointsize);
    return img_ptr;
}

} // namespace wallpaper
