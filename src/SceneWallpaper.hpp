#pragma once
#include <memory>
#include <string_view>
#include <string>
#include <vector>
#include <functional>
#include "Type.hpp"
#include "Swapchain/ExSwapchain.hpp"

namespace wallpaper::audio { class AudioAnalyzer; }

namespace wallpaper
{

using FirstFrameCallback = std::function<void()>;

struct TextScriptInfo {
    int32_t     id;
    std::string script;
    std::string initialValue;
};

struct ColorScriptInfo {
    int32_t              id;
    std::string          script;
    std::string          scriptProperties;
    std::array<float, 3> initialColor;
};

struct PropertyScriptInfo {
    int32_t     id;
    std::string property;          // "visible", "origin", "scale", "angles", "alpha"
    std::string script;
    std::string scriptProperties;  // JSON string
    std::string layerName;         // name of the object for thisLayer.name
    bool                 initialVisible {true};
    std::array<float, 3> initialVec3 {0, 0, 0};
    float                initialFloat {1.0f};
};

constexpr std::string_view PROPERTY_SOURCE               = "source";
constexpr std::string_view PROPERTY_ASSETS               = "assets";
constexpr std::string_view PROPERTY_FPS                  = "fps";
constexpr std::string_view PROPERTY_FILLMODE             = "fillmode";
constexpr std::string_view PROPERTY_SPEED                = "speed";
constexpr std::string_view PROPERTY_GRAPHIVZ             = "graphivz";
constexpr std::string_view PROPERTY_VOLUME               = "volume";
constexpr std::string_view PROPERTY_MUTED                = "muted";
constexpr std::string_view PROPERTY_CACHE_PATH           = "cache_path";
constexpr std::string_view PROPERTY_FIRST_FRAME_CALLBACK = "first_frame_callback";
constexpr std::string_view PROPERTY_USER_PROPS           = "user_props";
constexpr std::string_view PROPERTY_HDR_OUTPUT           = "hdr_output";
constexpr std::string_view PROPERTY_HDR_CONTENT          = "hdr_content";
constexpr std::string_view PROPERTY_SYSTEM_AUDIO_CAPTURE = "system_audio_capture";

#include "Core/NoCopyMove.hpp"
class MainHandler;
struct RenderInitInfo;

class SceneWallpaper : NoCopy {
public:
    SceneWallpaper();
    ~SceneWallpaper();
    bool init();
    bool inited() const;

    void initVulkan(const RenderInitInfo&);

    void play();
    void pause();
    void mouseInput(double x, double y);
    void updateText(int32_t id, const std::string& text);
    void updateColor(int32_t id, float r, float g, float b);
    std::vector<TextScriptInfo> getTextScripts() const;
    std::vector<ColorScriptInfo> getColorScripts() const;
    std::vector<PropertyScriptInfo> getPropertyScripts() const;
    std::unordered_map<std::string, int32_t> getNodeNameToIdMap() const;
    std::string getLayerInitialStatesJson() const;
    void updateNodeTransform(int32_t id, const std::string& property, float x, float y, float z);
    void updateNodeVisible(int32_t id, bool visible);
    void updateNodeAlpha(int32_t id, float alpha);

    void setPropertyBool(std::string_view, bool);
    void setPropertyInt32(std::string_view, int32_t);
    void setPropertyFloat(std::string_view, float);
    void setPropertyString(std::string_view, std::string);
    void setPropertyObject(std::string_view, std::shared_ptr<void>);

    ExSwapchain* exSwapchain() const;
    std::shared_ptr<audio::AudioAnalyzer> audioAnalyzer() const;

private:
    bool m_inited { false };

private:
    friend class MainHandler;

    bool                         m_offscreen { false };
    std::shared_ptr<MainHandler> m_main_handler;
};
} // namespace wallpaper
