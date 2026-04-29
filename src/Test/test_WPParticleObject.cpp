#include <doctest.h>

#include "Fs/VFS.h"
#include "Fs/MemBinaryStream.h"
#include "wpscene/WPParticleObject.h"

#include <memory>
#include <nlohmann/json.hpp>
#include <unordered_map>

using namespace wallpaper;
using namespace wallpaper::fs;
using namespace wallpaper::wpscene;

namespace
{

ParticleRender ParseRender(const char* text) {
    auto           j = nlohmann::json::parse(text);
    ParticleRender r;
    REQUIRE(r.FromJson(j));
    return r;
}

ParticleControlpoint ParseControlPoint(const char* text) {
    auto                 j = nlohmann::json::parse(text);
    ParticleControlpoint cp;
    REQUIRE(cp.FromJson(j));
    return cp;
}

Emitter ParseEmitter(const char* text) {
    auto    j = nlohmann::json::parse(text);
    Emitter e;
    REQUIRE(e.FromJson(j));
    return e;
}

ParticleInstanceoverride ParseOverride(const char* text) {
    auto                     j = nlohmann::json::parse(text);
    ParticleInstanceoverride o;
    REQUIRE(o.FromJosn(j));
    return o;
}

} // namespace

TEST_SUITE("ParticleControlpoint::FromJson") {
    TEST_CASE("explicit offset is parsed and offset_is_null is false") {
        auto cp = ParseControlPoint(R"({
            "id": 3, "flags": 4, "offset": "1 2 3", "parentcontrolpoint": 2
        })");
        CHECK(cp.id == 3);
        CHECK(cp.parentcontrolpoint == 2);
        CHECK(cp.offset[0] == doctest::Approx(1.0f));
        CHECK(cp.offset[1] == doctest::Approx(2.0f));
        CHECK(cp.offset[2] == doctest::Approx(3.0f));
        CHECK_FALSE(cp.offset_is_null);
    }

    TEST_CASE("offset:null sets offset_is_null") {
        auto cp = ParseControlPoint(R"({"id": 1, "offset": null})");
        CHECK(cp.offset_is_null);
    }

    TEST_CASE("absent offset leaves offset_is_null = false") {
        auto cp = ParseControlPoint(R"({"id": 1})");
        CHECK_FALSE(cp.offset_is_null);
    }

    TEST_CASE("flags bitfield captured") {
        auto cp = ParseControlPoint(R"({"id": 1, "flags": 6})");
        // 6 = worldspace(2) + follow_parent_particle(4)
        CHECK(cp.flags[ParticleControlpoint::FlagEnum::worldspace]);
        CHECK(cp.flags[ParticleControlpoint::FlagEnum::follow_parent_particle]);
        CHECK_FALSE(cp.flags[ParticleControlpoint::FlagEnum::link_mouse]);
    }
}

TEST_SUITE("ParticleRender::FromJson") {
    TEST_CASE("ropetrail picks up subdivision/length/maxlength") {
        auto r = ParseRender(R"({
            "name": "ropetrail",
            "subdivision": 8.0, "length": 0.5, "maxlength": 25.0
        })");
        CHECK(r.name == "ropetrail");
        CHECK(r.subdivision == doctest::Approx(8.0f));
        CHECK(r.length == doctest::Approx(0.5f));
        CHECK(r.maxlength == doctest::Approx(25.0f));
    }

    TEST_CASE("spritetrail picks up length/maxlength but defaults subdivision") {
        auto r = ParseRender(R"({
            "name": "spritetrail",
            "length": 0.1, "maxlength": 5.0,
            "subdivision": 99.0
        })");
        // spritetrail branch does NOT read subdivision → stays at default
        CHECK(r.name == "spritetrail");
        CHECK(r.length == doctest::Approx(0.1f));
        CHECK(r.maxlength == doctest::Approx(5.0f));
        CHECK(r.subdivision == doctest::Approx(3.0f));
    }

    TEST_CASE("rope* prefix branch reads only subdivision") {
        auto r = ParseRender(R"({
            "name": "ropething",
            "subdivision": 4.0, "length": 99.0
        })");
        CHECK(r.subdivision == doctest::Approx(4.0f));
        CHECK(r.length == doctest::Approx(0.05f)); // default
    }

    TEST_CASE("plain sprite leaves all numeric fields at defaults") {
        auto r = ParseRender(R"({"name": "sprite"})");
        CHECK(r.name == "sprite");
        CHECK(r.length == doctest::Approx(0.05f));
        CHECK(r.maxlength == doctest::Approx(10.0f));
        CHECK(r.subdivision == doctest::Approx(3.0f));
    }
}

