// Particle quads are drawn through a packed 16-bit index buffer: six indices
// per quad, four vertices per quad.  A 16-bit index cannot name a vertex past
// 65535, so quad 16384 is the first one it cannot reach.  These cases pin what
// the generator does when a scene asks for more quads than that — it stops at
// the addressable limit, reports how many quads it actually indexed, and says
// so in the log rather than wrapping indices back onto the first particles.

#include <doctest.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "Particle/WPParticleRawGener_TestHooks.hpp"
#include "Scene/SceneIndexArray.h"
#include "Utils/Logging.h"

using wallpaper::SceneIndexArray;
using wallpaper::test_hooks::MaxU16QuadCount;
using wallpaper::test_hooks::TestUpdateIndexArray;

namespace
{
uint16_t IndexAt(const SceneIndexArray& arr, std::size_t i) {
    return reinterpret_cast<const uint16_t*>(arr.Data())[i];
}

std::string g_last_error_log;

void errorCapturingSink(int level, const char* msg) {
    if (level == LOGLEVEL_ERROR) g_last_error_log = msg;
}

struct SinkGuard {
    SinkGuard() {
        g_last_error_log.clear();
        wallpaper_log_test::setSink(&errorCapturingSink);
    }
    ~SinkGuard() { wallpaper_log_test::setSink(nullptr); }
};
} // namespace

TEST_SUITE("particle quad index generation") {

    TEST_CASE("a quad count past the 16-bit reach is reported in the log") {
        // The overflow persists for as long as the scene keeps that many
        // particles alive, so the report is throttled.  Driving the clamp
        // repeatedly from the already-full state (no quads left to write, so
        // each call is cheap) guarantees we land on a reporting call whatever
        // phase the throttle is in.
        SinkGuard guard;
        for (int i = 0; i < 1000 && g_last_error_log.empty(); i++) {
            SceneIndexArray arr(1);
            TestUpdateIndexArray(MaxU16QuadCount(), MaxU16QuadCount() + 5000, arr);
        }
        CHECK(g_last_error_log.find("particle") != std::string::npos);
        CHECK(g_last_error_log.find("16384") != std::string::npos);
    }

    TEST_CASE("quads within the 16-bit reach get the two-triangle index pattern") {
        SceneIndexArray arr(3);
        CHECK(TestUpdateIndexArray(0, 3, arr) == 3);
        const uint16_t expected[18] = { 0, 1, 3, 1, 2,  3,  4, 5,  7,
                                        5, 6, 7, 8, 9, 11, 9, 10, 11 };
        for (std::size_t i = 0; i < 18; i++) {
            CHECK(IndexAt(arr, i) == expected[i]);
        }
        CHECK(arr.IndexElemCount() == 18);
    }

    TEST_CASE("generation resumes from a partially filled index array") {
        SceneIndexArray arr(4);
        REQUIRE(TestUpdateIndexArray(0, 2, arr) == 2);
        CHECK(TestUpdateIndexArray(2, 4, arr) == 4);
        CHECK(IndexAt(arr, 12) == 8);
        CHECK(IndexAt(arr, 17) == 11);
        CHECK(IndexAt(arr, 18) == 12);
        CHECK(IndexAt(arr, 23) == 15);
    }

    TEST_CASE("generation stops at the last quad a 16-bit index can address") {
        const std::size_t limit = MaxU16QuadCount();
        CHECK(limit == 16384);

        // Capacity for every requested quad, so nothing that follows is an
        // artefact of the array refusing the write.
        SceneIndexArray arr(limit + 3616);
        CHECK(TestUpdateIndexArray(0, limit + 3616, arr) == limit);

        // Last reachable quad: vertices 65532..65535.
        const std::size_t last = (limit - 1) * 6;
        CHECK(IndexAt(arr, last + 0) == 65532);
        CHECK(IndexAt(arr, last + 1) == 65533);
        CHECK(IndexAt(arr, last + 2) == 65535);
        CHECK(IndexAt(arr, last + 5) == 65535);

        // The first unreachable quad must be left alone, not wrapped back to
        // vertex 0 of the first particle.
        CHECK(IndexAt(arr, limit * 6 + 1) == 0);
        CHECK(IndexAt(arr, limit * 6 + 2) == 0);
        CHECK(arr.IndexElemCount() == limit * 6);
    }

    TEST_CASE("a quad count past 65535 terminates instead of spinning") {
        // The loop counter used to be 16 bits wide, so any count it could not
        // represent made it wrap and run forever — a hung render thread.
        SceneIndexArray arr(70000);
        CHECK(TestUpdateIndexArray(0, 70000, arr) == MaxU16QuadCount());
    }
}
