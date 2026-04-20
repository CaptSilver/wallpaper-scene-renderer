#include <doctest.h>

#include "Core/StringHelper.hpp"
#include "Core/MapSet.hpp"
#include "Core/ArrayHelper.hpp"
#include "Utils/Algorism.h"
#include "Utils/BitFlags.hpp"
#include "Type.hpp"
#include "Fs/Bswap.hpp"
#include "WPCommon.hpp"
#include "Fs/MemBinaryStream.h"
#include "Utils/Logging.h"

#include <cmath>
#include <cstring>
#include <cstdio>
#include <vector>

using namespace wallpaper;
using namespace wallpaper::fs;

// ===========================================================================
// StringHelper
// ===========================================================================

TEST_SUITE("StringHelper") {

TEST_CASE("sstart_with") {
    CHECK(sstart_with("hello world", "hello") == true);
    CHECK(sstart_with("hello", "hello") == true);
    CHECK(sstart_with("hel", "hello") == false);
    CHECK(sstart_with("", "hello") == false);
    CHECK(sstart_with("hello", "") == true);
    CHECK(sstart_with("", "") == true);
    CHECK(sstart_with("_rt_default", "_rt_") == true);
    CHECK(sstart_with("_rt", "_rt_") == false);
}

TEST_CASE("send_with") {
    CHECK(send_with("hello world", "world") == true);
    CHECK(send_with("hello", "hello") == true);
    CHECK(send_with("lo", "hello") == false);
    CHECK(send_with("", "hello") == false);
    CHECK(send_with("hello", "") == true);
    CHECK(send_with("", "") == true);
    CHECK(send_with("image.tex", ".tex") == true);
    CHECK(send_with("image.png", ".tex") == false);
}

} // TEST_SUITE

// ===========================================================================
// Algorism
// ===========================================================================

TEST_SUITE("Algorism") {

TEST_CASE("IsPowOfTwo") {
    CHECK(algorism::IsPowOfTwo(0) == false);
    CHECK(algorism::IsPowOfTwo(1) == false);
    CHECK(algorism::IsPowOfTwo(2) == true);
    CHECK(algorism::IsPowOfTwo(3) == false);
    CHECK(algorism::IsPowOfTwo(4) == true);
    CHECK(algorism::IsPowOfTwo(8) == true);
    CHECK(algorism::IsPowOfTwo(16) == true);
    CHECK(algorism::IsPowOfTwo(15) == false);
    CHECK(algorism::IsPowOfTwo(256) == true);
    CHECK(algorism::IsPowOfTwo(1024) == true);
    CHECK(algorism::IsPowOfTwo(1023) == false);
}

TEST_CASE("PowOfTwo") {
    // Minimum returned value is 8
    CHECK(algorism::PowOfTwo(0) == 8);
    CHECK(algorism::PowOfTwo(1) == 8);
    CHECK(algorism::PowOfTwo(7) == 8);
    CHECK(algorism::PowOfTwo(8) == 8);
    CHECK(algorism::PowOfTwo(9) == 16);
    CHECK(algorism::PowOfTwo(16) == 16);
    CHECK(algorism::PowOfTwo(17) == 32);
    CHECK(algorism::PowOfTwo(100) == 128);
    CHECK(algorism::PowOfTwo(256) == 256);
    CHECK(algorism::PowOfTwo(257) == 512);
}

} // TEST_SUITE

// ===========================================================================
// BitFlags
// ===========================================================================

TEST_SUITE("BitFlags") {

enum class TestFlag : uint32_t
{
    A = 0,
    B = 1,
    C = 2,
    D = 31
};

TEST_CASE("Default construction — all cleared") {
    BitFlags<TestFlag> flags;
    CHECK(flags.none() == true);
    CHECK(flags.any() == false);
    CHECK(flags[TestFlag::A] == false);
    CHECK(flags[TestFlag::B] == false);
}

TEST_CASE("Construction from integer") {
    BitFlags<TestFlag> flags(0b101u); // bits 0 and 2
    CHECK(flags[TestFlag::A] == true);
    CHECK(flags[TestFlag::B] == false);
    CHECK(flags[TestFlag::C] == true);
}

TEST_CASE("Set and reset") {
    BitFlags<TestFlag> flags;
    flags.set(TestFlag::B);
    CHECK(flags[TestFlag::B] == true);
    CHECK(flags.count() == 1);

    flags.set(TestFlag::D);
    CHECK(flags[TestFlag::D] == true);
    CHECK(flags.count() == 2);

    flags.reset(TestFlag::B);
    CHECK(flags[TestFlag::B] == false);
    CHECK(flags.count() == 1);

    flags.reset();
    CHECK(flags.none() == true);
}

TEST_CASE("Index by underlying type") {
    BitFlags<TestFlag> flags(0b010u); // bit 1
    CHECK(flags[1u] == true);
    CHECK(flags[0u] == false);
}

TEST_CASE("WPTexFlags simulation") {
    // Simulate the flags used in WPTexImageParser
    enum class WPTexFlagEnum : uint32_t
    {
        noInterpolation = 0,
        clampUVs        = 1,
        sprite          = 2,
        compo1          = 20,
        compo2          = 21,
        compo3          = 22
    };

    // Sprite flag = bit 2 = value 4
    BitFlags<WPTexFlagEnum> flags(4u);
    CHECK(flags[WPTexFlagEnum::sprite] == true);
    CHECK(flags[WPTexFlagEnum::noInterpolation] == false);
    CHECK(flags[WPTexFlagEnum::clampUVs] == false);

    // All three flags
    BitFlags<WPTexFlagEnum> flags2(0b111u);
    CHECK(flags2[WPTexFlagEnum::noInterpolation] == true);
    CHECK(flags2[WPTexFlagEnum::clampUVs] == true);
    CHECK(flags2[WPTexFlagEnum::sprite] == true);
}

} // TEST_SUITE

