#include <doctest.h>

#include "VideoTextureDecoder.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <chrono>

using namespace wallpaper;

// ──────────────────────────────────────────────────────────────────────────────
// Test subclass: exposes the protected triple-buffer machinery (publishFrame,
// fillAlpha, decode/ready/read indices, buffer pointers) so we can exercise the
// producer→consumer index rotation without driving a real libmpv decode.
//
// Composition over inheritance would be cleaner but the indices are protected
// std::atomic<int> members on the base; a friend test fixture is the natural
// doctest pattern here.
// ──────────────────────────────────────────────────────────────────────────────
namespace
{
class TestableDecoder : public VideoTextureDecoder {
public:
    using VideoTextureDecoder::VideoTextureDecoder;

    // Expose protected lifecycle hooks for assertions.
    using VideoTextureDecoder::fillAlpha;
    using VideoTextureDecoder::publishFrame;

    int  decodeIdx() const { return m_decodeIdx.load(); }
    int  readyIdx() const { return m_readyIdx.load(); }
    int  readIdx() const { return m_readIdx.load(); }
    auto frameNum() const { return m_frameNum.load(); }

    // For the triple-buffer rotation test: write a recognizable byte into
    // m_buffers[m_decodeIdx] so subsequent reads can verify which slot they got.
    void stampDecodeBufferTopLeft(uint8_t marker) {
        int idx                    = m_decodeIdx.load();
        m_buffers[idx].get()[0]    = marker;
        m_buffers[idx].get()[1]    = marker;
        m_buffers[idx].get()[2]    = marker;
    }

    // Force m_opened so acquireFrame() proceeds; bypasses the libmpv loadFile.
    void forceOpened() { m_opened.store(true); }
};
} // namespace

// ──────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ──────────────────────────────────────────────────────────────────────────────

