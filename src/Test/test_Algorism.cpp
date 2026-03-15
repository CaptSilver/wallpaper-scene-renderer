#include <doctest.h>
#include "Utils/Algorism.h"
#include <cmath>

using namespace wallpaper;

// ===========================================================================
// sph2cart
// ===========================================================================

TEST_SUITE("sph2cart") {

TEST_CASE("zero angles, unit radius → (1, 0, 0)") {
    auto v = algorism::sph2cart({0.0, 0.0, 1.0});
    CHECK(v.x() == doctest::Approx(1.0));
    CHECK(v.y() == doctest::Approx(0.0));
    CHECK(v.z() == doctest::Approx(0.0));
}

TEST_CASE("azimuth 90deg → (0, 1, 0)") {
    auto v = algorism::sph2cart({M_PI / 2.0, 0.0, 1.0});
    CHECK(v.x() == doctest::Approx(0.0).epsilon(1e-10));
    CHECK(v.y() == doctest::Approx(1.0));
    CHECK(v.z() == doctest::Approx(0.0));
}

TEST_CASE("elevation 90deg → (0, 0, 1)") {
    auto v = algorism::sph2cart({0.0, M_PI / 2.0, 1.0});
    CHECK(v.x() == doctest::Approx(0.0).epsilon(1e-10));
    CHECK(v.y() == doctest::Approx(0.0).epsilon(1e-10));
    CHECK(v.z() == doctest::Approx(1.0));
}

TEST_CASE("zero radius → origin") {
    auto v = algorism::sph2cart({1.23, 0.45, 0.0});
    CHECK(v.x() == doctest::Approx(0.0));
    CHECK(v.y() == doctest::Approx(0.0));
    CHECK(v.z() == doctest::Approx(0.0));
}

TEST_CASE("radius 2 scales correctly") {
    auto v1 = algorism::sph2cart({0.5, 0.3, 1.0});
    auto v2 = algorism::sph2cart({0.5, 0.3, 2.0});
    CHECK(v2.x() == doctest::Approx(v1.x() * 2.0));
    CHECK(v2.y() == doctest::Approx(v1.y() * 2.0));
    CHECK(v2.z() == doctest::Approx(v1.z() * 2.0));
}

} // TEST_SUITE

// ===========================================================================
// DragForce
// ===========================================================================

TEST_SUITE("DragForce") {

TEST_CASE("scalar basic") {
    double f = algorism::DragForce(10.0, 0.5, 1.0);
    CHECK(f == doctest::Approx(-10.0));
}

TEST_CASE("zero speed → zero force") {
    double f = algorism::DragForce(0.0, 1.0, 1.0);
    CHECK(f == doctest::Approx(0.0));
}

TEST_CASE("scalar formula: -2 * speed * strength * density") {
    double speed = 5.0, strength = 0.3, density = 2.0;
    double expected = -2.0 * speed * strength * density;
    CHECK(algorism::DragForce(speed, strength, density) == doctest::Approx(expected));
}

TEST_CASE("vector DragForce direction") {
    Eigen::Vector3d v(1.0, 0.0, 0.0);
    auto f = algorism::DragForce(v, 0.5, 1.0);
    // Force should oppose velocity direction
    CHECK(f.x() < 0.0);
    CHECK(f.y() == doctest::Approx(0.0));
    CHECK(f.z() == doctest::Approx(0.0));
}

} // TEST_SUITE

// ===========================================================================
// lerp
// ===========================================================================

TEST_SUITE("lerp") {

TEST_CASE("t=0 returns a") {
    CHECK(algorism::lerp(0.0, 3.0, 7.0) == doctest::Approx(3.0));
}

TEST_CASE("t=1 returns b") {
    CHECK(algorism::lerp(1.0, 3.0, 7.0) == doctest::Approx(7.0));
}

TEST_CASE("t=0.5 returns midpoint") {
    CHECK(algorism::lerp(0.5, 0.0, 10.0) == doctest::Approx(5.0));
}

TEST_CASE("t=0.25") {
    CHECK(algorism::lerp(0.25, 0.0, 100.0) == doctest::Approx(25.0));
}

} // TEST_SUITE

// ===========================================================================
// PerlinEase
// ===========================================================================

