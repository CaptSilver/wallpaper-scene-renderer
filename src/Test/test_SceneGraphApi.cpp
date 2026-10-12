#include <doctest.h>

#include "SceneGraphApi.hpp"

using scenebackend::CanShareFramesWith;

TEST_SUITE("SceneGraphApi") {
    TEST_CASE("frames reach Qt Quick only through an OpenGL scene graph") {
        // The renderer hands each frame over as an OpenGL texture imported
        // from Vulkan memory; any other Qt Quick backend has nothing to wrap it.
        CHECK(CanShareFramesWith(QSGRendererInterface::OpenGL));
        CHECK_FALSE(CanShareFramesWith(QSGRendererInterface::Vulkan));
        CHECK_FALSE(CanShareFramesWith(QSGRendererInterface::Software));
        CHECK_FALSE(CanShareFramesWith(QSGRendererInterface::Direct3D11));
        CHECK_FALSE(CanShareFramesWith(QSGRendererInterface::Unknown));
    }
}
