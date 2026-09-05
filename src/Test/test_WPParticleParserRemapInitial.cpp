// remapinitialvalue: the output shapes real content actually ships.
//
// Every instance of this initializer that Wallpaper Engine ships, and the one
// that turned up in a workshop scene, uses `input: distancetocontrolpoint` and
// either `output: "color"` (with vec3 output ranges) or no `output` key at all.
// `output: "size"` — what the older cases elsewhere in this suite pin — appears
// nowhere real, so these keep the ladder honest against the actual vocabulary.

#include <doctest.h>

#include "WPParticleParser.hpp"
#include "Particle/Particle.h"
#include "Particle/ParticleModify.h"
#include "Utils/Logging.h"
#include "wpscene/WPParticleObject.h"

#include <Eigen/Core>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace wallpaper;
using nlohmann::json;

namespace remap_initial_helpers
{

inline Particle makeParticle() {
    Particle p;
    p.lifetime        = 1.0f;
    p.init.lifetime   = 1.0f;
    p.alpha           = 1.0f;
    p.init.alpha      = 1.0f;
    p.size            = 20.0f;
    p.init.size       = 20.0f;
    p.color           = Eigen::Vector3f(1, 1, 1);
    p.init.color      = p.color;
    p.position        = Eigen::Vector3f(0, 0, 0);
    p.velocity        = Eigen::Vector3f(0, 0, 0);
    p.rotation        = Eigen::Vector3f(0, 0, 0);
    p.angularVelocity = Eigen::Vector3f(0, 0, 0);
    p.random_seed     = 0xdeadbeef;
    return p;
}

// A CP table with slot 1 pushed out along +x, matching how the shipped
// presets anchor the remap on `inputcontrolpoint0: 1`.
inline std::vector<ParticleControlpoint> cpsWithSlot1At(double x) {
    std::vector<ParticleControlpoint> cps(8, ParticleControlpoint {});
    for (auto& cp : cps) {
        cp.resolved = Eigen::Vector3d(0, 0, 0);
        cp.velocity = Eigen::Vector3d(0, 0, 0);
    }
    cps[1].resolved = Eigen::Vector3d(x, 0, 0);
    return cps;
}

inline ParticleInitOp build(const json& j, const std::vector<ParticleControlpoint>& cps) {
    return WPParticleParser::genParticleInitOp(
        j, std::span<const ParticleControlpoint>(cps.data(), cps.size()));
}

// Same table, but only the first `slots` entries are handed to the parser.  The
// entries past the span are still live objects with known values, so a read
// that overruns the span lands on something recognisable instead of garbage.
inline ParticleInitOp buildWithSlots(const json& j, const std::vector<ParticleControlpoint>& cps,
                                     std::size_t slots) {
    return WPParticleParser::genParticleInitOp(
        j, std::span<const ParticleControlpoint>(cps.data(), slots));
}

// These parse paths keep the wallpaper alive and only produce a log line, so
// the log is what a test has to look at.
struct LogCapture {
    LogCapture() {
        lines().clear();
        wallpaper_log_test::setSink(&append);
    }
    ~LogCapture() { wallpaper_log_test::setSink(nullptr); }

    LogCapture(const LogCapture&)            = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    static std::vector<std::string>& lines() {
        static std::vector<std::string> v;
        return v;
    }
    static void append(int, const char* msg) { lines().emplace_back(msg); }

    static bool saw(std::string_view needle) {
        for (const auto& l : lines()) {
            if (l.find(needle) != std::string::npos) return true;
        }
        return false;
    }
};

} // namespace remap_initial_helpers

using namespace remap_initial_helpers;

