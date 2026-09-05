#include <doctest.h>

#include "Scene/TextUploadCommit.hpp"
#include <string>

using namespace wallpaper;

namespace
{
// Mirrors the fields of TextLayerInfo the render-thread drain touches, so the
// commit gate is testable without a scene or a Vulkan device.
struct FakeTextLayer {
    std::string currentText;
    bool        pointsizeDirty { false };
    bool        textStyleDirty { false };
};
} // namespace

// The render thread re-rasterizes a text layer and pushes the bitmap to the
// GPU.  Whether that push landed decides whether the layer's change is done or
// has to be tried again — clearing the triggers on a refused upload strands the
// layer on its old texture until some unrelated text mutation comes along.
TEST_SUITE("TextUploadCommit") {
    TEST_CASE("a landed upload commits the new text and clears both triggers") {
        FakeTextLayer     layer { "old", true, true };
        const std::string fresh = "new";
        CHECK(commitTextUpload(layer, &fresh, true));
        CHECK(layer.currentText == "new");
        CHECK_FALSE(layer.pointsizeDirty);
        CHECK_FALSE(layer.textStyleDirty);
    }
    TEST_CASE("a refused upload leaves the text and both triggers untouched") {
        FakeTextLayer     layer { "old", true, true };
        const std::string fresh = "new";
        CHECK_FALSE(commitTextUpload(layer, &fresh, false));
        CHECK(layer.currentText == "old"); // still what the GPU is showing
        CHECK(layer.pointsizeDirty);
        CHECK(layer.textStyleDirty);
    }
    TEST_CASE("a re-render of the existing text clears the triggers without touching it") {
        FakeTextLayer layer { "keep", true, true };
        CHECK(commitTextUpload(layer, nullptr, true));
        CHECK(layer.currentText == "keep");
        CHECK_FALSE(layer.pointsizeDirty);
        CHECK_FALSE(layer.textStyleDirty);
    }
    TEST_CASE("a refused re-render keeps the triggers so the next frame retries") {
        FakeTextLayer layer { "keep", true, true };
        CHECK_FALSE(commitTextUpload(layer, nullptr, false));
        CHECK(layer.pointsizeDirty);
        CHECK(layer.textStyleDirty);
    }
}
