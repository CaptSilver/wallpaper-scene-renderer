#include <doctest.h>

#include "HWVideoTextureDecoder.hpp"

#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

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

// ──────────────────────────────────────────────────────────────────────────────
// GPU buffer lifetime
//
// Needs a working render node, so it degrades to a MESSAGE on a machine
// without one (lavapipe, containers with no /dev/dri).  Where it does run it
// is the only check that the decoder hands back the GPU memory it took.
// ──────────────────────────────────────────────────────────────────────────────

namespace
{
// The kernel names a descriptor pointing at a dma-buf "/dmabuf:" in
// /proc/self/fd, which tells it apart from the DRI render node and from
// libmpv's own descriptors.  Counting only those keeps the assertion immune to
// unrelated fd churn inside mpv.  Returns -1 if /proc is unreadable.
int countDmaBufFds() {
    DIR* d = ::opendir("/proc/self/fd");
    if (! d) return -1;
    int n = 0;
    while (const dirent* e = ::readdir(d)) {
        if (e->d_name[0] == '.') continue;
        const std::string link = std::string("/proc/self/fd/") + e->d_name;
        char              target[256];
        const ssize_t     len = ::readlink(link.c_str(), target, sizeof(target) - 1);
        if (len <= 0) continue;
        target[len] = '\0';
        if (std::strncmp(target, "/dmabuf:", 8) == 0) ++n;
    }
    ::closedir(d);
    return n;
}

// renderFrame() normally runs only when libmpv signals a new frame; drive it
// directly so the test doesn't wait on decode timing.
struct RenderProbe : HWVideoTextureDecoder {
    RenderProbe(int w, int h): HWVideoTextureDecoder(w, h) {}
    void renderOnce() {
        m_needsRender.store(true);
        renderFrame();
    }
};
} // namespace

TEST_SUITE("HWVideoTextureDecoder GPU buffer lifetime") {
    TEST_CASE("a destroyed decoder holds no dma-buf from the frames it rendered") {
        // Probe the real render node, not the fake-DRI fixture the diagnostics
        // cases install.
        ::unsetenv("WEK_HW_DECODE_DRI_PATH");

        const int before = countDmaBufFds();
        REQUIRE(before >= 0);

        bool opened = false;
        {
            RenderProbe dec(64, 64);
            // The path never resolves, and it doesn't need to: open() only
            // needs EGL, the GL FBO and an mpv render context — mpv reports the
            // missing file asynchronously.  A blank frame still renders, which
            // is all the GPU-side resource handling depends on.
            opened = dec.open("/var/empty/wek-no-such-video.mp4");
            if (opened) dec.renderOnce();
        }

        if (! opened) {
            MESSAGE("skipped: no usable GPU render node (EGL/GBM init failed)");
            return;
        }
        CHECK(countDmaBufFds() == before);
    }
}
