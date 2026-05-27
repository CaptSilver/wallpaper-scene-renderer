#include <doctest.h>

#include "HWVideoTextureDecoder.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace wallpaper;
namespace fs = std::filesystem;

// ──────────────────────────────────────────────────────────────────────────────
// HWVideoTextureDecoder::initEGL diagnostics + outError contract
//
// The WEK_HW_DECODE_DRI_PATH env override (see HWVideoTextureDecoder.cpp)
// lets us redirect the per-node probe at a tempdir of fake renderD* files so
// the failure paths are exercisable without a real GPU.  On lavapipe the
// regular /dev/dri also returns false (GBM init fails) so the override is a
// convenience for asserting against a controlled fixture.
// ──────────────────────────────────────────────────────────────────────────────

namespace
{
fs::path makeTempDir(const char* tag) {
    fs::path tmp = fs::temp_directory_path() / (std::string("wekde-e5-") + tag);
    // unique-ify so parallel test cases don't collide.
    int n = 0;
    while (fs::exists(tmp)) {
        ++n;
        tmp = fs::temp_directory_path() / (std::string("wekde-e5-") + tag + "-" + std::to_string(n));
    }
    fs::create_directory(tmp);
    return tmp;
}
} // namespace

TEST_SUITE("HWVideoTextureDecoder::initEGL diagnostics") {
    TEST_CASE("WEK_HW_DECODE_DRI_PATH empty dir → 'no working GPU render node'") {
        // Override the probe at a tempdir with zero renderD* entries.  The
        // for-loop never enters; the final LOG_ERROR fires and outError is
        // populated.
        fs::path tmp = makeTempDir("empty");
        setenv("WEK_HW_DECODE_DRI_PATH", tmp.c_str(), 1);

        HWVideoTextureDecoder dec(64, 64);
        std::string           err;
        const bool            ok = dec.open("/var/empty/x.mp4", &err);
        CHECK(ok == false);
        CHECK(err.find("no working GPU render node") != std::string::npos);

        unsetenv("WEK_HW_DECODE_DRI_PATH");
        fs::remove_all(tmp);
    }

    TEST_CASE("WEK_HW_DECODE_DRI_PATH with fake renderD128 → gbm_create_device skip path") {
        // Plain file named renderD128: ::open() succeeds, gbm_create_device()
        // fails on the non-DRI fd, the per-node LOG_INFO fires.  Final
        // outcome is still "no working GPU render node" + outError populated.
        fs::path tmp = makeTempDir("fakedri");
        { std::ofstream(tmp / "renderD128") << "fake"; }
        setenv("WEK_HW_DECODE_DRI_PATH", tmp.c_str(), 1);

        HWVideoTextureDecoder dec(64, 64);
        std::string           err;
        CHECK(dec.open("/var/empty/x.mp4", &err) == false);
        CHECK(err.find("no working GPU render node") != std::string::npos);

        unsetenv("WEK_HW_DECODE_DRI_PATH");
        fs::remove_all(tmp);
    }

    TEST_CASE("WEK_HW_DECODE_DRI_PATH at a non-existent path → 'no working GPU render node'") {
        // Defensive: directory_iterator on a missing path throws.  The new
        // is_directory guard short-circuits and the same error surface fires.
        fs::path tmp = makeTempDir("missing");
        fs::remove_all(tmp); // ensure it doesn't exist
        setenv("WEK_HW_DECODE_DRI_PATH", tmp.c_str(), 1);

        HWVideoTextureDecoder dec(64, 64);
        std::string           err;
        CHECK(dec.open("/var/empty/x.mp4", &err) == false);
        CHECK(err.find("no working GPU render node") != std::string::npos);

        unsetenv("WEK_HW_DECODE_DRI_PATH");
    }

    TEST_CASE("open(path, nullptr) preserves the legacy bool-only API") {
        // Default-arg overload: legacy bool-only call sites still work.
        fs::path tmp = makeTempDir("nullptrarg");
        setenv("WEK_HW_DECODE_DRI_PATH", tmp.c_str(), 1);

        HWVideoTextureDecoder dec(64, 64);
        CHECK(dec.open("/var/empty/x.mp4") == false); // no outError, must compile

        unsetenv("WEK_HW_DECODE_DRI_PATH");
        fs::remove_all(tmp);
    }
}
