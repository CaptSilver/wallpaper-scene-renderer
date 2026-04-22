#include <doctest.h>
#include "WPUserProperties.hpp"

#include <set>
#include <string>

using namespace wallpaper;

// ===========================================================================
// LoadFromProjectJson
// ===========================================================================

TEST_SUITE("WPUserProperties::LoadFromProjectJson") {
    TEST_CASE("valid project.json loads properties") {
        WPUserProperties props;
        bool             ok = props.LoadFromProjectJson(R"({
        "general": {
            "properties": {
                "schemecolor": { "type": "color", "value": "1 0.3 0.3" },
                "speed":       { "type": "slider", "value": 5 }
            }
        }
    })");
        CHECK(ok);
        CHECK_FALSE(props.Empty());
        CHECK(props.HasProperty("schemecolor"));
        CHECK(props.HasProperty("speed"));
    }

    TEST_CASE("HasProperty returns false for missing key") {
        WPUserProperties props;
        props.LoadFromProjectJson(R"({
        "general": { "properties": { "speed": { "type": "slider", "value": 5 } } }
    })");
        CHECK(props.HasProperty("speed") == true);
        CHECK(props.HasProperty("nonexistent") == false);
    }

    TEST_CASE("empty properties section") {
        WPUserProperties props;
        bool             ok = props.LoadFromProjectJson(R"({
        "general": { "properties": {} }
    })");
        CHECK(ok);
        CHECK(props.Empty());
    }

    TEST_CASE("missing properties key returns false") {
        WPUserProperties props;
        bool             ok = props.LoadFromProjectJson(R"({ "general": {} })");
        CHECK_FALSE(ok);
    }

    TEST_CASE("invalid JSON returns false") {
        WPUserProperties props;
        bool             ok = props.LoadFromProjectJson("not json");
        CHECK_FALSE(ok);
    }

    TEST_CASE("missing general key returns false") {
        WPUserProperties props;
        bool             ok = props.LoadFromProjectJson(R"({ "other": {} })");
        CHECK_FALSE(ok);
    }

} // TEST_SUITE

// ===========================================================================
// HasProperty / GetProperty / GetDefault / GetType
// ===========================================================================

TEST_SUITE("WPUserProperties::Accessors") {
    TEST_CASE("GetProperty returns value") {
        WPUserProperties props;
        props.LoadFromProjectJson(R"({
        "general": { "properties": { "speed": { "type": "slider", "value": 5 } } }
    })");
        auto val = props.GetProperty("speed");
        REQUIRE(val.has_value());
        CHECK(*val == 5);
    }

    TEST_CASE("GetProperty nonexistent returns nullopt") {
        WPUserProperties props;
        CHECK_FALSE(props.GetProperty("nope").has_value());
    }

    TEST_CASE("GetDefault returns original value") {
        WPUserProperties props;
        props.LoadFromProjectJson(R"({
        "general": { "properties": { "speed": { "type": "slider", "value": 5 } } }
    })");
        props.SetProperty("speed", 10);
        // GetProperty returns overridden
        CHECK(*props.GetProperty("speed") == 10);
        // GetDefault returns original
        CHECK(*props.GetDefault("speed") == 5);
    }

    TEST_CASE("GetDefault nonexistent returns nullopt") {
        WPUserProperties props;
        CHECK_FALSE(props.GetDefault("nope").has_value());
    }

    TEST_CASE("GetType returns type string") {
        WPUserProperties props;
        props.LoadFromProjectJson(R"({
        "general": { "properties": { "enabled": { "type": "bool", "value": true } } }
    })");
        auto t = props.GetType("enabled");
        REQUIRE(t.has_value());
        CHECK(*t == "bool");
    }

    TEST_CASE("GetType nonexistent returns nullopt") {
        WPUserProperties props;
        CHECK_FALSE(props.GetType("nope").has_value());
    }

} // TEST_SUITE

// ===========================================================================
// SetProperty
// ===========================================================================

TEST_SUITE("WPUserProperties::SetProperty") {
    TEST_CASE("override existing property") {
        WPUserProperties props;
        props.LoadFromProjectJson(R"({
        "general": { "properties": { "speed": { "type": "slider", "value": 5 } } }
    })");
        props.SetProperty("speed", 99);
        CHECK(*props.GetProperty("speed") == 99);
    }

    TEST_CASE("add new property") {
        WPUserProperties props;
        props.SetProperty("newprop", "hello");
        CHECK(props.HasProperty("newprop"));
        CHECK(*props.GetProperty("newprop") == "hello");
    }

    TEST_CASE("default not changed by SetProperty") {
        WPUserProperties props;
        props.LoadFromProjectJson(R"({
        "general": { "properties": { "x": { "type": "slider", "value": 1 } } }
    })");
        props.SetProperty("x", 42);
        CHECK(*props.GetDefault("x") == 1);
    }

} // TEST_SUITE

// ===========================================================================
// ApplyOverrides
// ===========================================================================

TEST_SUITE("WPUserProperties::ApplyOverrides") {
    TEST_CASE("valid overrides applied") {
        WPUserProperties props;
        props.LoadFromProjectJson(R"({
        "general": { "properties": { "a": { "value": 1 }, "b": { "value": 2 } } }
    })");
        bool ok = props.ApplyOverrides(R"({"a": 10, "b": 20})");
        CHECK(ok);
        CHECK(*props.GetProperty("a") == 10);
        CHECK(*props.GetProperty("b") == 20);
    }

    TEST_CASE("empty string returns true (no-op)") {
        WPUserProperties props;
        CHECK(props.ApplyOverrides(""));
    }

    TEST_CASE("invalid JSON returns false") {
        WPUserProperties props;
        CHECK_FALSE(props.ApplyOverrides("not json"));
    }

    TEST_CASE("non-object JSON returns false") {
        WPUserProperties props;
        CHECK_FALSE(props.ApplyOverrides("[1, 2, 3]"));
    }

} // TEST_SUITE

