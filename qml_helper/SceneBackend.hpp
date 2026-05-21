#pragma once

#include <QtQuick/QQuickFramebufferObject>
#include <QtCore/QLoggingCategory>
#include <QtCore/QTimer>
#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonObject>
#include <QtGui/QMouseEvent>
#include <QtGui/QHoverEvent>
#include <QtQml/QJSEngine>
#include <QtQml/QJSValue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "HoverLeaveDebounce.h"
#include "JsWatchdog.h"
#include "ScriptDiagState.h"
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
    Q_PROPERTY(QString userProperties READ userProperties WRITE setUserProperties NOTIFY
                   userPropertiesChanged)
    Q_PROPERTY(bool hdrOutput READ hdrOutput WRITE setHdrOutput)
    Q_PROPERTY(bool systemAudioCapture READ systemAudioCapture WRITE setSystemAudioCapture)
    // Override scene.general.orthogonalprojection.postprocessing.  Empty string
    // means "use the scene's own value".  Common values: "low", "medium",
    // "ultra" (HDR mip-chain bloom), "displayhdr".  Effective when scene also
    // declares hdr+bloom; otherwise the legacy LDR bloom path runs.
    Q_PROPERTY(QString postprocessingOverride READ postprocessingOverride WRITE
                   setPostprocessingOverride)
    // Standalone viewer override: when non-zero, initVulkan() uses these
    // values verbatim instead of item-width * devicePixelRatio.  Lets -R
    // on sceneviewer-script land an exact physical pixel size regardless of
    // Wayland fractional-scaling / HiDPI quirks that make dpr racy at
    // window-show time.  The KDE plugin leaves this at 0 and uses the
    // regular dpr path.
    Q_PROPERTY(int renderPixelWidth MEMBER m_renderPixelWidth)
    Q_PROPERTY(int renderPixelHeight MEMBER m_renderPixelHeight)
    // Wallpaper's native aspect ratio (orthoW / orthoH from the parsed scene).
    // 0.0 until the scene finishes loading; QML uses that sentinel to fall
    // back to anchors.fill while the scene comes up.  Lets QML size this
    // QQuickItem to the wallpaper's native aspect in Keep-Aspect-Ratio mode
    // so the wrapper's BackgroundColor Rectangle shows through the bars
    // instead of the renderer drawing them itself.
    Q_PROPERTY(qreal nativeAspectRatio READ nativeAspectRatio NOTIFY nativeAspectRatioChanged)
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

    int     fps() const;
    int     fillMode() const;
    float   speed() const;
    float   volume() const;
    bool    muted() const;
    bool    hdrOutput() const;
    bool    systemAudioCapture() const;
    QString userProperties() const;
    QString postprocessingOverride() const;
    qreal   nativeAspectRatio() const;

    void setFps(int);
    void setFillMode(int);
    void setSpeed(float);
    void setVolume(float);
    void setMuted(bool);
    void setUserProperties(const QString&);
    void setHdrOutput(bool);
    void setSystemAudioCapture(bool);
    void setPostprocessingOverride(const QString&);

    // debug
    bool vulkanValid() const;
    void enableVulkanValid();
    void enableGenGraphviz();

    Q_INVOKABLE void setAcceptMouse(bool);
    Q_INVOKABLE void setAcceptHover(bool);

    // Headless test hooks — let standalone viewers drive cursor + capture
    // frames without an X11/Wayland event pump.  Same dispatch path as the
    // normal Qt event handlers, so JS cursorEnter/Leave/Click scripts fire.
    Q_INVOKABLE void simulateHoverAt(double x, double y);
    Q_INVOKABLE void simulateClickAt(double x, double y);
    // Press at (x1,y1) → drag through optional midpoint → release at (x2,y2).
    // Drives the same event handlers as real Qt input so SceneScript drag
    // handlers (cursorDown/cursorMove/cursorUp) fire end-to-end.
    Q_INVOKABLE void simulateDragAt(double x1, double y1, double x2, double y2);
    Q_INVOKABLE void requestScreenshot(const QString& path);
    Q_INVOKABLE bool screenshotDone() const;
    Q_INVOKABLE void requestPassDump(const QString& dir);
    Q_INVOKABLE bool passDumpDone() const;
    // Debug: pass through to SceneWallpaper::setHidePattern so
    // sceneviewer-script can isolate visual contributions of named layers
    // (e.g. `--hide-pattern audio_bars` to confirm a layer is the source
    // of an artifact).  Must be set before the scene loads — applied at
    // CMD_SET_SCENE parse time, not retroactively.
    Q_INVOKABLE void setHidePattern(const QString& pattern);
    // Debug hook — evaluate an arbitrary JS snippet in the SceneScript
    // QJSEngine.  Used by sceneviewer-script --js-eval to force state
    // machine transitions (e.g. `shared.rst=1` to trigger 3body's universe
    // reset without needing the right cursor conditions to line up).
    Q_INVOKABLE QString debugEvalJs(const QString& src);
    QString             m_pendingJsEval;

    // Media integration events (called from QML MprisMonitor, dispatched to JS)
    Q_INVOKABLE void mediaPlaybackChanged(int state);
    Q_INVOKABLE void mediaPropertiesChanged(const QString& title, const QString& artist,
                                            const QString& albumTitle, const QString& albumArtist,
                                            const QString& genres,
                                            double         duration = 0.0);
    Q_INVOKABLE void mediaThumbnailChanged(bool hasThumbnail, const QVariantList& colors);
    Q_INVOKABLE void mediaTimelineChanged(double position, double duration, int state = 0);
    Q_INVOKABLE void mediaStatusChanged(bool enabled);

    // Video texture control — bridge for thisLayer.getVideoTexture().
    // Takes a layer name; all methods are safe no-ops / zero-return on unknown
    // layers.  Bound into QJSEngine as __sceneBridge so JS proxies can call them.
    Q_INVOKABLE double videoGetCurrentTime(const QString& layerName) const;
    Q_INVOKABLE double videoGetDuration(const QString& layerName) const;
    Q_INVOKABLE bool   videoIsPlaying(const QString& layerName) const;
    Q_INVOKABLE void   videoPlay(const QString& layerName);
    Q_INVOKABLE void   videoPause(const QString& layerName);
    Q_INVOKABLE void   videoStop(const QString& layerName);
    Q_INVOKABLE void   videoSetCurrentTime(const QString& layerName, double t);
    Q_INVOKABLE void   videoSetRate(const QString& layerName, double rate);

    // Material uniform bridge — thisLayer.getMaterial().setValue(name, value).
    // Resolves layerName -> nodeId via m_nodeNameToId; unknown layers, empty
    // names, and malformed values are silently no-ops (matching the JS-side
    // _packMaterialValue guards).
    Q_INVOKABLE void materialSetValue(const QString& layerName,
                                      const QString& name,
                                      const QJSValue& value);

    // Per-effect material bridge — thisLayer.getEffect(name).getMaterial()
    // returns an IMaterial proxy whose writes route through this call instead
    // of materialSetValue.  effectIdx is the 0-based index into the layer's
    // effect chain (matches the order parsed from scene.json `effects[]`).
    // Render thread resolves effectIdx → SceneImageEffect → first-pass material.
    Q_INVOKABLE void effectMaterialSetValue(const QString&  layerName,
                                            int             effectIdx,
                                            const QString&  name,
                                            const QJSValue& value);

    // thisLayer.getTextureAnimation().setFrame(N) / .play() / .pause() bridge.
    // wantsManual=true pins the sprite to frameIdx; false restores
    // time-driven auto-advance.  Applied per-pass each render tick in
    // WPShaderValueUpdater::UpdateUniforms.
    Q_INVOKABLE void setLayerSpriteFrame(const QString& layerName,
                                         bool           wantsManual,
                                         int            frameIdx);

    // thisLayer.getTextureAnimation() read-back bridge.  Returns a JS object
    // `{frameCount, currentFrame, duration, isManualPin}` populated from the
    // per-node snapshot WPShaderValueUpdater publishes each render tick.
    // Empty/zero fields when the layer has no sprite or hasn't drawn yet —
    // the JS proxy maps that to `frameCount:1, currentFrame:0, duration:0,
    // isPlaying:true` so scripts that touch missing-sprite layers don't crash.
    // Lets authoring patterns like `frame === ani.frameCount - 1` (Rella
    // firework 3363252053) observe real playback instead of a hardcoded `1`
    // that fired the end-of-cycle branch on every tick.
    Q_INVOKABLE QJSValue getLayerSpriteInfo(const QString& layerName) const;

    // Text-style bridge — thisLayer.{horizontalalign,verticalalign,alignment,font}.
    // Each string is "" to leave that field unchanged.  Font name is resolved
    // to bytes on the render thread.  The JS shim parses `alignment` into its
    // h/v components before calling this so we only deal with two axes here.
    Q_INVOKABLE void setTextStyle(const QString& layerName,
                                  const QString& halign,
                                  const QString& valign,
                                  const QString& fontName);

    // World-transform readback for thisLayer.getTransformMatrix().  Returns a
    // 16-element flat array (column-major) snapshotted at the end of the most
    // recent drawFrame.  Identity for unknown layers.  Scripts mainly read
    // [12]/[13] for world X/Y translation.
    Q_INVOKABLE QJSValue getLayerWorldTransform(const QString& layerName) const;

    // thisLayer.getBoneIndex(name) — resolves the named MDAT attachment in the
    // layer's puppet (or its parent's puppet, mirroring WE rigging semantics)
    // and returns the attachment's bone_index.  Returns 0 when not found,
    // matching WE's "origin / missing" sentinel that authoring scripts treat
    // as the absence signal.
    Q_INVOKABLE int getBoneIndex(const QString& layerName, const QString& boneName) const;

    // Layer-hierarchy bridge — thisLayer.setParent(other) JS path enqueues
    // a (childId, parentId) pair into Scene::m_pending_parent_changes,
    // which is drained at the start of RenderHandler::CMD_DRAW.  parentId
    // == -1 means "reattach to scene root".  Unknown ids are silently
    // dropped during drain (no-op).  Named `setLayerParent` instead of
    // `setParent` to avoid collision with QObject::setParent.
    Q_INVOKABLE void setLayerParent(int childId, int parentId);

    // SceneScript thisScene.sortLayer(layer, index) bridge.  Enqueues a
    // (childId, targetIndex) pair into Scene::m_pending_child_sorts, drained
    // at the start of RenderHandler::CMD_DRAW.  Reorders the child within
    // its current parent's children list — `targetIndex` clamps to
    // [0, parent->children.size() - 1].  Used by Blue Archive (2764537029)
    // visualizer to keep all 64 dynamically-spawned audio bars at the same
    // depth as the template layer.  Unknown ids are silently dropped during
    // drain.
    Q_INVOKABLE void sortLayer(int childId, int targetIndex);

    // engine.openUserShortcut(name) — routes a named user-shortcut fired from
    // SceneScript.  Emits userShortcutRequested() for the main plugin to map
    // to MPRIS (media-control names like "bplay"/"b11"/"bprev") and fires a
    // `userShortcut` event on the scene bus so wallpapers can add their own
    // `scene.on('userShortcut', ...)` handlers.  Unmapped names are logged so
    // per feedback_no_stubs they surface rather than silently vanishing.
    Q_INVOKABLE void openUserShortcut(const QString& name);

    // localStorage bridge — backs the JS localStorage shim with disk-backed
    // stores.  `loc` is 0 (LOCATION_GLOBAL, shared across every wallpaper in
    // the cache) or 1 (LOCATION_SCREEN, per-scene keyed by the workshop id or
    // project directory name).  Writes debounce-flush to JSON files under
    // ~/.cache/wescene-renderer/ so user-configured state (icon visibility
    // toggles, hit counters etc.) survives across sceneviewer/plasmashell
    // restarts.  Any other `loc` value is treated as LOCATION_SCREEN since
    // WE scripts sometimes omit the argument.
    Q_INVOKABLE QJSValue lsGet(int loc, const QString& key);
    Q_INVOKABLE void     lsSet(int loc, const QString& key, const QJSValue& value);
    Q_INVOKABLE void     lsRemove(int loc, const QString& key);
    Q_INVOKABLE void     lsClear(int loc);

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
    void nativeAspectRatioChanged();
    void firstFrame();
    // Emitted when SceneScript calls engine.openUserShortcut(name).  The main
    // plugin's Scene.qml maps common media-control names to MPRIS actions;
    // standalone sceneviewer has no listener, so the action surfaces only
    // through LOG_INFO and the in-scene `userShortcut` event bus.
    void userShortcutRequested(const QString& name);