TEST_SUITE("PerlinEase") {

TEST_CASE("t=0 → 0") {
    CHECK(algorism::PerlinEase(0.0) == doctest::Approx(0.0));
}

TEST_CASE("t=1 → 1") {
    CHECK(algorism::PerlinEase(1.0) == doctest::Approx(1.0));
}

TEST_CASE("t=0.5 → 0.5") {
    CHECK(algorism::PerlinEase(0.5) == doctest::Approx(0.5));
}

TEST_CASE("monotonically increasing in [0,1]") {
    double prev = algorism::PerlinEase(0.0);
    for (double t = 0.1; t <= 1.0; t += 0.1) {
        double cur = algorism::PerlinEase(t);
        CHECK(cur >= prev);
        prev = cur;
    }
}

} // TEST_SUITE

// ===========================================================================
// PerlinNoise
// ===========================================================================

TEST_SUITE("PerlinNoise") {

TEST_CASE("deterministic: same input → same output") {
    double a = algorism::PerlinNoise(1.5, 2.5, 3.5);
    double b = algorism::PerlinNoise(1.5, 2.5, 3.5);
    CHECK(a == doctest::Approx(b));
}

TEST_CASE("integer coordinates → 0") {
    // Perlin noise at integer lattice points is 0
    CHECK(algorism::PerlinNoise(1.0, 2.0, 3.0) == doctest::Approx(0.0));
    CHECK(algorism::PerlinNoise(0.0, 0.0, 0.0) == doctest::Approx(0.0));
}

TEST_CASE("range approximately [-1, 1]") {
    // Sample many points and check range
    double minVal = 1e9, maxVal = -1e9;
    for (double x = 0.0; x < 10.0; x += 0.37) {
        for (double y = 0.0; y < 10.0; y += 0.41) {
            double v = algorism::PerlinNoise(x, y, 0.5);
            if (v < minVal) minVal = v;
            if (v > maxVal) maxVal = v;
        }
    }
    CHECK(minVal >= -1.01);
    CHECK(maxVal <= 1.01);
}

} // TEST_SUITE

// ===========================================================================
// CurlNoise
// ===========================================================================

TEST_SUITE("CurlNoise") {

TEST_CASE("deterministic repeated calls") {
    auto a = algorism::CurlNoise({1.0, 2.0, 3.0});
    auto b = algorism::CurlNoise({1.0, 2.0, 3.0});
    CHECK(a.x() == doctest::Approx(b.x()));
    CHECK(a.y() == doctest::Approx(b.y()));
    CHECK(a.z() == doctest::Approx(b.z()));
}

TEST_CASE("different inputs produce different outputs") {
    auto a = algorism::CurlNoise({1.0, 2.0, 3.0});
    auto b = algorism::CurlNoise({4.5, 5.5, 6.5});
    // Extremely unlikely to be equal
    bool different = (std::abs(a.x() - b.x()) > 1e-10) ||
                     (std::abs(a.y() - b.y()) > 1e-10) ||
                     (std::abs(a.z() - b.z()) > 1e-10);
    CHECK(different);
}

} // TEST_SUITE

// ===========================================================================
// PerlinNoiseVec3
// ===========================================================================

TEST_SUITE("PerlinNoiseVec3") {

TEST_CASE("deterministic: same input gives same output") {
    auto a = algorism::PerlinNoiseVec3({1.5, 2.5, 3.5});
    auto b = algorism::PerlinNoiseVec3({1.5, 2.5, 3.5});
    CHECK(a.x() == doctest::Approx(b.x()));
    CHECK(a.y() == doctest::Approx(b.y()));
    CHECK(a.z() == doctest::Approx(b.z()));
}

TEST_CASE("components differ due to offsets") {
    auto v = algorism::PerlinNoiseVec3({1.5, 2.5, 3.5});
    // Each component uses different offsets, so they should generally differ
    bool all_same = (std::abs(v.x() - v.y()) < 1e-10) &&
                    (std::abs(v.y() - v.z()) < 1e-10);
    CHECK_FALSE(all_same);
}

TEST_CASE("different inputs produce different outputs") {
    auto a = algorism::PerlinNoiseVec3({1.0, 2.0, 3.0});
    auto b = algorism::PerlinNoiseVec3({4.5, 5.5, 6.5});
    bool different = (std::abs(a.x() - b.x()) > 1e-10) ||
                     (std::abs(a.y() - b.y()) > 1e-10) ||
                     (std::abs(a.z() - b.z()) > 1e-10);
    CHECK(different);
}

} // TEST_SUITE