// ===========================================================================
// WPCommon — ReadTexVesion / ReadVersion
// ===========================================================================

TEST_SUITE("WPCommon") {

namespace
{
// Helper: create a 9-byte version header in a MemBinaryStream
std::vector<uint8_t> makeVersionBytes(const char* prefix, int ver) {
    char buf[9] {};
    std::snprintf(buf, sizeof(buf), "%.4s%.4d", prefix, ver);
    return std::vector<uint8_t>(buf, buf + 9);
}
} // namespace

TEST_CASE("ReadTexVesion") {
    auto data = makeVersionBytes("TEX", 1);
    MemBinaryStream stream(std::move(data));
    CHECK(ReadTexVesion(stream) == 1);
}

TEST_CASE("ReadTexVesion version 2") {
    auto data = makeVersionBytes("TEX", 2);
    MemBinaryStream stream(std::move(data));
    CHECK(ReadTexVesion(stream) == 2);
}

TEST_CASE("ReadTexVesion version 3") {
    auto data = makeVersionBytes("TEX", 3);
    MemBinaryStream stream(std::move(data));
    CHECK(ReadTexVesion(stream) == 3);
}

TEST_CASE("ReadMDLVesion") {
    auto data = makeVersionBytes("MDL", 5);
    MemBinaryStream stream(std::move(data));
    CHECK(ReadMDLVesion(stream) == 5);
}

TEST_CASE("ReadVersion with wrong prefix returns 0") {
    auto data = makeVersionBytes("XXX", 1);
    MemBinaryStream stream(std::move(data));
    CHECK(ReadTexVesion(stream) == 0);
}

TEST_CASE("ReadVersion consecutive reads") {
    // Two version headers back to back
    auto v1 = makeVersionBytes("TEX", 1);
    auto v2 = makeVersionBytes("TEX", 2);
    std::vector<uint8_t> data;
    data.insert(data.end(), v1.begin(), v1.end());
    data.insert(data.end(), v2.begin(), v2.end());

    MemBinaryStream stream(std::move(data));
    CHECK(ReadTexVesion(stream) == 1);
    CHECK(ReadTexVesion(stream) == 2);
}

} // TEST_SUITE

// ===========================================================================
// MemBinaryStream
// ===========================================================================

