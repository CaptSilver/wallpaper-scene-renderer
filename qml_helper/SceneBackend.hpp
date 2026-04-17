#pragma once

#include <QtQuick/QQuickFramebufferObject>
#include <QtCore/QLoggingCategory>
#include <QtCore/QTimer>
#include <QtCore/QElapsedTimer>
#include <QtGui/QMouseEvent>
#include <QtGui/QHoverEvent>
#include <QtQml/QJSEngine>
#include <QtQml/QJSValue>
#include <unordered_set>
#include <vector>

#include "SceneWallpaper.hpp"

Q_DECLARE_LOGGING_CATEGORY(wekdeScene)

namespace scenebackend
{

class SceneTimerBridge;

class SceneObject : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(QUrl assets READ assets WRITE setAssets)
    Q_PROPERTY(int fps READ fps WRITE setFps NOTIFY fpsChanged)
    Q_PROPERTY(int fillMode READ fillMode WRITE setFillMode NOTIFY fillModeChanged)
    Q_PROPERTY(float speed READ speed WRITE setSpeed NOTIFY speedChanged)
    Q_PROPERTY(float volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted)
    Q_PROPERTY(QString userProperties READ userProperties WRITE setUserProperties NOTIFY userPropertiesChanged)
    Q_PROPERTY(bool hdrOutput READ hdrOutput WRITE setHdrOutput)
    Q_PROPERTY(bool systemAudioCapture READ systemAudioCapture WRITE setSystemAudioCapture)
public:
    constexpr static std::string_view CACHE_DIR { "wescene-renderer" };
    static std::string                GetDefaultCachePath();

    enum FillMode
    {
        STRETCH,
        ASPECTFIT,
        ASPECTCROP
    };
    Q_ENUM(FillMode)

    QUrl source() const;
    QUrl assets() const;
    void setSource(const QUrl& source);
    void setAssets(const QUrl& assets);

    int   fps() const;
    int   fillMode() const;
    float speed() const;
    float volume() const;
    bool  muted() const;
    bool  hdrOutput() const;
    bool  systemAudioCapture() const;
    QString userProperties() const;

    void setFps(int);
    void setFillMode(int);
    void setSpeed(float);
    void setVolume(float);
    void setMuted(bool);
    void setUserProperties(const QString&);
    void setHdrOutput(bool);
    void setSystemAudioCapture(bool);

    // debug
    bool vulkanValid() const;
    void enableVulkanValid();
    void enableGenGraphviz();

    Q_INVOKABLE void setAcceptMouse(bool);
    Q_INVOKABLE void setAcceptHover(bool);

    // Media integration events (called from QML MprisMonitor, dispatched to JS)
    Q_INVOKABLE void mediaPlaybackChanged(int state);
    Q_INVOKABLE void mediaPropertiesChanged(const QString& title, const QString& artist,
                                            const QString& albumTitle, const QString& albumArtist,
                                            const QString& genres);
    Q_INVOKABLE void mediaThumbnailChanged(bool hasThumbnail, const QVariantList& colors);
    Q_INVOKABLE void mediaTimelineChanged(double position, double duration);
    Q_INVOKABLE void mediaStatusChanged(bool enabled);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;

public slots:
    void resizeFb();
    void play();
    void pause();

signals:
    void sourceChanged();
    void fpsChanged();
    void fillModeChanged();
    void speedChanged();
    void volumeChanged();
    void userPropertiesChanged();
    void firstFrame();

private:
    QUrl m_source;
    QUrl m_assets;

    int   m_fps { 15 };
    int   m_fillMode { FillMode::ASPECTCROP };
    float m_speed { 1.0f };
    float m_volume { 1.0f };
    bool  m_muted { false };
    bool  m_hdrOutput { false };
    bool  m_systemAudioCapture { false };
    QString m_userProperties;

public:
    static void on_update(void* ctx);

    explicit SceneObject(QQuickItem* parent = nullptr);
    virtual ~SceneObject();

private:
    void setScenePropertyQurl(std::string_view, QUrl);
    void setupTextScripts();
    void refreshJsUserProperties();
    void fireApplyUserProperties();
    void fireDestroyEvent();
    void fireResizeScreen(int width, int height);
    void evaluateTextScripts();
    void evaluateColorScripts();
    void evaluatePropertyScripts();
    void cleanupTextScripts();

    bool m_inited { false };
    bool m_enable_valid { false };

    std::shared_ptr<wallpaper::SceneWallpaper> m_scene { nullptr };