TEST_SUITE("Emitter::FromJson") {
    TEST_CASE("baseline fields parse") {
        auto e = ParseEmitter(R"({
            "name": "box", "id": 5,
            "speedmin": 1.0, "speedmax": 2.0,
            "rate": 30.0,
            "instantaneous": 7,
            "directions":  "1 0 0",
            "distancemin": "0 0 0",
            "distancemax": "5 5 5",
            "origin":      "0 1 0",
            "sign":        "0 0 0"
        })");
        CHECK(e.name == "box");
        CHECK(e.id == 5);
        CHECK(e.rate == doctest::Approx(30.0f));
        CHECK(e.instantaneous == 7u);
        CHECK(e.directions[0] == doctest::Approx(1.0f));
    }

    TEST_CASE("controlpoint > 7 is wrapped (% 8)") {
        auto e = ParseEmitter(R"({"name": "x", "id": 1, "controlpoint": 9})");
        CHECK(e.controlpoint == 1); // 9 % 8
    }

    TEST_CASE("controlpoint == 7 is preserved (boundary)") {
        auto e = ParseEmitter(R"({"name": "x", "id": 1, "controlpoint": 7})");
        CHECK(e.controlpoint == 7);
    }

    TEST_CASE("sign array is normalised to {-1, 0, 1}") {
        auto e = ParseEmitter(R"({
            "name": "x", "id": 1,
            "sign": "42 -5 0"
        })");
        CHECK(e.sign[0] == 1);
        CHECK(e.sign[1] == -1);
        CHECK(e.sign[2] == 0);
    }

    TEST_CASE("periodic emission fields parse") {
        auto e = ParseEmitter(R"({
            "name": "x", "id": 1,
            "minperiodicdelay":    0.5,
            "maxperiodicdelay":    1.5,
            "minperiodicduration": 0.2,
            "maxperiodicduration": 0.8,
            "maxtoemitperperiod":  10,
            "duration":            5.0
        })");
        CHECK(e.minperiodicdelay == doctest::Approx(0.5f));
        CHECK(e.maxperiodicdelay == doctest::Approx(1.5f));
        CHECK(e.minperiodicduration == doctest::Approx(0.2f));
        CHECK(e.maxperiodicduration == doctest::Approx(0.8f));
        CHECK(e.maxtoemitperperiod == 10u);
        CHECK(e.duration == doctest::Approx(5.0f));
    }

    TEST_CASE("flags bitfield captured (one_per_frame at bit 1)") {
        // FlagEnum::one_per_frame = 1 → BitFlags treats this as bit index 1,
        // i.e. raw value 2.
        auto e = ParseEmitter(R"({"name": "x", "id": 1, "flags": 2})");
        CHECK(e.flags[Emitter::FlagEnum::one_per_frame]);
    }

    TEST_CASE("audioprocessingmode parses as the WE channel enum (1=L, 2=R, 3=Stereo)") {
        auto eOff = ParseEmitter(R"({"name": "x", "id": 1})");
        CHECK(eOff.audioprocessingmode == 0u);
        auto eL = ParseEmitter(R"({"name": "x", "id": 1, "audioprocessingmode": 1})");
        CHECK(eL.audioprocessingmode == 1u);
        auto eR = ParseEmitter(R"({"name": "x", "id": 1, "audioprocessingmode": 2})");
        CHECK(eR.audioprocessingmode == 2u);
        auto eS = ParseEmitter(R"({"name": "x", "id": 1, "audioprocessingmode": 3})");
        CHECK(eS.audioprocessingmode == 3u);
    }

    TEST_CASE("WE-shape audio keys default identity values when absent") {
        auto e = ParseEmitter(R"({"name": "x", "id": 1, "audioprocessingmode": 1})");
        CHECK_FALSE(e.audio_we_shape_authored);
        CHECK(e.audioprocessingfrequencystart == 0);
        CHECK(e.audioprocessingfrequencyend == 4);
        CHECK(e.audioprocessingbounds[0] == doctest::Approx(0.0f));
        CHECK(e.audioprocessingbounds[1] == doctest::Approx(1.0f));
        CHECK(e.audioprocessingexponent == doctest::Approx(1.0f));
        CHECK(e.audioprocessing == doctest::Approx(1.0f));
    }

    TEST_CASE("audioprocessingfrequencystart/end parse and trip the WE-shape flag") {
        auto e = ParseEmitter(R"({
            "name": "x", "id": 1,
            "audioprocessingmode": 1,
            "audioprocessingfrequencystart": 8,
            "audioprocessingfrequencyend": 12
        })");
        CHECK(e.audioprocessingfrequencystart == 8);
        CHECK(e.audioprocessingfrequencyend == 12);
        CHECK(e.audio_we_shape_authored);
    }

    TEST_CASE("audioprocessingbounds parses as a 2-vec and trips the WE-shape flag") {
        auto e = ParseEmitter(R"({
            "name": "x", "id": 1,
            "audioprocessingmode": 1,
            "audioprocessingbounds": "0.25 0.75"
        })");
        CHECK(e.audioprocessingbounds[0] == doctest::Approx(0.25f));
        CHECK(e.audioprocessingbounds[1] == doctest::Approx(0.75f));
        CHECK(e.audio_we_shape_authored);
    }

    TEST_CASE("audioprocessingexponent parses and trips the WE-shape flag") {
        auto e = ParseEmitter(R"({
            "name": "x", "id": 1,
            "audioprocessingmode": 1,
            "audioprocessingexponent": 2.5
        })");
        CHECK(e.audioprocessingexponent == doctest::Approx(2.5f));
        CHECK(e.audio_we_shape_authored);
    }

    TEST_CASE("audioprocessing (post-multiplier) parses and trips the WE-shape flag") {
        auto e = ParseEmitter(R"({
            "name": "x", "id": 1,
            "audioprocessingmode": 1,
            "audioprocessing": 1.5
        })");
        CHECK(e.audioprocessing == doctest::Approx(1.5f));
        CHECK(e.audio_we_shape_authored);
    }
}

