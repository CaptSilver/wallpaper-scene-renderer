#include <doctest.h>

#include "Scene/SceneShader.h"

#include <Eigen/Dense>
#include <array>
#include <vector>

using namespace wallpaper;

TEST_SUITE("ShaderValue") {
    TEST_CASE("default constructed: size 0") {
        ShaderValue v;
        CHECK(v.size() == 0);
    }

    TEST_CASE("construct from scalar: stores one value") {
        ShaderValue v(3.14f);
        REQUIRE(v.size() == 1);
        CHECK(v[0] == doctest::Approx(3.14f));
    }

    TEST_CASE("construct from std::array (static path — fits ShaderValueInter<16>)") {
        std::array<float, 4> a { 1, 2, 3, 4 };
        ShaderValue          v(a);
        REQUIRE(v.size() == 4);
        for (size_t i = 0; i < 4; i++) CHECK(v[i] == doctest::Approx((float)(i + 1)));
    }

    TEST_CASE("construct from std::vector exceeding 16 → dynamic storage") {
        std::vector<float> big(20);
        for (size_t i = 0; i < 20; i++) big[i] = (float)i;
        ShaderValue v(big);
        REQUIRE(v.size() == 20);
        CHECK(v[0] == 0.f);
        CHECK(v[19] == 19.f);
    }

    TEST_CASE("construct from pointer + count") {
        float       arr[] = { 10, 20, 30 };
        ShaderValue v(arr, 3);
        REQUIRE(v.size() == 3);
        CHECK(v[2] == 30.f);
    }

    TEST_CASE("setSize clamps to the underlying storage capacity") {
        ShaderValue v(7.0f);
        v.setSize(100); // clamp to ShaderValueInter::size() = 16 (when not dynamic)
        CHECK(v.size() == 16);
    }

    TEST_CASE("mutable operator[] writes via non-const reference") {
        std::array<float, 2> a { 1, 2 };
        ShaderValue          v(a);
        v[1] = 99.f;
        CHECK(v[1] == 99.f);
    }

    TEST_CASE("data() returns pointer to first element") {
        std::array<float, 3> a { 5, 6, 7 };
        ShaderValue          v(a);
        REQUIRE(v.data() != nullptr);
        CHECK(v.data()[0] == 5.f);
        CHECK(v.data()[2] == 7.f);
    }

    TEST_CASE("fromMatrix(MatrixXf): copies matrix elements in column-major order") {
        Eigen::Matrix2f m;
        m << 1, 2, 3, 4; // row-major init:  [[1,2],[3,4]]
        ShaderValue v = ShaderValue::fromMatrix(m);
        REQUIRE(v.size() == 4);
        // Eigen stores column-major by default: m(0,0), m(1,0), m(0,1), m(1,1)
        CHECK(v[0] == doctest::Approx(1.f));
        CHECK(v[1] == doctest::Approx(3.f));
        CHECK(v[2] == doctest::Approx(2.f));
        CHECK(v[3] == doctest::Approx(4.f));
    }

    TEST_CASE("fromMatrix(MatrixXd): doubles cast to float via intermediate MatrixXf") {
        Eigen::Matrix2d m;
        m << 0.5, 1.5, 2.5, 3.5;
        ShaderValue v = ShaderValue::fromMatrix(m);
        REQUIRE(v.size() == 4);
        CHECK(v[0] == doctest::Approx(0.5f));
    }

} // ShaderValue
