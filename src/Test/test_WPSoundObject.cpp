#include <doctest.h>

#include "wpscene/WPSoundObject.h"
#include "Fs/VFS.h"

#include <nlohmann/json.hpp>

using wallpaper::wpscene::WPSoundObject;
using wallpaper::fs::VFS;

namespace
{

WPSoundObject Parse(const char* json_text) {
    auto          json = nlohmann::json::parse(json_text);
    VFS           vfs;
    WPSoundObject obj;
    bool          ok = obj.FromJson(json, vfs);
    REQUIRE(ok); // every test below provides at least a valid sound array
    return obj;
}

bool TryParse(const char* json_text, WPSoundObject& out) {
    auto json = nlohmann::json::parse(json_text);
    VFS  vfs;
    return out.FromJson(json, vfs);
}

} // namespace

TEST_SUITE("WPSoundObject::FromJson") {
    TEST_CASE("missing sound array — returns false") {
        WPSoundObject obj;
        CHECK_FALSE(TryParse(R"({"name": "x"})", obj));
    }

    TEST_CASE("non-array sound — returns false") {
        WPSoundObject obj;
        CHECK_FALSE(TryParse(R"({"sound": "single.ogg"})", obj));
    }

    TEST_CASE("plain track list parses with defaults") {
        auto obj = Parse(R"({
            "name": "music",
            "sound": ["a.ogg", "b.ogg"]
        })");
        CHECK(obj.name == "music");
        REQUIRE(obj.sound.size() == 2u);
        CHECK(obj.sound[0] == "a.ogg");
        CHECK(obj.sound[1] == "b.ogg");
        CHECK(obj.playbackmode == "loop"); // default
        CHECK(obj.volume == doctest::Approx(1.0f));
        CHECK(obj.visible == true);
        CHECK(obj.startsilent == false);
        CHECK_FALSE(obj.hasVolumeScript);
        CHECK_FALSE(obj.hasVolumeAnimation);
    }

    TEST_CASE("empty-string entries in sound array are filtered out") {
        auto obj = Parse(R"({
            "sound": ["", "track1.ogg", ""]
        })");
        REQUIRE(obj.sound.size() == 1u);
        CHECK(obj.sound[0] == "track1.ogg");
    }

    TEST_CASE("scalar volume number is captured") {
        auto obj = Parse(R"({
            "sound": ["x.ogg"],
            "volume": 0.42
        })");
        CHECK(obj.volume == doctest::Approx(0.42f));
    }

    TEST_CASE("object volume with value uses the embedded value") {
        auto obj = Parse(R"({
            "sound": ["x.ogg"],
            "volume": { "value": 0.7 }
        })");
        CHECK(obj.volume == doctest::Approx(0.7f));
        CHECK_FALSE(obj.hasVolumeScript);
        CHECK_FALSE(obj.hasVolumeAnimation);
    }

    TEST_CASE("object volume with script captures script + scriptproperties") {
        auto obj = Parse(R"({
            "sound": ["x.ogg"],
            "volume": {
                "value": 0.5,
                "script": "return 1.0;",
                "scriptproperties": { "k": 1 }
            }
        })");
        CHECK(obj.hasVolumeScript);
        CHECK(obj.volumeScript == "return 1.0;");
        CHECK(obj.volumeScriptProperties.find("\"k\":1") != std::string::npos);
    }

    TEST_CASE("object volume with animation parses keyframes + options") {
        auto obj = Parse(R"({
            "sound": ["x.ogg"],
            "volume": {
                "value": 1.0,
                "animation": {
                    "c0": [
                        {"frame": 0,  "value": 0.0},
                        {"frame": 30, "value": 1.0},
                        {"frame": 60, "value": 0.5}
                    ],
                    "options": {
                        "name":   "fade-in",
                        "fps":    60.0,
                        "length": 90.0,
                        "mode":   "single"
                    }
                }
            }
        })");
        CHECK(obj.hasVolumeAnimation);
        REQUIRE(obj.volumeAnimation.keyframes.size() == 3u);
        CHECK(obj.volumeAnimation.keyframes[0].frame == 0);
        CHECK(obj.volumeAnimation.keyframes[1].value == doctest::Approx(1.0f));
        CHECK(obj.volumeAnimation.fps == doctest::Approx(60.0f));
        CHECK(obj.volumeAnimation.length == doctest::Approx(90.0f));
        CHECK(obj.volumeAnimation.mode == "single");
        CHECK(obj.volumeAnimation.name == "fade-in");
    }

    TEST_CASE("object volume with animation but no c0 array — no keyframes parsed") {
        auto obj = Parse(R"({
            "sound": ["x.ogg"],
            "volume": {
                "value": 1.0,
                "animation": { "options": { "fps": 30.0 } }
            }
        })");
        CHECK_FALSE(obj.hasVolumeAnimation);
        CHECK(obj.volumeAnimation.keyframes.empty());
    }

    TEST_CASE("playbackmode + min/max time are parsed") {
        auto obj = Parse(R"({
            "sound": ["x.ogg"],
            "playbackmode": "random",
            "mintime": 1.5,
            "maxtime": 8.0
        })");
        CHECK(obj.playbackmode == "random");
        CHECK(obj.mintime == doctest::Approx(1.5f));
        CHECK(obj.maxtime == doctest::Approx(8.0f));
    }

    TEST_CASE("startsilent + visible are parsed") {
        auto obj = Parse(R"({
            "sound": ["x.ogg"],
            "startsilent": true,
            "visible": false
        })");
        CHECK(obj.startsilent);
        CHECK(obj.visible == false);
    }
}