TEST_SUITE("MemBinaryStream") {

TEST_CASE("SeekEnd with negative offset positions from end") {
    std::vector<uint8_t> data(10, 0);
    MemBinaryStream stream(std::move(data));
    // SeekEnd(-1) → pos = 10 + (-1) = 9
    CHECK(stream.SeekEnd(-1) == true);
    CHECK(stream.Tell() == 9);
}

TEST_CASE("SeekEnd(0) positions at end") {
    std::vector<uint8_t> data(10, 0);
    MemBinaryStream stream(std::move(data));
    CHECK(stream.SeekEnd(0) == true);
    CHECK(stream.Tell() == 10);
}

TEST_CASE("SeekEnd beyond bounds fails") {
    std::vector<uint8_t> data(10, 0);
    MemBinaryStream stream(std::move(data));
    // SeekEnd(1) → 10 + 1 = 11, out of bounds
    CHECK(stream.SeekEnd(1) == false);
    // SeekEnd(-11) → 10 + (-11) = -1, out of bounds
    CHECK(stream.SeekEnd(-11) == false);
}

TEST_CASE("Read clamps at end of stream") {
    std::vector<uint8_t> data = {1, 2, 3, 4, 5};
    MemBinaryStream stream(std::move(data));
    stream.SeekSet(3); // 2 bytes remaining
    uint8_t buf[10];
    auto read = stream.Read(buf, 10); // request 10 but only 2 available
    CHECK(read == 2);
    CHECK(buf[0] == 4);
    CHECK(buf[1] == 5);
}

TEST_CASE("InArea boundary: position 0 is valid") {
    std::vector<uint8_t> data(5, 0);
    MemBinaryStream stream(std::move(data));
    // pos=0 should be valid (>= 0 boundary)
    CHECK(stream.SeekSet(0) == true);
    CHECK(stream.Tell() == 0);
}

TEST_CASE("moveForward at end returns 0") {
    std::vector<uint8_t> data(5, 42);
    MemBinaryStream stream(std::move(data));
    stream.SeekSet(5); // at end
    uint8_t buf;
    auto read = stream.Read(&buf, 1);
    CHECK(read == 0);
}

TEST_CASE("moveForward exactly to end") {
    std::vector<uint8_t> data = {10, 20, 30};
    MemBinaryStream stream(std::move(data));
    stream.SeekSet(0);
    uint8_t buf[3];
    auto read = stream.Read(buf, 3); // reads exactly to Size()
    CHECK(read == 3);
    CHECK(stream.Tell() == 3);
    CHECK(buf[2] == 30);
}

TEST_CASE("InArea boundary: position at Size() is valid") {
    std::vector<uint8_t> data(5, 0);
    MemBinaryStream stream(std::move(data));
    // SeekSet to Size() should succeed (valid position, even though no data to read)
    CHECK(stream.SeekSet(5) == true);
    CHECK(stream.Tell() == 5);
}

TEST_CASE("InArea boundary: position past Size() is invalid") {
    std::vector<uint8_t> data(5, 0);
    MemBinaryStream stream(std::move(data));
    CHECK(stream.SeekSet(6) == false);
}

} // TEST_SUITE

// ===========================================================================
// ToString (TextureFormat)
// ===========================================================================

TEST_SUITE("ToString_TextureFormat") {

TEST_CASE("all 14 TextureFormat enum values") {
    CHECK(ToString(TextureFormat::RGBA8) == "RGBA8");
    CHECK(ToString(TextureFormat::BC1) == "BC1");
    CHECK(ToString(TextureFormat::BC2) == "BC2");
    CHECK(ToString(TextureFormat::BC3) == "BC3");
    CHECK(ToString(TextureFormat::BC7) == "BC7");
    CHECK(ToString(TextureFormat::RGB8) == "RGB8");
    CHECK(ToString(TextureFormat::RG8) == "RG8");
    CHECK(ToString(TextureFormat::R8) == "R8");
    CHECK(ToString(TextureFormat::RGBA16F) == "RGBA16F");
    CHECK(ToString(TextureFormat::RG16F) == "RG16F");
    CHECK(ToString(TextureFormat::R16F) == "R16F");
    CHECK(ToString(TextureFormat::BC6H) == "BC6H");
    CHECK(ToString(TextureFormat::RGB565) == "RGB565");
    CHECK(ToString(TextureFormat::RGBA1010102) == "RGBA1010102");
}

TEST_CASE("all results are non-empty") {
    CHECK_FALSE(ToString(TextureFormat::RGBA8).empty());
    CHECK_FALSE(ToString(TextureFormat::BC1).empty());
    CHECK_FALSE(ToString(TextureFormat::BC3).empty());
    CHECK_FALSE(ToString(TextureFormat::BC7).empty());
    CHECK_FALSE(ToString(TextureFormat::RGBA16F).empty());
}

} // TEST_SUITE

// ===========================================================================
// ToString (ImageType)
// ===========================================================================

TEST_SUITE("ToString_ImageType") {

TEST_CASE("known ImageType values") {
    CHECK(ToString(ImageType::UNKNOWN) == "UNKNOWN");
    CHECK(ToString(ImageType::BMP) == "BMP");
    CHECK(ToString(ImageType::ICO) == "ICO");
    CHECK(ToString(ImageType::JPEG) == "JPEG");
    CHECK(ToString(ImageType::JNG) == "JNG");
    CHECK(ToString(ImageType::PNG) == "PNG");
}

TEST_CASE("non-empty for known types") {
    CHECK_FALSE(ToString(ImageType::UNKNOWN).empty());
    CHECK_FALSE(ToString(ImageType::PNG).empty());
}

} // TEST_SUITE

// ===========================================================================
// Bswap
// ===========================================================================

