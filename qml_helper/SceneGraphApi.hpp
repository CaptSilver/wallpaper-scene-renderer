#pragma once
#include <QSGRendererInterface>

namespace scenebackend
{

// The scene renderer hands each frame to Qt Quick as an OpenGL texture
// imported from Vulkan memory. Under any other Qt Quick backend (Vulkan,
// Software, Direct3D -- QSG_RHI_BACKEND picks one) there is no GL texture to
// wrap: QSGOpenGLTexture::fromNative returns null, and a texture node built
// around that null was handed to the renderer.
inline bool CanShareFramesWith(QSGRendererInterface::GraphicsApi api) {
    return api == QSGRendererInterface::OpenGL;
}

} // namespace scenebackend