TEST_SUITE("remapinitialvalue color output") {
    TEST_CASE("vec3 output range drives the spawn colour across the input range") {
        // The shape WE's own element preview ships: blue at the control point,
        // red at the far end of the input range.
        auto cps = cpsWithSlot1At(300.0);
        json j   = { { "name", "remapinitialvalue" },
                     { "input", "distancetocontrolpoint" },
                     { "inputcontrolpoint0", 1 },
                     { "inputrangemax", 300 },
                     { "operation", "remap" },
                     { "output", "color" },
                     { "outputrangemin", "0 0 1" },
                     { "outputrangemax", "1 0 0" } };
        auto init = build(j, cps);

        Particle near = makeParticle();
        near.position = Eigen::Vector3f(300, 0, 0); // sitting on CP1 → t = 0
        init(near, 0.0);
        CHECK(near.color.x() == doctest::Approx(0.0f));
        CHECK(near.color.y() == doctest::Approx(0.0f));
        CHECK(near.color.z() == doctest::Approx(1.0f));

        Particle far = makeParticle();
        far.position = Eigen::Vector3f(0, 0, 0); // 300 away → t = 1
        init(far, 0.0);
        CHECK(far.color.x() == doctest::Approx(1.0f));
        CHECK(far.color.y() == doctest::Approx(0.0f));
        CHECK(far.color.z() == doctest::Approx(0.0f));

        Particle mid = makeParticle();
        mid.position = Eigen::Vector3f(150, 0, 0); // 150 away → t = 0.5
        init(mid, 0.0);
        CHECK(mid.color.x() == doctest::Approx(0.5f));
        CHECK(mid.color.y() == doctest::Approx(0.0f));
        CHECK(mid.color.z() == doctest::Approx(0.5f));
    }

    TEST_CASE("colour write survives the per-frame Reset") {
        // ParticleSystem calls Reset() on every live particle each tick, which
        // restores colour from init.color — an initializer that only wrote the
        // live field would be erased before the first draw.
        auto cps  = cpsWithSlot1At(100.0);
        json j    = { { "name", "remapinitialvalue" },
                      { "input", "distancetocontrolpoint" },
                      { "inputcontrolpoint0", 1 },
                      { "inputrangemax", 100 },
                      { "operation", "remap" },
                      { "output", "color" },
                      { "outputrangemin", "0 0 1" },
                      { "outputrangemax", "1 0 0" } };
        auto init = build(j, cps);

        Particle p = makeParticle();
        p.position = Eigen::Vector3f(0, 0, 0);
        init(p, 0.0);
        ParticleModify::Reset(p);
        CHECK(p.color.x() == doctest::Approx(1.0f));
        CHECK(p.color.z() == doctest::Approx(0.0f));
    }

    TEST_CASE("multiply tints the randomized spawn colour") {
        auto cps  = cpsWithSlot1At(100.0);
        json j    = { { "name", "remapinitialvalue" },
                      { "input", "distancetocontrolpoint" },
                      { "inputcontrolpoint0", 1 },
                      { "inputrangemax", 100 },
                      { "operation", "multiply" },
                      { "output", "color" },
                      { "outputrangemin", 0.0 },
                      { "outputrangemax", 0.5 } };
        auto init = build(j, cps);

        Particle p = makeParticle();
        p.color    = Eigen::Vector3f(1.0f, 0.8f, 0.4f);
        p.position = Eigen::Vector3f(0, 0, 0); // t = 1 → factor 0.5 on every channel
        init(p, 0.0);
        CHECK(p.color.x() == doctest::Approx(0.5f));
        CHECK(p.color.y() == doctest::Approx(0.4f));
        CHECK(p.color.z() == doctest::Approx(0.2f));
    }

    TEST_CASE("particlecolor is the same output as color") {
        auto cps  = cpsWithSlot1At(100.0);
        json j    = { { "name", "remapinitialvalue" },
                      { "input", "distancetocontrolpoint" },
                      { "inputcontrolpoint0", 1 },
                      { "inputrangemax", 100 },
                      { "operation", "remap" },
                      { "output", "particlecolor" },
                      { "outputrangemin", "0 0 1" },
                      { "outputrangemax", "1 0 0" } };
        auto     init = build(j, cps);
        Particle p    = makeParticle();
        p.position    = Eigen::Vector3f(0, 0, 0);
        init(p, 0.0);
        CHECK(p.color.x() == doctest::Approx(1.0f));
        CHECK(p.color.z() == doctest::Approx(0.0f));
    }
}

TEST_SUITE("remapinitialvalue size output") {
    TEST_CASE("multiply survives the per-frame Reset") {
        auto cps  = cpsWithSlot1At(50.0);
        json j    = { { "name", "remapinitialvalue" },
                      { "input", "distancetocontrolpoint" },
                      { "inputcontrolpoint0", 1 },
                      { "inputrangemax", 50 },
                      { "operation", "multiply" },
                      { "output", "size" } };
        auto init = build(j, cps);

        Particle p = makeParticle();
        p.size     = 10.0f;
        p.position = Eigen::Vector3f(25, 0, 0); // 25 of 50 → t = 0.5
        init(p, 0.0);
        CHECK(p.size == doctest::Approx(5.0f));
        ParticleModify::Reset(p);
        CHECK(p.size == doctest::Approx(5.0f));
    }
}