TEST_SUITE("Bswap") {

TEST_CASE("uint16_t known swap") {
    CHECK(bswap<uint16_t>(0x0102) == 0x0201);
    CHECK(bswap<uint16_t>(0) == 0);
}

TEST_CASE("uint32_t known swap") {
    CHECK(bswap<uint32_t>(0x01020304u) == 0x04030201u);
    CHECK(bswap<uint32_t>(0) == 0);
}

TEST_CASE("uint64_t known swap") {
    CHECK(bswap<uint64_t>(0x0102030405060708ull) == 0x0807060504030201ull);
    CHECK(bswap<uint64_t>(0) == 0);
}

TEST_CASE("uint8_t identity") {
    CHECK(bswap<uint8_t>(0xAB) == 0xAB);
}

TEST_CASE("int32_t signed wrapper") {
    int32_t val = (int32_t)0x01020304;
    int32_t swapped = bswap<int32_t>(val);
    CHECK(swapped == (int32_t)0x04030201);
}

TEST_CASE("int16_t signed wrapper") {
    int16_t val = (int16_t)0x0102;
    int16_t swapped = bswap<int16_t>(val);
    CHECK(swapped == (int16_t)0x0201);
}

TEST_CASE("double-swap roundtrip uint16") {
    uint16_t orig = 0xBEEF;
    CHECK(bswap<uint16_t>(bswap<uint16_t>(orig)) == orig);
}

TEST_CASE("double-swap roundtrip uint32") {
    uint32_t orig = 0xDEADBEEFu;
    CHECK(bswap<uint32_t>(bswap<uint32_t>(orig)) == orig);
}

TEST_CASE("double-swap roundtrip uint64") {
    uint64_t orig = 0xDEADBEEFCAFEBABEull;
    CHECK(bswap<uint64_t>(bswap<uint64_t>(orig)) == orig);
}

} // TEST_SUITE

// ===========================================================================
// Perspective camera math
// ===========================================================================

TEST_SUITE("PerspectiveCamera") {

TEST_CASE("CalculatePersperctiveDistance fov=90 deg") {
    // tan(45deg) = 1, so distance = height / (2 * 1) = height / 2
    double dist = algorism::CalculatePersperctiveDistance(90.0, 100.0);
    CHECK(dist == doctest::Approx(50.0));
}

TEST_CASE("CalculatePersperctiveDistance larger fov means shorter distance") {
    double d1 = algorism::CalculatePersperctiveDistance(60.0, 100.0);
    double d2 = algorism::CalculatePersperctiveDistance(90.0, 100.0);
    CHECK(d1 > d2);
}

TEST_CASE("CalculatePersperctiveDistance taller height means greater distance") {
    double d1 = algorism::CalculatePersperctiveDistance(60.0, 100.0);
    double d2 = algorism::CalculatePersperctiveDistance(60.0, 200.0);
    CHECK(d2 == doctest::Approx(d1 * 2.0));
}

TEST_CASE("CalculatePersperctiveFov inverse of Distance") {
    double fov = 60.0;
    double height = 100.0;
    double dist = algorism::CalculatePersperctiveDistance(fov, height);
    double recovered_fov = algorism::CalculatePersperctiveFov(dist, height);
    CHECK(recovered_fov == doctest::Approx(fov).epsilon(1e-6));
}

TEST_CASE("CalculatePersperctiveFov known value fov=90") {
    // distance = height/2 when fov = 90
    double fov = algorism::CalculatePersperctiveFov(50.0, 100.0);
    CHECK(fov == doctest::Approx(90.0).epsilon(1e-6));
}

TEST_CASE("roundtrip Fov to Distance to Fov") {
    double fov = 75.0;
    double height = 200.0;
    double dist = algorism::CalculatePersperctiveDistance(fov, height);
    double fov2 = algorism::CalculatePersperctiveFov(dist, height);
    CHECK(fov2 == doctest::Approx(fov).epsilon(1e-6));
}

} // TEST_SUITE

// ===========================================================================
// MapSet utilities
// ===========================================================================

TEST_SUITE("MapSet") {

TEST_CASE("exists map key found") {
    Map<std::string, int> m;
    m["hello"] = 42;
    CHECK(exists(m, "hello") == true);
}

TEST_CASE("exists map key not found") {
    Map<std::string, int> m;
    m["hello"] = 42;
    CHECK(exists(m, "world") == false);
}

TEST_CASE("exists set key found") {
    Set<std::string> s;
    s.insert("hello");
    CHECK(exists(s, "hello") == true);
}

TEST_CASE("exists set key not found") {
    Set<std::string> s;
    s.insert("hello");
    CHECK(exists(s, "world") == false);
}

TEST_CASE("exists heterogeneous lookup with string_view") {
    Map<std::string, int> m;
    m["test"] = 1;
    std::string_view sv = "test";
    CHECK(exists(m, sv) == true);
}

} // TEST_SUITE