TEST_SUITE("ParticleInstanceoverride::FromJosn") {
    TEST_CASE("absent fields stay at their defaults; enabled flips on") {
        auto o = ParseOverride(R"({})");
        CHECK(o.enabled);
        CHECK(o.alpha == doctest::Approx(1.0f));
        CHECK(o.brightness == doctest::Approx(1.0f));
        CHECK(o.size == doctest::Approx(1.0f));
        CHECK(o.lifetime == doctest::Approx(0.0f)); // defaults to 0 (no override)
        CHECK(o.rate == doctest::Approx(1.0f));
        CHECK(o.speed == doctest::Approx(1.0f));
        CHECK(o.count == doctest::Approx(1.0f));
        CHECK_FALSE(o.overColor);
        CHECK_FALSE(o.overColorn);
    }

    TEST_CASE("color override flips overColor flag") {
        auto o = ParseOverride(R"({"color": "1 0 0"})");
        CHECK(o.overColor);
        CHECK_FALSE(o.overColorn);
        CHECK(o.color[0] == doctest::Approx(1.0f));
        CHECK(o.color[1] == doctest::Approx(0.0f));
    }

    TEST_CASE("colorn override flips overColorn flag (mutually exclusive with color)") {
        auto o = ParseOverride(R"({"colorn": "0 0.5 1"})");
        CHECK_FALSE(o.overColor);
        CHECK(o.overColorn);
        CHECK(o.colorn[2] == doctest::Approx(1.0f));
    }

    TEST_CASE("controlpointN offset overrides activate one slot") {
        auto o = ParseOverride(R"({
            "controlpoint0": "1 0 0",
            "controlpoint3": "0 0 5"
        })");
        CHECK(o.controlpointOverrides[0].active);
        CHECK(o.controlpointOverrides[0].offset[0] == doctest::Approx(1.0f));
        CHECK_FALSE(o.controlpointOverrides[1].active);
        CHECK_FALSE(o.controlpointOverrides[2].active);
        CHECK(o.controlpointOverrides[3].active);
        CHECK(o.controlpointOverrides[3].offset[2] == doctest::Approx(5.0f));
        CHECK_FALSE(o.controlpointOverrides[4].active);
    }

    TEST_CASE("controlpointangleN overrides activate independent of position") {
        auto o = ParseOverride(R"({
            "controlpointangle2": "0 1.5 0"
        })");
        CHECK_FALSE(o.controlpointOverrides[2].active);
        CHECK(o.controlpointOverrides[2].anglesActive);
        CHECK(o.controlpointOverrides[2].angles[1] == doctest::Approx(1.5f));
    }

    TEST_CASE("position + angle on the same CP set both flags") {
        auto o = ParseOverride(R"({
            "controlpoint5":      "10 0 0",
            "controlpointangle5": "0 0 0.7"
        })");
        CHECK(o.controlpointOverrides[5].active);
        CHECK(o.controlpointOverrides[5].anglesActive);
    }

    TEST_CASE("lifetime override is captured as absolute value") {
        auto o = ParseOverride(R"({"lifetime": 3.5})");
        CHECK(o.lifetime == doctest::Approx(3.5f));
    }
}

