#pragma once

#include <algorithm>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace wekde::sceneresolver {

// Build an ordered list of candidate scene-JSON filenames for the loader to
// try opening under /assets/.  The first candidate that exists and is
// non-empty wins.
//
// Inputs:
//   source        — path the caller handed to SceneWallpaper (file or dir)
//   source_is_dir — whether that path refers to a directory
//   project_json  — contents of project.json if already read (empty if not)
//
// Candidates (in priority order):
//   1. <source-stem>.json, when source is a file.  Preserves the historical
//      mapping from workshop "<id>/scene.pkg" → "scene.json" and mirrors
//      whatever explicit name the caller passed.
//   2. project.json's `file` field, extension forced to .json.  Authoritative
//      WE convention — defaultprojects rely on this (file: "scene.json").
//   3. "scene.json" as the ultimate canonical fallback.
//
// Duplicates are skipped so the loader does not reopen the same entry twice.
inline std::vector<std::string> BuildSceneFileCandidates(const std::string& source,
                                                         bool               source_is_dir,
                                                         const std::string& project_json)
{
    std::vector<std::string> out;
    auto                     push_unique = [&](std::string s) {
        if (s.empty() || s == ".json") return;
        if (std::find(out.begin(), out.end(), s) == out.end()) {
            out.push_back(std::move(s));
        }
    };

    if (! source_is_dir && ! source.empty()) {
        std::filesystem::path src { source };
        std::filesystem::path stem = src.filename();
        stem.replace_extension("json");
        push_unique(stem.native());
    }

    if (! project_json.empty()) {
        try {
            auto proj = nlohmann::json::parse(project_json);
            auto it   = proj.find("file");
            if (it != proj.end() && it->is_string()) {
                std::filesystem::path f { it->get<std::string>() };
                f.replace_extension("json");
                push_unique(f.filename().native());
            }
        } catch (const nlohmann::json::exception&) {
            // malformed project.json — fall through to scene.json fallback
        }
    }

    push_unique("scene.json");
    return out;
}

} // namespace wekde::sceneresolver
