#include <doctest.h>

#include "WPSceneFileResolver.hpp"

using wekde::sceneresolver::BuildSceneFileCandidates;

TEST_SUITE("BuildSceneFileCandidates") {
    TEST_CASE("workshop pkg: scene.pkg file resolves to scene.json + project file + fallback") {
        // Workshop layout: .../431960/<id>/scene.pkg, project.json next to it.
        const std::string project = R"({"file":"scene.pkg","type":"scene"})";
        auto out = BuildSceneFileCandidates("/path/to/workshop/1234567890/scene.pkg",
                                            /*is_dir=*/false, project);
        REQUIRE(out.size() >= 1);
        CHECK(out[0] == "scene.json");           // from source filename (scene.pkg → scene.json)
        // scene.json dedupes with project.json's file → "scene.json" → fallback; no dupes.
        CHECK(out.size() == 1);
    }

    TEST_CASE("defaultprojects file form: scene.json path resolves cleanly") {
        const std::string project = R"({"file":"scene.json","type":"scene"})";
        auto out = BuildSceneFileCandidates(
            "/home/u/wp/defaultprojects/shimmering_particles/scene.json",
            /*is_dir=*/false, project);
        REQUIRE(out.size() == 1);
        CHECK(out[0] == "scene.json");
    }

    TEST_CASE("bare directory: no source-derived candidate, falls back via project.json") {
        // Caller handed us the wallpaper directory (sceneviewer-script CLI).
        const std::string project = R"({"file":"scene.json"})";
        auto              out     = BuildSceneFileCandidates(
            "/home/u/wp/defaultprojects/shimmering_particles",
            /*is_dir=*/true, project);
        REQUIRE(out.size() == 1);
        CHECK(out[0] == "scene.json");
    }

    TEST_CASE("bare directory, no project.json: ultimate fallback to scene.json") {
        auto out = BuildSceneFileCandidates("/some/wallpaper", /*is_dir=*/true, "");
        REQUIRE(out.size() == 1);
        CHECK(out[0] == "scene.json");
    }

    TEST_CASE("source filename overrides project.json (explicit wins)") {
        // Hypothetical: caller passes custom.json even though project.json says scene.json.
        // Caller's explicit choice should be tried first.
        const std::string project = R"({"file":"scene.json"})";
        auto              out     = BuildSceneFileCandidates("/some/wp/custom.json",
                                                /*is_dir=*/false, project);
        REQUIRE(out.size() == 2);
        CHECK(out[0] == "custom.json");
        CHECK(out[1] == "scene.json");
    }

    TEST_CASE("project.json `file` with non-.json extension is coerced to .json") {
        // Workshop convention: project.json's `file` is "scene.pkg"; we need the
        // scene-entry name inside the pkg, which is the .json peer.
        const std::string project = R"({"file":"scene.pkg"})";
        auto              out     = BuildSceneFileCandidates("/some/wp", /*is_dir=*/true, project);
        REQUIRE(out.size() == 1);
        CHECK(out[0] == "scene.json");
    }

    TEST_CASE("malformed project.json does not throw, still yields scene.json fallback") {
        auto out = BuildSceneFileCandidates("/some/wp", /*is_dir=*/true,
                                            "{{{not valid json");
        REQUIRE(out.size() == 1);
        CHECK(out[0] == "scene.json");
    }

    TEST_CASE("project.json without `file` key still yields fallback") {
        const std::string project = R"({"title":"Some Wallpaper","type":"scene"})";
        auto              out     = BuildSceneFileCandidates("/some/wp", /*is_dir=*/true, project);
        REQUIRE(out.size() == 1);
        CHECK(out[0] == "scene.json");
    }

    TEST_CASE("project.json `file` with non-string value is ignored") {
        const std::string project = R"({"file":42})";
        auto              out     = BuildSceneFileCandidates("/some/wp", /*is_dir=*/true, project);
        REQUIRE(out.size() == 1);
        CHECK(out[0] == "scene.json");
    }

    TEST_CASE("project.json `file` with path component takes only the filename") {
        // Defensive: if project.json says "sub/scene.json", we should still mount
        // /assets/ at the wallpaper root and open "scene.json" there.
        const std::string project = R"({"file":"sub/scene.json"})";
        auto              out     = BuildSceneFileCandidates("/some/wp", /*is_dir=*/true, project);
        REQUIRE(out.size() == 1);
        CHECK(out[0] == "scene.json");
    }

    TEST_CASE("distinct source stem + project.json value → both tried, source first") {
        // E.g. workshop pkg with unusual internal entry named after a differently-
        // stemmed project.json `file`.  Both should be tried, caller's source first.
        const std::string project = R"({"file":"entry.json"})";
        auto              out     = BuildSceneFileCandidates("/wp/other.pkg",
                                                /*is_dir=*/false, project);
        REQUIRE(out.size() == 3);
        CHECK(out[0] == "other.json");
        CHECK(out[1] == "entry.json");
        CHECK(out[2] == "scene.json");
    }

    TEST_CASE("empty source is tolerated") {
        auto out = BuildSceneFileCandidates("", /*is_dir=*/true, "");
        REQUIRE(out.size() == 1);
        CHECK(out[0] == "scene.json");
    }

    TEST_CASE("project.json `file` == the same as source stem dedupes") {
        // Workshop case: both derivations point at scene.json; only one entry.
        const std::string project = R"({"file":"scene.pkg"})";
        auto              out     = BuildSceneFileCandidates("/wp/scene.pkg",
                                                /*is_dir=*/false, project);
        REQUIRE(out.size() == 1);
        CHECK(out[0] == "scene.json");
    }
}