// =============================================================================
// Particle / ParticleChild / WPParticleObject — VFS-backed parses
// =============================================================================

namespace
{

class MemFs : public Fs {
public:
    void add(std::string path, std::string content) {
        std::vector<uint8_t> bytes(content.begin(), content.end());
        m_files[std::move(path)] = std::move(bytes);
    }
    bool Contains(std::string_view path) const override {
        return m_files.count(std::string(path)) > 0;
    }
    std::shared_ptr<IBinaryStream> Open(std::string_view path) override {
        auto it = m_files.find(std::string(path));
        if (it == m_files.end()) return nullptr;
        auto copy = it->second;
        return std::make_shared<MemBinaryStream>(std::move(copy));
    }
    std::shared_ptr<IBinaryStreamW> OpenW(std::string_view) override { return nullptr; }

private:
    std::unordered_map<std::string, std::vector<uint8_t>> m_files;
};

std::unique_ptr<VFS> Vfs(const std::unordered_map<std::string, std::string>& files) {
    auto v  = std::make_unique<VFS>();
    auto fs = std::make_unique<MemFs>();
    for (const auto& [p, c] : files) fs->add("/" + p, c);
    REQUIRE(v->Mount("/assets", std::move(fs)));
    return v;
}

constexpr const char* kFlatMat = R"({"passes": [{"shader": "flat", "blending": "translucent"}]})";

// Minimal valid particle file (ref'd by particle/<name>.json reads).  Has at
// least one emitter (parser requires that) and a material.
constexpr const char* kMinimalParticleFile = R"({
    "emitter": [{"name": "box", "id": 1}],
    "material": "materials/util/m.json",
    "maxcount": 16,
    "starttime": 0
})";

constexpr const char* kMinimalParticleFileWithRenderer = R"({
    "emitter":  [{"name": "box", "id": 1}],
    "renderer": [{"name": "ropetrail", "subdivision": 4}],
    "material": "materials/util/m.json",
    "maxcount": 16,
    "starttime": 0
})";

} // namespace

