#include <doctest.h>

#include "Scene/SceneLight.hpp"
#include "Scene/SceneNode.h"

#include <memory>

using namespace wallpaper;

TEST_SUITE("SceneLight") {

TEST_CASE("constructor caches premultiplied = color * intensity * radius^2") {
    Eigen::Vector3f color(1.0f, 0.5f, 0.25f);
    SceneLight      light(color, 2.0f, 3.0f);
    CHECK(light.color().isApprox(color));
    CHECK(light.radius() == doctest::Approx(2.0f));
    CHECK(light.intensity() == doctest::Approx(3.0f));
    auto p = light.premultipliedColor();
    // r=2, i=3 → r² * i = 12 multiplier
    CHECK(p.x() == doctest::Approx(12.0f));
    CHECK(p.y() == doctest::Approx(6.0f));
    CHECK(p.z() == doctest::Approx(3.0f));
}

TEST_CASE("colorIntensity multiplies color by intensity") {
    SceneLight light(Eigen::Vector3f(1, 1, 1), 1.0f, 0.5f);
    auto       ci = light.colorIntensity();
    CHECK(ci.x() == doctest::Approx(0.5f));
    CHECK(ci.y() == doctest::Approx(0.5f));
    CHECK(ci.z() == doctest::Approx(0.5f));
}

TEST_CASE("setColor re-computes premultiplied") {
    SceneLight light(Eigen::Vector3f(1, 0, 0), 2.0f, 1.0f);
    // initial: premul = (1,0,0) * 1 * 4 = (4, 0, 0)
    CHECK(light.premultipliedColor().x() == doctest::Approx(4.0f));
    light.setColor(Eigen::Vector3f(0, 1, 0));
    CHECK(light.premultipliedColor().y() == doctest::Approx(4.0f));
    CHECK(light.premultipliedColor().x() == doctest::Approx(0.0f));
}

TEST_CASE("setRadius recomputes premultiplied (radius squared)") {
    SceneLight light(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
    CHECK(light.premultipliedColor().x() == doctest::Approx(1.0f));
    light.setRadius(3.0f);
    CHECK(light.premultipliedColor().x() == doctest::Approx(9.0f)); // 3² = 9
}

TEST_CASE("setIntensity scales premultiplied linearly") {
    SceneLight light(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
    light.setIntensity(5.0f);
    CHECK(light.premultipliedColor().x() == doctest::Approx(5.0f));
}

TEST_CASE("setNode stores shared pointer and node() retrieves raw") {
    SceneLight light(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
    CHECK(light.node() == nullptr);
    auto node = std::make_shared<SceneNode>();
    light.setNode(node);
    CHECK(light.node() == node.get());
}

} // SceneLight
