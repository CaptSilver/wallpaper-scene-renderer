#include <doctest.h>

#include "Fs/VFS.h"
#include "Fs/MemBinaryStream.h"
#include "wpscene/WPImageObject.h"

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace wallpaper;
using namespace wallpaper::fs;
using namespace wallpaper::wpscene;

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
    auto vfs = std::make_unique<VFS>();
    auto fs  = std::make_unique<MemFs>();
    for (const auto& [path, content] : files) fs->add("/" + path, content);
    REQUIRE(vfs->Mount("/assets", std::move(fs)));
    return vfs;
}

constexpr const char* kFlatMat = R"({"passes": [{"shader": "flat", "blending": "translucent"}]})";

} // namespace

// =============================================================================
// WPEffectCommand
// =============================================================================

TEST_SUITE("WPEffectCommand::FromJson") {
    TEST_CASE("populates command + target + source") {
        auto j = nlohmann::json::parse(R"({
            "command": "copy", "target": "_rt_default", "source": "_rt_FullCompoBuffer"
        })");
        WPEffectCommand c;
        REQUIRE(c.FromJson(j));
        CHECK(c.command == "copy");
        CHECK(c.target == "_rt_default");
        CHECK(c.source == "_rt_FullCompoBuffer");
    }
}

// =============================================================================
// WPEffectFbo
// =============================================================================

TEST_SUITE("WPEffectFbo::FromJson") {
    TEST_CASE("name + format + scale") {
        auto j = nlohmann::json::parse(R"({
            "name": "_rt_blur1", "format": "rgba8", "scale": 2
        })");
        WPEffectFbo fbo;
        REQUIRE(fbo.FromJson(j));
        CHECK(fbo.name == "_rt_blur1");
        CHECK(fbo.format == "rgba8");
        CHECK(fbo.scale == 2u);
    }

    TEST_CASE("scale=0 is auto-corrected to 1") {
        auto j = nlohmann::json::parse(R"({"name": "x", "format": "rgba8", "scale": 0})");
        WPEffectFbo fbo;
        REQUIRE(fbo.FromJson(j));
        CHECK(fbo.scale == 1u);
    }
}

// =============================================================================
// WPImageEffect — full effect parse with passes, fbos, commands, compose
// =============================================================================