private:
    QUrl m_source;
    QUrl m_assets;

    int     m_fps { 15 };
    int     m_fillMode { FillMode::ASPECTCROP };
    float   m_speed { 1.0f };
    float   m_volume { 1.0f };
    bool    m_muted { false };
    bool    m_hdrOutput { false };
    bool    m_systemAudioCapture { false };
    QString m_postprocessingOverride;
    QString m_userProperties;
    int     m_renderPixelWidth { 0 };
    int     m_renderPixelHeight { 0 };

public:
    static void on_update(void* ctx);

    explicit SceneObject(QQuickItem* parent = nullptr);
    virtual ~SceneObject();

private:
    void setScenePropertyQurl(std::string_view, QUrl);
    void setupTextScripts();
    void refreshJsUserProperties();
    void fireApplyUserProperties();
    // Fires the "applyGeneralSettings" scene event (distinct from
    // applyUserProperties, which is per-wallpaper user config).  Dispatched
    // once during setup so scripts can key off it for init-time app-level
    // state, and is safe to call again later if plugin-wide settings change.
    void fireApplyGeneralSettings();
    // Populates engine.scriptId / scriptName / getScriptHash() from a
    // stable fingerprint of the loaded property scripts.  Must be called
    // AFTER m_propertyScriptStates is populated; the init-time stub writes
    // zero values that this overrides.
    void setScriptIdentity();
    void fireDestroyEvent();
    void fireResizeScreen(int width, int height);
    void evaluateTextScripts();
    void evaluateColorScripts();
    void evaluatePropertyScripts();
    void cleanupTextScripts();

    // A3-T2 — run an untrusted-JS dispatch under the off-thread watchdog.
    // Arms the deadline, invokes `fn` (the QJSEngine::call() site), disarms, and
    // clears the engine's interrupted flag if the watchdog fired.  `fn` is
    // wrapped in try/catch so the interrupt-induced JS exception (or any C++
    // throw) is contained, not propagated up the GUI-thread event loop.  Sets
    // `*outInterrupted` (when non-null) to whether the watchdog tripped, so the
    // property tick can drive the consecutive-interrupt back-off.  Returns the
    // call's QJSValue (undefined on interrupt/throw).
    QJSValue callJsGuarded(const std::function<QJSValue()>& fn, bool* outInterrupted = nullptr);

    // A3-T2 back-off shared by the property/text/color tick loops.  On an
    // interrupted tick increments `consecutive`; once the run reaches
    // JsWatchdog::kDisableAfterInterrupts (and not already disabled) it latches
    // `disabledFlag`, stops `timer`, logs once, and returns true (caller bails
    // this tick).  A clean tick zeroes `consecutive`.  `what` names the loop.
    bool applyInterruptBackoff(bool tickInterrupted, int& consecutive, bool& disabledFlag,
                               QTimer* timer, const char* what);

    bool m_inited { false };
    bool m_enable_valid { false };
    // Set by pause()/play().  Gates the property + color script loops (F19) via
    // scenebackend::scriptLoopShouldRun so a paused wallpaper (TTY-switch /
    // suspend / battery / occlusion) stops burning its 125Hz/30Hz GUI-thread JS
    // evaluation.  pause()/play() also stop/restart the timers themselves; this
    // flag is the belt that idles the seed eval + any already-queued timer tick.
    bool m_paused { false };

    std::shared_ptr<wallpaper::SceneWallpaper> m_scene { nullptr };

    // Text script evaluation
    struct TextScriptState {
        int32_t  id;
        QJSValue updateFn;
        QString  currentText;
    };
    // Color script evaluation
    // Shader-value script state — drives per-effect uniform updates from
    // inline scripts attached to `effects[N].passes[M].constantshadervalues[X]`.
    // Game of Life (3453251764) Canvas: ~50 scripts per scene driving
    // mouseDown/mousePos/cmd/drawRadius/birthSurvive into the cell shader.
    struct ShaderValueScriptState {
        int32_t            id { -1 };
        int32_t            effectIdx { 0 };
        std::string        uniformName;
        std::string        layerName;
        QJSValue           updateFn;
        QJSValue           thisLayerProxy;
        QJSValue           thisObjectProxy;
        QJSValue           cursorEnterFn;
        QJSValue           cursorLeaveFn;
        QJSValue           cursorUpFn;
        QJSValue           cursorDownFn;
        QJSValue           cursorClickFn;
        QJSValue           cursorMoveFn;
        int                argShape { 1 };
        std::vector<float> cachedValue;
    };

    struct ColorScriptState {
        int32_t              id;
        std::string          layerName;       // resolved from id via nodeNameToId
        QJSValue             updateFn;
        QJSValue             thisLayerProxy;
        QJSValue             thisObjectProxy;
        // Cursor handlers exported by the color script.  Game of Life
        // (3453251764) buttons attach their hover/click logic to the
        // layer's `color` property script (not a top-level property
        // script), so the cursor-target collection has to walk these
        // alongside m_propertyScriptStates or the buttons stay inert.
        QJSValue             cursorEnterFn;
        QJSValue             cursorLeaveFn;
        QJSValue             cursorUpFn;
        QJSValue             cursorDownFn;
        QJSValue             cursorClickFn;
        QJSValue             cursorMoveFn;
        std::array<float, 3> currentColor;
    };
    // Property script evaluation
    struct PropertyScriptState {
        // kind is computed once from `property` at load-time so the hot tick
        // loop can dispatch via a single integer compare instead of two
        // `property == "visible"` / `property == "alpha"` std::string checks.
        // Values are ordered so a stable partition by kind yields the
        // (visible, vec3, alpha) execution order that dependent scripts rely
        // on (visibles compute shared.* that vec3/alpha scripts then read).
        // ParticleRate (NieR:Automata audio-reactive starfields) returns a
        // scalar just like Alpha, so the JS dispatch loop handles it with
        // the same [xe, N) scalar code path — we only need a distinct Kind
        // so the C++ switch routes the value to updateParticleRate instead
        // of updateNodeAlpha.
        enum class Kind : uint8_t
        {
            Visible      = 0,
            Vec3         = 1,
            Alpha        = 2,
            ParticleRate = 3
        };
        int32_t     id;
        std::string property;
        std::string layerName;
        Kind        kind { Kind::Vec3 };
        QJSValue    updateFn;
        QJSValue    initFn;
        QJSValue    cursorClickFn; // optional cursorClick handler from IIFE
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
        QJSValue    animationEventFn; // optional animationEvent(event,value) handler
        QJSValue    thisLayerProxy;   // cached layer proxy (avoids evaluate per frame)
        // Per-call thisObject proxy.  For Object-attached scripts this is
        // equivalent to thisLayerProxy; for AnimationLayer-attached scripts
        // (e.g. Lucy's per-rig offset scripts on /objects[N]/animationlayers[M])
        // this resolves to thisLayer.getAnimationLayer(animationLayerIndex),
        // so scripts can call thisObject.setFrame / play / frameCount on the
        // specific rig layer rather than the parent image.
        QJSValue             thisObjectProxy;
        int32_t              animationLayerIndex { -1 };
        bool                 currentVisible { true };
        std::array<float, 3> currentVec3 { 0, 0, 0 };
        float                currentFloat { 1.0f };
    };
    // Sound volume script evaluation
    struct SoundVolumeAnimState {
        std::string name;
        std::string mode; // "loop", "single", "mirror"
        float       fps { 1 };
        float       length { 0 };
        struct Keyframe {
            float frame;
            float value;
        };
        std::vector<Keyframe> keyframes;
        double                time { 0 }; // current animation time (seconds)
        bool                  playing { false };
        // Scene-time of the last eval (for computing delta against scene clock).
        // Negative sentinel means "not yet sampled" — first eval skips the delta.
        double lastSceneTime { -1.0 };
        // Evaluate at current time → interpolated value
        float evaluate() const;
    };
    struct SoundVolumeScriptState {
        int32_t              index;
        QJSValue             updateFn;
        QJSValue             applyUserPropertiesFn;
        QJSValue             thisLayerProxy;
        std::string          layerName;
        float                currentVolume { 1.0f };
        bool                 hasAnimation { false };
        SoundVolumeAnimState anim;
    };
    QJSEngine*                               m_jsEngine { nullptr };
    SceneTimerBridge*                        m_timerBridge { nullptr };
    // A3-T2 — off-GUI-thread watchdog that aborts a runaway author script via
    // QJSEngine::setInterrupted(true) when a JS .call() exceeds its budget.
    // Armed/disarmed around each dispatch site (property/text/color/lifecycle).
    // Stopped (thread joined) before m_jsEngine is deleted in cleanupTextScripts.
    JsWatchdog                               m_jsWatchdog;
    // Count of consecutive watchdog interrupts on the property tick; a clean
    // tick resets it.  After JsWatchdog::kDisableAfterInterrupts, the property
    // timer is disabled (the wallpaper stops animating instead of freezing the
    // shell) and m_propertyScriptsDisabled latches so we log only once.
    int                                      m_consecutivePropInterrupts { 0 };
    bool                                     m_propertyScriptsDisabled { false };
    // A3-T2 back-off for the text and color tick loops — separate methods/timers
    // (m_textTimer/m_colorTimer), so they can't reuse the property latch.  Same
    // disable-after-K policy via applyInterruptBackoff.  (Sound-volume rides the
    // property latch via tickInterrupted; shader-value rides the color latch —
    // same method/timer.)  Reset on wallpaper switch (setup/cleanupTextScripts).
    int  m_consecutiveTextInterrupts { 0 };
    bool m_textScriptsDisabled { false };
    int  m_consecutiveColorInterrupts { 0 };
    bool m_colorScriptsDisabled { false };
    // Drives the watchdog deadline (ms).  Overridable so a unit/integration
    // test can inject a tiny budget without waiting 250ms; production leaves
    // it at the default.
    int64_t                                  m_jsWatchdogBudgetMs { JsWatchdog::kDefaultBudgetMs };
    QTimer*                                  m_textTimer { nullptr };
    QTimer*                                  m_colorTimer { nullptr };
    QTimer*                                  m_propertyTimer { nullptr };
    QElapsedTimer                            m_runtimeTimer;
    qint64                                   m_lastPropertyTickMs { -1 };
    qint64                                   m_lastColorTickMs { -1 };
    qint64                                   m_lastTextTickMs { -1 };
    // Last render-thread frame index seen by evaluateTextScripts.  The text
    // timer polls SceneWallpaper::getFrameIdx() at ~125Hz; eval only fires
    // when the index advances, so user scripts that compute FPS as
    // `1000 / (Date.now() - prev)` measure the actual render rate instead of
    // the QTimer cadence (was: 2Hz, hard-coded — Miku 3363252053 was stuck at
    // "fps: 2" because of that).
    uint64_t                                 m_lastTextFrameIdx { 0 };
    // Spec 07 — last render-thread frame index seen by evaluatePropertyScripts.
    // The property timer polls getFrameIdx() at ~125Hz; eval only fires when the
    // index advances (or on the seed eval / a high-rate scene), so a non-high-
    // rate scripted wallpaper stops burning ~5.7ms/tick on ticks that no
    // rendered frame ever reads.
    uint64_t m_lastPropertyFrameIdx { 0 };
    // When true this wallpaper opted into sub-frame physics stepping and the
    // property loop runs at the full timer rate (~125Hz), bypassing the
    // render-frame gate.  Default off; set from WEKDE_SCRIPT_HIGHRATE=1 or a
    // workshop-id allowlist (3body 3509243656) in setupTextScripts.
    bool m_propertyHighRate { false };
    // Spec 08 — last render frame index for which the audio buffers were
    // refreshed.  The property/text/color loops all call refreshAudioBuffers();
    // this de-dups them to one actual rebuild per drawn frame (the analyzer only
    // produces new spectrum data per processed render frame).
    uint64_t m_lastAudioBufFrameIdx { 0 };
    // Monotonic frame counter exposed to scripts as `engine.frameCount`.
    // Ticked once per property-script evaluation (the 120Hz/8ms pulse,
    // not the render FIF).
    qint64                                   m_propFrameCount { 0 };
    std::vector<TextScriptState>             m_textScriptStates;
    std::vector<ColorScriptState>            m_colorScriptStates;
    std::vector<ShaderValueScriptState>      m_shaderValueScriptStates;
    std::vector<PropertyScriptState>         m_propertyScriptStates;
    std::vector<SoundVolumeScriptState>      m_soundVolumeScriptStates;
    // Per-load diagnostics dedup / one-shot state (Item 15) — one per SceneObject
    // (per monitor), cleared in cleanupTextScripts() on each wallpaper switch.
    // Was six function-local statics (process-global).
    ScriptDiagState                          m_scriptDiag;
    std::unordered_map<std::string, int32_t> m_nodeNameToId;
    QJSValue                                 m_collectDirtyLayersFn;
    QJSValue                                 m_collectDirtySceneFn;
    QJSValue                                 m_fireSceneEventFn;
    QJSValue                                 m_hasSceneListenersFn;
    // Batched property-script dispatch — see `_runAllPropertyScripts` in
    // SceneBackend.cpp.  One C++->JS call per tick instead of N.
    QJSValue m_runAllPropertyScriptsFn;
    void     fireSceneEventListeners(const QString& eventName, const QJSValueList& args = {});

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
    void                        refreshAudioBuffers();

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
        QJSValue    thisObjectProxy; // == thisLayerProxy for Object-attached; rig layer otherwise
    };
    std::vector<CursorTarget>       m_cursorTargets;
    std::unordered_set<std::string> m_hoveredLayers; // layers cursor is over
    std::string                     m_dragTarget;    // layer being dragged

    // Hover-leave debounce: when the cursor briefly leaves a layer we delay
    // firing cursorLeave by a short window; if the cursor re-enters within
    // the window, the pending leave is cancelled.  This is what lets the
    // user graze the edge of the music-player hover zone on wallpaper
    // 2866203962 without the UI fading out before they can click a button.
    // State machine lives in qml_helper/HoverLeaveDebounce.h (pure, unit-tested).
    std::unordered_map<std::string, PendingLeave> m_pendingLeaves;
    QTimer*                                       m_hoverLeaveTimer { nullptr };
    void  flushPendingLeaves(); // fires cursorLeave for layers past their deadline
    float m_sceneOrthoW { 1920.0f };
    float m_sceneOrthoH { 1080.0f };
    // gates nativeAspectRatio: returns 0.0 until the parser fills the real ortho size
    bool  m_sceneOrthoLoaded { false };
    float m_cursorSceneX { 0.0f }; // scene-space cursor X, updated on hover/drag
    float m_cursorSceneY { 0.0f }; // scene-space cursor Y, updated on hover/drag
    float m_mouseNx { 0.5f };      // widget-normalized cursor X (0..1)
    float m_mouseNy { 0.5f };      // widget-normalized cursor Y (0..1, top-down)

    // Camera-parallax config, cached from SceneWallpaper::getParallaxInfo()
    // at load time + refreshed with live mouse coords on every hit-test
    // so hitTestLayerProxy can mirror the shader MVP offset.
    struct CursorParallaxCache {
        bool  enable { false };
        float amount { 0.5f };
        float mouseInfluence { 0.1f };
        float camX { 0.0f };
        float camY { 0.0f };
    };
    CursorParallaxCache m_parallaxCache {};
    void                refreshParallaxCache();

    // localStorage persistence — see Q_INVOKABLE ls* in the public section.
    // `m_lsSceneId` is resolved from m_source on first scene load; empty
    // means LOCATION_SCREEN reads/writes stay in-memory only (matches the
    // old behavior).  The flush timer coalesces rapid writes into one disk
    // write — solar's media script pokes localStorage 30Hz when debug is on.
    QJsonObject m_lsGlobal;
    QJsonObject m_lsScreen;
    QString     m_lsSceneId;
    bool        m_lsGlobalDirty { false };
    bool        m_lsScreenDirty { false };
    bool        m_lsLoaded { false };
    QTimer*     m_lsFlushTimer { nullptr };
    void        ensureLocalStorageLoaded();
    void        scheduleLocalStorageFlush();
    void        flushLocalStorage();
    QString     localStoragePath(bool global) const;

protected:
    QSGNode* updatePaintNode(QSGNode*, UpdatePaintNodeData*) override;
};

} // namespace scenebackend
