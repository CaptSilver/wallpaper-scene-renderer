#include <doctest.h>

#include "WPPropertyScriptExtract.hpp"

#include <nlohmann/json.hpp>

using njson      = nlohmann::json;
using i32        = wallpaper::i32;
using ScenePropertyScript = wallpaper::ScenePropertyScript;
using Attachment = ScenePropertyScript::Attachment;

// Minimal scene.json object harness for the extraction helpers.  Keeps the
// test fixtures easy to eyeball against the real WE format.
namespace {

njson objectWithVisibleScript(i32 id, const char* script) {
    return njson {
        { "id", id },
        { "name", "bg" },
        { "visible", njson {
            { "value", true },
            { "script", script },
        }},
    };
}

njson objectWithAnimationLayers(i32 id, const char* layerName,
                                const std::vector<const char*>& perLayerScripts) {
    njson obj = njson {
        { "id", id },
        { "name", layerName },
    };
    njson alayers = njson::array();
    for (const auto* scriptBody : perLayerScripts) {
        alayers.push_back(njson {
            { "visible", njson {
                { "value", true },
                { "script", scriptBody },
            }},
        });
    }
    obj["animationlayers"] = std::move(alayers);
    return obj;
}

} // namespace

TEST_SUITE("WPPropertyScriptExtract") {
    TEST_CASE("top-level visible script → Attachment::Object, idx -1") {
        std::vector<ScenePropertyScript> out;
        njson obj = objectWithVisibleScript(42, "return true;");
        wek::extractPropertyScriptsFromHost(
            42, "bg", obj, Attachment::Object, -1, out);
        REQUIRE(out.size() == 1);
        CHECK(out[0].id == 42);
        CHECK(out[0].property == "visible");
        CHECK(out[0].layerName == "bg");
        CHECK(out[0].attachment == Attachment::Object);
        CHECK(out[0].animationLayerIndex == -1);
        CHECK(out[0].script == "return true;");
        CHECK(out[0].initialVisible == true);
    }

    TEST_CASE("origin script parses space-separated initial Vec3") {
        std::vector<ScenePropertyScript> out;
        njson obj = njson {
            { "id", 7 },
            { "origin", njson {
                { "value", "100 200 50" },
                { "script", "return value;" },
            }},
        };
        wek::extractPropertyScriptsFromHost(
            7, "layer", obj, Attachment::Object, -1, out);
        REQUIRE(out.size() == 1);
        CHECK(out[0].initialVec3[0] == 100);
        CHECK(out[0].initialVec3[1] == 200);
        CHECK(out[0].initialVec3[2] == 50);
    }

    TEST_CASE("alpha script stores initial float default 1.0") {
        std::vector<ScenePropertyScript> out;
        njson obj = njson {
            { "id", 5 },
            { "alpha", njson {
                { "script", "return value;" },
            }},
        };
        wek::extractPropertyScriptsFromHost(
            5, "layer", obj, Attachment::Object, -1, out);
        REQUIRE(out.size() == 1);
        CHECK(out[0].initialFloat == doctest::Approx(1.0));
    }

    TEST_CASE("host without any scripts yields no output") {
        std::vector<ScenePropertyScript> out;
        njson obj = njson {
            { "id", 99 },
            { "visible", true },  // plain bool, not a {script:} object
        };
        wek::extractPropertyScriptsFromHost(
            99, "x", obj, Attachment::Object, -1, out);
        CHECK(out.empty());
    }

    TEST_CASE("all 5 property kinds extracted together") {
        std::vector<ScenePropertyScript> out;
        njson obj = njson {
            { "id", 1 },
            { "visible", njson {{ "script", "return true;" }} },
            { "origin",  njson {{ "script", "return value;" }} },
            { "scale",   njson {{ "script", "return value;" }} },
            { "angles",  njson {{ "script", "return value;" }} },
            { "alpha",   njson {{ "script", "return 1.0;" }} },
        };
        wek::extractPropertyScriptsFromHost(
            1, "l", obj, Attachment::Object, -1, out);
        CHECK(out.size() == 5);
    }

    TEST_CASE("animationlayers[M] extraction stamps AnimationLayer + index") {
        std::vector<ScenePropertyScript> out;
        njson obj = objectWithAnimationLayers(20, "Lucy", {
            "return v;",   // layer 0
            "return v;",   // layer 1
            "return v;",   // layer 2
        });
        wek::extractAnimationLayerScripts(20, "Lucy", obj, out);
        REQUIRE(out.size() == 3);
        for (size_t i = 0; i < out.size(); i++) {
            CHECK(out[i].id == 20);
            CHECK(out[i].layerName == "Lucy");
            CHECK(out[i].attachment == Attachment::AnimationLayer);
            CHECK(out[i].animationLayerIndex == static_cast<i32>(i));
        }
    }

    TEST_CASE("object without animationlayers is skipped silently") {
        std::vector<ScenePropertyScript> out;
        njson obj = objectWithVisibleScript(1, "return true;");
        wek::extractAnimationLayerScripts(1, "bg", obj, out);
        CHECK(out.empty());
    }

    TEST_CASE("non-array animationlayers is skipped (malformed input)") {
        std::vector<ScenePropertyScript> out;
        njson obj = njson {
            { "id", 1 },
            { "animationlayers", "not an array" },
        };
        wek::extractAnimationLayerScripts(1, "bg", obj, out);
        CHECK(out.empty());
    }

    TEST_CASE("Lucy-style combined: obj visible + 3 animationlayer visibles") {
        // Reproduces the real Lucy scene.json layout: top-level puppet image
        // layer, three rigged animation layers each carrying an NSL init
        // script.  Caller extracts both shapes; attachments differ.
        std::vector<ScenePropertyScript> out;
        njson obj = njson {
            { "id", 20 },
            { "name", "Lucy" },
            { "visible", njson {
                { "value", true },
                { "script", "return true;" },   // top-level
            }},
        };
        njson alayers = njson::array();
        for (int i = 0; i < 3; i++) {
            alayers.push_back(njson {
                { "visible", njson {
                    { "value", true },
                    { "script", "return true;" },
                }},
            });
        }
        obj["animationlayers"] = std::move(alayers);

        wek::extractPropertyScriptsFromHost(
            20, "Lucy", obj, Attachment::Object, -1, out);
        wek::extractAnimationLayerScripts(20, "Lucy", obj, out);

        REQUIRE(out.size() == 4);
        CHECK(out[0].attachment == Attachment::Object);
        CHECK(out[0].animationLayerIndex == -1);
        for (size_t i = 1; i <= 3; i++) {
            CHECK(out[i].attachment == Attachment::AnimationLayer);
            CHECK(out[i].animationLayerIndex == static_cast<i32>(i - 1));
        }
    }

    TEST_CASE("scriptproperties stored as JSON string") {
        std::vector<ScenePropertyScript> out;
        njson obj = njson {
            { "id", 1 },
            { "visible", njson {
                { "script", "return true;" },
                { "scriptproperties", njson {
                    { "percentage", njson {{ "value", 0.5 }} },
                }},
            }},
        };
        wek::extractPropertyScriptsFromHost(
            1, "l", obj, Attachment::Object, -1, out);
        REQUIRE(out.size() == 1);
        CHECK(out[0].scriptProperties.find("\"percentage\"") != std::string::npos);
        CHECK(out[0].scriptProperties.find("0.5") != std::string::npos);
    }
} // TEST_SUITE WPPropertyScriptExtract