TEST_SUITE("WPImageEffect::FromJson") {
    TEST_CASE("simple effect with one pass and fbo parses") {
        auto vfs = Vfs({
            { "effects/myFx/effect.json", R"({
                "name": "fx", "version": 1,
                "fbos": [{"name":"_rt_a", "format":"rgba8", "scale":1}],
                "passes": [{"material": "materials/util/effA.json"}]
            })" },
            { "materials/util/effA.json", kFlatMat },
        });
        auto j = nlohmann::json::parse(R"({"file":"effects/myFx/effect.json", "id": 11})");

        WPImageEffect e;
        REQUIRE(e.FromJson(j, *vfs));
        CHECK(e.id == 11);
        REQUIRE(e.fbos.size() == 1u);
        CHECK(e.fbos[0].name == "_rt_a");
        REQUIRE(e.passes.size() == 1u);
        REQUIRE(e.materials.size() == 1u);
        CHECK(e.materials[0].shader == "flat");
    }

    TEST_CASE("effect with command-only pass populates commands") {
        auto vfs = Vfs({
            { "effects/cmd/effect.json", R"({
                "name": "fx", "version": 1,
                "passes": [
                    {"material": "materials/util/effA.json"},
                    {"command": "copy", "target": "_rt_X", "source": "_rt_Y"}
                ]
            })" },
            { "materials/util/effA.json", kFlatMat },
        });
        auto j = nlohmann::json::parse(R"({"file":"effects/cmd/effect.json"})");

        WPImageEffect e;
        REQUIRE(e.FromJson(j, *vfs));
        REQUIRE(e.passes.size() == 1u); // command is NOT a render pass
        REQUIRE(e.commands.size() == 1u);
        CHECK(e.commands[0].command == "copy");
        // afterpos = passes.size() AT THE MOMENT the command was consumed (= 1).
        CHECK(e.commands[0].afterpos == 1);
    }

    TEST_CASE("compose:true on a 2-pass effect injects _rt_FullCompoBuffer1 fbo + binds") {
        auto vfs = Vfs({
            { "effects/compose/effect.json", R"({
                "name": "fx", "version": 1,
                "passes": [
                    {"material":"materials/util/effA.json", "compose": true},
                    {"material":"materials/util/effA.json"}
                ]
            })" },
            { "materials/util/effA.json", kFlatMat },
        });
        auto j = nlohmann::json::parse(R"({"file":"effects/compose/effect.json"})");

        WPImageEffect e;
        REQUIRE(e.FromJson(j, *vfs));
        // FBO injected
        REQUIRE(e.fbos.size() >= 1u);
        bool found_compo = false;
        for (const auto& f : e.fbos) {
            if (f.name == "_rt_FullCompoBuffer1") found_compo = true;
        }
        CHECK(found_compo);
        // Pass 0 retargeted to compo buffer; pass 1 binds it as input.
        CHECK(e.passes[0].target == "_rt_FullCompoBuffer1");
        REQUIRE(! e.passes[0].bind.empty());
        CHECK(e.passes[0].bind.back().name == "previous");
        REQUIRE(! e.passes[1].bind.empty());
        CHECK(e.passes[1].bind.back().name == "_rt_FullCompoBuffer1");
    }

    TEST_CASE("compose error: pass count != 2 returns false") {
        auto vfs = Vfs({
            { "effects/bad/effect.json", R"({
                "name": "fx", "version": 1,
                "passes": [
                    {"material":"materials/util/effA.json", "compose": true}
                ]
            })" },
            { "materials/util/effA.json", kFlatMat },
        });
        auto j = nlohmann::json::parse(R"({"file":"effects/bad/effect.json"})");

        WPImageEffect e;
        CHECK_FALSE(e.FromJson(j, *vfs));
    }

    TEST_CASE("scene-side `passes` array overrides effect-file pass with Update merge") {
        auto vfs = Vfs({
            { "effects/over/effect.json", R"({
                "name": "fx", "version": 1,
                "passes": [{"material":"materials/util/effA.json"}]
            })" },
            { "materials/util/effA.json", kFlatMat },
        });
        // Scene-side override adds a combo + texture slot.  WPMaterialPass::Update
        // overlays the scene-side fields onto the parsed pass.
        auto j = nlohmann::json::parse(R"({
            "file":"effects/over/effect.json",
            "passes": [{"combos": {"BLOOM": 1}}]
        })");

        WPImageEffect e;
        REQUIRE(e.FromJson(j, *vfs));
        REQUIRE(e.passes.size() == 1u);
        CHECK(e.passes[0].combos.at("BLOOM") == 1);
    }

    // Multi-pass override exercises the `passes[i++].Update(pass)` post-increment
    // — needed to distinguish i++ from i-- (mutation would access passes[-1]).
    TEST_CASE("scene-side `passes` array of 2 entries indexes both passes in order") {
        auto vfs = Vfs({
            { "effects/twoPass/effect.json", R"({
                "name": "fx", "version": 1,
                "passes": [
                    {"material":"materials/util/effA.json"},
                    {"material":"materials/util/effA.json"}
                ]
            })" },
            { "materials/util/effA.json", kFlatMat },
        });
        auto j = nlohmann::json::parse(R"({
            "file":"effects/twoPass/effect.json",
            "passes": [
                {"combos": {"PASS0": 1}},
                {"combos": {"PASS1": 2}}
            ]
        })");

        WPImageEffect e;
        REQUIRE(e.FromJson(j, *vfs));
        REQUIRE(e.passes.size() == 2u);
        // i=0 step set PASS0 on passes[0].
        CHECK(e.passes[0].combos.at("PASS0") == 1);
        // i=1 step set PASS1 on passes[1].  An `i--` mutation would write to
        // passes[-1] (UB), or never reach passes[1] — either way this CHECK
        // distinguishes original from mutation.
        CHECK(e.passes[1].combos.at("PASS1") == 2);
    }

    TEST_CASE("scene-side `passes` array longer than effect's is rejected") {
        auto vfs = Vfs({
            { "effects/over/effect.json", R"({
                "name": "fx", "version": 1,
                "passes": [{"material":"materials/util/effA.json"}]
            })" },
            { "materials/util/effA.json", kFlatMat },
        });
        auto j = nlohmann::json::parse(R"({
            "file":"effects/over/effect.json",
            "passes": [{}, {}]
        })");

        WPImageEffect e;
        CHECK_FALSE(e.FromJson(j, *vfs));
    }

    TEST_CASE("missing `name` in effect file is tolerated (warns + continues)") {
        // GET_JSON_NAME_VALUE warns when the key is absent but doesn't gate the
        // function's return value; the parser remains successful so partial data
        // can still feed the renderer.
        auto vfs = Vfs({
            { "effects/noname/effect.json", R"({
                "version": 1,
                "passes": [{"material":"materials/util/effA.json"}]
            })" },
            { "materials/util/effA.json", kFlatMat },
        });
        auto j = nlohmann::json::parse(R"({"file":"effects/noname/effect.json"})");

        WPImageEffect e;
        REQUIRE(e.FromJson(j, *vfs));
        CHECK(e.name.empty());
    }

    TEST_CASE("blacklisted effect ID flips visible to false") {
        // 2799421411 is the only id in BLACKLISTED_WORKSHOP_EFFECTS.
        auto vfs = Vfs({
            { "effects/2799421411/myFx/effect.json", R"({
                "name": "fx", "version": 1,
                "passes": [{"material":"materials/util/effA.json"}]
            })" },
            { "materials/util/effA.json", kFlatMat },
        });
        auto j = nlohmann::json::parse(R"({
            "file":"effects/2799421411/myFx/effect.json",
            "visible": true
        })");

        WPImageEffect e;
        REQUIRE(e.FromJson(j, *vfs));
        CHECK_FALSE(e.visible);
    }
}