// ===========================================================================
// ArrayHelper utilities
// ===========================================================================

TEST_SUITE("ArrayHelper") {

TEST_CASE("array_cast int to float") {
    std::array<int, 3> src = { 1, 2, 3 };
    auto result = array_cast<float>(src);
    CHECK(result[0] == doctest::Approx(1.0f));
    CHECK(result[1] == doctest::Approx(2.0f));
    CHECK(result[2] == doctest::Approx(3.0f));
}

TEST_CASE("array_cast double to int truncation") {
    std::array<double, 2> src = { 1.9, 2.1 };
    auto result = array_cast<int>(src);
    CHECK(result[0] == 1);
    CHECK(result[1] == 2);
}

TEST_CASE("spanone size is always 1") {
    int val = 42;
    spanone<int> s(val);
    CHECK(s.size() == 1);
}

TEST_CASE("spanone data points to value") {
    int val = 42;
    spanone<int> s(val);
    CHECK(*s.data() == 42);
    CHECK(s[0] == 42);
}

TEST_CASE("spanone iteration") {
    int val = 99;
    spanone<int> s(val);
    int count = 0;
    for (auto it = s.begin(); it != s.end(); ++it) {
        CHECK(*it == 99);
        count++;
    }
    CHECK(count == 1);
}

TEST_CASE("spanone mutation through reference") {
    int val = 10;
    spanone<int> s(val);
    s[0] = 20;
    CHECK(val == 20);
}

TEST_CASE("transform doubles values") {
    std::vector<int> src = { 1, 2, 3 };
    auto result = transform<int>(std::span<const int>(src), [](int x) { return x * 2; });
    CHECK(result.size() == 3);
    CHECK(result[0] == 2);
    CHECK(result[1] == 4);
    CHECK(result[2] == 6);
}

TEST_CASE("transform type conversion") {
    std::vector<int> src = { 1, 2, 3 };
    auto result = transform<int>(std::span<const int>(src), [](int x) { return (double)x + 0.5; });
    CHECK(result.size() == 3);
    CHECK(result[0] == doctest::Approx(1.5));
    CHECK(result[1] == doctest::Approx(2.5));
    CHECK(result[2] == doctest::Approx(3.5));
}

} // TEST_SUITE ArrayHelper

// ===========================================================================
// past_last_slash (Logging.h)
// ===========================================================================