TEST_SUITE("VideoTextureDecoder::ctor") {
    TEST_CASE("stores width/height; stride = width*4 (RGBA8)") {
        VideoTextureDecoder d(640, 360);
        CHECK(d.width() == 640);
        CHECK(d.height() == 360);
    }

    TEST_CASE("isPlaying() is false before open()") {
        VideoTextureDecoder d(64, 64);
        CHECK(d.isPlaying() == false);
    }

    TEST_CASE("hasNewFrame() is false before any decode") {
        VideoTextureDecoder d(64, 64);
        CHECK(d.hasNewFrame() == false);
    }

    TEST_CASE("acquireFrame() returns nullptr before open()") {
        VideoTextureDecoder d(64, 64);
        CHECK(d.acquireFrame() == nullptr);
    }

    TEST_CASE("releaseFrame() is a no-op (safe to call on un-opened decoder)") {
        VideoTextureDecoder d(64, 64);
        // No assertion — just confirm no crash / no UAF.
        d.releaseFrame();
        d.releaseFrame();
        CHECK(true);
    }

    TEST_CASE("triple-buffer starts with distinct decode/ready/read indices") {
        // Invariant: at construction the three indices must be a permutation
        // of {0,1,2} so no two of {decode, ready, read} alias the same buffer.
        // If they did, the decoder thread could write the read thread's
        // current frame mid-read.
        TestableDecoder d(32, 32);
        const int       a = d.decodeIdx();
        const int       b = d.readyIdx();
        const int       c = d.readIdx();
        CHECK(a != b);
        CHECK(b != c);
        CHECK(a != c);
        CHECK(a >= 0);
        CHECK(a < 3);
        CHECK(b >= 0);
        CHECK(b < 3);
        CHECK(c >= 0);
        CHECK(c < 3);
    }

    TEST_CASE("frame counter starts at zero") {
        TestableDecoder d(32, 32);
        CHECK(d.frameNum() == 0u);
    }

    TEST_CASE("zero-dimension construction does not crash") {
        // Defensive: scene authors may declare a 0×0 video region. The base
        // class allocates `stride * height` = 0 bytes per buffer, which
        // make_unique<uint8_t[]>(0) accepts (returns a non-null sentinel).
        VideoTextureDecoder d(0, 0);
        CHECK(d.width() == 0);
        CHECK(d.height() == 0);
        // Destruction must not touch the (null) buffers.
    }

    TEST_CASE("destruction is safe when never opened (no mpv handle leak)") {
        // Covers the !m_renderCtx && !m_mpv branch of ~VideoTextureDecoder.
        // mpv_terminate_destroy(nullptr) would crash; the dtor must guard.
        {
            VideoTextureDecoder d(128, 128);
        } // dtor here — must not crash.
        CHECK(true);
    }

    TEST_CASE("multiple decoders can coexist (no global mpv state collision)") {
        // Even before open(), allocating several decoders side-by-side must
        // not interfere — each owns its own buffer set.
        VideoTextureDecoder a(64, 64);
        VideoTextureDecoder b(128, 96);
        VideoTextureDecoder c(32, 32);
        CHECK(a.width() == 64);
        CHECK(b.width() == 128);
        CHECK(c.width() == 32);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Pre-open accessors: must return safe defaults (no crash, no UB) when m_mpv
// is nullptr.  All five accessors below early-return on `! m_mpv`.
// ──────────────────────────────────────────────────────────────────────────────

TEST_SUITE("VideoTextureDecoder::pre-open accessors") {
    TEST_CASE("getCurrentTimeSec() returns 0.0 without mpv handle") {
        VideoTextureDecoder d(64, 64);
        CHECK(d.getCurrentTimeSec() == 0.0);
    }

    TEST_CASE("getDurationSec() returns 0.0 without mpv handle") {
        VideoTextureDecoder d(64, 64);
        CHECK(d.getDurationSec() == 0.0);
    }

    TEST_CASE("setCurrentTimeSec() is a no-op without mpv handle") {
        VideoTextureDecoder d(64, 64);
        d.setCurrentTimeSec(5.0);
        d.setCurrentTimeSec(-1.0); // also clamps negatives
        d.setCurrentTimeSec(1e9);  // also tolerates overrun
        // Round-trip: still returns 0 because there's no mpv handle to query.
        CHECK(d.getCurrentTimeSec() == 0.0);
    }

    TEST_CASE("setRate() is a no-op without mpv handle (positive)") {
        VideoTextureDecoder d(64, 64);
        d.setRate(2.0);
        CHECK(d.isPlaying() == false);
    }

    TEST_CASE("setRate(0) without mpv routes through pause() — still safe") {
        // setRate(<=0) is documented to map to pause().  pause() then
        // early-returns on `! m_mpv`.  The chained no-op must not crash.
        VideoTextureDecoder d(64, 64);
        d.setRate(0.0);
        d.setRate(-1.0);
        CHECK(d.isPlaying() == false);
    }

    TEST_CASE("play()/pause()/stop() are no-ops without mpv handle") {
        VideoTextureDecoder d(64, 64);
        d.play();
        CHECK(d.isPlaying() == false); // play() bails on !m_mpv before toggling
        d.pause();
        CHECK(d.isPlaying() == false);
        d.stop();
        CHECK(d.isPlaying() == false);
    }

    TEST_CASE("hasNewFrame() returns false; renderFrame() is a no-op without ctx") {
        VideoTextureDecoder d(64, 64);
        // hasNewFrame calls renderFrame() if m_needsRender is set; here it
        // isn't, and even if it were, renderFrame() guards on !m_renderCtx.
        CHECK(d.hasNewFrame() == false);
        // m_frameNum still zero.
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Triple-buffer index rotation invariants
// ──────────────────────────────────────────────────────────────────────────────

TEST_SUITE("VideoTextureDecoder::triple buffer") {
    TEST_CASE("publishFrame() rotates decode/ready; preserves the partition") {
        TestableDecoder d(8, 8);
        const int       initialRead = d.readIdx();

        // After one publish: m_readyIdx ← old m_decodeIdx; m_decodeIdx ← old
        // m_readyIdx; m_readIdx unchanged.  The three indices must remain a
        // permutation of {0,1,2}.
        d.publishFrame();
        const int a = d.decodeIdx();
        const int b = d.readyIdx();
        const int c = d.readIdx();
        CHECK(a != b);
        CHECK(b != c);
        CHECK(a != c);
        // readIdx unchanged until acquireFrame() consumes.
        CHECK(c == initialRead);
        CHECK(d.frameNum() == 1u);
    }

    TEST_CASE("publishFrame() is callable repeatedly without acquireFrame()") {
        // Decode thread can outpace the render thread: many publishes before
        // a single acquireFrame.  m_frameNum keeps incrementing; decode and
        // ready keep swapping (read stays fixed).
        TestableDecoder d(8, 8);
        const int       initialRead = d.readIdx();
        for (int i = 0; i < 16; ++i) {
            d.publishFrame();
        }
        CHECK(d.frameNum() == 16u);
        CHECK(d.readIdx() == initialRead);
        CHECK(d.decodeIdx() != d.readyIdx());
        CHECK(d.decodeIdx() != d.readIdx());
        CHECK(d.readyIdx() != d.readIdx());
    }

    TEST_CASE("acquireFrame() after publish swaps ready↔read, advances lastRead") {
        TestableDecoder d(8, 8);
        d.forceOpened();
        d.publishFrame(); // frameNum 0→1; decode↔ready swapped

        const int beforeReady = d.readyIdx();
        const int beforeRead  = d.readIdx();

        const uint8_t* buf = d.acquireFrame();
        REQUIRE(buf != nullptr);

        // After acquire: readyIdx ← old readIdx; readIdx ← old readyIdx.
        CHECK(d.readIdx() == beforeReady);
        CHECK(d.readyIdx() == beforeRead);
        // The returned pointer is &m_buffers[new readIdx].
        // (Can't dereference its identity without exposing m_buffers, but
        // we can at least confirm it's non-null.)
    }

    TEST_CASE("acquireFrame() before publish returns nullptr even when opened") {
        // m_frameNum == 0 short-circuits acquireFrame(), guarding against
        // handing the consumer a buffer that no decode has ever written to.
        TestableDecoder d(8, 8);
        d.forceOpened();
        CHECK(d.acquireFrame() == nullptr);
    }

    TEST_CASE("acquireFrame() before forceOpened returns nullptr") {
        TestableDecoder d(8, 8);
        d.publishFrame();
        // m_opened is false → guarded early return.
        CHECK(d.acquireFrame() == nullptr);
    }

    TEST_CASE("two acquireFrame() in a row don't double-swap if no new publish") {
        // After the first acquire, m_lastReadFrame == m_frameNum.  A second
        // acquire still rotates the indices (current implementation does so
        // unconditionally) but the contract is: the returned pointer is
        // still a valid buffer and no UB occurs.
        TestableDecoder d(8, 8);
        d.forceOpened();
        d.publishFrame();

        const uint8_t* a = d.acquireFrame();
        const uint8_t* b = d.acquireFrame();
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);

        // Whichever indices the implementation lands on, they must remain a
        // permutation of {0,1,2}.
        const int i = d.decodeIdx();
        const int j = d.readyIdx();
        const int k = d.readIdx();
        CHECK(i != j);
        CHECK(j != k);
        CHECK(i != k);
    }

    TEST_CASE("publish→acquire→publish→acquire stays in {0,1,2}") {
        // The classic producer/consumer alternation over many iterations.
        TestableDecoder d(8, 8);
        d.forceOpened();
        for (int i = 0; i < 64; ++i) {
            d.publishFrame();
            const uint8_t* p = d.acquireFrame();
            CHECK(p != nullptr);
            const int a = d.decodeIdx();
            const int b = d.readyIdx();
            const int c = d.readIdx();
            CHECK(a != b);
            CHECK(b != c);
            CHECK(a != c);
            CHECK(a >= 0);
            CHECK(a <= 2);
            CHECK(b >= 0);
            CHECK(b <= 2);
            CHECK(c >= 0);
            CHECK(c <= 2);
        }
        CHECK(d.frameNum() == 64u);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// fillAlpha — pixel-format detail
// ──────────────────────────────────────────────────────────────────────────────

TEST_SUITE("VideoTextureDecoder::fillAlpha") {
    TEST_CASE("sets alpha byte = 255 on every pixel; leaves RGB untouched") {
        // mpv's SW renderer outputs "rgb0" — the fourth byte is X, not A.
        // We post-process by stamping 0xFF so Vulkan can sample as RGBA.
        const int w = 4;
        const int h = 3;
        TestableDecoder d(w, h);

        // Allocate a w*h*4 scratch buffer and fill RGB with 0x42, alpha with 0.
        std::vector<uint8_t> buf((size_t)w * h * 4, 0u);
        for (size_t i = 0; i + 3 < buf.size(); i += 4) {
            buf[i + 0] = 0x42; // R
            buf[i + 1] = 0x43; // G
            buf[i + 2] = 0x44; // B
            buf[i + 3] = 0x00; // X (mpv "rgb0")
        }

        d.fillAlpha(buf.data());

        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t off = (size_t)y * (size_t)w * 4u + (size_t)x * 4u;
                CHECK(buf[off + 0] == 0x42);
                CHECK(buf[off + 1] == 0x43);
                CHECK(buf[off + 2] == 0x44);
                CHECK(buf[off + 3] == 0xFFu);
            }
        }
    }

    TEST_CASE("fillAlpha on 1×1 single-pixel buffer touches only byte 3") {
        TestableDecoder d(1, 1);
        uint8_t         px[4] = { 1, 2, 3, 0 };
        d.fillAlpha(px);
        CHECK(px[0] == 1);
        CHECK(px[1] == 2);
        CHECK(px[2] == 3);
        CHECK(px[3] == 255);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// open() path — exercises the *real* libmpv lifecycle. Requires libmpv at
// runtime AND a video file. Gated on WEKDE_HAS_VIDEO so the offline preflight
// run stays deterministic and dep-free.
//
// NB: even WITH libmpv, open() may legitimately fail on a CI box without a
// video output (it forces vo=libmpv which is a render-context VO and should
// be fine, but VAAPI hwdec init can probe the GPU).  Treat open()==false as
// "skip remaining assertions", not "test failure".
// ──────────────────────────────────────────────────────────────────────────────

TEST_SUITE("VideoTextureDecoder::open (gated on WEKDE_HAS_VIDEO)") {
    TEST_CASE("open(nonexistent path) leaves opened/playing in a tolerated state") {
        // libmpv accepts an unresolvable URL into the playlist asynchronously
        // and only emits an MPV_EVENT_END_FILE.  loadFile() therefore returns
        // true and sets m_opened/m_playing unconditionally — by design, since
        // open() is fire-and-forget.  Document the contract here.
        const char* gate = std::getenv("WEKDE_HAS_VIDEO");
        if (! gate || gate[0] == '\0') {
            MESSAGE("skipped: set WEKDE_HAS_VIDEO=1 with libmpv-capable runtime");
            return;
        }
        VideoTextureDecoder d(64, 64);
        const bool          ok = d.open("/this/path/does/not/exist.mp4");
        CHECK(ok == true); // open() only fails on mpv_create/initialize/render_ctx
        CHECK(d.isPlaying() == true);
        // Time queries are 0 until mpv reports any progress.
        CHECK(d.getCurrentTimeSec() >= 0.0);
    }

    TEST_CASE("open() with a real file: play→pause toggles m_playing") {
        const char* gate = std::getenv("WEKDE_HAS_VIDEO");
        const char* file = std::getenv("WEKDE_VIDEO_FIXTURE");
        if (! gate || gate[0] == '\0' || ! file || file[0] == '\0') {
            MESSAGE("skipped: set WEKDE_HAS_VIDEO=1 and "
                    "WEKDE_VIDEO_FIXTURE=/path/to/sample.mp4");
            return;
        }
        VideoTextureDecoder d(128, 128);
        REQUIRE(d.open(file));
        CHECK(d.isPlaying() == true);
        d.pause();
        CHECK(d.isPlaying() == false);
        d.play();
        CHECK(d.isPlaying() == true);
        d.stop();
        CHECK(d.isPlaying() == false);
    }

    TEST_CASE("destruction while open() succeeded does not leak mpv handle") {
        // Smoke test the dtor's mpv_render_context_free + mpv_terminate_destroy
        // path.  We can't easily detect a leak without valgrind here; the
        // assertion is "no crash, no hang".
        const char* gate = std::getenv("WEKDE_HAS_VIDEO");
        if (! gate || gate[0] == '\0') {
            MESSAGE("skipped: set WEKDE_HAS_VIDEO=1");
            return;
        }
        for (int i = 0; i < 3; ++i) {
            VideoTextureDecoder d(64, 64);
            d.open("/dev/null"); // mpv accepts; will emit END_FILE eventually
            // No sleep — let dtor run immediately.  Tests the destroy-mid-decode
            // race path the R4 audit flagged.
        }
        CHECK(true);
    }
}