    // Text script evaluation
    struct TextScriptState {
        int32_t  id;
        QJSValue updateFn;
        QString  currentText;
    };
    // Color script evaluation
    struct ColorScriptState {
        int32_t  id;
        QJSValue updateFn;
        std::array<float, 3> currentColor;
    };
    // Property script evaluation
    struct PropertyScriptState {
        int32_t     id;
        std::string property;
        std::string layerName;
        QJSValue    updateFn;
        QJSValue    initFn;
        QJSValue    cursorClickFn;   // optional cursorClick handler from IIFE
        QJSValue    cursorEnterFn;
        QJSValue    cursorLeaveFn;
        QJSValue    cursorDownFn;
        QJSValue    cursorUpFn;
        QJSValue    cursorMoveFn;
        QJSValue    applyUserPropertiesFn; // optional applyUserProperties handler
        QJSValue    destroyFn;             // optional destroy handler
        QJSValue    resizeScreenFn;        // optional resizeScreen handler
        QJSValue    mediaPlaybackChangedFn;
        QJSValue    mediaPropertiesChangedFn;
        QJSValue    mediaThumbnailChangedFn;
        QJSValue    mediaTimelineChangedFn;
        QJSValue    mediaStatusChangedFn;
        QJSValue    animationEventFn;           // optional animationEvent(event,value) handler
        QJSValue    thisLayerProxy;  // cached layer proxy (avoids evaluate per frame)
        bool                 currentVisible {true};
        std::array<float, 3> currentVec3 {0, 0, 0};
        float                currentFloat {1.0f};
    };
    // Sound volume script evaluation
    struct SoundVolumeAnimState {
        std::string name;
        std::string mode;   // "loop", "single", "mirror"
        float       fps { 1 };
        float       length { 0 };
        struct Keyframe { float frame; float value; };
        std::vector<Keyframe> keyframes;
        double      time { 0 };      // current animation time (seconds)
        bool        playing { false };
        // Scene-time of the last eval (for computing delta against scene clock).
        // Negative sentinel means "not yet sampled" — first eval skips the delta.
        double      lastSceneTime { -1.0 };
        // Evaluate at current time → interpolated value
        float evaluate() const;
    };
    struct SoundVolumeScriptState {
        int32_t     index;
        QJSValue    updateFn;
        QJSValue    applyUserPropertiesFn;
        QJSValue    thisLayerProxy;
        std::string layerName;
        float       currentVolume {1.0f};
        bool        hasAnimation { false };
        SoundVolumeAnimState anim;
    };
    QJSEngine*                        m_jsEngine { nullptr };
    SceneTimerBridge*                 m_timerBridge { nullptr };
    QTimer*                           m_textTimer { nullptr };
    QTimer*                           m_colorTimer { nullptr };
    QTimer*                           m_propertyTimer { nullptr };
    QElapsedTimer                     m_runtimeTimer;
    std::vector<TextScriptState>      m_textScriptStates;
    std::vector<ColorScriptState>     m_colorScriptStates;
    std::vector<PropertyScriptState>  m_propertyScriptStates;
    std::vector<SoundVolumeScriptState> m_soundVolumeScriptStates;
    std::unordered_map<std::string, int32_t> m_nodeNameToId;
    QJSValue                          m_collectDirtyLayersFn;
    QJSValue                          m_collectDirtySceneFn;
    QJSValue                          m_fireSceneEventFn;
    QJSValue                          m_hasSceneListenersFn;
    void fireSceneEventListeners(const QString& eventName,
                                 const QJSValueList& args = {});

    // Sound layer control state for SceneScript play/stop/pause API
    struct SoundLayerState {
        int32_t     index;
        std::string name;
    };
    std::vector<SoundLayerState>             m_soundLayerStates;
    std::unordered_map<std::string, int32_t> m_soundLayerNameToIndex;
    QJSValue                                 m_collectDirtySoundLayersFn;

    // Audio buffer registrations for SceneScript
    struct AudioBufferReg {
        int      resolution; // 16, 32, or 64
        QJSValue leftArray;
        QJSValue rightArray;
        QJSValue averageArray;
    };
    std::vector<AudioBufferReg> m_audioBufferRegs;
    void refreshAudioBuffers();

    // Cursor event targets (deduplicated by layer name)
    struct CursorTarget {
        std::string layerName;
        QJSValue    clickFn;
        QJSValue    enterFn;
        QJSValue    leaveFn;
        QJSValue    downFn;
        QJSValue    upFn;
        QJSValue    moveFn;
        QJSValue    thisLayerProxy;
    };
    std::vector<CursorTarget>      m_cursorTargets;
    std::unordered_set<std::string> m_hoveredLayers;   // layers cursor is over
    std::string                    m_dragTarget;       // layer being dragged
    float m_sceneOrthoW {1920.0f};
    float m_sceneOrthoH {1080.0f};
    float m_cursorSceneX {0.0f};   // scene-space cursor X, updated on hover/drag
    float m_cursorSceneY {0.0f};   // scene-space cursor Y, updated on hover/drag

protected:
    QSGNode* updatePaintNode(QSGNode*, UpdatePaintNodeData*);
};

} // namespace scenebackend