TEST_SUITE("Particle::FromJson") {
    TEST_CASE("missing emitter array rejects parse") {
        auto vfs = Vfs({ { "materials/util/m.json", kFlatMat } });
        auto j   = nlohmann::json::parse(R"({
            "material": "materials/util/m.json",
            "maxcount": 8, "starttime": 0
        })");
        Particle p;
        CHECK_FALSE(p.FromJson(j, *vfs));
    }

    TEST_CASE("missing material rejects parse") {
        auto vfs = Vfs({});
        auto j   = nlohmann::json::parse(R"({
            "emitter": [{"name": "box", "id": 1}],
            "maxcount": 8, "starttime": 0
        })");
        Particle p;
        CHECK_FALSE(p.FromJson(j, *vfs));
    }

    TEST_CASE("absent renderer block injects a default sprite renderer") {
        auto vfs = Vfs({ { "materials/util/m.json", kFlatMat } });
        auto j   = nlohmann::json::parse(R"({
            "emitter":  [{"name": "box", "id": 1}],
            "material": "materials/util/m.json",
            "maxcount": 4,  "starttime": 0
        })");
        Particle p;
        REQUIRE(p.FromJson(j, *vfs));
        REQUIRE(p.renderers.size() == 1u);
        CHECK(p.renderers[0].name == "sprite");
    }

    TEST_CASE("explicit empty renderer array does NOT inject default") {
        auto vfs = Vfs({ { "materials/util/m.json", kFlatMat } });
        auto j   = nlohmann::json::parse(R"({
            "emitter":  [{"name": "box", "id": 1}],
            "renderer": [],
            "material": "materials/util/m.json",
            "maxcount": 4,  "starttime": 0
        })");
        Particle p;
        REQUIRE(p.FromJson(j, *vfs));
        CHECK(p.renderers.empty());
    }

    TEST_CASE("initializer + operator + controlpoint arrays are captured") {
        auto vfs = Vfs({ { "materials/util/m.json", kFlatMat } });
        auto j   = nlohmann::json::parse(R"({
            "emitter":     [{"name": "box", "id": 1}],
            "initializer": [{"name": "lifetimerandom", "min": "1", "max": "2"}],
            "operator":    [{"name": "movement"}],
            "controlpoint":[{"id": 0, "offset": "0 1 0"}, {"id": 1, "offset": null}],
            "material":    "materials/util/m.json",
            "maxcount": 4, "starttime": 0
        })");
        Particle p;
        REQUIRE(p.FromJson(j, *vfs));
        CHECK(p.initializers.size() == 1u);
        CHECK(p.operators.size() == 1u);
        REQUIRE(p.controlpoints.size() == 2u);
        CHECK(p.controlpoints[0].id == 0);
        CHECK(p.controlpoints[1].offset_is_null);
    }

    TEST_CASE("animationmode + sequencemultiplier + flags") {
        auto vfs = Vfs({ { "materials/util/m.json", kFlatMat } });
        auto j   = nlohmann::json::parse(R"({
            "emitter":  [{"name": "box", "id": 1}],
            "material": "materials/util/m.json",
            "maxcount": 4, "starttime": 0,
            "animationmode": "loop",
            "sequencemultiplier": 2.0,
            "flags": 4
        })");
        Particle p;
        REQUIRE(p.FromJson(j, *vfs));
        CHECK(p.animationmode == "loop");
        CHECK(p.sequencemultiplier == doctest::Approx(2.0f));
        // flags=4 sets bit 2 (perspective)
        CHECK(p.flags[Particle::FlagEnum::perspective]);
    }

    TEST_CASE("renderer block of multiple kinds is parsed") {
        auto vfs = Vfs({ { "materials/util/m.json", kFlatMat } });
        auto j   = nlohmann::json::parse(R"({
            "emitter":  [{"name": "box", "id": 1}],
            "renderer": [
                {"name": "ropetrail", "subdivision": 8, "length": 0.5, "maxlength": 25},
                {"name": "sprite"}
            ],
            "material": "materials/util/m.json",
            "maxcount": 4, "starttime": 0
        })");
        Particle p;
        REQUIRE(p.FromJson(j, *vfs));
        REQUIRE(p.renderers.size() == 2u);
        CHECK(p.renderers[0].name == "ropetrail");
        CHECK(p.renderers[0].subdivision == doctest::Approx(8.0f));
        CHECK(p.renderers[1].name == "sprite");
    }
}