// ===========================================================================
// ResolveValue
// ===========================================================================

TEST_SUITE("WPUserProperties::ResolveValue") {
    TEST_CASE("non-object pass-through") {
        WPUserProperties props;
        auto             result = props.ResolveValue(42);
        CHECK(result == 42);
    }

    TEST_CASE("object without user key pass-through") {
        WPUserProperties props;
        auto             j = nlohmann::json { { "color", "red" } };
        CHECK(props.ResolveValue(j) == j);
    }

    TEST_CASE("simple user reference") {
        WPUserProperties props;
        props.LoadFromProjectJson(R"({
        "general": { "properties": { "speed": { "value": 5 } } }
    })");
        auto ref = nlohmann::json { { "user", "speed" }, { "value", 1 } };
        CHECK(props.ResolveValue(ref) == 5);
    }

    TEST_CASE("simple user reference — missing property uses default") {
        WPUserProperties props;
        auto             ref = nlohmann::json { { "user", "missing" }, { "value", 42 } };
        CHECK(props.ResolveValue(ref) == 42);
    }

    TEST_CASE("conditional match returns true") {
        WPUserProperties props;
        props.LoadFromProjectJson(R"({
        "general": { "properties": { "mode": { "type": "combo", "value": "3" } } }
    })");
        auto ref = nlohmann::json { { "user", { { "condition", "3" }, { "name", "mode" } } },
                                    { "value", true } };
        CHECK(props.ResolveValue(ref) == true);
    }

    TEST_CASE("conditional non-match returns false") {
        WPUserProperties props;
        props.LoadFromProjectJson(R"({
        "general": { "properties": { "mode": { "type": "combo", "value": "3" } } }
    })");
        auto ref = nlohmann::json { { "user", { { "condition", "5" }, { "name", "mode" } } },
                                    { "value", true } };
        CHECK(props.ResolveValue(ref) == false);
    }

    // Regression: WPSceneParser computes userPropVisBindings.defaultVisible
    // from ResolveValue(visibleJson).  For a bool prop bound to a bool
    // visibility, the resolver must return the CURRENT prop value (post
    // override), not the scene.json `value` literal.  Previously the parser
    // read the literal directly and visibility-bound props ignored
    // --set / persisted overrides at load time (Purple Void / blackhole
    // wallpaper: --set clock=true left clock layer hidden).
    TEST_CASE("bool prop bound to bool visibility — returns current value not literal") {
        WPUserProperties props;
        props.LoadFromProjectJson(R"({
        "general": { "properties": { "clock": { "type": "bool", "value": false } } }
    })");
        // scene.json shape: {"user":"clock","value":false}
        auto ref = nlohmann::json { { "user", "clock" }, { "value", false } };
        CHECK(props.ResolveValue(ref) == false); // matches default

        props.SetProperty("clock", true);
        CHECK(props.ResolveValue(ref) == true); // override wins over literal
    }

    TEST_CASE("bool default flip — current equals default → visDefault") {
        WPUserProperties props;
        props.LoadFromProjectJson(R"({
        "general": { "properties": { "style": { "type": "combo", "value": "6" } } }
    })");
        // style is still at default "6", binding expects bool visibility
        auto ref = nlohmann::json { { "user", "style" }, { "value", true } };
        CHECK(props.ResolveValue(ref) == true);
    }

    TEST_CASE("bool default flip — current differs from default → !visDefault") {
        WPUserProperties props;
        props.LoadFromProjectJson(R"({
        "general": { "properties": { "style": { "type": "combo", "value": "6" } } }
    })");
        props.SetProperty("style", "3");
        auto ref = nlohmann::json { { "user", "style" }, { "value", true } };
        CHECK(props.ResolveValue(ref) == false);
    }

} // TEST_SUITE

// ===========================================================================
// InsertAllNames / Empty
// ===========================================================================

TEST_SUITE("WPUserProperties::Misc") {
    TEST_CASE("InsertAllNames") {
        WPUserProperties props;
        props.LoadFromProjectJson(R"({
        "general": { "properties": {
            "a": { "value": 1 },
            "b": { "value": 2 },
            "c": { "value": 3 }
        }}
    })");
        std::set<std::string> names;
        props.InsertAllNames(names);
        CHECK(names.size() == 3);
        CHECK(names.count("a") == 1);
        CHECK(names.count("b") == 1);
        CHECK(names.count("c") == 1);
    }

    TEST_CASE("Empty on default-constructed") {
        WPUserProperties props;
        CHECK(props.Empty());
    }

    TEST_CASE("Empty after loading") {
        WPUserProperties props;
        props.LoadFromProjectJson(R"({
        "general": { "properties": { "x": { "value": 1 } } }
    })");
        CHECK_FALSE(props.Empty());
    }

    TEST_CASE("UserPropertiesScope RAII") {
        WPUserProperties props;
        CHECK(g_currentUserProperties == nullptr);
        {
            UserPropertiesScope scope(&props);
            CHECK(g_currentUserProperties == &props);
        }
        CHECK(g_currentUserProperties == nullptr);
    }

} // TEST_SUITE
