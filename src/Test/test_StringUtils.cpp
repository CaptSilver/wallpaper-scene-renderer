#include <doctest.h>
#include "Utils/String.h"
#include "Core/StringHelper.hpp"

#include <array>
#include <vector>

using namespace wallpaper;

// ===========================================================================
// SpliteString
// ===========================================================================

TEST_SUITE("SpliteString") {

TEST_CASE("single delimiter") {
    auto v = utils::SpliteString("a b c", ' ');
    REQUIRE(v.size() == 3);
    CHECK(v[0] == "a");
    CHECK(v[1] == "b");
    CHECK(v[2] == "c");
}

TEST_CASE("no delimiter present") {
    auto v = utils::SpliteString("hello", ' ');
    REQUIRE(v.size() == 1);
    CHECK(v[0] == "hello");
}

TEST_CASE("leading delimiter") {
    auto v = utils::SpliteString(" a b", ' ');
    REQUIRE(v.size() == 3);
    CHECK(v[0] == "");
    CHECK(v[1] == "a");
    CHECK(v[2] == "b");
}

TEST_CASE("trailing delimiter") {
    auto v = utils::SpliteString("a b ", ' ');
    REQUIRE(v.size() == 3);
    CHECK(v[0] == "a");
    CHECK(v[1] == "b");
    CHECK(v[2] == "");
}

TEST_CASE("consecutive delimiters") {
    auto v = utils::SpliteString("a  b", ' ');
    REQUIRE(v.size() == 3);
    CHECK(v[0] == "a");
    CHECK(v[1] == "");
    CHECK(v[2] == "b");
}

TEST_CASE("different delimiter char") {
    auto v = utils::SpliteString("x,y,z", ',');
    REQUIRE(v.size() == 3);
    CHECK(v[0] == "x");
    CHECK(v[1] == "y");
    CHECK(v[2] == "z");
}

TEST_CASE("empty string") {
    auto v = utils::SpliteString("", ' ');
    REQUIRE(v.size() == 1);
    CHECK(v[0] == "");
}

} // TEST_SUITE

// ===========================================================================
// StrToNum
// ===========================================================================

TEST_SUITE("StrToNum") {

TEST_CASE("int32 valid") {
    int32_t num = 0;
    utils::_StrToNum<int32_t>("42", num);
    CHECK(num == 42);
}

TEST_CASE("int32 negative") {
    int32_t num = 0;
    utils::_StrToNum<int32_t>("-7", num);
    CHECK(num == -7);
}

TEST_CASE("uint32 valid") {
    uint32_t num = 0;
    utils::_StrToNum<uint32_t>("100", num);
    CHECK(num == 100);
}

TEST_CASE("float valid") {
    float num = 0;
    utils::_StrToNum<float>("3.14", num);
    CHECK(num == doctest::Approx(3.14f));
}

TEST_CASE("float negative") {
    float num = 0;
    utils::_StrToNum<float>("-0.5", num);
    CHECK(num == doctest::Approx(-0.5f));
}

TEST_CASE("int32 invalid string leaves value unchanged") {
    int32_t num = 99;
    // StrToNum logs error but doesn't throw
    utils::StrToNum("not_a_number", num, __FILE__, __LINE__);
    CHECK(num == 99);
}

} // TEST_SUITE

// ===========================================================================
// StrToArray
// ===========================================================================

TEST_SUITE("StrToArray") {

TEST_CASE("vector of floats") {
    std::vector<float> v;
    utils::StrToArray::Convert<float>("1.0 2.5 3.7", v);
    REQUIRE(v.size() == 3);
    CHECK(v[0] == doctest::Approx(1.0f));
    CHECK(v[1] == doctest::Approx(2.5f));
    CHECK(v[2] == doctest::Approx(3.7f));
}

TEST_CASE("array of ints correct size") {
    std::array<int32_t, 3> arr {};
    utils::StrToArray::Convert<int32_t, 3>("10 20 30", arr);
    CHECK(arr[0] == 10);
    CHECK(arr[1] == 20);
    CHECK(arr[2] == 30);
}

TEST_CASE("array wrong size throws") {
    std::array<int32_t, 2> arr {};
    bool threw = false;
    try {
        utils::StrToArray::Convert<int32_t, 2>("10 20 30", arr);
    } catch (const utils::StrToArray::WrongSizeExp&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE("single element vector") {
    std::vector<float> v;
    utils::StrToArray::Convert<float>("5.0", v);
    REQUIRE(v.size() == 1);
    CHECK(v[0] == doctest::Approx(5.0f));
}

} // TEST_SUITE

// ===========================================================================
// sview_nullsafe
// ===========================================================================

TEST_SUITE("sview_nullsafe") {

TEST_CASE("null pointer returns empty view") {
    auto sv = sview_nullsafe(nullptr);
    CHECK(sv.empty());
    CHECK(sv.size() == 0);
}

TEST_CASE("valid pointer returns view") {
    const char* s = "hello";
    auto sv = sview_nullsafe(s);
    CHECK(sv == "hello");
}

TEST_CASE("empty string returns empty view") {
    auto sv = sview_nullsafe("");
    CHECK(sv.empty());
}

} // TEST_SUITE