// =============================================================================
// WPImageObject — shape-quad branch
// =============================================================================

TEST_SUITE("WPImageObject::FromJson — shape-quad") {
    TEST_CASE("basic shape:quad object with no image parses") {
        auto vfs = Vfs({});
        auto j   = nlohmann::json::parse(R"({
            "id": 99, "name": "quad",
            "shape": "quad",
            "origin": "10 20 0", "scale":"1 1 1", "angles":"0 0 0",
            "size":  "200 100"
        })");
        WPImageObject obj;
        REQUIRE(obj.FromJson(j, *vfs));
        CHECK(obj.id == 99);
        CHECK(obj.name == "quad");
        CHECK(obj.size[0] == doctest::Approx(200.0f));
        CHECK(obj.size[1] == doctest::Approx(100.0f));
        CHECK(obj.material.shader == "genericimage2");
    }

    TEST_CASE("unsupported shape type rejected") {
        auto vfs = Vfs({});
        auto j   = nlohmann::json::parse(R"({
            "shape": "circle",
            "origin": "0 0 0", "scale":"1 1 1", "angles":"0 0 0", "size":"10 10"
        })");
        WPImageObject obj;
        CHECK_FALSE(obj.FromJson(j, *vfs));
    }

    TEST_CASE("config.passthrough is captured on shape-quad") {
        auto vfs = Vfs({});
        auto j   = nlohmann::json::parse(R"({
            "shape": "quad",
            "origin": "0 0 0", "scale":"1 1 1", "angles":"0 0 0", "size":"100 100",
            "config": {"passthrough": true}
        })");
        WPImageObject obj;
        REQUIRE(obj.FromJson(j, *vfs));
        CHECK(obj.config.passthrough);
    }
}

