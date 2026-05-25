#include <doctest.h>

#include "WPShaderValueUpdater.hpp"
#include "Scene/Scene.h"
#include "Scene/SceneLight.hpp"
#include "Scene/SceneNode.h"

#include <array>
#include <vector>
#include <tuple>

using namespace wallpaper;

TEST_SUITE("WPShaderValueUpdater::UpdateVolumetricLightUniforms") {
    TEST_CASE("WritePerLightVarOp type is invocable") {
        // Sanity-check that the type alias is usable: build a no-op closure
        // and call it.  Behaviour tests for the upload loop land in a
        // subsequent commit.
        std::vector<std::tuple<SceneLight*, int, std::array<float, 4>>> writes;
        WritePerLightVarOp op = [&](SceneLight* l, int slot,
                                    const std::array<float, 4>& v) {
            writes.emplace_back(l, slot, v);
        };
        SceneLight light(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
        op(&light, 2, std::array<float, 4> { 0.0f, 1.0f, 2.0f, 3.0f });
        REQUIRE(writes.size() == 1);
        CHECK(std::get<0>(writes[0]) == &light);
        CHECK(std::get<1>(writes[0]) == 2);
        CHECK(std::get<2>(writes[0])[3] == doctest::Approx(3.0f));
    }
}