TEST_SUITE("past_last_slash") {

// Force runtime evaluation by using a non-constexpr string
static const char* forceRuntime(const char* s) {
    static char buf[256];
    std::strncpy(buf, s, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    return past_last_slash(buf);
}

TEST_CASE("extracts filename from path") {
    CHECK(std::strcmp(forceRuntime("/a/b/c.cpp"), "c.cpp") == 0);
}

TEST_CASE("extracts from single slash") {
    CHECK(std::strcmp(forceRuntime("/file.h"), "file.h") == 0);
}

TEST_CASE("no slash returns whole string") {
    CHECK(std::strcmp(forceRuntime("nopath.cpp"), "nopath.cpp") == 0);
}

TEST_CASE("trailing slash returns empty") {
    CHECK(std::strcmp(forceRuntime("/a/b/"), "") == 0);
}

TEST_CASE("empty string returns empty") {
    CHECK(std::strcmp(forceRuntime(""), "") == 0);
}

TEST_CASE("deep path") {
    CHECK(std::strcmp(forceRuntime("/usr/local/include/Scene/SceneCamera.h"),
                      "SceneCamera.h") == 0);
}

} // TEST_SUITE past_last_slash

// Regex patterns used by WPSceneParser to pre-scan SceneScript sources
// for dynamic assets so the parser can pre-allocate hidden-node pools.
// These are duplicated here to keep the tests hermetic; if the parser
// copies drift from these, you'll see the pool allocation regress on the
// Three-Body (3509243656) or dino_run wallpapers.
#include <regex>
#include <set>
#include <string>

namespace {
std::set<std::string> scanRegisterAsset(const std::string& src) {
    static const std::regex re(
        R"(registerAsset\s*\(\s*['"]([^'"]+)['"]\s*\))");
    std::set<std::string> out;
    for (auto it = std::sregex_iterator(src.begin(), src.end(), re);
         it != std::sregex_iterator(); ++it) {
        out.insert((*it)[1].str());
    }
    return out;
}
std::set<std::string> scanCreateLayerLiteral(const std::string& src) {
    static const std::regex re(
        R"(createLayer\s*\(\s*\{[\s\S]*?(?:image|"image"|'image')\s*:\s*['"]([^'"]+)['"])");
    std::set<std::string> out;
    for (auto it = std::sregex_iterator(src.begin(), src.end(), re);
         it != std::sregex_iterator(); ++it) {
        out.insert((*it)[1].str());
    }
    return out;
}

// Mirror of WPSceneParser's pool-size-hint heuristic.  Returns 0 when the
// script has no script-managed pool pattern (no hint → backend uses 8-slot
// default).  Otherwise returns min(2048, 3 × largest integer-slider max:).
size_t deriveAssetPoolHint(const std::string& src) {
    static const std::regex poolRe(R"(\w*[Pp]ool\s*\.\s*(pop|push|length))");
    static const std::regex maxRe(R"(max\s*:\s*(\d+)\s*[,\n])");
    if (!std::regex_search(src, poolRe)) return 0;
    size_t largest = 0;
    for (auto it = std::sregex_iterator(src.begin(), src.end(), maxRe);
         it != std::sregex_iterator(); ++it) {
        try {
            size_t v = std::stoull((*it)[1].str());
            if (v > largest) largest = v;
        } catch (...) {}
    }
    size_t hint = std::min<size_t>(2048, std::max<size_t>(8, largest * 3));
    return hint;
}
} // namespace

TEST_SUITE("asset pre-scan regex") {

TEST_CASE("registerAsset with single-quoted string") {
    auto s = scanRegisterAsset(R"(var a = engine.registerAsset('particles/coin.json');)");
    CHECK(s.size() == 1);
    CHECK(s.count("particles/coin.json") == 1);
}

TEST_CASE("registerAsset with double-quoted string") {
    auto s = scanRegisterAsset(R"(var a = engine.registerAsset("models/bar.json");)");
    CHECK(s.count("models/bar.json") == 1);
}

TEST_CASE("registerAsset — multiple calls all captured") {
    auto s = scanRegisterAsset(
        "var a = engine.registerAsset('x.json');\n"
        "var b = engine.registerAsset('y.json');\n"
        "var c = engine.registerAsset('z.json');\n");
    CHECK(s.size() == 3);
    CHECK(s.count("x.json") == 1);
    CHECK(s.count("y.json") == 1);
    CHECK(s.count("z.json") == 1);
}

TEST_CASE("createLayer literal — single-line with image key first") {
    auto s = scanCreateLayerLiteral(
        R"(thisScene.createLayer({image: 'trail.json', alpha: 0});)");
    CHECK(s.count("trail.json") == 1);
}

TEST_CASE("createLayer literal — image key is string-quoted") {
    auto s = scanCreateLayerLiteral(
        R"(thisScene.createLayer({"castshadow": false, "image": "models/ta.json"}))");
    CHECK(s.count("models/ta.json") == 1);
}

TEST_CASE("createLayer literal — multiline, image key not first") {
    // Three-Body wallpaper 3509243656 MAIN script: getTrailPoint().
    auto s = scanCreateLayerLiteral(
        "thisScene.createLayer({\n"
        "    \"castshadow\": false,\n"
        "    \"image\": \"models/ta.json\",\n"
        "    \"origin\": new Vec3(0, 0, -zOffset),\n"
        "    \"color\": new Vec3(1, 1, 1),\n"
        "    \"alpha\": 0,\n"
        "    \"scale\": new Vec3(trailStartSize, trailStartSize, trailStartSize),\n"
        "    \"visible\": false,\n"
        "});\n");
    CHECK(s.count("models/ta.json") == 1);
}

TEST_CASE("createLayer literal — double-quoted image with single-quoted path") {
    auto s = scanCreateLayerLiteral(
        R"(thisScene.createLayer({"image": 'mixed.json'}))");
    CHECK(s.count("mixed.json") == 1);
}

TEST_CASE("createLayer literal — ignore call with no image key") {
    // asset descriptor form: createLayer(myAsset).  No literal, nothing
    // to extract here — pre-scan should leave that case to registerAsset.
    auto s = scanCreateLayerLiteral(
        R"(var l = thisScene.createLayer(myAsset);)");
    CHECK(s.empty());
}

TEST_CASE("createLayer literal — repeated paths collapse to unique set") {
    auto s = scanCreateLayerLiteral(
        "thisScene.createLayer({image: 'a.json'});\n"
        "thisScene.createLayer({image: 'a.json'});\n"
        "thisScene.createLayer({image: 'b.json'});\n");
    CHECK(s.size() == 2);
    CHECK(s.count("a.json") == 1);
    CHECK(s.count("b.json") == 1);
}

// Pool-size hint heuristic tests.  When a SceneScript runs its own object
// pool (`trailPointsPool.pop()` etc.), the 8-slot backend default is too
// small — it expects as many layers as its own pool can hold, often 100s.
// deriveAssetPoolHint mirrors the parser's scan; see WPSceneParser.cpp.
TEST_CASE("pool-size hint — no pool pattern returns 0 (stay on default)") {
    // Normal createLayer usage without a script-managed pool.
    CHECK(deriveAssetPoolHint(
              "thisScene.createLayer({image: 'coin.json'});\n"
              ".addSlider({name: 'trailLength', max: 400, integer: true});") == 0);
}

TEST_CASE("pool-size hint — trailPointsPool.pop triggers larger pool") {
    // 3body MAIN: own pool + trailLength slider.  Hint = 400 × 3 = 1200.
    std::string src =
        "let trailPointsPool = [];\n"
        "function get() {\n"
        "  if (trailPointsPool.length > 0) return trailPointsPool.pop();\n"
        "  return thisScene.createLayer({image: 'models/ta.json'});\n"
        "}\n"
        ".addSlider({name: 'trailLength', min: 50, max: 400, integer: true});\n";
    CHECK(deriveAssetPoolHint(src) == 1200);
}

TEST_CASE("pool-size hint — caps at 2048 (WE's documented layer limit)") {
    // A ludicrously high slider max shouldn't blow past the cap.
    std::string src =
        "pool.push(x);\n"
        ".addSlider({max: 10000, integer: true});\n";
    CHECK(deriveAssetPoolHint(src) == 2048);
}

TEST_CASE("pool-size hint — ignores float maxes (skips 1e10, 0.5)") {
    // constraint_M (3body) has max: 1e10 but we only care about layer counts,
    // which are integer.  Largest INTEGER max wins.  Here only `10` matches
    // \d+ since `1e10` has 'e', so hint = max(8, 10 × 3) = 30.
    std::string src =
        "pool.length;\n"
        ".addSlider({name: 'constraint_M', max: 1e10, integer: false});\n"
        ".addSlider({name: 'exp', max: 10, integer: true});\n";
    CHECK(deriveAssetPoolHint(src) == 30);
}

TEST_CASE("pool-size hint — pool pattern alone (no large maxes) uses floor of 8") {
    // Script has a pool but no integer sliders.  Still hint=max(8, 0×3)=8.
    std::string src =
        "let myPool = [];\n"
        "myPool.push(1);\n";
    CHECK(deriveAssetPoolHint(src) == 8);
}

} // TEST_SUITE asset pre-scan regex

// -----------------------------------------------------------------------------
// wp-pkg pack format: length-prefixed version string, u32 count, per-entry
// (len-prefixed path, u32 rel-offset, u32 size), then data blob.  Format is
// defined in tools/wp_pkg.cpp (load_pkg / write_sized_string / write_u32_le).
// We reimplement parsing here so the test is self-contained and catches any
// drift between pack() and load() layouts.
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace wp_pkg_fmt {
struct Entry { std::string path; uint32_t off; uint32_t size; };
struct Pkg   { std::string version; uint32_t data_start; std::vector<Entry> entries; };

static bool read_u32_le(std::ifstream& f, uint32_t& out) {
    uint8_t b[4];
    if (!f.read(reinterpret_cast<char*>(b), 4)) return false;
    out = uint32_t(b[0]) | (uint32_t(b[1]) << 8) |
          (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
    return true;
}
static bool read_sized_string(std::ifstream& f, std::string& out) {
    uint32_t n;
    if (!read_u32_le(f, n)) return false;
    out.resize(n);
    if (n && !f.read(out.data(), n)) return false;
    return true;
}
static bool load(const std::string& path, Pkg& p) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    uint32_t count;
    if (!read_sized_string(f, p.version) || !read_u32_le(f, count)) return false;
    for (uint32_t i = 0; i < count; i++) {
        Entry e;
        if (!read_sized_string(f, e.path) ||
            !read_u32_le(f, e.off) || !read_u32_le(f, e.size)) return false;
        p.entries.push_back(std::move(e));
    }
    p.data_start = static_cast<uint32_t>(f.tellg());
    return true;
}
} // namespace wp_pkg_fmt

TEST_SUITE("wp-pkg pack format") {

// Pack invariants: header size matches computed offset, offsets monotonic,
// payload concatenated contiguously.  Doesn't exercise the pack() code path
// itself (that's in a standalone main), but catches regressions where the
// format drifts from load_pkg expectations.
TEST_CASE("Packed pkg parses with load() invariants") {
    // Build a tiny pkg with known entries under a temp dir, run wp-pkg pack,
    // load it back via load(), check byte-level correctness.
    const std::string dir  = "/tmp/wp_pkg_pack_test_fixture";
    const std::string pkg  = "/tmp/wp_pkg_pack_test.pkg";
    std::filesystem::remove_all(dir);
    std::filesystem::remove(pkg);
    std::filesystem::create_directories(dir + "/sub");
    // Two files so we exercise multi-entry offset patching.
    { std::ofstream f(dir + "/a.txt",     std::ios::binary); f << "hello"; }
    { std::ofstream f(dir + "/sub/b.bin", std::ios::binary); f << "xyz"; }

    const std::string tool =
        "/var/home/bazzite/build/wallpaper-engine-kde-plugin"
        "/src/backend_scene/build-tools/tools/wp-pkg";
    std::string cmd = tool + " pack " + dir + " " + pkg + " > /dev/null 2>&1";
    int rc = std::system(cmd.c_str());
    REQUIRE(rc == 0);

    wp_pkg_fmt::Pkg p;
    REQUIRE(wp_pkg_fmt::load(pkg, p));
    CHECK(p.version == "PKGV0023");
    REQUIRE(p.entries.size() == 2);
    // Stable sort: a.txt before sub/b.bin
    CHECK(p.entries[0].path == "a.txt");
    CHECK(p.entries[0].size == 5);
    CHECK(p.entries[0].off  == 0);
    CHECK(p.entries[1].path == "sub/b.bin");
    CHECK(p.entries[1].size == 3);
    // Second entry sits right after the first in the data blob.
    CHECK(p.entries[1].off  == 5);

    // Bytes at data_start + off match the source contents.
    std::ifstream f(pkg, std::ios::binary);
    f.seekg(p.data_start);
    std::string data0(5, '\0'); f.read(data0.data(), 5);
    CHECK(data0 == "hello");
    f.seekg(p.data_start + p.entries[1].off);
    std::string data1(3, '\0'); f.read(data1.data(), 3);
    CHECK(data1 == "xyz");

    std::filesystem::remove_all(dir);
    std::filesystem::remove(pkg);
}

TEST_CASE("Pack rejects missing directory") {
    const std::string tool =
        "/var/home/bazzite/build/wallpaper-engine-kde-plugin"
        "/src/backend_scene/build-tools/tools/wp-pkg";
    // Non-existent dir should exit non-zero without creating output.
    std::filesystem::remove("/tmp/wp_pkg_nope_out.pkg");
    std::string cmd = tool + " pack /tmp/__not_a_real_dir_for_wp_pkg "
                             "/tmp/wp_pkg_nope_out.pkg > /dev/null 2>&1";
    int rc = std::system(cmd.c_str());
    CHECK(rc != 0);
    CHECK_FALSE(std::filesystem::exists("/tmp/wp_pkg_nope_out.pkg"));
}

TEST_CASE("Pack skips .info.txt sidecars extract --raw=off writes") {
    // Extract writes <file>.info.txt next to each .tex for human-readable
    // header summaries.  Repacking should NOT include those synthetic files.
    const std::string dir = "/tmp/wp_pkg_pack_skip_test";
    const std::string pkg = "/tmp/wp_pkg_pack_skip_test.pkg";
    std::filesystem::remove_all(dir);
    std::filesystem::remove(pkg);
    std::filesystem::create_directories(dir);
    { std::ofstream f(dir + "/real.tex",         std::ios::binary); f << "Xyz"; }
    { std::ofstream f(dir + "/real.tex.info.txt",std::ios::binary); f << "summary"; }
    { std::ofstream f(dir + "/scene.json.orig",  std::ios::binary); f << "{}"; }
    { std::ofstream f(dir + "/scene.json",       std::ios::binary); f << "{}"; }

    const std::string tool =
        "/var/home/bazzite/build/wallpaper-engine-kde-plugin"
        "/src/backend_scene/build-tools/tools/wp-pkg";
    std::string cmd = tool + " pack " + dir + " " + pkg + " > /dev/null 2>&1";
    REQUIRE(std::system(cmd.c_str()) == 0);

    wp_pkg_fmt::Pkg p;
    REQUIRE(wp_pkg_fmt::load(pkg, p));
    // Only real.tex and scene.json should be present; .info.txt and .orig
    // sidecars are filtered out.
    CHECK(p.entries.size() == 2);
    std::set<std::string> paths;
    for (auto& e : p.entries) paths.insert(e.path);
    CHECK(paths.count("real.tex") == 1);
    CHECK(paths.count("scene.json") == 1);
    CHECK(paths.count("real.tex.info.txt") == 0);
    CHECK(paths.count("scene.json.orig") == 0);

    std::filesystem::remove_all(dir);
    std::filesystem::remove(pkg);
}

} // TEST_SUITE wp-pkg pack format
