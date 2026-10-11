#include <doctest.h>

#include "VulkanRender/PassthroughShader.hpp"
#include "WPShaderParser.hpp"

#include <mutex>

namespace
{
void ensureGlslangInit() {
    static std::once_flag once;
    std::call_once(once, [] {
        wallpaper::WPShaderParser::InitGlslang();
    });
}
} // namespace

// Every hideable pass draws the same tiny copy shader. It is compiled once per
// process. The SPIR-V words come back by value, so every pass gets its own
// copy; reflection stays per pass because each pipeline takes ownership of
// its reflected stage objects.
TEST_SUITE("PassthroughShader") {
    TEST_CASE("returns a vertex and a fragment stage, compiled only once") {
        ensureGlslangInit();
        std::vector<std::vector<uint>> a, b;
        REQUIRE(wallpaper::vulkan::PassthroughSpirv(a));
        REQUIRE(wallpaper::vulkan::PassthroughSpirv(b));
        REQUIRE(a.size() == 2u);
        CHECK_FALSE(a[0].empty());
        CHECK_FALSE(a[1].empty());
        CHECK(a == b);
        CHECK(wallpaper::vulkan::PassthroughSpirvCompileCountForTesting() == 1u);
    }
}