TEST_SUITE("remapinitialvalue reports the shapes it cannot serve") {
    TEST_CASE("an absent output leaves the particle alone and says so") {
        LogCapture log;
        auto       cps = cpsWithSlot1At(50.0);
        // The shipped lightning presets, and the workshop scene that copies
        // them, look exactly like this — no output key at all.
        json j    = { { "name", "remapinitialvalue" },
                      { "input", "distancetocontrolpoint" },
                      { "inputcontrolpoint0", 1 },
                      { "inputrangemax", 50 },
                      { "operation", "multiply" } };
        auto init = build(j, cps);

        Particle p = makeParticle();
        p.position = Eigen::Vector3f(25, 0, 0);
        init(p, 0.0);
        CHECK(p.size == doctest::Approx(20.0f));
        CHECK(p.alpha == doctest::Approx(1.0f));
        CHECK(p.color.x() == doctest::Approx(1.0f));
        CHECK(log.saw("remapinitialvalue"));
        CHECK(log.saw("output"));
    }

    TEST_CASE("an output it cannot write names the value") {
        LogCapture log;
        auto       cps = cpsWithSlot1At(50.0);
        json       j   = { { "name", "remapinitialvalue" },
                           { "input", "distancetocontrolpoint" },
                           { "inputcontrolpoint0", 1 },
                           { "output", "position" } };
        auto init      = build(j, cps);

        Particle p = makeParticle();
        init(p, 0.0);
        CHECK(p.position.norm() == doctest::Approx(0.0f));
        CHECK(log.saw("position"));
    }

    TEST_CASE("an input it cannot read names the value") {
        LogCapture log;
        auto       cps = cpsWithSlot1At(50.0);
        json       j   = { { "name", "remapinitialvalue" },
                           { "input", "particlelifetime" },
                           { "output", "size" } };
        auto init      = build(j, cps);

        Particle p = makeParticle();
        init(p, 0.0);
        CHECK(log.saw("particlelifetime"));
    }

    TEST_CASE("the shapes it does serve stay quiet") {
        LogCapture log;
        auto       cps = cpsWithSlot1At(50.0);
        json       j   = { { "name", "remapinitialvalue" },
                           { "input", "distancetocontrolpoint" },
                           { "inputcontrolpoint0", 1 },
                           { "output", "color" },
                           { "operation", "remap" } };
        auto init      = build(j, cps);
        Particle p     = makeParticle();
        init(p, 0.0);
        CHECK_FALSE(log.saw("remapinitialvalue"));
    }
}

TEST_SUITE("remapinitialvalue stays inside the control-point table it was handed") {
    // The authored index only clamps against the 0..7 ceiling; nothing ties it
    // to how many slots the span actually carries.  Both cases below name slot
    // 1, sitting 300 units out along +x, and differ only in whether the table
    // reaches it: in range the spawn colour ends up red (t = 1), out of range
    // the distance reads as 0 and the colour stays blue.
    static json remapToSlot1() {
        return { { "name", "remapinitialvalue" },
                 { "input", "distancetocontrolpoint" },
                 { "inputcontrolpoint0", 1 },
                 { "inputrangemax", 300 },
                 { "operation", "remap" },
                 { "output", "color" },
                 { "outputrangemin", "0 0 1" },
                 { "outputrangemax", "1 0 0" } };
    }

    TEST_CASE("a table of one slot does not read the slot after it") {
        auto     cps  = cpsWithSlot1At(300.0);
        auto     init = buildWithSlots(remapToSlot1(), cps, 1);
        Particle p    = makeParticle();
        p.position    = Eigen::Vector3f(0, 0, 0);
        init(p, 0.0);
        CHECK(p.color.x() == doctest::Approx(0.0f));
        CHECK(p.color.z() == doctest::Approx(1.0f));
    }

    TEST_CASE("a table of two slots reads the last one") {
        auto     cps  = cpsWithSlot1At(300.0);
        auto     init = buildWithSlots(remapToSlot1(), cps, 2);
        Particle p    = makeParticle();
        p.position    = Eigen::Vector3f(0, 0, 0);
        init(p, 0.0);
        CHECK(p.color.x() == doctest::Approx(1.0f));
        CHECK(p.color.z() == doctest::Approx(0.0f));
    }
}

TEST_SUITE("remapvalue reports the outputs it cannot write") {
    TEST_CASE("output position names the value instead of silently doing nothing") {
        LogCapture log;
        json       j = { { "name", "remapvalue" },
                         { "input", "particlesystemtime" },
                         { "output", "position" } };
        wpscene::ParticleInstanceoverride over;
        over.enabled = false;
        auto op      = WPParticleParser::genParticleOperatorOp(j, over);
        (void)op;
        CHECK(log.saw("remapvalue"));
        CHECK(log.saw("position"));
    }
}