// =============================================================================
// WPImageObject — image-backed branch (full optional surface)
// =============================================================================

TEST_SUITE("WPImageObject::FromJson — image-backed") {
    TEST_CASE("autosize:true (model) populates flag and leaves size at default") {
        auto vfs = Vfs({
            { "models/auto.json", R"({
                "material":"materials/util/m.json", "autosize": true
            })" },
            { "materials/util/m.json", kFlatMat },
        });
        auto j = nlohmann::json::parse(R"({
            "id": 1, "name":"a", "image":"models/auto.json",
            "origin":"0 0 0", "scale":"1 1 1", "angles":"0 0 0"
        })");
        WPImageObject obj;
        REQUIRE(obj.FromJson(j, *vfs));
        CHECK(obj.autosize);
    }

    TEST_CASE("model 'width'/'height' wins over scene 'size' and clears autosize") {
        auto vfs = Vfs({
            { "models/sized.json", R"({
                "material":"materials/util/m.json",
                "width": 512, "height": 256, "autosize": true
            })" },
            { "materials/util/m.json", kFlatMat },
        });
        auto j = nlohmann::json::parse(R"({
            "id": 1, "name":"x", "image":"models/sized.json",
            "origin":"0 0 0", "scale":"1 1 1", "angles":"0 0 0",
            "size":"100 100"
        })");
        WPImageObject obj;
        REQUIRE(obj.FromJson(j, *vfs));
        CHECK(obj.size[0] == doctest::Approx(512.0f));
        CHECK(obj.size[1] == doctest::Approx(256.0f));
        CHECK_FALSE(obj.autosize); // explicit dimensions win
    }

    TEST_CASE("scene 'size' wins when model has no width and no autosize") {
        auto vfs = Vfs({
            { "models/x.json", R"({"material":"materials/util/m.json"})" },
            { "materials/util/m.json", kFlatMat },
        });
        auto j = nlohmann::json::parse(R"({
            "id": 1, "name":"x", "image":"models/x.json",
            "origin":"0 0 0", "scale":"1 1 1", "angles":"0 0 0",
            "size":"333 444"
        })");
        WPImageObject obj;
        REQUIRE(obj.FromJson(j, *vfs));
        CHECK(obj.size[0] == doctest::Approx(333.0f));
        CHECK(obj.size[1] == doctest::Approx(444.0f));
        CHECK_FALSE(obj.autosize);
    }

    TEST_CASE("absent size + non-autosize falls back to origin*2") {
        auto vfs = Vfs({
            { "models/x.json", R"({"material":"materials/util/m.json"})" },
            { "materials/util/m.json", kFlatMat },
        });
        auto j = nlohmann::json::parse(R"({
            "id": 1, "name":"x", "image":"models/x.json",
            "origin":"5 7 0", "scale":"1 1 1", "angles":"0 0 0"
        })");
        WPImageObject obj;
        REQUIRE(obj.FromJson(j, *vfs));
        CHECK(obj.size[0] == doctest::Approx(10.0f));
        CHECK(obj.size[1] == doctest::Approx(14.0f));
    }

    TEST_CASE("fullscreen:true skips origin/size parsing") {
        auto vfs = Vfs({
            { "models/x.json",
              R"({"material":"materials/util/m.json", "fullscreen": true})" },
            { "materials/util/m.json", kFlatMat },
        });
        auto j = nlohmann::json::parse(R"({
            "id": 1, "name":"x", "image":"models/x.json"
        })");
        WPImageObject obj;
        REQUIRE(obj.FromJson(j, *vfs));
        CHECK(obj.fullscreen);
    }

    TEST_CASE("nopadding flag from model JSON") {
        auto vfs = Vfs({
            { "models/x.json",
              R"({"material":"materials/util/m.json", "nopadding": true})" },
            { "materials/util/m.json", kFlatMat },
        });
        auto j = nlohmann::json::parse(R"({
            "id": 1, "name":"x", "image":"models/x.json",
            "origin":"0 0 0", "scale":"1 1 1", "angles":"0 0 0"
        })");
        WPImageObject obj;
        REQUIRE(obj.FromJson(j, *vfs));
        CHECK(obj.nopadding);
    }

    TEST_CASE("parent_id, alignment, attachment, perspective, parallaxDepth") {
        auto vfs = Vfs({
            { "models/x.json", R"({"material":"materials/util/m.json"})" },
            { "materials/util/m.json", kFlatMat },
        });
        auto j = nlohmann::json::parse(R"({
            "id": 1, "name":"x", "image":"models/x.json",
            "origin":"0 0 0", "scale":"1 1 1", "angles":"0 0 0",
            "size":"10 10",
            "parent": 5,
            "alignment": "topleft",
            "attachment": "head",
            "perspective": true,
            "parallaxDepth": "0.5 1.0"
        })");
        WPImageObject obj;
        REQUIRE(obj.FromJson(j, *vfs));
        CHECK(obj.parent_id == 5);
        CHECK(obj.alignment == "topleft");
        CHECK(obj.attachment == "head");
        CHECK(obj.perspective);
        CHECK(obj.parallaxDepth[0] == doctest::Approx(0.5f));
        CHECK(obj.parallaxDepth[1] == doctest::Approx(1.0f));
    }

    TEST_CASE("color script object captures script + scriptproperties + initial value") {
        auto vfs = Vfs({
            { "models/x.json", R"({"material":"materials/util/m.json"})" },
            { "materials/util/m.json", kFlatMat },
        });
        auto j = nlohmann::json::parse(R"({
            "id": 1, "name":"x", "image":"models/x.json",
            "origin":"0 0 0", "scale":"1 1 1", "angles":"0 0 0", "size":"10 10",
            "color": {
                "script": "return [1,0,0];",
                "scriptproperties": { "k": 1 },
                "value": "0.5 0.6 0.7"
            }
        })");
        WPImageObject obj;
        REQUIRE(obj.FromJson(j, *vfs));
        CHECK(obj.colorScript == "return [1,0,0];");
        CHECK(obj.colorScriptProperties.find("\"k\":1") != std::string::npos);
        CHECK(obj.color[0] == doctest::Approx(0.5f));
        CHECK(obj.color[2] == doctest::Approx(0.7f));
    }

    TEST_CASE("alpha animation block populates propertyAnimations") {
        auto vfs = Vfs({
            { "models/x.json", R"({"material":"materials/util/m.json"})" },
            { "materials/util/m.json", kFlatMat },
        });
        auto j = nlohmann::json::parse(R"({
            "id": 1, "name":"x", "image":"models/x.json",
            "origin":"0 0 0", "scale":"1 1 1", "angles":"0 0 0", "size":"10 10",
            "alpha": {
                "value": 0.5,
                "animation": {
                    "c0": [
                        {"frame": 0,  "value": 0.0},
                        {"frame": 60, "value": 1.0}
                    ],
                    "options": { "name": "fade", "fps": 30, "length": 90, "mode": "loop" }
                }
            }
        })");
        WPImageObject obj;
        REQUIRE(obj.FromJson(j, *vfs));
        REQUIRE(obj.propertyAnimations.size() == 1u);
        const auto& a = obj.propertyAnimations[0];
        CHECK(a.property == "alpha");
        CHECK(a.name == "fade");
        CHECK(a.fps == doctest::Approx(30.0f));
        CHECK(a.length == doctest::Approx(90.0f));
        CHECK(a.initialValue == doctest::Approx(0.5f));
        REQUIRE(a.keyframes.size() == 2u);
        CHECK(a.keyframes[1].value == doctest::Approx(1.0f));
    }

    TEST_CASE("animation block without name uses property name as fallback") {
        auto vfs = Vfs({
            { "models/x.json", R"({"material":"materials/util/m.json"})" },
            { "materials/util/m.json", kFlatMat },
        });
        auto j = nlohmann::json::parse(R"({
            "id": 1, "name":"x", "image":"models/x.json",
            "origin":"0 0 0", "scale":"1 1 1", "angles":"0 0 0", "size":"10 10",
            "alpha": {
                "value": 1.0,
                "animation": {
                    "c0": [{"frame":0,"value":0}]
                }
            }
        })");
        WPImageObject obj;
        REQUIRE(obj.FromJson(j, *vfs));
        REQUIRE(obj.propertyAnimations.size() == 1u);
        CHECK(obj.propertyAnimations[0].name == "alpha");
    }

    TEST_CASE("missing material in model JSON is rejected") {
        auto vfs = Vfs({
            { "models/x.json", R"({"width":100,"height":100})" }, // no material
        });
        auto j = nlohmann::json::parse(R"({
            "id": 1, "name":"x", "image":"models/x.json",
            "origin":"0 0 0", "scale":"1 1 1", "angles":"0 0 0"
        })");
        WPImageObject obj;
        CHECK_FALSE(obj.FromJson(j, *vfs));
    }

    TEST_CASE("missing image file in VFS is rejected") {
        auto vfs = Vfs({});
        auto j   = nlohmann::json::parse(R"({
            "id": 1, "name":"x", "image":"models/missing.json",
            "origin":"0 0 0", "scale":"1 1 1", "angles":"0 0 0"
        })");
        WPImageObject obj;
        CHECK_FALSE(obj.FromJson(j, *vfs));
    }

    TEST_CASE("animationlayers populate puppet_layers") {
        auto vfs = Vfs({
            { "models/x.json", R"({"material":"materials/util/m.json", "puppet": "x.puppet"})" },
            { "materials/util/m.json", kFlatMat },
        });
        auto j = nlohmann::json::parse(R"({
            "id": 1, "name":"x", "image":"models/x.json",
            "origin":"0 0 0", "scale":"1 1 1", "angles":"0 0 0", "size":"10 10",
            "animationlayers": [
                {"animation": 1, "blend": 1.0, "rate": 1.0},
                {"animation": 2, "blend": 0.5, "rate": 2.0, "visible": false}
            ]
        })");
        WPImageObject obj;
        REQUIRE(obj.FromJson(j, *vfs));
        REQUIRE(obj.puppet_layers.size() == 2u);
        CHECK(obj.puppet_layers[0].id == 1);
        CHECK(obj.puppet_layers[1].rate == doctest::Approx(2.0f));
        CHECK(obj.puppet_layers[1].visible == false);
    }

    TEST_CASE("config.passthrough on image-backed object") {
        auto vfs = Vfs({
            { "models/x.json", R"({"material":"materials/util/m.json"})" },
            { "materials/util/m.json", kFlatMat },
        });
        auto j = nlohmann::json::parse(R"({
            "id": 1, "name":"x", "image":"models/x.json",
            "origin":"0 0 0", "scale":"1 1 1", "angles":"0 0 0", "size":"10 10",
            "config": {"passthrough": true}
        })");
        WPImageObject obj;
        REQUIRE(obj.FromJson(j, *vfs));
        CHECK(obj.config.passthrough);
    }

    TEST_CASE("visibleIsComboSelector flips when visible is a combo-condition object") {
        auto vfs = Vfs({
            { "models/x.json", R"({"material":"materials/util/m.json"})" },
            { "materials/util/m.json", kFlatMat },
        });
        auto j = nlohmann::json::parse(R"({
            "id": 1, "name":"x", "image":"models/x.json",
            "origin":"0 0 0", "scale":"1 1 1", "angles":"0 0 0", "size":"10 10",
            "visible": {"user": {"condition": "1==1", "name": "char_a"}}
        })");
        WPImageObject obj;
        REQUIRE(obj.FromJson(j, *vfs));
        CHECK(obj.visibleIsComboSelector);
    }
}
