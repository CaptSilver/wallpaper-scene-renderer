#pragma once

// Seam between the SceneScript dispatch in SceneBackend.cpp and the real
// SceneWallpaper.  The 30 Hz property/text/color eval loops route every
// script-driven mutation through this interface instead of calling
// SceneWallpaper directly, so a test can drive that dispatch against a
// recording fake with no SceneWallpaper / Vulkan device.
//
// The signatures are copied verbatim from SceneWallpaper.hpp so no implicit
// conversion differs between the real forward and the interface — the
// production sink below is a 1:1 forwarder and won't compile if a
// SceneWallpaper signature drifts, which is the point (compile-time drift
// guard, since this TU is built into wescene-renderer-qml).
//
// applyLayerBatch takes SceneWallpaper::LayerBatchUpdate, so this header
// includes SceneWallpaper.hpp.  That's fine: the interface lives in
// qml_helper/, already coupled to SceneWallpaper.

#include <string>
#include <vector>

#include "SceneWallpaper.hpp"

namespace scenebackend
{

struct IPropertyDispatchSink {
    virtual ~IPropertyDispatchSink() = default;

    virtual void updateText(int32_t id, const std::string& text)    = 0;
    virtual void updateTextPointsize(int32_t id, float pointsize)   = 0;
    virtual void updateColor(int32_t id, float r, float g, float b) = 0;
    virtual void updateNodeTransform(int32_t id, const std::string& property, float x, float y,
                                     float z)                       = 0;
    virtual void updateNodeVisible(int32_t id, bool visible)        = 0;
    virtual void updateNodeAlpha(int32_t id, float alpha)           = 0;
    virtual void updateParticleRate(int32_t id, float rate)         = 0;
    virtual void
    applyLayerBatch(const std::vector<wallpaper::SceneWallpaper::LayerBatchUpdate>& batch) = 0;
    virtual void updateEffectVisible(int32_t nodeId, int32_t effectIndex, bool visible)    = 0;
    virtual void updateMaterialValue(int32_t nodeId, std::string name,
                                     std::vector<float> floats)                            = 0;
    virtual void updateEffectMaterialValue(int32_t nodeId, int32_t effectIdx, std::string name,
                                           std::vector<float> floats)                      = 0;
    virtual void setLayerSpriteFrame(int32_t nodeId, bool wantsManual, int32_t frameIdx)   = 0;
    virtual void updateTextStyle(int32_t nodeId, std::string halign, std::string valign,
                                 std::string fontName)                                     = 0;
    virtual void updateSoundVolume(int32_t index, float volume)                            = 0;
    virtual void updateClearColor(float r, float g, float b)                               = 0;
    virtual void updateBloomStrength(float strength)                                       = 0;
    virtual void updateBloomThreshold(float threshold)                                     = 0;
    virtual void updateCameraFov(float fov)                                                = 0;
    virtual void updateCameraLookAt(float ex, float ey, float ez, float cx, float cy, float cz,
                                    float ux, float uy, float uz)                          = 0;
    virtual void updateAmbientColor(float r, float g, float b)                             = 0;
    virtual void updateSkylightColor(float r, float g, float b)                            = 0;
    virtual void updateLightColor(int32_t index, float r, float g, float b)                = 0;
    virtual void updateLightRadius(int32_t index, float radius)                            = 0;
    virtual void updateLightIntensity(int32_t index, float intensity)                      = 0;
    virtual void updateLightPosition(int32_t index, float x, float y, float z)             = 0;
};

// Production sink: a trivial 1:1 forward to the owned SceneWallpaper.  Does not
// own the pointer (SceneObject owns the shared_ptr).  Each method is inline so
// there's no per-call indirection cost beyond the virtual dispatch the seam
// already introduces.
struct SceneWallpaperDispatchSink final : IPropertyDispatchSink {
    explicit SceneWallpaperDispatchSink(wallpaper::SceneWallpaper* scene): m_scene(scene) {}

    void updateText(int32_t id, const std::string& text) override { m_scene->updateText(id, text); }
    void updateTextPointsize(int32_t id, float pointsize) override {
        m_scene->updateTextPointsize(id, pointsize);
    }
    void updateColor(int32_t id, float r, float g, float b) override {
        m_scene->updateColor(id, r, g, b);
    }
    void updateNodeTransform(int32_t id, const std::string& property, float x, float y,
                             float z) override {
        m_scene->updateNodeTransform(id, property, x, y, z);
    }
    void updateNodeVisible(int32_t id, bool visible) override {
        m_scene->updateNodeVisible(id, visible);
    }
    void updateNodeAlpha(int32_t id, float alpha) override { m_scene->updateNodeAlpha(id, alpha); }
    void updateParticleRate(int32_t id, float rate) override {
        m_scene->updateParticleRate(id, rate);
    }
    void applyLayerBatch(
        const std::vector<wallpaper::SceneWallpaper::LayerBatchUpdate>& batch) override {
        m_scene->applyLayerBatch(batch);
    }
    void updateEffectVisible(int32_t nodeId, int32_t effectIndex, bool visible) override {
        m_scene->updateEffectVisible(nodeId, effectIndex, visible);
    }
    void updateMaterialValue(int32_t nodeId, std::string name, std::vector<float> floats) override {
        m_scene->updateMaterialValue(nodeId, std::move(name), std::move(floats));
    }
    void updateEffectMaterialValue(int32_t nodeId, int32_t effectIdx, std::string name,
                                   std::vector<float> floats) override {
        m_scene->updateEffectMaterialValue(nodeId, effectIdx, std::move(name), std::move(floats));
    }
    void setLayerSpriteFrame(int32_t nodeId, bool wantsManual, int32_t frameIdx) override {
        m_scene->setLayerSpriteFrame(nodeId, wantsManual, frameIdx);
    }
    void updateTextStyle(int32_t nodeId, std::string halign, std::string valign,
                         std::string fontName) override {
        m_scene->updateTextStyle(nodeId, std::move(halign), std::move(valign), std::move(fontName));
    }
    void updateSoundVolume(int32_t index, float volume) override {
        m_scene->updateSoundVolume(index, volume);
    }
    void updateClearColor(float r, float g, float b) override {
        m_scene->updateClearColor(r, g, b);
    }
    void updateBloomStrength(float strength) override { m_scene->updateBloomStrength(strength); }
    void updateBloomThreshold(float threshold) override {
        m_scene->updateBloomThreshold(threshold);
    }
    void updateCameraFov(float fov) override { m_scene->updateCameraFov(fov); }
    void updateCameraLookAt(float ex, float ey, float ez, float cx, float cy, float cz, float ux,
                            float uy, float uz) override {
        m_scene->updateCameraLookAt(ex, ey, ez, cx, cy, cz, ux, uy, uz);
    }
    void updateAmbientColor(float r, float g, float b) override {
        m_scene->updateAmbientColor(r, g, b);
    }
    void updateSkylightColor(float r, float g, float b) override {
        m_scene->updateSkylightColor(r, g, b);
    }
    void updateLightColor(int32_t index, float r, float g, float b) override {
        m_scene->updateLightColor(index, r, g, b);
    }
    void updateLightRadius(int32_t index, float radius) override {
        m_scene->updateLightRadius(index, radius);
    }
    void updateLightIntensity(int32_t index, float intensity) override {
        m_scene->updateLightIntensity(index, intensity);
    }
    void updateLightPosition(int32_t index, float x, float y, float z) override {
        m_scene->updateLightPosition(index, x, y, z);
    }

private:
    wallpaper::SceneWallpaper* m_scene;
};

} // namespace scenebackend
