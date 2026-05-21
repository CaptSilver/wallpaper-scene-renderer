#include <doctest.h>

#include "Scene/TextStyleMerge.hpp"
#include <string>

using namespace wallpaper;

// The render-thread text-style bridge: SceneScript posts halign/valign/font
// independently (thisLayer.horizontalalign = 'right'; thisLayer.font = 'X'),
// merged per-id with "empty = leave unchanged" (mergeTextStyle), then applied to
// the live text layer with a value-changed gate (applyTextStyle).  Lifted out of
// RenderHandler so the merge logic is testable without Vulkan.
TEST_SUITE("TextStyleMerge") {
    TEST_CASE("merge: empty field leaves prior value; non-empty fields accumulate") {
        PendingTextStyleUpdate cur;
        mergeTextStyle(cur, "right", "", "");     // only halign
        mergeTextStyle(cur, "", "", "Heavy.otf"); // only fontName
        CHECK(cur.halign == "right");
        CHECK(cur.valign == "");
        CHECK(cur.fontName == "Heavy.otf");
    }
    TEST_CASE("merge: a later non-empty value overwrites an earlier one") {
        PendingTextStyleUpdate cur;
        mergeTextStyle(cur, "left", "", "");
        mergeTextStyle(cur, "center", "", ""); // halign overwritten
        CHECK(cur.halign == "center");
    }
    TEST_CASE("merge: an all-empty post does NOT clobber earlier non-empty fields") {
        PendingTextStyleUpdate cur;
        mergeTextStyle(cur, "right", "top", "Bold.otf");
        mergeTextStyle(cur, "", "", ""); // no-op
        CHECK(cur.halign == "right");
        CHECK(cur.valign == "top");
        CHECK(cur.fontName == "Bold.otf");
    }

    TEST_CASE("apply: a style equal to current is a no-op (no spurious dirty)") {
        TextStyleTarget        tl { "right", "top", "Bold.otf", "<bytes>" };
        PendingTextStyleUpdate same { "right", "top", "" }; // same halign/valign, no font
        bool                   changed = applyTextStyle(same, tl, [](const std::string&) {
            return std::string();
        });
        CHECK_FALSE(changed);
        CHECK(tl.fontData == "<bytes>"); // font untouched
    }
    TEST_CASE("apply: a changed halign flips the value + reports change") {
        TextStyleTarget        tl { "left", "", "", "" };
        PendingTextStyleUpdate upd { "right", "", "" };
        bool                   changed = applyTextStyle(upd, tl, [](const std::string&) {
            return std::string();
        });
        CHECK(changed);
        CHECK(tl.halign == "right");
    }
    TEST_CASE("apply: font change takes effect only when resolveFont yields bytes") {
        TextStyleTarget        tl { "", "", "Old.otf", "<old>" };
        PendingTextStyleUpdate upd { "", "", "New.otf" };
        // resolveFont returns empty -> font NOT changed (the missing-font path).
        bool c1 = applyTextStyle(upd, tl, [](const std::string&) {
            return std::string();
        });
        CHECK_FALSE(c1);
        CHECK(tl.fontName == "Old.otf");
        CHECK(tl.fontData == "<old>");
        // resolveFont returns bytes -> font changed.
        bool c2 = applyTextStyle(upd, tl, [](const std::string& n) {
            return "BYTES:" + n;
        });
        CHECK(c2);
        CHECK(tl.fontName == "New.otf");
        CHECK(tl.fontData == "BYTES:New.otf");
    }
}