TEST_SUITE("ParticleChild::FromJson") {
    TEST_CASE("name + child obj load") {
        auto vfs = Vfs({
            { "child.json",                 kMinimalParticleFileWithRenderer },
            { "materials/util/m.json",      kFlatMat },
        });
        auto j = nlohmann::json::parse(R"({
            "name": "child.json", "type": "static",
            "maxcount": 32, "controlpointstartindex": 1, "probability": 0.5,
            "origin": "1 2 3", "scale": "0.5 0.5 0.5", "angles": "0 0 0"
        })");
        ParticleChild c;
        REQUIRE(c.FromJson(j, *vfs));
        CHECK(c.name == "child.json");
        CHECK(c.type == "static");
        CHECK(c.maxcount == 32);
        CHECK(c.controlpointstartindex == 1);
        CHECK(c.probability == doctest::Approx(0.5f));
        CHECK(c.origin[0] == doctest::Approx(1.0f));
        CHECK(c.scale[1] == doctest::Approx(0.5f));
    }

    TEST_CASE("empty name rejects parse") {
        auto vfs = Vfs({});
        auto j   = nlohmann::json::parse(R"({"type": "static"})");
        ParticleChild c;
        CHECK_FALSE(c.FromJson(j, *vfs));
    }

    TEST_CASE("missing child file rejects parse") {
        auto vfs = Vfs({});
        auto j   = nlohmann::json::parse(R"({"name": "no_such_child.json"})");
        ParticleChild c;
        CHECK_FALSE(c.FromJson(j, *vfs));
    }
}

TEST_SUITE("WPParticleObject::FromJson") {
    TEST_CASE("complete particle object parses end-to-end") {
        auto vfs = Vfs({
            { "particles/myFx.json",   kMinimalParticleFile },
            { "materials/util/m.json", kFlatMat },
        });
        auto j = nlohmann::json::parse(R"({
            "id": 100, "name": "myParticles",
            "particle": "particles/myFx.json",
            "origin": "10 20 30", "scale": "1 1 1", "angles": "0 0 0",
            "parent": 5,
            "visible": true,
            "parallaxDepth": "0.1 0.2",
            "disablepropagation": true
        })");
        WPParticleObject obj;
        REQUIRE(obj.FromJson(j, *vfs));
        CHECK(obj.id == 100);
        CHECK(obj.parent_id == 5);
        CHECK(obj.name == "myParticles");
        CHECK(obj.particle == "particles/myFx.json");
        CHECK(obj.origin[2] == doctest::Approx(30.0f));
        CHECK(obj.disablepropagation);
        CHECK(obj.parallaxDepth[1] == doctest::Approx(0.2f));
    }

    TEST_CASE("instanceoverride object flips override.enabled") {
        auto vfs = Vfs({
            { "particles/myFx.json",   kMinimalParticleFile },
            { "materials/util/m.json", kFlatMat },
        });
        auto j = nlohmann::json::parse(R"({
            "id": 1, "name": "x",
            "particle": "particles/myFx.json",
            "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
            "instanceoverride": { "rate": 2.0 }
        })");
        WPParticleObject obj;
        REQUIRE(obj.FromJson(j, *vfs));
        CHECK(obj.instanceoverride.enabled);
        CHECK(obj.instanceoverride.rate == doctest::Approx(2.0f));
    }

    TEST_CASE("instanceoverride: null is treated as absent") {
        auto vfs = Vfs({
            { "particles/myFx.json",   kMinimalParticleFile },
            { "materials/util/m.json", kFlatMat },
        });
        auto j = nlohmann::json::parse(R"({
            "id": 1, "name": "x",
            "particle": "particles/myFx.json",
            "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
            "instanceoverride": null
        })");
        WPParticleObject obj;
        REQUIRE(obj.FromJson(j, *vfs));
        CHECK_FALSE(obj.instanceoverride.enabled);
    }

    TEST_CASE("missing particle file rejects parse") {
        auto vfs = Vfs({});
        auto j   = nlohmann::json::parse(R"({
            "id": 1, "name": "x",
            "particle": "particles/missing.json",
            "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0"
        })");
        WPParticleObject obj;
        CHECK_FALSE(obj.FromJson(j, *vfs));
    }
}
