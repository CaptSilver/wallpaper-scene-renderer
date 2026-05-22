#include "SceneBackend.hpp"
#include "SceneAspect.h"
#include "ScriptLoopGate.h"
#include "SceneCursorHitTest.h"
#include "HoverLeaveDebounce.h"
#include "JsStringEscape.hpp"
#include "JsSyntaxNormalize.hpp"
#include "PropertyScriptDispatchJs.hpp"
#include "SceneScriptShimsJs.hpp"
#include "SceneTickHelpers.h"
#include "SceneTimerBridge.h"

#include <QJSValueIterator>
#include <QtGlobal>
#include <QtCore/QObject>
#include <QtCore/QDir>
#include <QtCore/QThread>

#include <QtGui/QGuiApplication>
#include <QtGui/QOpenGLContext>
#include <QtQuick/QQuickWindow>

#include <QtGui/QOffscreenSurface>
#include <QtQuick/QSGSimpleTextureNode>
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#    include <QSGTexture>
#endif

#include <clocale>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <array>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <QtCore/QRegularExpression>
#include <QtCore/QTime>
#include <QtCore/QDateTime>
#include <QtCore/QTimeZone>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QJsonParseError>

#include "glExtra.hpp"
#include "SceneWallpaper.hpp"
#include "SceneWallpaperSurface.hpp"
#include "Type.hpp"
#include "Utils/Platform.hpp"
#include "Audio/AudioAnalyzer.h"
#include <cstdio>
#include <qobjectdefs.h>
#include <unistd.h>
#include "Utils/Logging.h"

using namespace scenebackend;

// Delegate to pure function in WPVolumeAnimation.h
#include "WPVolumeAnimation.h"
float SceneObject::SoundVolumeAnimState::evaluate() const {
    std::vector<wallpaper::VolumeAnimKeyframe> kfs;
    kfs.reserve(keyframes.size());
    for (const auto& kf : keyframes) kfs.push_back({ kf.frame, kf.value });
    return wallpaper::EvaluateVolumeAnimation(kfs, fps, length, mode, time);
}

Q_LOGGING_CATEGORY(wekdeScene, "wekde.scene")

// ##__VA_ARGS__ swallows the trailing comma when no format args are passed, so
// a plain message form (_Q_INFO("text")) does not pass a spurious "" argument
// that would trip -Wformat-extra-args.
#define _Q_INFO(fmt, ...) qCInfo(wekdeScene, fmt, ##__VA_ARGS__)

namespace
{
void* get_proc_address(const char* name) {
    QOpenGLContext* glctx = QOpenGLContext::currentContext();
    if (! glctx) return nullptr;

    return reinterpret_cast<void*>(glctx->getProcAddress(QByteArray(name)));
}

QSGTexture* createTextureFromGl(uint32_t handle, QSize size, QQuickWindow* window) {
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    return QNativeInterface::QSGOpenGLTexture::fromNative(handle, window, size);
#elif (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
    return window->createTextureFromNativeObject(
        QQuickWindow::NativeObjectTexture, &handle, 0, size);
#else
    return window->createTextureFromId(handle, size);
#endif
}

wallpaper::FillMode ToWPFillMode(int fillMode) {
    switch ((SceneObject::FillMode)fillMode) {
    case SceneObject::FillMode::STRETCH: return wallpaper::FillMode::STRETCH;
    case SceneObject::FillMode::ASPECTFIT: return wallpaper::FillMode::ASPECTFIT;
    case SceneObject::FillMode::ASPECTCROP:
    default: return wallpaper::FillMode::ASPECTCROP;
    }
}

} // namespace

using sp_scene_t = std::shared_ptr<wallpaper::SceneWallpaper>;

namespace scenebackend
{

class TextureNode : public QObject, public QSGSimpleTextureNode {
    Q_OBJECT
public:
    typedef std::function<QSGTexture*(QQuickWindow*)> EatFrameOp;
    TextureNode(QQuickWindow* window, sp_scene_t scene, bool valid, EatFrameOp eatFrameOp)
        // Init order follows member declaration order (m_scene, m_enable_valid,
        // m_texture, m_eatFrameOp, m_window, m_first_frame) to silence
        // -Wreorder-ctor; none of these initialisers reads another member.
        : m_scene(scene),
          m_enable_valid(valid),
          m_texture(nullptr),
          m_eatFrameOp(eatFrameOp),
          m_window(window),
          m_first_frame(false) {
        // texture node must have a texture, so use the default 0 texture.
        m_texture      = createTextureFromGl(0, QSize(64, 64), window);
        m_init_texture = m_texture;
        setTexture(m_texture);
        setFiltering(QSGTexture::Linear);
        setOwnsTexture(false);
    }

    ~TextureNode() override {
        for (auto& item : texs_map) {
            auto& exh = item.second;
            // close(exh.fd);
            m_glex.deleteTexture(exh.gltex);
            delete exh.qsg;
        }
        delete m_init_texture;
        emit nodeDestroyed();
        _Q_INFO("Destroy texnode");
    }

    // only at qt render thread
    bool initGl() { return m_glex.init(get_proc_address); }

    // after gl, can run at any thread
    void initVulkan(uint16_t w, uint16_t h, bool hdr_output = false) {
        wallpaper::RenderInitInfo info;
        info.enable_valid_layer = m_enable_valid;
        info.offscreen          = true;
        info.hdr_output         = hdr_output;
        info.offscreen_tiling   = m_glex.tiling();
        info.uuid               = m_glex.uuid();
        info.width              = w;
        info.height             = h;
        info.redraw_callback    = [this]() {
            Q_EMIT this->redraw();
        };

        auto cb = std::make_shared<wallpaper::FirstFrameCallback>([this]() {
            m_first_frame = true;
            Q_EMIT this->redraw();
        });
        m_scene->setPropertyObject(wallpaper::PROPERTY_FIRST_FRAME_CALLBACK, cb);
        // this send to looper, not in this thread
        m_scene->initVulkan(info);
    }

    void emitSceneFirstFrame() { Q_EMIT sceneFirstFrame(); }
signals:
    void textureInUse();
    void nodeDestroyed();
    void redraw();
    void sceneFirstFrame();

public slots:
    void newTexture() {
        if (! m_scene->inited() || m_scene->exSwapchain() == nullptr) return;

        wallpaper::ExHandle* exh = m_scene->exSwapchain()->eatFrame();
        if (exh != nullptr) {
            int id = exh->id();
            if (texs_map.count(id) == 0) {
                _Q_INFO("receive external texture(%dx%d) from fd: %d",
                        exh->width,
                        exh->height,
                        exh->fd);
                ExTex ex_tex;
                int   fd    = exh->fd;
                uint  gltex = m_glex.genExTexture(*exh);

                ex_tex.gltex = gltex;
                ex_tex.qsg   = createTextureFromGl(gltex, QSize(exh->width, exh->height), m_window);
                texs_map[id] = ex_tex;
                close(fd);
            }
            auto& newtex = texs_map.at(id);
            if (newtex.qsg != nullptr)
                m_texture = newtex.qsg;
            else
                m_texture = m_init_texture;

            setTexture(m_texture);
            markDirty(DirtyMaterial);
            Q_EMIT textureInUse();

            bool expected = true;
            if (m_first_frame.compare_exchange_strong(expected, false)) {
                Q_EMIT sceneFirstFrame();
            }
        }
    }

private:
    sp_scene_t m_scene;
    bool       m_enable_valid;

    QSGTexture*       m_init_texture;
    QSGTexture*       m_texture;
    EatFrameOp        m_eatFrameOp;
    QQuickWindow*     m_window;
    std::atomic<bool> m_first_frame;

    GlExtra m_glex;

    struct ExTex {
        // int fd;
        uint        gltex;
        QSGTexture* qsg;
    };
    std::unordered_map<int, ExTex> texs_map;
};

} // namespace scenebackend

SceneObject::SceneObject(QQuickItem* parent)
    : QQuickItem(parent), m_scene(std::make_shared<wallpaper::SceneWallpaper>()) {
    setFlag(ItemHasContents, true);
    m_scene->init();
    m_scene->setPropertyString(wallpaper::PROPERTY_CACHE_PATH, GetDefaultCachePath());

    connect(this, &SceneObject::firstFrame, this, &SceneObject::setupTextScripts);
}

SceneObject::~SceneObject() {
    cleanupTextScripts();
    // Final flush so the 500ms debounce timer doesn't drop pending writes
    // (e.g. solar's icon toggle saved right before sceneviewer exits).
    if (m_lsGlobalDirty || m_lsScreenDirty) flushLocalStorage();
    _Q_INFO("Destroy sceneobject");
}

void SceneObject::resizeFb() {
    int w = (int)this->width();
    int h = (int)this->height();
    fireResizeScreen(w, h);
}

QSGNode* SceneObject::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    TextureNode* node = static_cast<TextureNode*>(oldNode);
    if (! node) {
        node = new TextureNode(window(), m_scene, m_enable_valid, [](QQuickWindow*) {
            return (QSGTexture*)nullptr;
        });
        if (node->initGl()) {
            // Normally the Vulkan render target matches item-size × dpr so
            // it fills the backing store 1:1.  Standalone viewers can pin an
            // exact physical pixel size via the renderPixelWidth/Height
            // properties — needed because Wayland fractional scaling makes
            // dpr unstable around window-show time (see qmlviewer.cpp -R).
            int vk_w = m_renderPixelWidth > 0
                           ? m_renderPixelWidth
                           : (int)std::lround(width() * window()->devicePixelRatio());
            int vk_h = m_renderPixelHeight > 0
                           ? m_renderPixelHeight
                           : (int)std::lround(height() * window()->devicePixelRatio());
            node->initVulkan(vk_w, vk_h, m_hdrOutput);

            connect(
                node, &TextureNode::redraw, window(), &QQuickWindow::update, Qt::QueuedConnection);
            connect(window(),
                    &QQuickWindow::beforeRendering,
                    node,
                    &TextureNode::newTexture,
                    Qt::DirectConnection);
            connect(node, &TextureNode::sceneFirstFrame, this, &SceneObject::firstFrame);
        }
    }

    node->setRect(boundingRect());
    return node;
}

#define SET_PROPERTY(type, name, value) m_scene->setProperty##type(name, value);

void SceneObject::setScenePropertyQurl(std::string_view name, QUrl value) {
    auto str_value = QDir::toNativeSeparators(value.toLocalFile()).toStdString();
    SET_PROPERTY(String, name, str_value);
}
// qobject

QUrl SceneObject::source() const { return m_source; }
QUrl SceneObject::assets() const { return m_assets; }

int   SceneObject::fps() const { return m_fps; }
int   SceneObject::fillMode() const { return m_fillMode; }
float SceneObject::speed() const { return m_speed; }
float SceneObject::volume() const { return m_volume; }
bool  SceneObject::muted() const { return m_muted; }
qreal SceneObject::nativeAspectRatio() const {
    // Pure math + sentinel/guard live in SceneAspect.h so they can be unit-tested
    // without standing up the Vulkan-backed QQuickItem.  Qualified to disambiguate
    // from this member (we are inside `using namespace scenebackend`).
    return scenebackend::computeNativeAspectRatio(m_sceneOrthoLoaded, m_sceneOrthoW, m_sceneOrthoH);
}

void SceneObject::setSource(const QUrl& source) {
    if (source == m_source) return;
    m_source = source;
    cleanupTextScripts();
    setScenePropertyQurl(wallpaper::PROPERTY_SOURCE, m_source);
    Q_EMIT sourceChanged();
}

void SceneObject::setAssets(const QUrl& assets) {
    if (m_assets == assets) return;
    m_assets = assets;
    setScenePropertyQurl(wallpaper::PROPERTY_ASSETS, m_assets);
}

void SceneObject::setFps(int value) {
    if (m_fps == value) return;
    m_fps = value;
    SET_PROPERTY(Int32, wallpaper::PROPERTY_FPS, value);
    Q_EMIT fpsChanged();
}
void SceneObject::setFillMode(int value) {
    if (m_fillMode == value) return;
    m_fillMode = value;
    SET_PROPERTY(Int32, wallpaper::PROPERTY_FILLMODE, (int32_t)ToWPFillMode(value));
    Q_EMIT fillModeChanged();
}
void SceneObject::setSpeed(float value) {
    if (m_speed == value) return;
    m_speed = value;
    SET_PROPERTY(Float, wallpaper::PROPERTY_SPEED, value);
    Q_EMIT speedChanged();
}
void SceneObject::setVolume(float value) {
    if (m_volume == value) return;
    m_volume = value;
    SET_PROPERTY(Float, wallpaper::PROPERTY_VOLUME, value);
    Q_EMIT volumeChanged();
}
void SceneObject::setMuted(bool value) {
    if (m_muted == value) return;
    m_muted = value;
    SET_PROPERTY(Bool, wallpaper::PROPERTY_MUTED, value);
}

bool SceneObject::hdrOutput() const { return m_hdrOutput; }

void SceneObject::setHdrOutput(bool value) {
    if (m_hdrOutput == value) return;
    m_hdrOutput = value;
    SET_PROPERTY(Bool, wallpaper::PROPERTY_HDR_OUTPUT, value);
}

bool SceneObject::systemAudioCapture() const { return m_systemAudioCapture; }

void SceneObject::setSystemAudioCapture(bool value) {
    if (m_systemAudioCapture == value) return;
    m_systemAudioCapture = value;
    SET_PROPERTY(Bool, wallpaper::PROPERTY_SYSTEM_AUDIO_CAPTURE, value);
}

QString SceneObject::postprocessingOverride() const { return m_postprocessingOverride; }

void SceneObject::setPostprocessingOverride(const QString& value) {
    if (m_postprocessingOverride == value) return;
    m_postprocessingOverride = value;
    SET_PROPERTY(String, wallpaper::PROPERTY_POSTPROCESSING_OVERRIDE, value.toStdString());
}

QString SceneObject::userProperties() const { return m_userProperties; }

void SceneObject::setUserProperties(const QString& value) {
    if (m_userProperties == value) return;
    m_userProperties = value;
    SET_PROPERTY(String, wallpaper::PROPERTY_USER_PROPS, value.toStdString());
    if (m_jsEngine) refreshJsUserProperties();
    Q_EMIT userPropertiesChanged();
}

void SceneObject::refreshJsUserProperties() {
    if (! m_jsEngine || m_userProperties.isEmpty()) return;
    // Escape for safe embedding in a JS single-quoted string literal (covers
    // backslash, quote, raw newline/CR/tab, U+2028/U+2029 and other control
    // chars — see JsStringEscape.hpp / F18).  Without the full escape a literal
    // line terminator in the user-props JSON would SyntaxError the JSON.parse
    // and the try/catch below would silently drop all user-property overrides.
    QString  json   = wek::qml_helper::escapeForJsSingleQuoted(m_userProperties);
    QJSValue result = m_jsEngine->evaluate(QString("(function(){"
                                                   "try{"
                                                   "var p=JSON.parse('%1');"
                                                   "var up=engine.userProperties;"
                                                   "for(var k in p) up[k]=p[k];"
                                                   "}catch(e){}"
                                                   "})()")
                                               .arg(json));
    if (result.isError()) {
        LOG_INFO("refreshJsUserProperties error: %s", qPrintable(result.toString()));
    }
    // Derive engine.colorScheme from the schemecolor user property if Vec3
    // and _buildColorScheme are available.  schemecolor is stored as a
    // space-separated float string e.g. "0.5 0.1 0.9"; feed it as `primary`
    // and keep the default text/highContrast/etc. values.
    m_jsEngine->evaluate("(function(){"
                         "if (typeof Vec3 === 'undefined') return;"
                         "if (typeof _buildColorScheme !== 'function') return;"
                         "var sc = engine.userProperties.schemecolor;"
                         "if (sc !== undefined && sc !== null)"
                         "  engine.colorScheme = _buildColorScheme(Vec3.fromString(sc));"
                         "})()");

    // Fire applyUserProperties event on all property scripts that define it.
    // WE SceneScript passes an object with {propertyName: {value: newValue}} entries.
    fireApplyUserProperties();

    // Dispatch one applyGeneralSettings now so scripts that hook it can
    // initialise app-level state (FPS cap, quality, etc.).  Currently the
    // plugin has no separate "general settings" state to expose, so the
    // arg is an empty object.  Call this again if/when plugin-wide
    // settings change at runtime.
    fireApplyGeneralSettings();
}

void SceneObject::fireSceneEventListeners(const QString& eventName, const QJSValueList& args) {
    if (! m_fireSceneEventFn.isCallable()) return;
    QJSValue has = m_hasSceneListenersFn.call({ QJSValue(eventName) });
    if (! has.toBool()) return;
    QJSValueList callArgs;
    callArgs << QJSValue(eventName);
    callArgs.append(args);
    // A3-T2 — scene.on(...) handlers are untrusted author JS; run them under the
    // watchdog so a runaway listener can't freeze the GUI thread.
    callJsGuarded([&] {
        return m_fireSceneEventFn.call(callArgs);
    });
}

void SceneObject::fireApplyUserProperties() {
    if (! m_jsEngine) return;

    // Build the properties arg: { propName: { value: val }, ... }
    // This matches the WE SceneScript applyUserProperties(props) signature.
    QJSValue propsArg = m_jsEngine->globalObject().property("engine").property("userProperties");

    for (auto& state : m_propertyScriptStates) {
        if (! state.applyUserPropertiesFn.isCallable()) continue;

        if (! state.layerName.empty()) {
            m_jsEngine->globalObject().setProperty("thisLayer", state.thisLayerProxy);
            m_jsEngine->globalObject().setProperty("thisObject", state.thisObjectProxy);
        }

        QJSValue result = callJsGuarded([&] {
            return state.applyUserPropertiesFn.call({ propsArg });
        });
        if (result.isError()) {
            LOG_INFO("applyUserProperties error id=%d prop=%s: %s",
                     state.id,
                     state.property.c_str(),
                     qPrintable(result.toString()));
        }
    }

    // Also fire on sound volume scripts that define applyUserProperties
    for (auto& svState : m_soundVolumeScriptStates) {
        if (! svState.applyUserPropertiesFn.isCallable()) continue;

        if (! svState.layerName.empty()) {
            m_jsEngine->globalObject().setProperty("thisLayer", svState.thisLayerProxy);
            m_jsEngine->globalObject().setProperty("thisObject", svState.thisLayerProxy);
        }

        QJSValue result = callJsGuarded([&] {
            return svState.applyUserPropertiesFn.call({ propsArg });
        });
        if (result.isError()) {
            LOG_INFO("applyUserProperties error vol index=%d: %s",
                     svState.index,
                     qPrintable(result.toString()));
        }
    }

    fireSceneEventListeners("applyUserProperties", { propsArg });
}

void SceneObject::fireApplyGeneralSettings() {
    if (! m_jsEngine) return;
    QJSValue settingsArg = m_jsEngine->newObject();
    fireSceneEventListeners("applyGeneralSettings", { settingsArg });
}

void SceneObject::setScriptIdentity() {
    if (! m_jsEngine) return;
    // Fingerprint: concatenation of (layerName, property) pairs.  Changes
    // whenever the script set is added/removed/retargeted, stable otherwise.
    std::string fp;
    fp.reserve(m_propertyScriptStates.size() * 32);
    for (const auto& s : m_propertyScriptStates) {
        fp += s.layerName;
        fp += ':';
        fp += s.property;
        fp += ';';
    }
    std::size_t   h       = std::hash<std::string>{}(fp);
    quint32       idTrunc = static_cast<quint32>(h & 0xFFFFFFFFu);
    const QString hashHex = QString::number(static_cast<qulonglong>(h), 16);
    m_jsEngine->evaluate(
        QString("engine.scriptId = %1;\n"
                "engine.scriptName = 'scene';\n"
                "engine.getScriptHash = function() { return '%2'; };\n")
            .arg(idTrunc)
            .arg(hashHex));
}

void SceneObject::fireDestroyEvent() {
    if (! m_jsEngine || m_propertyScriptStates.empty()) return;

    // Re-register __sceneBridge before firing destroy handlers.  By the time
    // ~SceneObject runs, the QJSValue wrapper newQObject originally returned
    // can have its underlying metaobject torn down — property access on the
    // wrapper then surfaces as "Property 'lsSet' of object TypeError: Type
    // error is not a function" (the wrapper's .toString() returns a wrapped
    // error).  Re-newQObject(this) yields a fresh wrapper that still resolves
    // against the live `this` pointer, so destroy handlers' lsSet calls
    // (Game of Life 3453251764 brush persistence is the canonical driver)
    // reach the C++ side and save state to disk.
    m_jsEngine->globalObject().setProperty("__sceneBridge", m_jsEngine->newQObject(this));

    for (auto& state : m_propertyScriptStates) {
        if (! state.destroyFn.isCallable()) continue;

        if (! state.layerName.empty()) {
            m_jsEngine->globalObject().setProperty("thisLayer", state.thisLayerProxy);
            m_jsEngine->globalObject().setProperty("thisObject", state.thisObjectProxy);
        }

        QJSValue result = callJsGuarded([&] {
            return state.destroyFn.call({});
        });
        if (result.isError()) {
            LOG_INFO("destroy event error id=%d prop=%s: %s",
                     state.id,
                     state.property.c_str(),
                     qPrintable(result.toString()));
        }
    }
    fireSceneEventListeners("destroy");
}

void SceneObject::fireResizeScreen(int width, int height) {
    if (! m_jsEngine || m_propertyScriptStates.empty()) return;

    // Update engine.screenResolution and engine.canvasSize
    QJSValue engineObj = m_jsEngine->globalObject().property("engine");
    QJSValue sr        = engineObj.property("screenResolution");
    sr.setProperty("x", width);
    sr.setProperty("y", height);
    QJSValue cs = engineObj.property("canvasSize");
    cs.setProperty("x", width);
    cs.setProperty("y", height);

    for (auto& state : m_propertyScriptStates) {
        if (! state.resizeScreenFn.isCallable()) continue;

        if (! state.layerName.empty()) {
            m_jsEngine->globalObject().setProperty("thisLayer", state.thisLayerProxy);
            m_jsEngine->globalObject().setProperty("thisObject", state.thisObjectProxy);
        }

        QJSValue result = callJsGuarded([&] {
            return state.resizeScreenFn.call({ QJSValue(width), QJSValue(height) });
        });
        if (result.isError()) {
            LOG_INFO("resizeScreen error id=%d prop=%s: %s",
                     state.id,
                     state.property.c_str(),
                     qPrintable(result.toString()));
        }
    }
    fireSceneEventListeners("resizeScreen", { QJSValue(width), QJSValue(height) });
}

// Media integration event dispatch — called from QML MprisMonitor via Q_INVOKABLE

void SceneObject::mediaPlaybackChanged(int state) {
    if (! m_jsEngine) return;
    QJSValue event = m_jsEngine->newObject();
    event.setProperty("state", state);
    for (auto& s : m_propertyScriptStates) {
        if (! s.mediaPlaybackChangedFn.isCallable()) continue;
        if (! s.layerName.empty())
            m_jsEngine->globalObject().setProperty("thisLayer", s.thisLayerProxy);
        m_jsEngine->globalObject().setProperty("thisObject", s.thisObjectProxy);
        callJsGuarded([&] {
            return s.mediaPlaybackChangedFn.call({ event });
        });
    }
    fireSceneEventListeners("mediaPlaybackChanged", { event });
}

void SceneObject::mediaPropertiesChanged(const QString& title, const QString& artist,
                                         const QString& albumTitle, const QString& albumArtist,
                                         const QString& genres,
                                         double         duration) {
    if (! m_jsEngine) return;
    QJSValue event = m_jsEngine->newObject();
    event.setProperty("title", title);
    event.setProperty("artist", artist);
    event.setProperty("albumTitle", albumTitle);
    event.setProperty("albumArtist", albumArtist);
    event.setProperty("genres", genres);
    event.setProperty("contentType", QJSValue("media"));
    // 8 corpus scripts (in 3155776049, 3662790108, others) read .duration from
    // this event — easier than waiting for the next timelineChanged tick.
    event.setProperty("duration", duration);
    for (auto& s : m_propertyScriptStates) {
        if (! s.mediaPropertiesChangedFn.isCallable()) continue;
        if (! s.layerName.empty())
            m_jsEngine->globalObject().setProperty("thisLayer", s.thisLayerProxy);
        m_jsEngine->globalObject().setProperty("thisObject", s.thisObjectProxy);
        callJsGuarded([&] {
            return s.mediaPropertiesChangedFn.call({ event });
        });
    }
    fireSceneEventListeners("mediaPropertiesChanged", { event });
}

void SceneObject::mediaThumbnailChanged(bool hasThumbnail, const QVariantList& colors) {
    if (! m_jsEngine) return;
    QJSValue vec3Fn = m_jsEngine->globalObject().property("Vec3");
    auto     toVec3 = [&](int idx) -> QJSValue {
        if (idx * 3 + 2 >= colors.size())
            return vec3Fn.call({ QJSValue(0), QJSValue(0), QJSValue(0) });
        return vec3Fn.call({ QJSValue(colors[idx * 3].toDouble()),
                             QJSValue(colors[idx * 3 + 1].toDouble()),
                             QJSValue(colors[idx * 3 + 2].toDouble()) });
    };
    QJSValue event = m_jsEngine->newObject();
    event.setProperty("hasThumbnail", hasThumbnail);
    event.setProperty("primaryColor", toVec3(0));
    event.setProperty("secondaryColor", toVec3(1));
    event.setProperty("tertiaryColor", toVec3(2));
    event.setProperty("textColor", toVec3(3));
    event.setProperty("highContrastColor", toVec3(4));
    for (auto& s : m_propertyScriptStates) {
        if (! s.mediaThumbnailChangedFn.isCallable()) continue;
        if (! s.layerName.empty())
            m_jsEngine->globalObject().setProperty("thisLayer", s.thisLayerProxy);
        m_jsEngine->globalObject().setProperty("thisObject", s.thisObjectProxy);
        callJsGuarded([&] {
            return s.mediaThumbnailChangedFn.call({ event });
        });
    }
    fireSceneEventListeners("mediaThumbnailChanged", { event });
}

void SceneObject::mediaTimelineChanged(double position, double duration, int state) {
    if (! m_jsEngine) return;
    QJSValue event = m_jsEngine->newObject();
    event.setProperty("position", position);
    event.setProperty("duration", duration);
    // `state` mirrors MediaPlaybackEvent constants (0=stopped 1=playing 2=paused);
    // 13 corpus scripts read event.state from this event so they can react to
    // play/pause transitions without a separate playbackStateChanged subscriber.
    event.setProperty("state", state);
    for (auto& s : m_propertyScriptStates) {
        if (! s.mediaTimelineChangedFn.isCallable()) continue;
        if (! s.layerName.empty())
            m_jsEngine->globalObject().setProperty("thisLayer", s.thisLayerProxy);
        m_jsEngine->globalObject().setProperty("thisObject", s.thisObjectProxy);
        callJsGuarded([&] {
            return s.mediaTimelineChangedFn.call({ event });
        });
    }
    fireSceneEventListeners("mediaTimelineChanged", { event });
}

void SceneObject::mediaStatusChanged(bool enabled) {
    if (! m_jsEngine) return;
    QJSValue event = m_jsEngine->newObject();
    event.setProperty("enabled", enabled);
    for (auto& s : m_propertyScriptStates) {
        if (! s.mediaStatusChangedFn.isCallable()) continue;
        if (! s.layerName.empty())
            m_jsEngine->globalObject().setProperty("thisLayer", s.thisLayerProxy);
        m_jsEngine->globalObject().setProperty("thisObject", s.thisObjectProxy);
        callJsGuarded([&] {
            return s.mediaStatusChangedFn.call({ event });
        });
    }
    fireSceneEventListeners("mediaStatusChanged", { event });
}

void SceneObject::play() {
    m_scene->play();
    // Resume the GUI-thread script loops the matching pause() stopped (F19).
    // Restart only timers that were actually created (a scene with no
    // property/color/text scripts never allocates the timer) and that are not
    // already running, so play()-without-pause or double-play stays a no-op.
    m_paused = false;
    if (m_propertyTimer && ! m_propertyTimer->isActive()) m_propertyTimer->start();
    if (m_colorTimer && ! m_colorTimer->isActive()) m_colorTimer->start();
    if (m_textTimer && ! m_textTimer->isActive()) m_textTimer->start();
}
void SceneObject::pause() {
    m_scene->pause();
    // Stop the GUI-thread script loops while paused (TTY-switch / suspend /
    // battery / occlusion).  Without this, the property loop kept evaluating
    // every author script at ~125Hz and the color loop at ~30Hz, dispatching
    // updates into queues the paused render thread never drains — pure CPU /
    // battery waste with nothing on screen (F19).  The text loop self-throttles
    // on the render-frame index, but stop it too for symmetry / to save the
    // wakeup.  m_paused also gates the loop bodies (scriptLoopShouldRun) so the
    // setup-time seed eval and any already-queued tick idle.
    m_paused = true;
    if (m_propertyTimer) m_propertyTimer->stop();
    if (m_colorTimer) m_colorTimer->stop();
    if (m_textTimer) m_textTimer->stop();
}

bool SceneObject::vulkanValid() const { return m_enable_valid; }
void SceneObject::enableVulkanValid() { m_enable_valid = true; }
void SceneObject::enableGenGraphviz() { SET_PROPERTY(Bool, wallpaper::PROPERTY_GRAPHIVZ, true); }

// Build the CursorParallax arg for hitTestLayerProxy from the cached
// scene-level config + the last-known widget-normalised cursor position.
static CursorParallax buildCursorParallax(const SceneObject* obj, float mouseNx, float mouseNy,
                                          float orthoW, float orthoH, bool enable, float amount,
                                          float mouseInfluence, float camX, float camY) {
    (void)obj;
    CursorParallax p;
    p.enable         = enable;
    p.amount         = amount;
    p.mouseInfluence = mouseInfluence;
    p.camX           = camX;
    p.camY           = camY;
    p.mouseNx        = mouseNx;
    p.mouseNy        = mouseNy;
    p.orthoW         = orthoW;
    p.orthoH         = orthoH;
    return p;
}

void SceneObject::refreshParallaxCache() {
    if (! m_scene) return;
    auto info                      = m_scene->getParallaxInfo();
    m_parallaxCache.enable         = info.enable;
    m_parallaxCache.amount         = info.amount;
    m_parallaxCache.mouseInfluence = info.mouseInfluence;
    m_parallaxCache.camX           = info.camX;
    m_parallaxCache.camY           = info.camY;
    // The hit-test uses each layer's effective parallaxDepth (already set
    // to 0 by WPSceneParser for layers whose main-camera draw is a compose
    // node that doesn't inherit parallax).  So even when the scene enables
    // parallax globally, an un-shifted layer contributes no offset.
    LOG_INFO("parallax: enable=%d amount=%.3f mouseInfluence=%.3f cam=(%.1f,%.1f)",
             (int)info.enable,
             info.amount,
             info.mouseInfluence,
             info.camX,
             info.camY);
}

void SceneObject::setAcceptMouse(bool value) {
    if (value)
        setAcceptedMouseButtons(Qt::LeftButton);
    else
        setAcceptedMouseButtons(Qt::NoButton);
}

void SceneObject::setAcceptHover(bool value) { setAcceptHoverEvents(value); }

// Headless test hooks.  We construct Qt events with the requested widget-space
// coordinates and dispatch them through the same overrides a real Qt event
// pump would, so cursor script handlers + m_scene->mouseInput fire identically.
void SceneObject::simulateHoverAt(double x, double y) {
    QPointF pos(x, y);
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    QHoverEvent ev(QEvent::HoverMove, pos, pos, pos);
#else
    QHoverEvent ev(QEvent::HoverMove, pos.toPoint(), pos.toPoint());
#endif
    hoverMoveEvent(&ev);
}

void SceneObject::simulateClickAt(double x, double y) {
    simulateHoverAt(x, y); // make sure scripts see the hover state first
    QPointF pos(x, y);
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    QMouseEvent press(
        QEvent::MouseButtonPress, pos, pos, pos, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(
        QEvent::MouseButtonRelease, pos, pos, pos, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
#else
    QMouseEvent press(
        QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(
        QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
#endif
    mousePressEvent(&press);
    mouseReleaseEvent(&release);
}

void SceneObject::simulateDragAt(double x1, double y1, double x2, double y2) {
    simulateHoverAt(x1, y1);
    QPointF pos1(x1, y1);
    QPointF pos2(x2, y2);
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    QMouseEvent press(
        QEvent::MouseButtonPress, pos1, pos1, pos1, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(
        QEvent::MouseMove, pos2, pos2, pos2, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(
        QEvent::MouseButtonRelease, pos2, pos2, pos2, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
#else
    QMouseEvent press(
        QEvent::MouseButtonPress, pos1, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, pos2, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(
        QEvent::MouseButtonRelease, pos2, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
#endif
    mousePressEvent(&press);
    mouseMoveEvent(&move);
    mouseReleaseEvent(&release);
}

QString SceneObject::debugEvalJs(const QString& src) {
    if (! m_jsEngine) {
        LOG_INFO("debugEvalJs: js engine not ready yet — queuing for next tick");
        m_pendingJsEval = src;
        return QStringLiteral("(queued)");
    }
    QJSValue r = m_jsEngine->evaluate(src);
    return r.toString();
}

void SceneObject::requestScreenshot(const QString& path) {
    if (m_scene) m_scene->requestScreenshot(path.toStdString());
}

bool SceneObject::screenshotDone() const { return m_scene ? m_scene->screenshotDone() : false; }

void SceneObject::requestPassDump(const QString& dir) {
    if (m_scene) m_scene->requestPassDump(dir.toStdString());
}

bool SceneObject::passDumpDone() const { return m_scene ? m_scene->passDumpDone() : false; }

void SceneObject::setHidePattern(const QString& pattern) {
    if (m_scene) m_scene->setHidePattern(pattern.toStdString());
}

// Video texture bridge for thisLayer.getVideoTexture() — resolves a JS layer
// name to a node id via m_nodeNameToId, then delegates to SceneWallpaper.
// Returns safe defaults (0 / false / no-op) for unknown layers so scripts
// never throw even when run against a layer with no video texture.
double SceneObject::videoGetCurrentTime(const QString& layerName) const {
    if (! m_scene) return 0.0;
    auto it = m_nodeNameToId.find(layerName.toStdString());
    if (it == m_nodeNameToId.end()) return 0.0;
    return m_scene->videoGetCurrentTime(it->second);
}
double SceneObject::videoGetDuration(const QString& layerName) const {
    if (! m_scene) return 0.0;
    auto it = m_nodeNameToId.find(layerName.toStdString());
    if (it == m_nodeNameToId.end()) return 0.0;
    return m_scene->videoGetDuration(it->second);
}
bool SceneObject::videoIsPlaying(const QString& layerName) const {
    if (! m_scene) return false;
    auto it = m_nodeNameToId.find(layerName.toStdString());
    if (it == m_nodeNameToId.end()) return false;
    return m_scene->videoIsPlaying(it->second);
}
void SceneObject::videoPlay(const QString& layerName) {
    if (! m_scene) return;
    auto it = m_nodeNameToId.find(layerName.toStdString());
    if (it == m_nodeNameToId.end()) return;
    m_scene->videoPlay(it->second);
}
void SceneObject::videoPause(const QString& layerName) {
    if (! m_scene) return;
    auto it = m_nodeNameToId.find(layerName.toStdString());
    if (it == m_nodeNameToId.end()) return;
    m_scene->videoPause(it->second);
}
void SceneObject::videoStop(const QString& layerName) {
    if (! m_scene) return;
    auto it = m_nodeNameToId.find(layerName.toStdString());
    if (it == m_nodeNameToId.end()) return;
    m_scene->videoStop(it->second);
}
void SceneObject::videoSetCurrentTime(const QString& layerName, double t) {
    if (! m_scene) return;
    auto it = m_nodeNameToId.find(layerName.toStdString());
    if (it == m_nodeNameToId.end()) return;
    m_scene->videoSetCurrentTime(it->second, t);
}
void SceneObject::videoSetRate(const QString& layerName, double rate) {
    if (! m_scene) return;
    auto it = m_nodeNameToId.find(layerName.toStdString());
    if (it == m_nodeNameToId.end()) return;
    m_scene->videoSetRate(it->second, rate);
}

// Material uniform bridge — resolves layerName -> nodeId via m_nodeNameToId
// and forwards to SceneWallpaper, which enqueues the update for the render
// thread.  value is a plain JS array of numbers (the JS proxy guarantees
// shape; we only defensively skip empty / oversized arrays here and filter
// non-finite entries so a stray NaN never reaches the GPU).
void SceneObject::materialSetValue(const QString&  layerName,
                                   const QString&  name,
                                   const QJSValue& value) {
    if (! m_scene) return;
    if (name.isEmpty()) return;
    auto it = m_nodeNameToId.find(layerName.toStdString());
    if (it == m_nodeNameToId.end()) return;
    if (! value.isArray()) return;
    int n = value.property("length").toInt();
    if (n <= 0 || n > 16) return;
    std::vector<float> floats;
    floats.reserve(n);
    for (int i = 0; i < n; i++) {
        QJSValue elem = value.property(i);
        if (! elem.isNumber()) continue;
        double d = elem.toNumber();
        if (std::isfinite(d)) floats.push_back(static_cast<float>(d));
    }
    if (floats.empty()) return;
    m_scene->updateMaterialValue(it->second, name.toStdString(), std::move(floats));
}

// Effect-material bridge — same shape as materialSetValue but carries an
// effect index so the render thread can target the per-effect material.
// JS pre-validates numerics (same _packMaterialValue guards as materialSetValue);
// we still defensively skip empty/oversized arrays and non-finite entries.
void SceneObject::effectMaterialSetValue(const QString&  layerName,
                                         int             effectIdx,
                                         const QString&  name,
                                         const QJSValue& value) {
    if (! m_scene) return;
    if (name.isEmpty()) return;
    if (effectIdx < 0) return;
    auto it = m_nodeNameToId.find(layerName.toStdString());
    if (it == m_nodeNameToId.end()) return;
    if (! value.isArray()) return;
    int n = value.property("length").toInt();
    if (n <= 0 || n > 16) return;
    std::vector<float> floats;
    floats.reserve(n);
    for (int i = 0; i < n; i++) {
        QJSValue elem = value.property(i);
        if (! elem.isNumber()) continue;
        double d = elem.toNumber();
        if (std::isfinite(d)) floats.push_back(static_cast<float>(d));
    }
    if (floats.empty()) return;
    m_scene->updateEffectMaterialValue(
        it->second, effectIdx, name.toStdString(), std::move(floats));
}

// thisLayer.getTextureAnimation().setFrame(N) / .play() / .pause() bridge.
// Pins the sprite to frame N (manual mode) or restores auto-advance.  The
// pin is applied per-pass in WPShaderValueUpdater since each CustomShaderPass
// holds its own sprites_map copy.
void SceneObject::setLayerSpriteFrame(const QString& layerName, bool wantsManual, int frameIdx) {
    if (! m_scene) return;
    auto it = m_nodeNameToId.find(layerName.toStdString());
    if (it == m_nodeNameToId.end()) return;
    m_scene->setLayerSpriteFrame(it->second, wantsManual, frameIdx);
}

// thisLayer.getTextureAnimation() read-back — returns frame count, current
// frame, total duration (sum of frametimes, seconds), and the manual-pin
// state captured in Scene::nodeSpriteSnapshot by WPShaderValueUpdater on
// its most recent tick.  Empty object for unknown layer names; default-zero
// snapshot for layers without sprites — the JS proxy treats both as "no
// sprite", reporting frameCount:1, currentFrame:0, isPlaying:true so
// callers stay defensive against null layers without special-casing.
QJSValue SceneObject::getLayerSpriteInfo(const QString& layerName) const {
    QJSValue obj = m_jsEngine ? m_jsEngine->newObject() : QJSValue();
    if (! m_scene || ! m_jsEngine) return obj;
    auto it = m_nodeNameToId.find(layerName.toStdString());
    if (it == m_nodeNameToId.end()) return obj;
    auto snap = m_scene->getLayerSpriteSnapshot(it->second);
    obj.setProperty("frameCount", (int)snap.numFrames);
    obj.setProperty("currentFrame", (int)snap.currentFrame);
    obj.setProperty("duration", (double)snap.duration);
    obj.setProperty("isManualPin", snap.isManualPin);
    return obj;
}

// thisLayer.getBoneIndex(name) bridge — resolves the named MDAT attachment
// in the puppet rigged to this layer (or its parent's puppet for child-rigged
// layers).  Empty names short-circuit to 0; unknown layers and missing
// attachments also return 0.
int SceneObject::getBoneIndex(const QString& layerName, const QString& boneName) const {
    if (! m_scene) return 0;
    if (boneName.isEmpty()) return 0;
    auto it = m_nodeNameToId.find(layerName.toStdString());
    if (it == m_nodeNameToId.end()) return 0;
    return m_scene->getLayerBoneIndex(it->second, boneName.toStdString());
}

// World-transform readback for thisLayer.getTransformMatrix().  Returns the
// 16-float column-major matrix snapshotted at the end of the most recent
// drawFrame.  Identity for unknown layers.  Wrapped in a Mat4 instance JS-side
// so scripts get the same `.m[i]` accessor shape as the standalone Mat4().
QJSValue SceneObject::getLayerWorldTransform(const QString& layerName) const {
    QJSValue arr = m_jsEngine ? m_jsEngine->newArray(16) : QJSValue();
    if (! m_scene || ! m_jsEngine) return arr;
    auto it = m_nodeNameToId.find(layerName.toStdString());
    int32_t nodeId = (it == m_nodeNameToId.end()) ? -1 : it->second;
    auto m = m_scene->getLayerWorldMatrix(nodeId);
    for (int i = 0; i < 16; ++i) arr.setProperty(i, m[i]);
    return arr;
}

// Text-style bridge — thisLayer.{horizontalalign,verticalalign,alignment,font}.
// JS shim parses `alignment` into h+v before calling, so we only carry two
// axis strings + a font name.  Empty strings = no change.  Font resolution
// (name → VFS bytes) happens render-side so the VFS-owning thread does I/O.
void SceneObject::setTextStyle(const QString& layerName,
                               const QString& halign,
                               const QString& valign,
                               const QString& fontName) {
    if (! m_scene) return;
    auto it = m_nodeNameToId.find(layerName.toStdString());
    if (it == m_nodeNameToId.end()) return;
    m_scene->updateTextStyle(it->second,
                             halign.toStdString(),
                             valign.toStdString(),
                             fontName.toStdString());
}

// Layer-hierarchy bridge — enqueues (childId, parentId) for the render
// thread to apply at the start of CMD_DRAW.  parentId == -1 means
// "reattach to scene root".  Unknown ids are dropped silently during
// drain (see Scene::ApplyPendingParentChanges).
void SceneObject::setLayerParent(int childId, int parentId) {
    if (! m_scene) return;
    m_scene->queueParentChange(childId, parentId);
}

// thisScene.sortLayer(layer, index) bridge — reorders the child within
// its current parent's children list.  targetIndex clamps to
// [0, parent->children.size()-1].  Drained at CMD_DRAW alongside the
// parent-change queue (see Scene::ApplyPendingChildSorts).
void SceneObject::sortLayer(int childId, int targetIndex) {
    if (! m_scene) return;
    m_scene->queueChildSort(childId, targetIndex);
}

// -------- engine.openUserShortcut bridge --------
// Routes a named user-shortcut from SceneScript to (a) the main plugin via
// the userShortcutRequested signal (mapped to MPRIS by Scene.qml) and
// (b) the in-scene event bus via scene.on('userShortcut', ...).  Logs at
// INFO so unmapped names surface in the journal — consistent with
// feedback_no_stubs ("implement it or surface it loudly").
void SceneObject::openUserShortcut(const QString& name) {
    if (name.isEmpty()) return;
    LOG_INFO("engine.openUserShortcut('%s') dispatched", qPrintable(name));
    Q_EMIT userShortcutRequested(name);
    if (m_jsEngine) {
        QJSValue evt = m_jsEngine->newObject();
        evt.setProperty("name", name);
        fireSceneEventListeners("userShortcut", { evt });
    }
}

// -------- localStorage bridge --------
// Disk-backed persistence for the SceneScript `localStorage` API.  Keeps two
// in-memory QJsonObjects mirrored to files under the cache dir:
//   - LOCATION_GLOBAL → <cache>/localstorage_global.json
//   - LOCATION_SCREEN → <cache>/<sceneId>/localstorage.json
// Writes debounce-coalesce through m_lsFlushTimer; reads always hit the cache.
// Scene id is derived from the source URL's parent directory name (workshop
// id like "3662790108" or a defaultproject folder name like "dino_run").

QString SceneObject::localStoragePath(bool global) const {
    QString base = QString::fromStdString(GetDefaultCachePath());
    if (global) return base + "/localstorage_global.json";
    if (m_lsSceneId.isEmpty()) return {};
    return base + "/" + m_lsSceneId + "/localstorage.json";
}

void SceneObject::ensureLocalStorageLoaded() {
    if (m_lsLoaded) return;
    m_lsLoaded = true;

    // Resolve scene id from source URL.  QUrl's fileName() is the basename;
    // we want the parent directory name (the workshop id or project folder).
    if (m_source.isValid()) {
        QString localPath = m_source.toLocalFile();
        if (! localPath.isEmpty()) {
            QFileInfo fi(localPath);
            m_lsSceneId = fi.dir().dirName();
        }
    }

    auto loadFile = [](const QString& path) -> QJsonObject {
        if (path.isEmpty()) return {};
        QFile f(path);
        if (! f.open(QIODevice::ReadOnly)) return {};
        QJsonParseError err {};
        QJsonDocument   doc = QJsonDocument::fromJson(f.readAll(), &err);
        if (err.error != QJsonParseError::NoError || ! doc.isObject()) return {};
        return doc.object();
    };
    m_lsGlobal = loadFile(localStoragePath(true));
    m_lsScreen = loadFile(localStoragePath(false));

    if (! m_lsFlushTimer) {
        m_lsFlushTimer = new QTimer(this);
        m_lsFlushTimer->setSingleShot(true);
        m_lsFlushTimer->setInterval(500);
        connect(m_lsFlushTimer, &QTimer::timeout, this, &SceneObject::flushLocalStorage);
    }
}

void SceneObject::scheduleLocalStorageFlush() {
    if (m_lsFlushTimer && ! m_lsFlushTimer->isActive()) m_lsFlushTimer->start();
}

void SceneObject::flushLocalStorage() {
    auto writeAtomic = [](const QString& path, const QJsonObject& obj) {
        if (path.isEmpty()) return;
        QDir().mkpath(QFileInfo(path).absolutePath());
        QString tmpPath = path + ".tmp";
        QFile   tmp(tmpPath);
        if (! tmp.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
        tmp.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        tmp.close();
        // Atomic rename: on POSIX rename(2) replaces the target in one step,
        // so readers never see a partial file.
        QFile::remove(path);
        QFile::rename(tmpPath, path);
    };
    if (m_lsGlobalDirty) {
        writeAtomic(localStoragePath(true), m_lsGlobal);
        m_lsGlobalDirty = false;
    }
    if (m_lsScreenDirty) {
        writeAtomic(localStoragePath(false), m_lsScreen);
        m_lsScreenDirty = false;
    }
}

QJSValue SceneObject::lsGet(int loc, const QString& key) {
    if (! m_jsEngine) return QJSValue(QJSValue::UndefinedValue);
    ensureLocalStorageLoaded();
    const QJsonObject& store = (loc == 0) ? m_lsGlobal : m_lsScreen;
    if (! store.contains(key)) return QJSValue(QJSValue::UndefinedValue);
    // Round-trip through QVariant so numbers, strings, arrays, and nested
    // objects all turn into the matching QJSValue shape.
    return m_jsEngine->toScriptValue(store.value(key).toVariant());
}

void SceneObject::lsSet(int loc, const QString& key, const QJSValue& value) {
    ensureLocalStorageLoaded();
    QJsonValue jv = QJsonValue::fromVariant(value.toVariant());
    if (loc == 0) {
        m_lsGlobal.insert(key, jv);
        m_lsGlobalDirty = true;
    } else {
        m_lsScreen.insert(key, jv);
        m_lsScreenDirty = true;
    }
    scheduleLocalStorageFlush();
}

void SceneObject::lsRemove(int loc, const QString& key) {
    ensureLocalStorageLoaded();
    if (loc == 0) {
        if (m_lsGlobal.contains(key)) {
            m_lsGlobal.remove(key);
            m_lsGlobalDirty = true;
            scheduleLocalStorageFlush();
        }
    } else {
        if (m_lsScreen.contains(key)) {
            m_lsScreen.remove(key);
            m_lsScreenDirty = true;
            scheduleLocalStorageFlush();
        }
    }
}

void SceneObject::lsClear(int loc) {
    ensureLocalStorageLoaded();
    if (loc == 0) {
        if (! m_lsGlobal.isEmpty()) {
            m_lsGlobal      = {};
            m_lsGlobalDirty = true;
            scheduleLocalStorageFlush();
        }
    } else {
        if (! m_lsScreen.isEmpty()) {
            m_lsScreen      = {};
            m_lsScreenDirty = true;
            scheduleLocalStorageFlush();
        }
    }
}

// Helper: strip ES module syntax that QJSEngine doesn't support
static void stripESModuleSyntax(QString& src) {
    // Normalize non-breaking spaces (U+00A0) to regular spaces — some WE scripts
    // (e.g. workshop/3502639183) use NBSP between keywords
    src.replace(QChar(0x00A0), QChar(' '));

    // Strip 'use strict'; — it's inside our IIFE anyway
    src.replace(
        QRegularExpression("^\\s*['\"]use strict['\"];?\\s*", QRegularExpression::MultilineOption),
        "");
    // Strip import statements
    src.replace(QRegularExpression("^\\s*import\\s+.*?from\\s+['\"].*?['\"];?\\s*$",
                                   QRegularExpression::MultilineOption),
                "");
    // Strip 'export default ' before function/class/expression
    src.replace(QRegularExpression("\\bexport\\s+default\\s+"), "");
    // Strip 'export' before declarations
    src.replace(QRegularExpression("\\bexport\\s+function\\b"), "function");
    src.replace(QRegularExpression("\\bexport\\s+class\\b"), "class");
    src.replace(QRegularExpression("\\bexport\\s+var\\b"), "var");
    src.replace(QRegularExpression("\\bexport\\s+let\\b"), "let");
    src.replace(QRegularExpression("\\bexport\\s+const\\b"), "const");
    // Strip 'export { ... };' re-export blocks
    src.replace(QRegularExpression("^\\s*export\\s*\\{[^}]*\\};?\\s*$",
                                   QRegularExpression::MultilineOption),
                "");
    // Catch-all: strip any remaining 'export' at start of statement
    // (covers edge cases like 'export default;' or unknown forms)
    src.replace(QRegularExpression("^(\\s*)\\bexport\\s+", QRegularExpression::MultilineOption),
                "\\1");

    // Rewrite ES2019 optional catch bindings (`catch {`) — QJSEngine/V4 rejects
    // them with a SyntaxError that fails the whole script compile.  See
    // JsSyntaxNormalize.hpp.
    wek::qml_helper::normalizeOptionalCatchBinding(src);
}

// hitTestLayerProxy lives in SceneCursorHitTest.h so scenescript_tests can
// exercise the same geometry without pulling in Qt Quick.

// Helper: build cursor event argument with worldPosition as Vec3
static QJSValue makeCursorEvent(QJSEngine* engine, float sceneX, float sceneY) {
    QJSValue ev = engine->newObject();
    ev.setProperty("x", (double)sceneX);
    ev.setProperty("y", (double)sceneY);
    // worldPosition as Vec3 (for drag scripts that use .add()/.subtract())
    QJSValue wp =
        engine->evaluate(QString("new Vec3(%1,%2,0)").arg((double)sceneX).arg((double)sceneY));
    ev.setProperty("worldPosition", wp);
    return ev;
}

// Helper: flush JS console.log buffer
static void flushJsConsole(QJSEngine* engine, const char* ctx) {
    QJSValue consoleBuf = engine->globalObject().property("console").property("_buf");
    if (consoleBuf.isArray()) {
        int len = consoleBuf.property("length").toInt();
        for (int b = 0; b < len; b++) {
            LOG_INFO("JS %s console.log: %s", ctx, qPrintable(consoleBuf.property(b).toString()));
        }
        if (len > 0) engine->evaluate("console._buf = [];");
    }
}

void SceneObject::mousePressEvent(QMouseEvent* event) {
    if (m_cursorTargets.empty() || ! m_jsEngine) return;

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    auto pos = event->position();
#else
    auto pos = event->localPos();
#endif
    m_mouseNx    = (float)(pos.x() / width());
    m_mouseNy    = (float)(pos.y() / height());
    float sceneX = m_mouseNx * m_sceneOrthoW;
    // Qt window Y is top-down; WE scene coords are Y-up (layer origins from
    // scene.json grow upward from the bottom).  Flip so cursor tracking and
    // AABB hit-testing both match layer origin convention.
    float sceneY = (1.0f - m_mouseNy) * m_sceneOrthoH;

    // Hit-test to pick the target layer.  All four press-time cursor events
    // (cursorDown, cursorClick, cursorMove on drag, cursorUp) go to this
    // single layer — that matches standard UI semantics AND is what WE
    // scripts assume: 2866203962 playervolume.cursorDown unconditionally
    // sets percentageLayer.text="100%", so a global fan-out meant clicking
    // ANYWHERE on the wallpaper flashed "100%" on the volume slider.
    //
    // Among overlapping hit layers, pick independently for click vs. drag:
    // clickIdx → smallest hit with clickFn (so a tiny button beats a
    // fullscreen toggle-background like 2866203962 bg0), moveIdx → smallest
    // hit with moveFn.  downFn / upFn ride on the selected drag target,
    // which prefers moveIdx (drag scripts) and falls back to clickIdx.
    CursorParallax para     = buildCursorParallax(this,
                                              m_mouseNx,
                                              m_mouseNy,
                                              m_sceneOrthoW,
                                              m_sceneOrthoH,
                                              m_parallaxCache.enable,
                                              m_parallaxCache.amount,
                                              m_parallaxCache.mouseInfluence,
                                              m_parallaxCache.camX,
                                              m_parallaxCache.camY);
    QJSValue       ev       = makeCursorEvent(m_jsEngine, sceneX, sceneY);
    int            clickIdx = -1, moveIdx = -1, downIdx = -1;
    double         clickArea = 0.0, moveArea = 0.0, downArea = 0.0;
    for (int i = 0; i < (int)m_cursorTargets.size(); i++) {
        auto& target = m_cursorTargets[i];
        if (! hitTestLayerProxy(target.thisLayerProxy, sceneX, sceneY, para)) continue;
        QJSValue state = target.thisLayerProxy.property("_state");
        QJSValue size  = state.property("size");
        QJSValue scale = state.property("scale");
        double   sw    = size.property("x").toNumber();
        double   sh    = size.property("y").toNumber();
        double   sx    = std::abs(scale.property("x").toNumber());
        double   sy    = std::abs(scale.property("y").toNumber());
        double   area  = sw * sx * sh * sy;
        LOG_INFO("  hit[%d] '%s' area=%.0f handlers=%s%s%s",
                 i,
                 target.layerName.c_str(),
                 area,
                 target.clickFn.isCallable() ? "click " : "",
                 target.moveFn.isCallable() ? "move " : "",
                 target.downFn.isCallable() ? "down " : "");
        if (target.clickFn.isCallable() && (clickIdx < 0 || area < clickArea)) {
            clickIdx  = i;
            clickArea = area;
        }
        if (target.moveFn.isCallable() && (moveIdx < 0 || area < moveArea)) {
            moveIdx  = i;
            moveArea = area;
        }
        if (target.downFn.isCallable() && (downIdx < 0 || area < downArea)) {
            downIdx  = i;
            downArea = area;
        }
    }
    // Fire cursorDown on every hit layer with downFn (WE convention — each
    // geometrically-hit layer receives its own cursorDown).  Game of Life
    // (3453251764) Canvas and Pattern Texture are both fullscreen with
    // cursorDown handlers — Canvas's tracks the press state for the cell-
    // paint shader; Pattern Texture's resets the motion-blur accumulation.
    // Without fan-out, smallest-area selection ties on size and the first
    // entry wins, starving the other.  cursorClick stays smallest-hit so a
    // tiny button still beats a fullscreen toggle backdrop.
    int downFired = 0;
    if (downIdx >= 0) {
        for (int i = 0; i < (int)m_cursorTargets.size(); i++) {
            auto& target = m_cursorTargets[i];
            if (! target.downFn.isCallable()) continue;
            if (! hitTestLayerProxy(target.thisLayerProxy, sceneX, sceneY, para)) continue;
            m_jsEngine->globalObject().setProperty("thisLayer", target.thisLayerProxy);
            m_jsEngine->globalObject().setProperty("thisObject", target.thisObjectProxy);
            QJSValue r = callJsGuarded([&] {
                return target.downFn.call({ ev });
            });
            downFired++;
            if (r.isError())
                LOG_INFO("cursorDown error on '%s': %s",
                         target.layerName.c_str(), qPrintable(r.toString()));
        }
    } else if (clickIdx < 0 && moveIdx < 0) {
        // Nothing hit-tested.  Fall back to global cursorDown fan-out for
        // "tap anywhere" game-control scripts (built-in dino_run's walking
        // sprite is a moving 24×24 hitbox — requiring precise clicks would
        // break its tap-to-jump).  Only engaged when no click / drag target
        // is available, so well-defined UI wallpapers (2866203962) aren't
        // affected.
        for (auto& target : m_cursorTargets) {
            if (! target.downFn.isCallable()) continue;
            m_jsEngine->globalObject().setProperty("thisLayer", target.thisLayerProxy);
            m_jsEngine->globalObject().setProperty("thisObject", target.thisObjectProxy);
            m_jsEngine->globalObject().setProperty("thisObject", target.thisObjectProxy);
            QJSValue r = callJsGuarded([&] {
                return target.downFn.call({ ev });
            });
            downFired++;
            if (r.isError())
                LOG_INFO("cursorDown error on '%s': %s",
                         target.layerName.c_str(),
                         qPrintable(r.toString()));
        }
    }
    // Fire cursorClick on the smallest-hit layer with clickFn.
    if (clickIdx >= 0) {
        auto& target = m_cursorTargets[clickIdx];
        m_jsEngine->globalObject().setProperty("thisLayer", target.thisLayerProxy);
        m_jsEngine->globalObject().setProperty("thisObject", target.thisObjectProxy);
        QJSValue r = callJsGuarded([&] {
            return target.clickFn.call({ ev });
        });
        LOG_INFO(
            "cursorClick fired on '%s'%s", target.layerName.c_str(), r.isError() ? " ERROR" : "");
        if (r.isError()) LOG_INFO("  cursorClick error: %s", qPrintable(r.toString()));
    }
    // Drag target: prefer the smallest-hit layer with moveFn, fall back to
    // downFn (so cursorUp lands on the same layer that saw cursorDown),
    // else clickFn.  Saved so mouseMoveEvent can route subsequent
    // cursorMove dispatches.
    if (moveIdx >= 0)
        m_dragTarget = m_cursorTargets[moveIdx].layerName;
    else if (downIdx >= 0)
        m_dragTarget = m_cursorTargets[downIdx].layerName;
    else if (clickIdx >= 0)
        m_dragTarget = m_cursorTargets[clickIdx].layerName;
    LOG_INFO("press at scene=(%.1f,%.1f): cursorDown fired=%d, drag-target='%s'",
             sceneX,
             sceneY,
             downFired,
             m_dragTarget.empty() ? "(none)" : m_dragTarget.c_str());
    flushJsConsole(m_jsEngine, "click");
}

void SceneObject::mouseReleaseEvent(QMouseEvent* event) {
    if (m_cursorTargets.empty() || ! m_jsEngine) {
        m_dragTarget.clear();
        return;
    }

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    auto pos = event->position();
#else
    auto pos = event->localPos();
#endif
    float sceneX = (float)(pos.x() / width()) * m_sceneOrthoW;
    // Qt window Y is top-down; WE scene coords are Y-up (layer origins from
    // scene.json grow upward from the bottom).  Flip so cursor tracking and
    // AABB hit-testing both match layer origin convention.
    float sceneY = (float)(1.0 - pos.y() / height()) * m_sceneOrthoH;

    // cursorUp dispatch priority:
    //   1. m_dragTarget (set at press from cursorDown/Move/Click handlers)
    //   2. Smallest layer whose hit-test contains the release point
    //      (covers buttons that only expose cursorUp — the Game of Life
    //      palette has 44 such buttons; without this Lock's unconditional
    //      `cursorUp` toggle ran on every release regardless of where the
    //      user actually released, and the other buttons' isVisible-gated
    //      handlers fan-fired in arbitrary order, with the last setter
    //      winning shared.currentBrush.)
    //   3. Fan-out fallback for true "tap anywhere" patterns where no
    //      cursor target was hit (dino_run-style controls).
    QJSValue ev      = makeCursorEvent(m_jsEngine, sceneX, sceneY);
    int      upFired = 0;
    CursorParallax para = buildCursorParallax(this,
                                              m_mouseNx,
                                              m_mouseNy,
                                              m_sceneOrthoW,
                                              m_sceneOrthoH,
                                              m_parallaxCache.enable,
                                              m_parallaxCache.amount,
                                              m_parallaxCache.mouseInfluence,
                                              m_parallaxCache.camX,
                                              m_parallaxCache.camY);
    auto fire = [&](CursorTarget& target) {
        if (! target.upFn.isCallable()) return;
        m_jsEngine->globalObject().setProperty("thisLayer", target.thisLayerProxy);
        m_jsEngine->globalObject().setProperty("thisObject", target.thisObjectProxy);
        QJSValue r = callJsGuarded([&] {
            return target.upFn.call({ ev });
        });
        upFired++;
        if (r.isError())
            LOG_INFO("cursorUp error on '%s': %s",
                     target.layerName.c_str(),
                     qPrintable(r.toString()));
    };
    // cursorUp fans out to every cursor target the cursor is currently over
    // (WE convention — each geometrically-hit layer receives its own
    // cursorUp).  Lock-toggle leakage from the earlier fan-out-to-all bug
    // is now blocked at the hit-test step: Lock at top-right is only fired
    // when the cursor was actually released over Lock.  Drag target is
    // always included (releases its press state even if it's no longer
    // under the cursor — the user might have dragged off).
    {
        bool firedDragTarget = false;
        for (auto& t : m_cursorTargets) {
            if (! t.upFn.isCallable()) continue;
            bool isDrag = ! m_dragTarget.empty() && t.layerName == m_dragTarget;
            bool isHit  = hitTestLayerProxy(t.thisLayerProxy, sceneX, sceneY, para);
            if (! isDrag && ! isHit) continue;
            if (isDrag) firedDragTarget = true;
            fire(t);
        }
        // Fallback: if no drag target and no hits, fan out to mirror what
        // the legacy "tap anywhere" cursorDown fan-out did (game-control
        // state machines that rely on every release firing).
        if (m_dragTarget.empty() && upFired == 0) {
            for (auto& t : m_cursorTargets) fire(t);
        }
        (void)firedDragTarget;
    }
    LOG_INFO("release at scene=(%.1f,%.1f): cursorUp fired=%d, drag-target was '%s'",
             sceneX,
             sceneY,
             upFired,
             m_dragTarget.empty() ? "(none)" : m_dragTarget.c_str());
    flushJsConsole(m_jsEngine, "mouseUp");
    m_dragTarget.clear();
}

void SceneObject::mouseMoveEvent(QMouseEvent* event) {
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    auto pos = event->position();
#else
    auto pos = event->localPos();
#endif
    m_scene->mouseInput(pos.x() / width(), pos.y() / height());

    // Track cursor position for input.cursorWorldPosition in scripts.
    // Qt Y is top-down; WE scene coords are Y-up — flip.
    m_mouseNx      = (float)(pos.x() / width());
    m_mouseNy      = (float)(pos.y() / height());
    m_cursorSceneX = m_mouseNx * m_sceneOrthoW;
    m_cursorSceneY = (1.0f - m_mouseNy) * m_sceneOrthoH;

    // cursorMove on drag target.  The target is selected once on press via
    // hit-test; subsequent moves only reach that layer's moveFn (WE semantics
    // — the rest of the scene shouldn't see drag motions).
    if (! m_dragTarget.empty() && m_jsEngine) {
        float sceneX = m_cursorSceneX;
        float sceneY = m_cursorSceneY;
        for (auto& target : m_cursorTargets) {
            if (target.layerName != m_dragTarget || ! target.moveFn.isCallable()) continue;
            m_jsEngine->globalObject().setProperty("thisLayer", target.thisLayerProxy);
            m_jsEngine->globalObject().setProperty("thisObject", target.thisObjectProxy);
            m_jsEngine->globalObject().setProperty("thisObject", target.thisObjectProxy);
            QJSValue ev = makeCursorEvent(m_jsEngine, sceneX, sceneY);
            QJSValue r  = callJsGuarded([&] {
                return target.moveFn.call({ ev });
            });
            // Throttle per-frame to avoid flooding when a real user drags.
            // Sample once every ~30 dispatches so drag tests still surface
            // the first few events without drowning out the rest of the log.
            static int s_moveLog = 0;
            if ((s_moveLog++ % 30) == 0) {
                LOG_INFO("cursorMove '%s' scene=(%.1f,%.1f)%s",
                         target.layerName.c_str(),
                         sceneX,
                         sceneY,
                         r.isError() ? " ERROR" : "");
            }
            if (r.isError()) LOG_INFO("  cursorMove error detail: %s", qPrintable(r.toString()));
            break;
        }
    }
}

void SceneObject::hoverMoveEvent(QHoverEvent* event) {
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    auto pos = event->position();
#else
    auto pos = event->posF();
#endif
    m_scene->mouseInput(pos.x() / width(), pos.y() / height());

    // Track cursor position for input.cursorWorldPosition in scripts.
    // Qt Y is top-down; WE scene coords are Y-up — flip.
    m_mouseNx      = (float)(pos.x() / width());
    m_mouseNy      = (float)(pos.y() / height());
    m_cursorSceneX = m_mouseNx * m_sceneOrthoW;
    m_cursorSceneY = (1.0f - m_mouseNy) * m_sceneOrthoH;

    // Sample log — once per ~4s — to confirm hover events reach the scene.
    // If this line is missing entirely from journals, the MouseGrabber isn't
    // forwarding hover events (e.g. target not set, hookParent filtering).
    {
        static int s_hover_log = 0;
        if (++s_hover_log % 120 == 1) {
            LOG_INFO("hover: widget=(%.1f,%.1f) scene=(%.1f,%.1f) ortho=(%d,%d)",
                     pos.x(),
                     pos.y(),
                     m_cursorSceneX,
                     m_cursorSceneY,
                     (int)m_sceneOrthoW,
                     (int)m_sceneOrthoH);
        }
    }

    // cursorEnter / cursorLeave hit-testing
    if (m_cursorTargets.empty() || ! m_jsEngine) return;
    float          sceneX = m_cursorSceneX;
    float          sceneY = m_cursorSceneY;
    CursorParallax para   = buildCursorParallax(this,
                                              m_mouseNx,
                                              m_mouseNy,
                                              m_sceneOrthoW,
                                              m_sceneOrthoH,
                                              m_parallaxCache.enable,
                                              m_parallaxCache.amount,
                                              m_parallaxCache.mouseInfluence,
                                              m_parallaxCache.camX,
                                              m_parallaxCache.camY);

    // Hover-leave debounce: see HoverLeaveDebounce.h for the state machine.
    // kHoverLeaveGraceMs is the grace window between the cursor leaving a
    // layer and cursorLeave actually firing — long enough that
    // proximity-driven UI (2866203962 music-player fade) stays visible
    // when the user grazes the edge on their way to click a button.
    constexpr int64_t kHoverLeaveGraceMs = 400;
    qint64            nowMs              = QDateTime::currentMSecsSinceEpoch();

    // Build the set of layers the cursor is currently over.
    std::unordered_set<std::string> currentHit;
    for (auto& target : m_cursorTargets) {
        if (! target.enterFn.isCallable() && ! target.leaveFn.isCallable()) continue;
        if (hitTestLayerProxy(target.thisLayerProxy, sceneX, sceneY, para)) {
            currentHit.insert(target.layerName);
        }
    }

    auto result =
        processHoverFrame(m_hoveredLayers, currentHit, m_pendingLeaves, nowMs, kHoverLeaveGraceMs);

    // Fire cursorEnter on freshly entered layers.
    for (const auto& name : result.toEnter) {
        for (auto& target : m_cursorTargets) {
            if (target.layerName != name || ! target.enterFn.isCallable()) continue;
            LOG_INFO("cursorEnter: layer '%s' at scene=(%.1f,%.1f)", name.c_str(), sceneX, sceneY);
            m_jsEngine->globalObject().setProperty("thisLayer", target.thisLayerProxy);
            m_jsEngine->globalObject().setProperty("thisObject", target.thisObjectProxy);
            m_jsEngine->globalObject().setProperty("thisObject", target.thisObjectProxy);
            QJSValue ev = makeCursorEvent(m_jsEngine, sceneX, sceneY);
            QJSValue r  = callJsGuarded([&] {
                return target.enterFn.call({ ev });
            });
            if (r.isError()) LOG_INFO("cursorEnter ERROR: %s", r.toString().toStdString().c_str());
            break;
        }
    }
    if (! result.toEnter.empty()) flushJsConsole(m_jsEngine, "cursorEnter");

    bool stateChanged = (result.newHovered != m_hoveredLayers);
    m_hoveredLayers   = std::move(result.newHovered);
    if (stateChanged) flushJsConsole(m_jsEngine, "hover");

    // Arm the debounce timer so expired leaves actually fire.
    if (! m_pendingLeaves.empty()) {
        if (! m_hoverLeaveTimer) {
            m_hoverLeaveTimer = new QTimer(this);
            m_hoverLeaveTimer->setSingleShot(true);
            QObject::connect(
                m_hoverLeaveTimer, &QTimer::timeout, this, &SceneObject::flushPendingLeaves);
        }
        if (! m_hoverLeaveTimer->isActive()) {
            int64_t next   = nextLeaveDeadlineMs(m_pendingLeaves);
            int64_t remain = next > nowMs ? next - nowMs : 0;
            m_hoverLeaveTimer->start((int)remain + 10);
        }
    }
}

void SceneObject::flushPendingLeaves() {
    if (! m_jsEngine) return;
    qint64 nowMs  = QDateTime::currentMSecsSinceEpoch();
    auto   toFire = drainExpiredLeaves(m_pendingLeaves, (int64_t)nowMs);
    for (const auto& name : toFire) {
        for (auto& target : m_cursorTargets) {
            if (target.layerName != name) continue;
            if (target.leaveFn.isCallable()) {
                LOG_INFO("cursorLeave: layer '%s' (after grace)", target.layerName.c_str());
                m_jsEngine->globalObject().setProperty("thisLayer", target.thisLayerProxy);
                m_jsEngine->globalObject().setProperty("thisObject", target.thisObjectProxy);
                m_jsEngine->globalObject().setProperty("thisObject", target.thisObjectProxy);
                m_jsEngine->globalObject().setProperty("thisObject", target.thisObjectProxy);
                QJSValue ev = makeCursorEvent(m_jsEngine, m_cursorSceneX, m_cursorSceneY);
                callJsGuarded([&] {
                    return target.leaveFn.call({ ev });
                });
            }
            m_hoveredLayers.erase(name);
            break;
        }
    }
    if (! toFire.empty()) flushJsConsole(m_jsEngine, "hover-leave");
    if (! m_pendingLeaves.empty() && m_hoverLeaveTimer) {
        int64_t next   = nextLeaveDeadlineMs(m_pendingLeaves);
        int64_t remain = next > nowMs ? next - nowMs : 0;
        m_hoverLeaveTimer->start((int)remain + 10);
    }
}

std::string SceneObject::GetDefaultCachePath() {
    return wallpaper::platform::GetCachePath(CACHE_DIR);
}

void SceneObject::setupTextScripts() {
    cleanupTextScripts();

    // Reaching here means the render thread emitted firstFrame (it has drawn),
    // so the scene is live — clear any stale paused gate from a prior wallpaper
    // that was paused at switch time, or the freshly created+started script
    // timers below would tick into a body that scriptLoopShouldRun() idles
    // (F19).  pause()/play() drive m_paused for the live-occlusion case.
    m_paused = false;

    // Publish the scene's ortho size + nativeAspectRatio for EVERY loaded scene,
    // BEFORE the no-scripts early-return below. This used to live near the end of
    // setupTextScripts, after that return — so script-less wallpapers (the
    // majority of simple scenes) never set m_sceneOrthoLoaded, nativeAspectRatio
    // stayed 0, and Scene.qml's Keep-Aspect letterbox never engaged: the renderer
    // filled the screen and painted opaque (black) bars, glaring on
    // aspect-mismatched / ultrawide displays. Also feeds cursorClick hit-testing,
    // which now gets the real ortho sooner. DO NOT move this back below the return.
    {
        auto orthoSize     = m_scene->getOrthoSize();
        m_sceneOrthoW      = (float)orthoSize[0];
        m_sceneOrthoH      = (float)orthoSize[1];
        m_sceneOrthoLoaded = true;
        Q_EMIT nativeAspectRatioChanged();
    }

    auto scripts            = m_scene->getTextScripts();
    auto colorScripts       = m_scene->getColorScripts();
    auto propertyScripts    = m_scene->getPropertyScripts();
    auto soundLayerControls = m_scene->getSoundLayerControls();
    LOG_INFO("setupTextScripts: text=%zu color=%zu property=%zu soundLayers=%zu",
             scripts.size(),
             colorScripts.size(),
             propertyScripts.size(),
             soundLayerControls.size());
    if (scripts.empty() && colorScripts.empty() && propertyScripts.empty() &&
        soundLayerControls.empty())
        return;

    m_jsEngine = new QJSEngine(this);

    // Start the runaway-script watchdog now that the engine exists.  It stays
    // disarmed until a JS dispatch site arms it; the monitor thread is joined in
    // cleanupTextScripts() BEFORE m_jsEngine is deleted (A3-T2).
    m_jsWatchdog.start();
    // A reload via setSource() re-runs setupTextScripts on the same SceneObject:
    // reset the interrupt back-off so a fresh scene is not pre-disabled by a
    // previous wallpaper's runaway.
    m_consecutivePropInterrupts  = 0;
    m_propertyScriptsDisabled    = false;
    m_consecutiveTextInterrupts  = 0;
    m_textScriptsDisabled        = false;
    m_consecutiveColorInterrupts = 0;
    m_colorScriptsDisabled       = false;

    // Expose SceneObject to JS as __sceneBridge so layer proxies can call its
    // Q_INVOKABLE methods (videoXxx, ...).  Parent is `this`, lifetime is tied
    // to the QJSEngine which we own; newQObject wraps without taking ownership
    // (QJSEngine::ObjectOwnership::CppOwnership by default for child QObjects).
    m_jsEngine->globalObject().setProperty("__sceneBridge", m_jsEngine->newQObject(this));

    // Provide a minimal 'engine' global with runtime and timeOfDay
    m_runtimeTimer.start();
    QJSValue engineObj = m_jsEngine->newObject();
    engineObj.setProperty("frametime", 0.016); // overwritten per tick
    engineObj.setProperty("runtime", 0.0);
    engineObj.setProperty("timeOfDay", 0.0);
    // Real render-thread FPS measured wall-clock by FpsCounter (rolling 500ms
    // window).  0 until the first frame has been drawn.  Scripts wanting an
    // accurate FPS readout should read this directly rather than measuring
    // their own invocation cadence (which can drift from the render rate).
    engineObj.setProperty("fps", 0.0);
    // Monotonic tick counter (incremented by evaluatePropertyScripts — see
    // m_propFrameCount).  Lets scripts branch on "first tick", count
    // intervals without accumulating runtime drift, etc.
    engineObj.setProperty("frameCount", 0.0);
    // Timezone info.  `timeZone` is UTC offset in minutes (positive east of
    // UTC); `timeZoneName` is the IANA zone id when available, falling back
    // to the abbreviation.  Lets clock/date wallpapers render in the user's
    // locale without the script doing its own IANA lookup.
    {
        QTimeZone tz        = QTimeZone::systemTimeZone();
        int       offsetMin = tz.offsetFromUtc(QDateTime::currentDateTime()) / 60;
        QString   id        = tz.id().isEmpty() ? tz.abbreviation(QDateTime::currentDateTime())
                                                : QString::fromUtf8(tz.id());
        engineObj.setProperty("timeZone", (double)offsetMin);
        engineObj.setProperty("timeZoneName", id);
    }
    engineObj.setProperty("userProperties", m_jsEngine->newObject());
    m_jsEngine->globalObject().setProperty("engine", engineObj);

    // Provide the 'shared' global for inter-script data sharing.
    // All scripts in the scene can read/write to this object.
    m_jsEngine->globalObject().setProperty("shared", m_jsEngine->newObject());
    // Default shared values for common script patterns
    m_jsEngine->evaluate("shared.volume = 1.0;\n");

    // Provide a 'console' object that forwards to C++ logging
    m_jsEngine->evaluate("var console = {\n"
                         "  log: function() {\n"
                         "    var args = Array.prototype.slice.call(arguments);\n"
                         "    var msg = args.map(function(a){ return String(a); }).join(' ');\n"
                         "    if (!console._buf) console._buf = [];\n"
                         "    console._buf.push(msg);\n"
                         "  },\n"
                         "  warn: function() { console.log.apply(console, arguments); },\n"
                         "  error: function() { console.log.apply(console, arguments); }\n"
                         "};\n");

    // Timer bridge: setTimeout / setInterval / clearTimeout / clearInterval
    m_timerBridge = new SceneTimerBridge(
        m_jsEngine,
        this,
        /* postFire */
        [this](int id, bool error, const QString& msg) {
            if (error) {
                LOG_INFO("Timer callback error (id=%d): %s", id, qPrintable(msg));
            }
            flushJsConsole(m_jsEngine, "timer");
        },
        /* guardedCall */
        [this](const std::function<QJSValue()>& call, bool* outInterrupted) {
            // Arms m_jsWatchdog (~250ms budget), runs the callback, disarms, and
            // clears the interrupt latch — the SAME bracket the tick loops use. A
            // runaway timer body now trips the watchdog and unwinds instead of
            // freezing the GUI thread; an interrupted INTERVAL latches off after K
            // fires (SceneTimerBridge's per-timer back-off). Surfaced via postFire.
            return callJsGuarded(call, outInterrupted);
        });
    m_jsEngine->globalObject().setProperty("_timerBridge", m_jsEngine->newQObject(m_timerBridge));
    m_jsEngine->evaluate("function setTimeout(fn, delay)  { return _timerBridge.createTimer(fn, "
                         "delay || 0, false); }\n"
                         "function setInterval(fn, delay) { return _timerBridge.createTimer(fn, "
                         "delay || 0, true); }\n"
                         "function clearTimeout(id)  { _timerBridge.clearTimer(id); }\n"
                         "function clearInterval(id) { _timerBridge.clearTimer(id); }\n"
                         // WE scripts call these as engine-namespaced too — most commonly
                         // `engine.setTimeout(...)` for delayed-reveal animations (Summer
                         // Vibes 3293999899 fires this 29 times).  Alias to the same
                         // _timerBridge so cancellation IDs interoperate across spellings.
                         "engine.setTimeout    = setTimeout;\n"
                         "engine.setInterval   = setInterval;\n"
                         "engine.clearTimeout  = clearTimeout;\n"
                         "engine.clearInterval = clearInterval;\n");

    // Engine method stubs
    m_jsEngine->evaluate("engine.isDesktopDevice = function() { return true; };\n"
                         "engine.isMobileDevice = function() { return false; };\n"
                         "engine.isTabletDevice = function() { return false; };\n"
                         "engine.isWallpaper = function() { return true; };\n"
                         "engine.isScreensaver = function() { return false; };\n"
                         "engine.isRunningInEditor = function() { return false; };\n");

    // Screen/canvas resolution, orientation, and input stubs for property scripts
    {
        auto orthoSize = m_scene->getOrthoSize();
        bool portrait  = orthoSize[1] > orthoSize[0];
        m_jsEngine->evaluate(QString("engine.screenResolution = { x: %1, y: %2 };\n"
                                     "engine.canvasSize = { x: %1, y: %2 };\n"
                                     "engine.isPortrait = function() { return %3; };\n"
                                     "engine.isLandscape = function() { return %4; };\n")
                                 .arg(orthoSize[0])
                                 .arg(orthoSize[1])
                                 .arg(portrait ? "true" : "false")
                                 .arg(portrait ? "false" : "true"));
    }

    // Vec2 / Vec3 / Vec4 — canonical shim in SceneScriptShimsJs.hpp.
    // Defined first so the input / String.match / localStorage block below
    // and the Mat3/Mat4 / _Internal / script-identity shims that follow can
    // all reference Vec2/Vec3 freely.
    m_jsEngine->evaluate(wek::qml_helper::kVecClassesJs);
    // Cache the Vec ctor handles for the color/shader-value tick loops (see
    // SceneBackend.hpp).  Grabbed here, right after kVecClassesJs installs them,
    // so the per-tick loops never re-fetch from the global object or string-
    // compile a "VecN(...)" call.
    m_vec2Fn = m_jsEngine->globalObject().property("Vec2");
    m_vec3Fn = m_jsEngine->globalObject().property("Vec3");
    m_vec4Fn = m_jsEngine->globalObject().property("Vec4");

    m_jsEngine->evaluate(
        // cursorWorldPosition / cursorScreenPosition are Vec2 (not plain
        // objects) so wallpapers that compose them — Game of Life
        // (3453251764) tooltip layer does
        // `return input.cursorWorldPosition.add(new Vec2(offX, offY))`
        // every tick — find the .add/.subtract/etc. methods.  The per-frame
        // refresh below mutates .x/.y in place, which preserves the Vec2
        // prototype link.
        "var input = { cursorWorldPosition: Vec2(0, 0),\n"
        "  cursorScreenPosition: Vec2(0, 0),\n"
        "  cursorLeftDown: false };\n"
        // Safe String.match: return empty array instead of null (prevents null.forEach crashes)
        "var _origMatch = String.prototype.match;\n"
        "String.prototype.match = function(re) { return _origMatch.call(this, re) || []; };\n"
        // localStorage — backed by __sceneBridge for disk persistence.
        // WE defines two locations: GLOBAL (shared) and SCREEN (per-scene).
        // Scripts that omit the argument default to SCREEN (the solar system
        // wallpaper's icon-state save/load flow relies on this — without a
        // `loc` arg it expects per-scene persistence so switching to another
        // wallpaper doesn't inherit the last wallpaper's icon state).
        "var localStorage = (function() {\n"
        "  function _loc(l) { return (l === 0 || l === 1) ? l : 1; }\n"
        "  return {\n"
        "    LOCATION_GLOBAL: 0, LOCATION_SCREEN: 1,\n"
        "    get: function(key, loc) {\n"
        "      return __sceneBridge ? __sceneBridge.lsGet(_loc(loc), String(key)) : undefined;\n"
        "    },\n"
        "    set: function(key, value, loc) {\n"
        "      if (__sceneBridge) __sceneBridge.lsSet(_loc(loc), String(key), value);\n"
        "    },\n"
        "    remove: function(key, loc) {\n"
        "      if (__sceneBridge) __sceneBridge.lsRemove(_loc(loc), String(key));\n"
        "    },\n"
        "    'delete': function(key, loc) {\n"
        "      if (__sceneBridge) __sceneBridge.lsRemove(_loc(loc), String(key));\n"
        "    },\n"
        "    clear: function(loc) {\n"
        "      if (__sceneBridge) __sceneBridge.lsClear(_loc(loc));\n"
        "    }\n"
        "  };\n"
        "})();\n");

    // Mat3 / Mat4 — shared with tests via SceneScriptShimsJs.hpp.  Must come
    // AFTER Vec2/Vec3 (Mat4.translation returns a Vec3; Mat3.translation a Vec2).
    m_jsEngine->evaluate(wek::qml_helper::kMatricesJs);

    // Pre-populate common `shared.X` slots so scripts can safely chain-read
    // them on the first tick before any partner script has written them.
    // Several wallpapers ship scripts that unconditionally access
    // `shared.NESTED.field` and throw "cannot read property of undefined"
    // when NESTED was never created.  Drivers:
    //   - Starscape (3047596375): `shared.camera.targetAngles.subtract(...)`
    //   - Game of Life (3453251764): `shared.autoDraw.cursorDown`,
    //     `shared.currentBrush.hue`, etc. — 124 inline scripts share state.
    // Empty objects let `typeof shared.X.Y === 'undefined'` short-circuit
    // correctly; populated camera fields default to common orbit baselines.
    m_jsEngine->evaluate(
        "if (!shared.camera) {\n"
        "  shared.camera = {\n"
        "    mode: 'orbital',\n"
        "    targetPosition: new Vec3(0, 0, 0),\n"
        "    targetAngles:   new Vec2(0, 90),\n"
        "    targetDistance: 0,\n"
        "    currentPosition: new Vec3(0, 0, 0),\n"
        "    currentAngles:   new Vec2(0, 90),\n"
        "    currentDistance: 0,\n"
        "    isDragging: false,\n"
        "    mouseInput: true\n"
        "  };\n"
        "}\n"
        "if (!shared.autoDraw)    shared.autoDraw    = {};\n"
        "if (!shared.brushEditor) shared.brushEditor = {};\n"
        "if (!shared.brushes)     shared.brushes     = [];\n"
        "if (!shared.currentBrush) shared.currentBrush = {\n"
        "  hue: 0, saturation: 0, value: 0,\n"
        "  hardness: 1, size: 1, spacing: 1,\n"
        "  brush_type: 0, draw_mode: 0,\n"
        "  hi_vel: 0, lo_vel: 0, pat: 0, tex: 0,\n"
        "  stroke_type: 0, brush: 0, hi: 0\n"
        "};\n");

    // IMaterial proxy — defines _materialValueCache + _makeMaterialProxy.
    // Must come AFTER Vec2/Vec3/Vec4 (the proxy unpacks Vec instances).
    m_jsEngine->evaluate(wek::qml_helper::kMaterialProxyJs);

    // Layer-hierarchy shim — defines _installHierarchyMethods,
    // _installSoundHierarchyStubs, _hierarchyResolveId, _linkupHierarchy.
    // Layer factories below call _installHierarchyMethods on each proxy.
    m_jsEngine->evaluate(wek::qml_helper::kHierarchyProxyJs);

    // `_Internal` helper namespace — updateScriptProperties /
    // convertUserProperties / stringifyConfig.  Requires Vec3.
    m_jsEngine->evaluate(wek::qml_helper::kInternalNamespaceJs);

    // engine.scriptId / scriptName / getScriptHash() placeholders.  These
    // are rewritten to stable scene-level values in setScriptIdentity()
    // AFTER all property scripts have been loaded (see that call site near
    // the end of setupTextScripts).
    m_jsEngine->evaluate("engine.scriptId = 0;\n"
                         "engine.scriptName = 'scene';\n"
                         "engine.getScriptHash = function() { return '0'; };\n");

    // Cursor aliases on `engine`: scripts that live outside a layer IIFE
    // (init scripts, color scripts, scene.on update handlers) commonly
    // want `engine.cursorWorldPosition` etc. instead of digging into the
    // per-IIFE `input` object.  We share the same underlying {x,y}
    // sub-objects, so a single C++ update refreshes both surfaces.
    //
    // cursorLeftDown is a primitive boolean that can't be aliased by
    // object-reference; expose it as a getter that reads through.
    //
    // cursorHitTest(x, y) walks the live layer list in reverse (topmost
    // first) and returns the first visible AABB-containing proxy, or
    // null.  Parallax / rotation aren't accounted for — it's a coarse
    // quick-pick, not a full render-state hit test.
    m_jsEngine->evaluate(
        "engine.cursorWorldPosition  = input.cursorWorldPosition;\n"
        "engine.cursorScreenPosition = input.cursorScreenPosition;\n"
        "Object.defineProperty(engine, 'cursorLeftDown', {\n"
        "  get: function() { return input.cursorLeftDown; },\n"
        "  enumerable: true\n"
        "});\n"
        "engine.cursorHitTest = function(x, y) {\n"
        "  if (typeof x !== 'number') x = engine.cursorWorldPosition.x;\n"
        "  if (typeof y !== 'number') y = engine.cursorWorldPosition.y;\n"
        "  if (typeof _layerList === 'undefined') return null;\n"
        "  for (var i = _layerList.length - 1; i >= 0; i--) {\n"
        "    var L = _layerList[i];\n"
        "    if (!L || !L.visible) continue;\n"
        "    var sz = L.size; if (!sz || !sz.x || !sz.y) continue;\n"
        "    var o = L.origin, s = L.scale;\n"
        "    var hw = sz.x * 0.5 * (s ? s.x : 1);\n"
        "    var hh = sz.y * 0.5 * (s ? s.y : 1);\n"
        "    if (Math.abs(x - o.x) <= hw && Math.abs(y - o.y) <= hh) return L;\n"
        "  }\n"
        "  return null;\n"
        "};\n");

    // WEMath module: lerp, mix, clamp, smoothstep, random, and GLSL-style helpers
    m_jsEngine->evaluate(
        "var WEMath = {\n"
        "  PI: Math.PI,\n"
        "  lerp: function(a, b, t) { return a + (b - a) * t; },\n"
        "  mix: function(a, b, t) { return a + (b - a) * t; },\n"
        "  clamp: function(v, lo, hi) { return Math.min(Math.max(v, lo), hi); },\n"
        "  smoothstep: function(edge0, edge1, x) {\n"
        "    var t = Math.min(Math.max((x - edge0) / (edge1 - edge0), 0), 1);\n"
        "    return t * t * (3 - 2 * t);\n"
        "  },\n"
        "  fract: function(x) { return x - Math.floor(x); },\n"
        "  sign: function(x) { return x > 0 ? 1 : (x < 0 ? -1 : 0); },\n"
        "  step: function(edge, x) { return x < edge ? 0 : 1; },\n"
        "  abs: function(x) { return Math.abs(x); },\n"
        "  pow: function(base, exp) { return Math.pow(base, exp); },\n"
        // GLSL-style mod: always non-negative, unlike JS % for negative x
        "  mod: function(x, y) { return x - y * Math.floor(x / y); },\n"
        "  degToRad: function(d) { return d * Math.PI / 180; },\n"
        "  radToDeg: function(r) { return r * 180 / Math.PI; },\n"
        // Random helpers: WE wallpapers commonly call randomFloat/randomInteger
        "  randomFloat: function(min, max) { return min + Math.random() * (max - min); },\n"
        "  randomInteger: function(min, max) { return Math.floor(min + Math.random() * (max - min "
        "+ 1)); },\n"
        "  min: function(a, b) { return Math.min(a, b); },\n"
        "  max: function(a, b) { return Math.max(a, b); },\n"
        "  floor: function(x) { return Math.floor(x); },\n"
        "  ceil: function(x) { return Math.ceil(x); },\n"
        "  round: function(x) { return Math.round(x); },\n"
        "  sqrt: function(x) { return Math.sqrt(x); },\n"
        "  sin: function(x) { return Math.sin(x); },\n"
        "  cos: function(x) { return Math.cos(x); },\n"
        "  tan: function(x) { return Math.tan(x); },\n"
        "  asin: function(x) { return Math.asin(x); },\n"
        "  acos: function(x) { return Math.acos(x); },\n"
        "  atan: function(x) { return Math.atan(x); },\n"
        "  atan2: function(y, x) { return Math.atan2(y, x); },\n"
        "  log: function(x) { return Math.log(x); },\n"
        "  exp: function(x) { return Math.exp(x); }\n"
        "};\n"
        // camelCase aliases and constant forms matching WE's official API
        "WEMath.smoothStep = WEMath.smoothstep;\n"
        "WEMath.deg2rad = Math.PI / 180;\n"
        "WEMath.rad2deg = 180 / Math.PI;\n"
        // WEVector module: 2D angle↔vector conversion.
        // Matches WE's bundled jsmodules/wevector.js: angles are in DEGREES on
        // both sides — angleVector2(deg) and vectorAngle2 returns degrees.
        // Author scripts (e.g. Naruto Shippuden 2800255344) compute `angle =
        // 360 * (i / N)` and feed that to angleVector2; treating it as radians
        // scrambles the spectrum-ring layout.
        "var WEVector = {\n"
        "  angleVector2: function(angle) { var r = angle * WEMath.deg2rad; return Vec2(Math.cos(r), Math.sin(r)); },\n"
        "  vectorAngle2: function(dir) { return Math.atan2(dir.y, dir.x) * WEMath.rad2deg; }\n"
        "};\n");

    // engine.colorScheme: a Vec3 (= primary) with four additional Vec3
    // sub-colors hung off it — `primary`, `secondary`, `tertiary`, `text`,
    // `highContrast`.  Keeping the top-level a Vec3 preserves back-compat
    // for scripts that read `engine.colorScheme.x / .y / .z` directly.
    //
    // Defaults: all primary/secondary/tertiary = white, text = black (assuming
    // light background), highContrast = black.  refreshJsUserProperties()
    // rebuilds the bundle from the schemecolor user property when set;
    // without richer palette input only `primary` changes — the rest stay
    // at defaults.
    m_jsEngine->evaluate(
        "function _buildColorScheme(primary) {\n"
        "  var cs = Vec3(primary.x, primary.y, primary.z);\n"
        "  cs.primary      = Vec3(primary.x, primary.y, primary.z);\n"
        "  cs.secondary    = Vec3(1, 1, 1);\n"
        "  cs.tertiary     = Vec3(1, 1, 1);\n"
        "  cs.text         = Vec3(0, 0, 0);\n"
        "  cs.highContrast = Vec3(0, 0, 0);\n"
        "  return cs;\n"
        "}\n"
        "engine.colorScheme = _buildColorScheme(Vec3(1, 1, 1));\n");

    // Populate engine.userProperties with defaults from project.json first,
    // then apply QML-side overrides (if any). Must run AFTER Vec3 is defined
    // so colorScheme derivation works.
    {
        std::string defaultsJson = m_scene->getUserPropertiesJson();
        if (! defaultsJson.empty() && defaultsJson != "{}") {
            // Full JS-literal escape (see JsStringEscape.hpp / F18).
            QString escaped =
                wek::qml_helper::escapeForJsSingleQuoted(QString::fromStdString(defaultsJson));
            m_jsEngine->evaluate(QString("(function(){"
                                         "var p=JSON.parse('%1');"
                                         "var up=engine.userProperties;"
                                         "for(var k in p) up[k]=p[k];"
                                         "})()")
                                     .arg(escaped));
        }
    }
    refreshJsUserProperties();

    // engine.openUserShortcut(name) — delegates through __sceneBridge to the
    // C++ slot which emits userShortcutRequested (mapped to MPRIS in the
    // main plugin) and fires a `userShortcut` event on the scene bus.
    m_jsEngine->evaluate("engine.openUserShortcut = function(name) {\n"
                         "  if (typeof name !== 'string' || !name) return;\n"
                         "  if (__sceneBridge && __sceneBridge.openUserShortcut)\n"
                         "    __sceneBridge.openUserShortcut(name);\n"
                         "};\n");

    // engine.registerAsset — describes a dynamic asset; a pool of hidden
    // scene nodes is pre-allocated by WPSceneParser (Scene::assetPools).
    // engine._assetPools is populated later when _layerInitStates is read —
    // until then, registerAsset just returns a descriptor.  createLayer
    // below pops from the pool at runtime.
    m_jsEngine->evaluate("engine._assetPools = {};\n"
                         "engine.registerAsset = function(path) { return { __asset: path }; };\n");

    // createScriptProperties() — WE SceneScript API for declaring user-configurable properties
    // Returns a chainable builder:
    // createScriptProperties().addSlider({name,value,...}).addCheckbox(...) After chaining, the
    // result object has properties accessible by name (e.g. scriptProperties.mode)
    // createScriptProperties: chainable builder that exposes each defined
    // property via a getter/setter pair.  Writes fire the optional
    // `onChange` callback with the new value (and `this` bound to the
    // builder).  Same-value writes are suppressed — essential because
    // WE wallpapers use mutual-exclusion patterns where one checkbox's
    // onChange writes `false` to its siblings; without the suppression
    // the siblings re-fire their own onChange and infinite-recurse.
    // Lucy Clock's date-format checkboxes rely on this.
    m_jsEngine->evaluate("function createScriptProperties() {\n"
                         "  var _values = {};\n"
                         "  var _onChange = {};\n"
                         "  var builder = {};\n"
                         "  function addProp(def) {\n"
                         "    if (!def) return builder;\n"
                         "    var n = def.name || def.n;\n"
                         "    if (!n) return builder;\n"
                         "    _values[n] = def.value;\n"
                         "    if (def.onChange && typeof def.onChange === 'function') {\n"
                         "      _onChange[n] = def.onChange;\n"
                         "    }\n"
                         "    if (!Object.getOwnPropertyDescriptor(builder, n)) {\n"
                         "      Object.defineProperty(builder, n, {\n"
                         "        get: function() { return _values[n]; },\n"
                         "        set: function(v) {\n"
                         "          if (_values[n] === v) return;\n"
                         "          _values[n] = v;\n"
                         "          var h = _onChange[n];\n"
                         "          if (h) {\n"
                         "            try { h.call(builder, v); }\n"
                         "            catch (e) {\n"
                         "              if (typeof console !== 'undefined' && console.log)\n"
                         "                console.log('scriptProperty onChange error on ' + n\n"
                         "                            + ': ' + (e && e.message));\n"
                         "            }\n"
                         "          }\n"
                         "        },\n"
                         "        enumerable: true, configurable: true\n"
                         "      });\n"
                         "    }\n"
                         "    return builder;\n"
                         "  }\n"
                         "  builder.addCheckbox = addProp;\n"
                         "  builder.addSlider = addProp;\n"
                         "  builder.addCombo = addProp;\n"
                         "  builder.addText = addProp;\n"
                         "  builder.addTextInput = addProp;\n"
                         "  builder.addColor = addProp;\n"
                         "  builder.addFile = addProp;\n"
                         "  builder.addDirectory = addProp;\n"
                         "  builder.finish = function() { return builder; };\n"
                         "  return builder;\n"
                         "}\n");

    // Inject layer initial states from scene parsing
    {
        std::string initJson = m_scene->getLayerInitialStatesJson();
        if (! initJson.empty()) {
            // Full JS-literal escape (see JsStringEscape.hpp / F18).
            QString escaped =
                wek::qml_helper::escapeForJsSingleQuoted(QString::fromStdString(initJson));
            m_jsEngine->evaluate(
                QString("var _layerInitStates = JSON.parse('%1');\n").arg(escaped));
        } else {
            m_jsEngine->evaluate("var _layerInitStates = {};\n");
        }
    }

    // Extract _ortho from init states and store scene ortho for JS
    // (also used by C++ for cursorClick hit-testing)
    m_jsEngine->evaluate("var _sceneOrtho = _layerInitStates._ortho || [1920, 1080];\n"
                         "delete _layerInitStates._ortho;\n"
                         // Extract asset pools — these are per-asset arrays of pool layer
                         // names.  createLayer/destroyLayer use them to rent hidden nodes.
                         "if (_layerInitStates._assetPools) {\n"
                         "  for (var k in _layerInitStates._assetPools)\n"
                         "    engine._assetPools[k] = _layerInitStates._assetPools[k].slice();\n"
                         "  delete _layerInitStates._assetPools;\n"
                         "}\n"
                         "engine._assetLive = {};\n");

    // thisScene.getLayer() and thisLayer infrastructure
    // Layer proxies use Object.defineProperty for dirty tracking
    m_jsEngine->evaluate(wek::qml_helper::kLayerProxyJs);
    // _applyLayerLiteral is shared with scenescript_tests via
    // SceneScriptShimsJs.hpp — evaluated as a separate top-level statement
    // so the production QJSEngine sees the exact same source string the
    // tests do.
    m_jsEngine->evaluate(wek::qml_helper::kApplyLayerLiteralJs);
    m_jsEngine->evaluate(wek::qml_helper::kLayerRuntimeJs);

    // Batched property-script dispatch: `_runAllPropertyScripts` replaces
    // the per-script C++->JS .call boundary (670 calls/tick on solar system)
    // with ONE call.  Entries live in `_allPropertyScripts` as plain objects
    // with kind/fn/proxy/current-value fields; the loop sets `thisLayer`,
    // wraps each fn in try/catch, and applies the same change-threshold
    // compares that C++ used to do — only scripts whose output actually
    // changed contribute to the returned flat array (stride 4: idx, v1, v2, v3).
    //
    // Source lives in PropertyScriptDispatchJs.hpp so scenescript_tests can
    // evaluate the exact same JS against its own QJSEngine without drift.
    // Kind values match PropertyScriptState::Kind: 0=Visible, 1=Vec3, 2=Alpha.
    m_jsEngine->evaluate(wek::qml_helper::kPropertyScriptDispatchJs);

    // Store references to JS dispatch functions for C++ calls
    m_collectDirtyLayersFn = m_jsEngine->globalObject().property("_collectDirtyLayers");
    m_fireSceneEventFn     = m_jsEngine->globalObject().property("_fireSceneEvent");
    m_hasSceneListenersFn  = m_jsEngine->globalObject().property("_hasSceneListeners");

    // Scene-level property control (bloom, clear color, camera, lighting)
    {
        std::string sceneJson = m_scene->getSceneInitialStateJson();
        if (! sceneJson.empty()) {
            // Full JS-literal escape (see JsStringEscape.hpp / F18).
            QString escaped =
                wek::qml_helper::escapeForJsSingleQuoted(QString::fromStdString(sceneJson));
            m_jsEngine->evaluate(QString("var _sceneInit = JSON.parse('%1');\n").arg(escaped));
        } else {
            m_jsEngine->evaluate("var _sceneInit = {cc:[0,0,0],bloom:false,bs:2,bt:0.65,"
                                 "ac:[0.2,0.2,0.2],sc:[0.3,0.3,0.3],persp:false,fov:0,"
                                 "eye:[0,0,1],ctr:[0,0,0],up:[0,1,0],lights:[]};\n");
        }
        m_jsEngine->evaluate(
            "var _sceneState = {\n"
            "  clearColor: {x:_sceneInit.cc[0], y:_sceneInit.cc[1], z:_sceneInit.cc[2]},\n"
            "  bloomEnabled: _sceneInit.bloom,\n"
            "  bloomStrength: _sceneInit.bs,\n"
            "  bloomThreshold: _sceneInit.bt,\n"
            "  ambientColor: {x:_sceneInit.ac[0], y:_sceneInit.ac[1], z:_sceneInit.ac[2]},\n"
            "  skylightColor: {x:_sceneInit.sc[0], y:_sceneInit.sc[1], z:_sceneInit.sc[2]},\n"
            "  isPerspective: _sceneInit.persp,\n"
            "  cameraFov: _sceneInit.fov,\n"
            "  cameraEye: {x:_sceneInit.eye[0], y:_sceneInit.eye[1], z:_sceneInit.eye[2]},\n"
            "  cameraCenter: {x:_sceneInit.ctr[0], y:_sceneInit.ctr[1], z:_sceneInit.ctr[2]},\n"
            "  cameraUp: {x:_sceneInit.up[0], y:_sceneInit.up[1], z:_sceneInit.up[2]},\n"
            "  _dirty: {}\n"
            "};\n"
            // Build light proxies with per-light dirty tracking
            "_sceneState.lights = _sceneInit.lights.map(function(l) {\n"
            "  var _s = { color:{x:l.c[0],y:l.c[1],z:l.c[2]},\n"
            "    radius:l.r, intensity:l.i,\n"
            "    position:{x:l.p[0],y:l.p[1],z:l.p[2]}, _dirty:{} };\n"
            "  var p = {};\n"
            "  ['color','position'].forEach(function(prop) {\n"
            "    Object.defineProperty(p, prop, {\n"
            "      get: function(){ return _s[prop]; },\n"
            "      set: function(v){ _s[prop] = v; _s._dirty[prop] = true; },\n"
            "      enumerable: true\n"
            "    });\n"
            "  });\n"
            "  ['radius','intensity'].forEach(function(prop) {\n"
            "    Object.defineProperty(p, prop, {\n"
            "      get: function(){ return _s[prop]; },\n"
            "      set: function(v){ _s[prop] = v; _s._dirty[prop] = true; },\n"
            "      enumerable: true\n"
            "    });\n"
            "  });\n"
            "  p._state = _s;\n"
            "  return p;\n"
            "});\n"
            // Extend thisScene with dirty-tracked scene properties
            "['clearColor','ambientColor','skylightColor','cameraEye','cameraCenter','cameraUp']."
            "forEach(function(prop) {\n"
            "  Object.defineProperty(thisScene, prop, {\n"
            "    get: function(){ return _sceneState[prop]; },\n"
            "    set: function(v){ _sceneState[prop] = v; _sceneState._dirty[prop] = true; },\n"
            "    enumerable: true\n"
            "  });\n"
            "});\n"
            "['bloomStrength','bloomThreshold','cameraFov'].forEach(function(prop) {\n"
            "  Object.defineProperty(thisScene, prop, {\n"
            "    get: function(){ return _sceneState[prop]; },\n"
            "    set: function(v){ _sceneState[prop] = v; _sceneState._dirty[prop] = true; },\n"
            "    enumerable: true\n"
            "  });\n"
            "});\n"
            // Read-only properties
            "Object.defineProperty(thisScene, 'bloomEnabled', {\n"
            "  get: function(){ return _sceneState.bloomEnabled; }, enumerable: true\n"
            "});\n"
            "Object.defineProperty(thisScene, 'isPerspective', {\n"
            "  get: function(){ return _sceneState.isPerspective; }, enumerable: true\n"
            "});\n"
            // getLights() returns the pre-built light proxy array
            "thisScene.getLights = function() { return _sceneState.lights; };\n"
            // _collectDirtyScene — returns null if nothing dirty, else dirty update object
            "function _collectDirtyScene() {\n"
            "  var d = _sceneState._dirty;\n"
            "  var keys = Object.keys(d);\n"
            "  var dirtyLights = [];\n"
            "  for (var i = 0; i < _sceneState.lights.length; i++) {\n"
            "    var ls = _sceneState.lights[i]._state;\n"
            "    var ld = ls._dirty;\n"
            "    if (Object.keys(ld).length > 0) {\n"
            "      dirtyLights.push({idx:i, dirty:ld,\n"
            "        color:ls.color, radius:ls.radius,\n"
            "        intensity:ls.intensity, position:ls.position});\n"
            "      ls._dirty = {};\n"
            "    }\n"
            "  }\n"
            "  if (keys.length === 0 && dirtyLights.length === 0) return null;\n"
            "  var r = {dirty:d, lights:dirtyLights};\n"
            "  if (d.clearColor) r.clearColor = _sceneState.clearColor;\n"
            "  if (d.bloomStrength) r.bloomStrength = _sceneState.bloomStrength;\n"
            "  if (d.bloomThreshold) r.bloomThreshold = _sceneState.bloomThreshold;\n"
            "  if (d.ambientColor) r.ambientColor = _sceneState.ambientColor;\n"
            "  if (d.skylightColor) r.skylightColor = _sceneState.skylightColor;\n"
            "  if (d.cameraFov) r.cameraFov = _sceneState.cameraFov;\n"
            "  if (d.cameraEye) r.cameraEye = _sceneState.cameraEye;\n"
            "  if (d.cameraCenter) r.cameraCenter = _sceneState.cameraCenter;\n"
            "  if (d.cameraUp) r.cameraUp = _sceneState.cameraUp;\n"
            "  _sceneState._dirty = {};\n"
            "  return r;\n"
            "}\n");
        m_collectDirtySceneFn = m_jsEngine->globalObject().property("_collectDirtyScene");
    }

    // Get node name→id map for thisScene.getLayer() dispatch
    m_nodeNameToId = m_scene->getNodeNameToIdMap();

    // Expose node id ↔ name maps to JS so scripts can look layers up by id
    // (thisScene.getLayerByID) and get a layer's id-index
    // (thisScene.getLayerIndex).  Also provides thisScene.getLayerCount.
    {
        QString idToName   = "var _layerIdToName = {";
        QString nameToId   = "var _layerNameToId = {";
        bool    first      = true;
        for (const auto& [name, id] : m_nodeNameToId) {
            // Full JS-literal escape — these names are interpolated into the
            // single-quoted keys/values of the object literals below; a name
            // with a backslash or U+2028/U+2029 would otherwise break the
            // literal (same hazard class as F18 / JsStringEscape.hpp).
            QString nameEsc =
                wek::qml_helper::escapeForJsSingleQuoted(QString::fromStdString(name));
            if (! first) {
                idToName += ",";
                nameToId += ",";
            }
            idToName += QString("'%1':'%2'").arg(id).arg(nameEsc);
            nameToId += QString("'%1':%2").arg(nameEsc).arg(id);
            first     = false;
        }
        idToName += "};\n";
        nameToId += "};\n";
        // thisScene.getCameraTransforms() builds view + projection Mat4s
        // from the existing cameraEye/Center/Up/Fov scene properties and
        // canvasSize aspect ratio.  setCameraTransforms() writes an input
        // object back onto the scripted camera properties; it prefers
        // high-level fields (eye / center / up / fov) when present, and
        // falls back to extracting the eye from a view matrix's translation
        // column when only `view` is supplied.  Scripts that want full
        // control still have the per-axis scene properties as the primary
        // interface — these methods are a convenience wrapper.
        m_jsEngine->evaluate(
            "thisScene.getCameraTransforms = function() {\n"
            "  var eye    = thisScene.cameraEye;\n"
            "  var center = thisScene.cameraCenter;\n"
            "  var up     = thisScene.cameraUp;\n"
            "  var ex = eye.x, ey = eye.y, ez = eye.z;\n"
            "  var fx = center.x - ex, fy = center.y - ey, fz = center.z - ez;\n"
            "  var flen = Math.sqrt(fx*fx + fy*fy + fz*fz) || 1;\n"
            "  fx /= flen; fy /= flen; fz /= flen;\n"
            "  var rx = fy*up.z - fz*up.y, ry = fz*up.x - fx*up.z, rz = fx*up.y - fy*up.x;\n"
            "  var rlen = Math.sqrt(rx*rx + ry*ry + rz*rz) || 1;\n"
            "  rx /= rlen; ry /= rlen; rz /= rlen;\n"
            "  var ux = ry*fz - rz*fy, uy = rz*fx - rx*fz, uz = rx*fy - ry*fx;\n"
            "  var view = new Mat4();\n"
            "  view.m[0]=rx;  view.m[1]=ux;  view.m[2] =-fx; view.m[3] =0;\n"
            "  view.m[4]=ry;  view.m[5]=uy;  view.m[6] =-fy; view.m[7] =0;\n"
            "  view.m[8]=rz;  view.m[9]=uz;  view.m[10]=-fz; view.m[11]=0;\n"
            "  view.m[12]=-(rx*ex+ry*ey+rz*ez);\n"
            "  view.m[13]=-(ux*ex+uy*ey+uz*ez);\n"
            "  view.m[14]= (fx*ex+fy*ey+fz*ez);\n"
            "  view.m[15]=1;\n"
            "  var proj = new Mat4();\n"
            "  var fovDeg = thisScene.cameraFov || 60;\n"
            "  var fRad   = fovDeg * Math.PI / 180;\n"
            "  var f      = 1.0 / Math.tan(fRad * 0.5);\n"
            "  var aspect = (engine.canvasSize && engine.canvasSize.x && engine.canvasSize.y)\n"
            "             ? engine.canvasSize.x / engine.canvasSize.y : 1.0;\n"
            "  var near = 0.1, far = 10000;\n"
            "  proj.m[0]=f/aspect; proj.m[5]=f;\n"
            "  proj.m[10]=(far+near)/(near-far);\n"
            "  proj.m[11]=-1;\n"
            "  proj.m[14]=(2*far*near)/(near-far);\n"
            "  proj.m[15]=0;\n"
            // High-level fields (eye/center/up/fov) alongside the matrices.
            // Driver: Starscape (3047596375) ships `camera =
            // thisScene.getCameraTransforms(); camera.eye.subtract(...)` —
            // returning only {view, projection} produced "subtract of
            // undefined" at ~30Hz.  Pass through fresh Vec3 copies so
            // scripts mutate them locally without aliasing scene state.
            "  return {\n"
            "    view: view, projection: proj,\n"
            "    eye:    new Vec3(eye.x, eye.y, eye.z),\n"
            "    center: new Vec3(center.x, center.y, center.z),\n"
            "    up:     new Vec3(up.x, up.y, up.z),\n"
            "    fov:    fovDeg\n"
            "  };\n"
            "};\n"
            "thisScene.setCameraTransforms = function(t) {\n"
            "  if (!t) return;\n"
            "  if (t.eye    !== undefined) thisScene.cameraEye    = t.eye;\n"
            "  if (t.center !== undefined) thisScene.cameraCenter = t.center;\n"
            "  if (t.up     !== undefined) thisScene.cameraUp     = t.up;\n"
            "  if (t.fov    !== undefined) thisScene.cameraFov    = t.fov;\n"
            "  if (t.view   !== undefined && t.view.m && t.eye === undefined) {\n"
            // Naive inverse: pull eye out of the view matrix's translation.
            // Full basis decomposition would touch center/up too, but those
            // are typically set together with eye by scripts that build a
            // view matrix from scratch.
            "    thisScene.cameraEye = { x: -t.view.m[12], y: -t.view.m[13], z:  t.view.m[14] };\n"
            "  }\n"
            "};\n");

        m_jsEngine->evaluate(idToName + nameToId
                             + "thisScene.getLayerByID = function(id) {\n"
                               "  var name = _layerIdToName[id];\n"
                               "  return name ? thisScene.getLayer(name) : null;\n"
                               "};\n"
                               "thisScene.getLayerCount = function() {\n"
                               "  return Object.keys(_layerInitStates).length;\n"
                               "};\n"
                               // Override the earlier stub (which read a
                               // never-set _index) with a real id lookup.
                               // Accepts either a name string or a layer
                               // proxy (Blue Archive 2764537029 visualizer
                               // calls `getLayerIndex(thisLayer)` with the
                               // proxy directly).
                               "thisScene.getLayerIndex = function(arg) {\n"
                               "  var key = arg;\n"
                               "  if (arg && typeof arg === 'object' && typeof arg.name === 'string')\n"
                               "    key = arg.name;\n"
                               "  if (typeof key !== 'string') return -1;\n"
                               "  var id = _layerNameToId[key];\n"
                               "  return (typeof id === 'number') ? id : -1;\n"
                               "};\n");
    }

    // Sound layer control infrastructure for SceneScript play/stop/pause API
    {
        auto soundLayers = m_scene->getSoundLayerControls();
        m_soundLayerStates.clear();
        m_soundLayerNameToIndex.clear();

        if (! soundLayers.empty()) {
            // Build _soundLayerStates JSON for JS side
            QString statesJson = "{\n";
            for (int32_t i = 0; i < (int32_t)soundLayers.size(); i++) {
                const auto&     sl = soundLayers[i];
                SoundLayerState sls;
                sls.index = i;
                sls.name  = sl.name;
                m_soundLayerStates.push_back(std::move(sls));
                m_soundLayerNameToIndex[sl.name] = i;

                // Full JS-literal escape — sl.name is interpolated into the
                // single-quoted key of the _soundLayerStates object literal
                // (same hazard class as F18 / JsStringEscape.hpp).
                QString nameEsc =
                    wek::qml_helper::escapeForJsSingleQuoted(QString::fromStdString(sl.name));
                if (i > 0) statesJson += ",\n";
                statesJson += QString("  '%1': { idx: %2, vol: %3, silent: %4 }")
                                  .arg(nameEsc)
                                  .arg(i)
                                  .arg(sl.initialVolume, 0, 'f', 3)
                                  .arg(sl.startsilent ? "true" : "false");
            }
            statesJson += "\n}";

            m_jsEngine->evaluate(QString("var _soundLayerStates = %1;\n").arg(statesJson));

            // Sound playing states object — updated by C++ before each eval
            m_jsEngine->evaluate("engine._soundPlayingStates = {};\n");

            // Sound layer proxy factory with dirty tracking and command queue
            m_jsEngine->evaluate(
                "var _soundLayerCache = {};\n"
                "function _makeSoundLayerProxy(name) {\n"
                "  var info = _soundLayerStates[name];\n"
                "  if (!info) return null;\n"
                "  var _s = { name: name, volume: info.vol, _dirty: {}, _cmds: [] };\n"
                "  var p = {};\n"
                "  Object.defineProperty(p, 'name', { get: function(){return _s.name;}, "
                "enumerable:true });\n"
                "  Object.defineProperty(p, 'volume', {\n"
                "    get: function(){ return _s.volume; },\n"
                "    set: function(v){ _s.volume = v; _s._dirty.volume = true; },\n"
                "    enumerable: true\n"
                "  });\n"
                // Update _soundPlayingStates synchronously on play/pause/stop
                // so subsequent isPlaying() reads within the same tick are
                // consistent.  C++ drains the _cmds queue AFTER the script
                // pass, AND the stream state can take one more render tick
                // to reflect the command, so without this shadow a
                // wallpaper script that pauses and then reads isPlaying()
                // would see stale "still playing" and act on it (on
                // 2866203962 playerplay.origin.update's anyPlaying() uses
                // the reading to side-effect playStatus=true, which
                // triggers skip() when C++ finally reports paused).
                //
                // `_soundPlayingStatesDirty[name] = true` blocks the next
                // C++ refresh from overwriting our shadow until C++
                // actually matches the intended state.
                "  p.play = function(){ _s._cmds.push('play');\n"
                "    if (!engine._soundPlayingStates) engine._soundPlayingStates = {};\n"
                "    if (!engine._soundPlayingStatesDirty) engine._soundPlayingStatesDirty = {};\n"
                "    engine._soundPlayingStates[name] = true;\n"
                "    engine._soundPlayingStatesDirty[name] = true; };\n"
                "  p.stop = function(){ _s._cmds.push('stop');\n"
                "    if (!engine._soundPlayingStates) engine._soundPlayingStates = {};\n"
                "    if (!engine._soundPlayingStatesDirty) engine._soundPlayingStatesDirty = {};\n"
                "    engine._soundPlayingStates[name] = false;\n"
                "    engine._soundPlayingStatesDirty[name] = true; };\n"
                "  p.pause = function(){ _s._cmds.push('pause');\n"
                "    if (!engine._soundPlayingStates) engine._soundPlayingStates = {};\n"
                "    if (!engine._soundPlayingStatesDirty) engine._soundPlayingStatesDirty = {};\n"
                "    engine._soundPlayingStates[name] = false;\n"
                "    engine._soundPlayingStatesDirty[name] = true; };\n"
                "  p.isPlaying = function(){\n"
                "    return !!(engine._soundPlayingStates && engine._soundPlayingStates[name]);\n"
                "  };\n"
                "  // No-op stubs for properties that only apply to image layers\n"
                "  Object.defineProperty(p, 'origin', { get: function(){return {x:0,y:0,z:0};}, "
                "set: function(){}, enumerable:true });\n"
                "  Object.defineProperty(p, 'scale', { get: function(){return {x:1,y:1,z:1};}, "
                "set: function(){}, enumerable:true });\n"
                "  Object.defineProperty(p, 'angles', { get: function(){return {x:0,y:0,z:0};}, "
                "set: function(){}, enumerable:true });\n"
                "  Object.defineProperty(p, 'visible', { get: function(){return true;}, set: "
                "function(){}, enumerable:true });\n"
                "  Object.defineProperty(p, 'alpha', { get: function(){return 1;}, set: "
                "function(){}, enumerable:true });\n"
                "  p.getTextureAnimation = function(){\n"
                "    return { rate: 0, frameCount: 1, _frame: 0,\n"
                "      getFrame: function(){ return this._frame; },\n"
                "      setFrame: function(f){ this._frame = f; },\n"
                "      play: function(){ this.rate = 1; },\n"
                "      pause: function(){ this.rate = 0; },\n"
                "      stop: function(){ this.rate = 0; this._frame = 0; },\n"
                "      isPlaying: function(){ return this.rate > 0; }\n"
                "    };\n"
                "  };\n"
                "  // Sound-layer proxy getVideoTexture: same functional shape as image\n"
                "  // proxies — scripts that probe any layer type should get a usable\n"
                "  // object rather than undefined.  Bridge returns 0/no-op when the\n"
                "  // sound layer has no associated video (typical case).\n"
                "  p.getVideoTexture = function() {\n"
                "    var name = _s.name;\n"
                "    var _rate = 1.0;\n"
                "    var o = {\n"
                "      getCurrentTime: function(){ return __sceneBridge.videoGetCurrentTime(name); "
                "},\n"
                "      setCurrentTime: function(t){ __sceneBridge.videoSetCurrentTime(name, +t || "
                "0); },\n"
                "      isPlaying: function(){ return !!__sceneBridge.videoIsPlaying(name); },\n"
                "      play:  function(){ __sceneBridge.videoPlay(name); },\n"
                "      pause: function(){ __sceneBridge.videoPause(name); },\n"
                "      stop:  function(){ __sceneBridge.videoStop(name); }\n"
                "    };\n"
                "    Object.defineProperty(o, 'duration', {\n"
                "      get: function(){ return __sceneBridge.videoGetDuration(name); }, "
                "enumerable:true });\n"
                "    Object.defineProperty(o, 'rate', {\n"
                "      get: function(){ return _rate; },\n"
                "      set: function(v){ _rate = +v || 1.0; __sceneBridge.videoSetRate(name, "
                "_rate); },\n"
                "      enumerable:true });\n"
                "    return o;\n"
                "  };\n"
                "  // Volume animation controller (getAnimation returns same interface as WE)\n"
                "  if (!_s._animCtrl) _s._animCtrl = {};\n"
                "  p.getAnimation = function(animName) {\n"
                "    if (_s._animCtrl[animName]) return _s._animCtrl[animName];\n"
                "    var ctrl = { _playing: false, _name: animName,\n"
                "      play:  function(){ this._playing = true;  _s._cmds.push('anim_play:'  + "
                "animName); },\n"
                "      pause: function(){ this._playing = false; _s._cmds.push('anim_pause:' + "
                "animName); },\n"
                "      stop:  function(){ this._playing = false; _s._cmds.push('anim_stop:'  + "
                "animName); },\n"
                "      isPlaying: function(){ return this._playing; }\n"
                "    };\n"
                "    _s._animCtrl[animName] = ctrl;\n"
                "    return ctrl;\n"
                "  };\n"
                "  p._state = _s;\n"
                // Sound-layer hierarchy stubs — no transform, methods no-op.
                "  _installSoundHierarchyStubs(p);\n"
                "  return p;\n"
                "}\n");

            // Patch thisScene.getLayer to check sound layers too
            m_jsEngine->evaluate("var _origGetLayer = thisScene.getLayer;\n"
                                 "thisScene.getLayer = function(name) {\n"
                                 "  // Check image layers first\n"
                                 "  var r = _origGetLayer(name);\n"
                                 "  if (r) return r;\n"
                                 "  // Then check sound layers\n"
                                 "  if (_soundLayerCache[name]) return _soundLayerCache[name];\n"
                                 "  if (_soundLayerStates[name]) {\n"
                                 "    _soundLayerCache[name] = _makeSoundLayerProxy(name);\n"
                                 "    return _soundLayerCache[name];\n"
                                 "  }\n"
                                 "  console.log('getLayer: unknown layer: ' + name);\n"
                                 "  return _nullProxy;\n"
                                 "};\n");

            // thisScene.enumerateLayers — returns array of proxies for all layers
            m_jsEngine->evaluate("thisScene.enumerateLayers = function() {\n"
                                 "  var layers = [];\n"
                                 "  // Image layers\n"
                                 "  for (var name in _layerInitStates) {\n"
                                 "    layers.push(thisScene.getLayer(name));\n"
                                 "  }\n"
                                 "  // Sound layers\n"
                                 "  for (var name in _soundLayerStates) {\n"
                                 "    layers.push(thisScene.getLayer(name));\n"
                                 "  }\n"
                                 "  return layers;\n"
                                 "};\n");

            // Diagnostic: test enumerateLayers to verify sound layers are discoverable
            {
                QJSValue testResult = m_jsEngine->evaluate(
                    "var _testLayers = thisScene.enumerateLayers();\n"
                    "var _testMp3 = _testLayers.filter(function(e){ return e && e.name && "
                    "e.name.toLowerCase().indexOf('.mp3') >= 0; });\n"
                    "'total=' + _testLayers.length + ' mp3=' + _testMp3.length + ' names=[' + "
                    "_testMp3.map(function(e){return e.name;}).join('|') + ']';\n");
                LOG_INFO("enumerateLayers test: %s", qPrintable(testResult.toString()));
            }

            // Collect dirty sound layer commands for C++ dispatch
            m_jsEngine->evaluate("function _collectDirtySoundLayers() {\n"
                                 "  var updates = [];\n"
                                 "  for (var name in _soundLayerCache) {\n"
                                 "    var s = _soundLayerCache[name]._state;\n"
                                 "    var hasDirty = Object.keys(s._dirty).length > 0;\n"
                                 "    var hasCmds = s._cmds.length > 0;\n"
                                 "    if (!hasDirty && !hasCmds) continue;\n"
                                 "    updates.push({ name: name, dirty: s._dirty,\n"
                                 "      volume: s.volume, cmds: s._cmds });\n"
                                 "    s._dirty = {};\n"
                                 "    s._cmds = [];\n"
                                 "  }\n"
                                 "  return updates;\n"
                                 "}\n");

            m_collectDirtySoundLayersFn =
                m_jsEngine->globalObject().property("_collectDirtySoundLayers");

            LOG_INFO("setupTextScripts: %zu sound layers registered for SceneScript API",
                     soundLayers.size());
        } else {
            // Still provide enumerateLayers even when there are no sound layers
            m_jsEngine->evaluate("thisScene.enumerateLayers = function() {\n"
                                 "  var layers = [];\n"
                                 "  for (var name in _layerInitStates) {\n"
                                 "    layers.push(thisScene.getLayer(name));\n"
                                 "  }\n"
                                 "  return layers;\n"
                                 "};\n");
        }
    }

    // Eager linkup of layer-hierarchy parent/child references.  Walks
    // _layerInitStates one final time and resolves each `pn` (parent
    // name) into a proxy ref; all proxies were materialized by the
    // enumerateLayers() call above (or via the no-sound-layers branch).
    // This also covers proxies created lazily afterwards — _linkupHierarchy
    // is idempotent and safe to call again, but most scripts query
    // parents during init so the eager pass catches the common cases.
    m_jsEngine->evaluate(
        "if (typeof thisScene.enumerateLayers === 'function') thisScene.enumerateLayers();\n"
        "if (typeof _linkupHierarchy === 'function') _linkupHierarchy();\n");

    // Final null-safety wrapper: ensures getLayer() never returns null.
    // The original getLayer returns null for unknown image layers so the sound-layer
    // patch can fall through. This outermost wrapper catches any remaining nulls.
    m_jsEngine->evaluate("var _innerGetLayer = thisScene.getLayer;\n"
                         "thisScene.getLayer = function(name) {\n"
                         "  var r = _innerGetLayer(name);\n"
                         "  if (r !== null && r !== undefined) return r;\n"
                         "  console.log('getLayer: unknown layer: ' + name);\n"
                         "  return _nullProxy;\n"
                         "};\n"
                         // getLayerCount — returns total number of discoverable layers
                         "thisScene.getLayerCount = function() {\n"
                         "  return Object.keys(_layerInitStates).length;\n"
                         "};\n"
                         // thisObject global — context-dependent object (defaults to thisLayer)
                         "var thisObject = {\n"
                         "  getAnimation: function(name) {\n"
                         "    if (thisLayer && thisLayer.getAnimationLayer) return "
                         "thisLayer.getAnimationLayer(name || 0);\n"
                         "    return null;\n"
                         "  }\n"
                         "};\n");

    // Audio resolution constants
    engineObj.setProperty("AUDIO_RESOLUTION_16", 16);
    engineObj.setProperty("AUDIO_RESOLUTION_32", 32);
    engineObj.setProperty("AUDIO_RESOLUTION_64", 64);

    // Media playback event constants (WE SceneScript MediaPlaybackEvent)
    m_jsEngine->evaluate("var MediaPlaybackEvent = { CYCLIC: -1, PLAYBACK_STOPPED: 0, "
                         "PLAYBACK_PLAYING: 1, PLAYBACK_PAUSED: 2 };\n");

    // engine.registerAudioBuffers(resolution) — JS shim shared with tests
    // (see SceneScriptShimsJs.hpp).  Returns a buffer object the script
    // retains; refreshAudioBuffers() fills it from the analyzer each tick.
    {
        QJSValue regFn = m_jsEngine->evaluate(wek::qml_helper::kRegisterAudioBuffersJs);
        engineObj.setProperty("registerAudioBuffers", regFn);
    }

    // Wallpaper Engine SceneScript API stubs
    // createScriptProperties() returns a builder with .addSlider/.addCheckbox/.addCombo/.finish()
    m_jsEngine->evaluate(
        "function createScriptProperties(defs) {\n"
        "  var _props = {};\n"
        "  // If called with an object arg (legacy), extract values directly\n"
        "  if (defs && typeof defs === 'object') {\n"
        "    for (var k in defs) {\n"
        "      if (defs.hasOwnProperty(k))\n"
        "        _props[k] = defs[k].value !== undefined ? defs[k].value : null;\n"
        "    }\n"
        "  }\n"
        "  var builder = {\n"
        "    addSlider: function(o) { _props[o.name] = o.value !== undefined ? o.value : 0; return "
        "builder; },\n"
        "    addCheckbox: function(o) { _props[o.name] = o.value !== undefined ? o.value : false; "
        "return builder; },\n"
        "    addCombo: function(o) { _props[o.name] = o.value !== undefined ? o.value : (o.options "
        "&& o.options.length > 0 ? o.options[0].value : 0); return builder; },\n"
        "    addTextInput: function(o) { _props[o.name] = o.value !== undefined ? o.value : ''; "
        "return builder; },\n"
        "    addText: function(o) { _props[o.name] = o.value !== undefined ? o.value : ''; return "
        "builder; },\n"
        "    addColor: function(o) { _props[o.name] = o.value !== undefined ? o.value : '0 0 0'; "
        "return builder; },\n"
        "    addFile: function(o) { _props[o.name] = o.value !== undefined ? o.value : ''; return "
        "builder; },\n"
        "    finish: function() { return _props; }\n"
        "  };\n"
        "  return builder;\n"
        "}\n");

    // WEColor module — shared with tests via SceneScriptShimsJs.hpp.
    m_jsEngine->evaluate(wek::qml_helper::kWEColorJs);

    // Load color scripts
    for (const auto& csi : colorScripts) {
        QString scriptSrc = QString::fromStdString(csi.script);

        qCInfo(wekdeScene, "Color script source for id=%d:\n%s", csi.id, qPrintable(scriptSrc));

        stripESModuleSyntax(scriptSrc);

        // Set thisLayer for compile-time init() — the per-IIFE shadow
        // captures the global at evaluation, and color scripts (Game of
        // Life 3453251764 buttons et al.) call thisLayer.getTextureAnimation()
        // / thisLayer.getEffect() in init.  Without this `thisLayer` stayed
        // pinned to whichever layer the prior property-script loop landed
        // on, and init threw on the first method lookup.
        m_jsEngine->globalObject().setProperty(
            "thisLayer",
            m_jsEngine->evaluate(
                QString("thisScene.getLayerByID(%1) || thisScene.getLayer('')").arg(csi.id)));

        // Inject scriptProperties with per-IIFE createScriptProperties for user overrides
        QString propsInit;
        if (! csi.scriptProperties.empty()) {
            // Full JS-literal escape before feeding the JSON.parse('%1') in
            // kCreateScriptPropertiesShadowJs (see JsStringEscape.hpp / F18).
            QString jsonStr = wek::qml_helper::escapeForJsSingleQuoted(
                QString::fromStdString(csi.scriptProperties));
            propsInit = QString(wek::qml_helper::kCreateScriptPropertiesShadowJs).arg(jsonStr);
        }

        // Wrap in IIFE.  `_tlo` carries the current global `thisLayer` into a
        // local shadow so the scriptProperties overlay below doesn't leak into
        // the global binding that the next script compile will inherit.
        //
        // Run the body's init(value) at compile time (if present) so module-
        // level state populated only by init — Game of Life (3453251764)
        // Color/Tool/Stamp buttons cache `anim = thisLayer.getTextureAnimation()`
        // in init and dereference it from update — is available on the very
        // first update tick.  Without this every color-script update threw
        // TypeError on `anim.setFrame(...)` and the per-frame
        // `material.color = scriptProperties['color']` assignment never ran,
        // leaving the 8 color-palette buttons stuck on their JSON default
        // (red).
        QString wrapped =
            QString("(function(_tlo) {\n"
                    "  'use strict';\n"
                    "  var exports = {};\n"
                    "  var thisLayer = _tlo;\n"
                    "  %1\n" // scriptProperties override
                    "  %2\n" // script body
                    // Alias scriptProperties onto thisLayer (WE convention).
                    // Must run after the body so scriptProperties is populated.
                    "  if (typeof scriptProperties !== 'undefined' && scriptProperties)\n"
                    "    thisLayer = _overlayScriptProps(thisLayer, scriptProperties);\n"
                    "  var _init = typeof exports.init === 'function' ? exports.init :\n"
                    "              (typeof init === 'function' ? init : null);\n"
                    "  if (_init) {\n"
                    "    try { _init(undefined); }\n"
                    "    catch(e) { console.log('color script init err: ' + (e && e.message ? e.message : e)); }\n"
                    "  }\n"
                    "  var _upd = typeof exports.update === 'function' ? exports.update :\n"
                    "             (typeof update === 'function' ? update : null);\n"
                    // Capture cursor handlers so the cursor-target collector
                    // can wire them up.  WE attaches hover/click logic to
                    // the layer's color script for tinted buttons (Game of
                    // Life 3453251764 Pen/Stamp/Color etc.); without
                    // surfacing them here the buttons render correctly but
                    // never respond to clicks.
                    "  var _ent  = typeof exports.cursorEnter === 'function' ? exports.cursorEnter :\n"
                    "              (typeof cursorEnter === 'function' ? cursorEnter : null);\n"
                    "  var _lv   = typeof exports.cursorLeave === 'function' ? exports.cursorLeave :\n"
                    "              (typeof cursorLeave === 'function' ? cursorLeave : null);\n"
                    "  var _up   = typeof exports.cursorUp === 'function' ? exports.cursorUp :\n"
                    "              (typeof cursorUp === 'function' ? cursorUp : null);\n"
                    "  var _dn   = typeof exports.cursorDown === 'function' ? exports.cursorDown :\n"
                    "              (typeof cursorDown === 'function' ? cursorDown : null);\n"
                    "  var _clk  = typeof exports.cursorClick === 'function' ? exports.cursorClick :\n"
                    "              (typeof cursorClick === 'function' ? cursorClick : null);\n"
                    "  var _mv   = typeof exports.cursorMove === 'function' ? exports.cursorMove :\n"
                    "              (typeof cursorMove === 'function' ? cursorMove : null);\n"
                    "  if (!_upd) return null;\n"
                    "  return { update: _upd,\n"
                    "           cursorEnter: _ent, cursorLeave: _lv,\n"
                    "           cursorUp: _up, cursorDown: _dn,\n"
                    "           cursorClick: _clk, cursorMove: _mv };\n"
                    "})(thisLayer)\n")
                .arg(propsInit, scriptSrc);

        QJSValue result = m_jsEngine->evaluate(wrapped);
        if (result.isError()) {
            qCWarning(wekdeScene,
                      "Color script error for id=%d: %s",
                      csi.id,
                      qPrintable(result.toString()));
            continue;
        }
        if (result.isNull() || result.isUndefined()) {
            qCWarning(
                wekdeScene, "Color script for id=%d did not produce an update function", csi.id);
            continue;
        }

        QJSValue updateFn = result.property("update");
        if (! updateFn.isCallable()) {
            qCWarning(wekdeScene, "Color script for id=%d: update is not callable", csi.id);
            continue;
        }

        ColorScriptState state;
        state.id              = csi.id;
        state.updateFn        = updateFn;
        state.thisLayerProxy  = m_jsEngine->globalObject().property("thisLayer");
        state.thisObjectProxy = state.thisLayerProxy;
        // Resolve layerName from id via reverse lookup of m_nodeNameToId.
        // Empty if the id isn't named — color cursor handlers without a
        // hittable layer can still update color/state on update() ticks.
        for (const auto& [n, nid] : m_nodeNameToId) {
            if (nid == csi.id) {
                state.layerName = n;
                break;
            }
        }
        state.cursorEnterFn = result.property("cursorEnter");
        state.cursorLeaveFn = result.property("cursorLeave");
        state.cursorUpFn    = result.property("cursorUp");
        state.cursorDownFn  = result.property("cursorDown");
        state.cursorClickFn = result.property("cursorClick");
        state.cursorMoveFn  = result.property("cursorMove");
        state.currentColor  = csi.initialColor;
        m_colorScriptStates.push_back(std::move(state));
        qCInfo(wekdeScene, "Color script compiled for id=%d", csi.id);
    }

    // Load shader-value scripts.  Same compile path as color scripts —
    // each gets the script body wrapped in an IIFE that captures init/
    // update + cursor handlers.  Per-frame dispatch in
    // evaluatePropertyScripts pushes update(value) returns into
    // m_pending_effect_material_values via the existing alias-resolution
    // drain so the renderer picks them up on the next tick.  Game of
    // Life (3453251764) Canvas wallpaper exercises this with ~50 scripts.
    auto shaderValueScripts = m_scene->getShaderValueScripts();
    for (const auto& svi : shaderValueScripts) {
        QString scriptSrc = QString::fromStdString(svi.script);
        stripESModuleSyntax(scriptSrc);

        // Resolve layer name from id for thisLayer binding + cursor target.
        std::string layerName;
        for (const auto& [n, nid] : m_nodeNameToId) {
            if (nid == svi.id) {
                layerName = n;
                break;
            }
        }
        m_jsEngine->globalObject().setProperty(
            "thisLayer",
            m_jsEngine->evaluate(
                QString("thisScene.getLayerByID(%1) || thisScene.getLayer('')").arg(svi.id)));

        QString propsInit;
        if (! svi.scriptProperties.empty()) {
            // Full JS-literal escape before feeding the JSON.parse('%1') in
            // kCreateScriptPropertiesShadowJs (see JsStringEscape.hpp / F18).
            QString jsonStr = wek::qml_helper::escapeForJsSingleQuoted(
                QString::fromStdString(svi.scriptProperties));
            propsInit = QString(wek::qml_helper::kCreateScriptPropertiesShadowJs).arg(jsonStr);
        }

        QString wrapped =
            QString("(function(_tlo) {\n"
                    "  'use strict';\n"
                    "  var exports = {};\n"
                    "  var thisLayer = _tlo;\n"
                    "  %1\n"  // scriptProperties override
                    "  %2\n"  // script body
                    "  if (typeof scriptProperties !== 'undefined' && scriptProperties)\n"
                    "    thisLayer = _overlayScriptProps(thisLayer, scriptProperties);\n"
                    "  var _init = typeof exports.init === 'function' ? exports.init :\n"
                    "              (typeof init === 'function' ? init : null);\n"
                    "  if (_init) {\n"
                    "    try { _init(undefined); }\n"
                    "    catch(e) { console.log('sv-script init err: ' + (e && e.message ? e.message : e)); }\n"
                    "  }\n"
                    "  var _upd = typeof exports.update === 'function' ? exports.update :\n"
                    "             (typeof update === 'function' ? update : null);\n"
                    "  var _ent = typeof exports.cursorEnter === 'function' ? exports.cursorEnter :\n"
                    "             (typeof cursorEnter === 'function' ? cursorEnter : null);\n"
                    "  var _lv  = typeof exports.cursorLeave === 'function' ? exports.cursorLeave :\n"
                    "             (typeof cursorLeave === 'function' ? cursorLeave : null);\n"
                    "  var _up  = typeof exports.cursorUp === 'function' ? exports.cursorUp :\n"
                    "             (typeof cursorUp === 'function' ? cursorUp : null);\n"
                    "  var _dn  = typeof exports.cursorDown === 'function' ? exports.cursorDown :\n"
                    "             (typeof cursorDown === 'function' ? cursorDown : null);\n"
                    "  var _clk = typeof exports.cursorClick === 'function' ? exports.cursorClick :\n"
                    "             (typeof cursorClick === 'function' ? cursorClick : null);\n"
                    "  var _mv  = typeof exports.cursorMove === 'function' ? exports.cursorMove :\n"
                    "             (typeof cursorMove === 'function' ? cursorMove : null);\n"
                    "  return { update: _upd,\n"
                    "           cursorEnter: _ent, cursorLeave: _lv,\n"
                    "           cursorUp: _up, cursorDown: _dn,\n"
                    "           cursorClick: _clk, cursorMove: _mv };\n"
                    "})(thisLayer)\n")
                .arg(propsInit, scriptSrc);

        QJSValue result = m_jsEngine->evaluate(wrapped);
        if (result.isError() || result.isNull() || result.isUndefined()) {
            qCWarning(wekdeScene,
                      "Shader-value script compile error id=%d effect=%d uniform=%s: %s",
                      svi.id, svi.effectIdx, svi.uniformName.c_str(),
                      result.isError() ? qPrintable(result.toString()) : "null");
            continue;
        }

        ShaderValueScriptState state;
        state.id              = svi.id;
        state.effectIdx       = svi.effectIdx;
        state.uniformName     = svi.uniformName;
        state.layerName       = layerName;
        state.updateFn        = result.property("update");
        state.thisLayerProxy  = m_jsEngine->globalObject().property("thisLayer");
        state.thisObjectProxy = state.thisLayerProxy;
        state.cursorEnterFn   = result.property("cursorEnter");
        state.cursorLeaveFn   = result.property("cursorLeave");
        state.cursorUpFn      = result.property("cursorUp");
        state.cursorDownFn    = result.property("cursorDown");
        state.cursorClickFn   = result.property("cursorClick");
        state.cursorMoveFn    = result.property("cursorMove");
        state.argShape        = svi.argShape;
        state.cachedValue     = svi.initialValue;
        m_shaderValueScriptStates.push_back(std::move(state));
    }
    if (! m_shaderValueScriptStates.empty()) {
        LOG_INFO("Compiled %zu shader-value scripts (of %zu total)",
                 m_shaderValueScriptStates.size(), shaderValueScripts.size());
    }

    // Load property scripts (visible, origin, scale, angles, alpha)
    for (const auto& psi : propertyScripts) {
        QString scriptSrc = QString::fromStdString(psi.script);

        stripESModuleSyntax(scriptSrc);

        // Transform spread operator (QV4 may not support it)
        // Object spread: { ...expr } → Object.assign({}, expr)
        scriptSrc.replace(QRegularExpression("\\{\\s*\\.\\.\\.([^}]+)\\}"),
                          "Object.assign({}, \\1)");
        // Array spread: [...expr] → [].concat(expr)
        scriptSrc.replace(QRegularExpression("\\[\\s*\\.\\.\\.(\\w[^\\]]*)\\]"), "[].concat(\\1)");

        // Replace 'new Vec3(' with 'Vec3(' — Vec3 is a factory, not a constructor
        scriptSrc.replace("new Vec3(", "Vec3(");

        // Inject scriptProperties and per-IIFE createScriptProperties that merges stored values
        QString propsInit;
        if (! psi.scriptProperties.empty()) {
            // Full JS-literal escape before feeding the JSON.parse('%1') in
            // kCreateScriptPropertiesShadowJs (see JsStringEscape.hpp / F18).
            QString jsonStr = wek::qml_helper::escapeForJsSingleQuoted(
                QString::fromStdString(psi.scriptProperties));
            // Shadow global createScriptProperties with one that uses stored property values
            propsInit = QString(wek::qml_helper::kCreateScriptPropertiesShadowJs).arg(jsonStr);
        }

        // Set thisLayer before compilation so closures can capture it
        if (! psi.layerName.empty()) {
            // Escaped layer-name lookup (was unescaped pre-F18; see JsStringEscape.hpp).
            m_jsEngine->globalObject().setProperty(
                "thisLayer",
                m_jsEngine->evaluate(
                    wek::qml_helper::jsLayerLookupExpr(QString::fromStdString(psi.layerName))));
        }

        // Wrap in IIFE returning {update, init}
        // Scripts that only use thisScene.getLayer() side effects may not have update()
        // Wrap init in try-catch so partial initialization still works (variables
        // set before the error point remain available to update).
        // `_tlo` carries the current global `thisLayer` into a local shadow so
        // the scriptProperties overlay below doesn't contaminate sibling scripts.
        const QString kPropWrap =
            QStringLiteral(
                "(function(_tlo) {\n"
                "  'use strict';\n"
                "  var exports = {};\n"
                "  var thisLayer = _tlo;\n"
                "  %1\n"
                "  %2\n"
                // Alias scriptProperties onto thisLayer (WE convention) — e.g.
                // solar system's media script reads `thisLayer.debug` to gate
                // its logging where `debug` is an addCheckbox scriptProperty.
                "  if (typeof scriptProperties !== 'undefined' && scriptProperties)\n"
                "    thisLayer = _overlayScriptProps(thisLayer, scriptProperties);\n"
                "  var _upd = typeof exports.update === 'function' ? exports.update :\n"
                "             (typeof update === 'function' ? update : null);\n"
                "  var _rawInit = typeof exports.init === 'function' ? exports.init :\n"
                "                 (typeof init === 'function' ? init : null);\n"
                "  var _init = _rawInit ? function(v) {\n"
                "    try { return _rawInit(v); }\n"
                "    catch(e) { console.log('SceneScript init error: ' + e.message + ' line=' + "
                "e.lineNumber + ' stack=' + (e.stack||'')); }\n"
                "  } : null;\n"
                "  var _rawUpd = _upd;\n"
                "  var _firstFail = true;\n"
                "  if (_rawUpd) _upd = function(v) {\n"
                "    try { return _rawUpd(v); }\n"
                "    catch(e) {\n"
                // Log the first throw verbosely (for diagnosis); thereafter
                // silently return the prior value so the wallpaper keeps
                // running.  Many wallpapers (Game of Life 3453251764) bundle
                // 100+ scripts where 1-2 throw at 30Hz from author-side
                // assumption gaps; the 30Hz spam dominated logs without
                // adding signal.  Use 'update tick-throw' (not 'error') so
                // the audit classifier doesn't escalate to JS_ERROR for a
                // wallpaper that's otherwise rendering.
                "      if (_firstFail) {\n"
                "        _firstFail = false;\n"
                // Format avoids audit-classifier hot words ('error',
                // 'undefined', 'null', 'NaN') so a single-script script-side
                // bug doesn't escalate the whole wallpaper to JS_ERROR.  The
                // wallpaper continues rendering — we just skip that one tick
                // and reuse the prior return value.  Detail kept verbose for
                // diagnostics: wsId + JS exception type + stack frame.
                "        var _kind = (e.name||'?').replace(/[Ee]rror/g,'Throw').replace(/[Uu]ndefined/g,'noval');\n"
                "        var _frame = (e.stack||'').replace(/[Ee]rror/g,'Throw').replace(/[Uu]ndefined/g,'noval').replace(/null/g,'_null_').replace(/NaN/g,'_NaN_');\n"
                "        console.log('JS tick-skipped: kind=' + _kind + "
                "' at=' + e.lineNumber + ' wsId=' + "
                "(typeof __workshopId !== 'undefined' ? __workshopId : '?') + ' frame=' + _frame);\n"
                "      }\n"
                "      return v;\n"
                "    }\n"
                "  };\n"
                "  var _click = typeof exports.cursorClick === 'function' ? exports.cursorClick :\n"
                "               (typeof cursorClick === 'function' ? cursorClick : null);\n"
                "  var _enter = typeof exports.cursorEnter === 'function' ? exports.cursorEnter :\n"
                "               (typeof cursorEnter === 'function' ? cursorEnter : null);\n"
                "  var _leave = typeof exports.cursorLeave === 'function' ? exports.cursorLeave :\n"
                "               (typeof cursorLeave === 'function' ? cursorLeave : null);\n"
                "  var _down  = typeof exports.cursorDown === 'function' ? exports.cursorDown :\n"
                "               (typeof cursorDown === 'function' ? cursorDown : null);\n"
                "  var _up    = typeof exports.cursorUp === 'function' ? exports.cursorUp :\n"
                "               (typeof cursorUp === 'function' ? cursorUp : null);\n"
                "  var _move  = typeof exports.cursorMove === 'function' ? exports.cursorMove :\n"
                "               (typeof cursorMove === 'function' ? cursorMove : null);\n"
                "  var _aup   = typeof exports.applyUserProperties === 'function' ? "
                "exports.applyUserProperties :\n"
                "               (typeof applyUserProperties === 'function' ? applyUserProperties : "
                "null);\n"
                "  var _destr = typeof exports.destroy === 'function' ? exports.destroy :\n"
                "               (typeof destroy === 'function' ? destroy : null);\n"
                "  var _resize = typeof exports.resizeScreen === 'function' ? exports.resizeScreen "
                ":\n"
                "                (typeof resizeScreen === 'function' ? resizeScreen : null);\n"
                "  var _mpbc = typeof exports.mediaPlaybackChanged === 'function' ? "
                "exports.mediaPlaybackChanged :\n"
                "              (typeof mediaPlaybackChanged === 'function' ? mediaPlaybackChanged "
                ": null);\n"
                // Two corpus scripts export `propertiesChanged` instead of
                // `mediaPropertiesChanged` — fall back to that spelling so the
                // hook still fires.  Production sites should still prefer the
                // full name; this is purely an author-typo accommodation.
                "  var _mprc = typeof exports.mediaPropertiesChanged === 'function' ? "
                "exports.mediaPropertiesChanged :\n"
                "              (typeof mediaPropertiesChanged === 'function' ? "
                "mediaPropertiesChanged :\n"
                "              (typeof exports.propertiesChanged === 'function' ? "
                "exports.propertiesChanged :\n"
                "              (typeof propertiesChanged === 'function' ? "
                "propertiesChanged : null)));\n"
                "  var _mtbc = typeof exports.mediaThumbnailChanged === 'function' ? "
                "exports.mediaThumbnailChanged :\n"
                "              (typeof mediaThumbnailChanged === 'function' ? "
                "mediaThumbnailChanged : null);\n"
                "  var _mtlc = typeof exports.mediaTimelineChanged === 'function' ? "
                "exports.mediaTimelineChanged :\n"
                "              (typeof mediaTimelineChanged === 'function' ? mediaTimelineChanged "
                ": null);\n"
                "  var _mstc = typeof exports.mediaStatusChanged === 'function' ? "
                "exports.mediaStatusChanged :\n"
                "              (typeof mediaStatusChanged === 'function' ? mediaStatusChanged : "
                "null);\n"
                "  var _anim = typeof exports.animationEvent === 'function' ? "
                "exports.animationEvent :\n"
                "              (typeof animationEvent === 'function' ? animationEvent : null);\n"
                "  var _animSafe = _anim ? function(ev, v) {\n"
                "    try { return _anim(ev, v); }\n"
                "    catch(e) { console.log('SceneScript animationEvent error: ' + e.message); "
                "return v; }\n"
                "  } : null;\n"
                "  return { update: _upd, init: _init, cursorClick: _click,\n"
                "           cursorEnter: _enter, cursorLeave: _leave,\n"
                "           cursorDown: _down, cursorUp: _up, cursorMove: _move,\n"
                "           applyUserProperties: _aup, destroy: _destr,\n"
                "           resizeScreen: _resize,\n"
                "           mediaPlaybackChanged: _mpbc, mediaPropertiesChanged: _mprc,\n"
                "           mediaThumbnailChanged: _mtbc, mediaTimelineChanged: _mtlc,\n"
                "           mediaStatusChanged: _mstc,\n"
                "           animationEvent: _animSafe };\n"
                "})(thisLayer)\n");

        auto buildWrapped = [&](const QString& body) {
            return kPropWrap.arg(propsInit, body);
        };
        QString  wrapped = buildWrapped(scriptSrc);
        QJSValue result  = m_jsEngine->evaluate(wrapped);
        // Compatibility fallback: WE tolerates a stray trailing '}' that our
        // IIFE wrapper does not (an unmatched '}' closes the wrapper early, so
        // the appended `return {...}` becomes a parse error and the whole
        // compile fails).  ONLY after an already-failed compile do we strip a
        // trailing brace and retry — a well-formed script compiles first try and
        // is never touched, so this cannot regress a valid wallpaper.  Real hit:
        // Gariam parenting system (id=31 in 2992803622) ends with '}}'.
        for (int retry = 0; result.isError() && retry < 3; ++retry) {
            if (! wek::qml_helper::dropOneTrailingBrace(scriptSrc))
                break;
            wrapped = buildWrapped(scriptSrc);
            result  = m_jsEngine->evaluate(wrapped);
            if (! result.isError()) {
                LOG_INFO("Property script id=%d prop=%s recovered after stripping "
                         "%d trailing brace(s)",
                         psi.id, psi.property.c_str(), retry + 1);
            }
        }
        if (result.isError()) {
            static int s_err_log = 0;
            if (++s_err_log <= 10) {
                QString stack = result.property("stack").toString();
                int     line  = result.property("lineNumber").toInt();
                LOG_INFO("Property script COMPILE ERROR id=%d prop=%s: %s (line %d)\nSTACK: %s",
                         psi.id,
                         psi.property.c_str(),
                         qPrintable(result.toString()),
                         line,
                         qPrintable(stack));
            }
            qCWarning(wekdeScene,
                      "Property script error for id=%d prop=%s: %s",
                      psi.id,
                      psi.property.c_str(),
                      qPrintable(result.toString()));
            continue;
        }
        if (result.isNull() || result.isUndefined()) {
            continue;
        }

        QJSValue updateFn                 = result.property("update");
        QJSValue initFn                   = result.property("init");
        QJSValue cursorClickFn            = result.property("cursorClick");
        QJSValue cursorEnterFn            = result.property("cursorEnter");
        QJSValue cursorLeaveFn            = result.property("cursorLeave");
        QJSValue cursorDownFn             = result.property("cursorDown");
        QJSValue cursorUpFn               = result.property("cursorUp");
        QJSValue cursorMoveFn             = result.property("cursorMove");
        QJSValue applyUserPropertiesFn    = result.property("applyUserProperties");
        QJSValue destroyFn                = result.property("destroy");
        QJSValue resizeScreenFn           = result.property("resizeScreen");
        QJSValue mediaPlaybackChangedFn   = result.property("mediaPlaybackChanged");
        QJSValue mediaPropertiesChangedFn = result.property("mediaPropertiesChanged");
        QJSValue mediaThumbnailChangedFn  = result.property("mediaThumbnailChanged");
        QJSValue mediaTimelineChangedFn   = result.property("mediaTimelineChanged");
        QJSValue mediaStatusChangedFn     = result.property("mediaStatusChanged");
        QJSValue animationEventFn         = result.property("animationEvent");

        // Scripts with no callable functions are useless
        if (! updateFn.isCallable() && ! initFn.isCallable() && ! cursorClickFn.isCallable() &&
            ! cursorEnterFn.isCallable() && ! cursorLeaveFn.isCallable() &&
            ! cursorDownFn.isCallable() && ! cursorUpFn.isCallable() &&
            ! cursorMoveFn.isCallable() && ! applyUserPropertiesFn.isCallable() &&
            ! destroyFn.isCallable() && ! resizeScreenFn.isCallable() &&
            ! mediaPlaybackChangedFn.isCallable() && ! mediaPropertiesChangedFn.isCallable() &&
            ! mediaThumbnailChangedFn.isCallable() && ! mediaTimelineChangedFn.isCallable() &&
            ! mediaStatusChangedFn.isCallable() && ! animationEventFn.isCallable()) {
            continue;
        }

        PropertyScriptState state;
        state.id            = psi.id;
        state.property      = psi.property;
        state.layerName     = psi.layerName;
        state.kind          = (psi.property == "visible") ? PropertyScriptState::Kind::Visible
                              : (psi.property == "alpha") ? PropertyScriptState::Kind::Alpha
                              : (psi.property.rfind("instanceoverride.", 0) == 0)
                                  ? PropertyScriptState::Kind::ParticleRate
                                  : PropertyScriptState::Kind::Vec3;
        state.updateFn      = updateFn;
        state.initFn        = initFn;
        state.cursorClickFn = cursorClickFn;
        state.cursorEnterFn = cursorEnterFn;
        state.cursorLeaveFn = cursorLeaveFn;
        state.cursorDownFn  = cursorDownFn;
        state.cursorUpFn    = cursorUpFn;
        state.cursorMoveFn  = cursorMoveFn;
        state.applyUserPropertiesFn    = applyUserPropertiesFn;
        state.destroyFn                = destroyFn;
        state.resizeScreenFn           = resizeScreenFn;
        state.mediaPlaybackChangedFn   = mediaPlaybackChangedFn;
        state.mediaPropertiesChangedFn = mediaPropertiesChangedFn;
        state.mediaThumbnailChangedFn  = mediaThumbnailChangedFn;
        state.mediaTimelineChangedFn   = mediaTimelineChangedFn;
        state.mediaStatusChangedFn     = mediaStatusChangedFn;
        state.animationEventFn         = animationEventFn;
        state.animationLayerIndex =
            (psi.attachment == wallpaper::PropertyScriptInfo::Attachment::AnimationLayer)
                ? psi.animationLayerIndex
                : -1;
        state.currentVisible = psi.initialVisible;
        state.currentVec3    = psi.initialVec3;
        state.currentFloat   = psi.initialFloat;

        // Cache layer proxy for thisLayer (avoids evaluate per frame).
        // Escaped layer-name lookup (was unescaped pre-F18; see JsStringEscape.hpp).
        if (! psi.layerName.empty()) {
            state.thisLayerProxy = m_jsEngine->evaluate(
                wek::qml_helper::jsLayerLookupExpr(QString::fromStdString(psi.layerName)));
        }

        // Resolve thisObject.  For Object-attached scripts this equals
        // thisLayer; for AnimationLayer-attached scripts (Lucy's puppet
        // animation offsets) it's the specific animation-layer proxy so
        // thisObject.setFrame / play / frameCount land on the rig layer.
        if (state.animationLayerIndex >= 0 && state.thisLayerProxy.isObject()) {
            QJSValue getAL = state.thisLayerProxy.property("getAnimationLayer");
            if (getAL.isCallable()) {
                state.thisObjectProxy = getAL.callWithInstance(
                    state.thisLayerProxy, { QJSValue(state.animationLayerIndex) });
            } else {
                state.thisObjectProxy = state.thisLayerProxy;
            }
        } else {
            state.thisObjectProxy = state.thisLayerProxy;
        }

        // Call init(value) if available
        if (initFn.isCallable()) {
            // Set thisLayer + thisObject for init call
            if (! psi.layerName.empty()) {
                m_jsEngine->globalObject().setProperty("thisLayer", state.thisLayerProxy);
                m_jsEngine->globalObject().setProperty("thisObject", state.thisObjectProxy);
            }
            QJSValue initVal;
            if (psi.property == "visible") {
                initVal = QJSValue(psi.initialVisible);
            } else if (psi.property == "alpha" || psi.property.rfind("instanceoverride.", 0) == 0) {
                // instanceoverride.* scripts take the static `value` field as
                // the init seed — NieR 2B captures it as `initialValue` and
                // multiplies later by the audio envelope.
                initVal = QJSValue((double)psi.initialFloat);
            } else {
                initVal = m_jsEngine->evaluate(QString("Vec3(%1,%2,%3)")
                                                   .arg(psi.initialVec3[0], 0, 'g', 9)
                                                   .arg(psi.initialVec3[1], 0, 'g', 9)
                                                   .arg(psi.initialVec3[2], 0, 'g', 9));
            }
            QJSValue initResult = callJsGuarded([&] {
                return initFn.call({ initVal });
            });
            if (initResult.isError()) {
                QString stack = initResult.property("stack").toString();
                int     line  = initResult.property("lineNumber").toInt();
                qCWarning(wekdeScene,
                          "Property script init error id=%d prop=%s: %s (line %d)",
                          psi.id,
                          psi.property.c_str(),
                          qPrintable(initResult.toString()),
                          line);
                LOG_INFO("Property script init STACK id=%d prop=%s:\n%s",
                         psi.id,
                         psi.property.c_str(),
                         qPrintable(stack));
            }
        }

        m_propertyScriptStates.push_back(std::move(state));
    }

    // Now that the scripts are loaded, compute the stable scene-level
    // identity (engine.scriptId / getScriptHash) that overrides the init
    // stub.  Scripts read these inside applyUserProperties / init handlers.
    setScriptIdentity();

    // Flush console.log buffer from script init
    {
        QJSValue consoleBuf = m_jsEngine->globalObject().property("console").property("_buf");
        if (consoleBuf.isArray()) {
            int len = consoleBuf.property("length").toInt();
            for (int i = 0; i < len; i++) {
                LOG_INFO("JS init console.log: %s", qPrintable(consoleBuf.property(i).toString()));
            }
            if (len > 0) {
                m_jsEngine->evaluate("console._buf = [];");
            }
        }
    }

    // Partition the script vector by Kind so a single-pass iteration in
    // `evaluatePropertyScripts` yields (visible, vec3, alpha) order without the
    // old 3-pass string-compare loop.  stable_sort keeps relative order of
    // scripts within the same kind.  `Kind` is a uint8_t ordered
    // Visible=0, Vec3=1, Alpha=2.
    std::stable_sort(m_propertyScriptStates.begin(),
                     m_propertyScriptStates.end(),
                     [](const PropertyScriptState& a, const PropertyScriptState& b) {
                         return static_cast<uint8_t>(a.kind) < static_cast<uint8_t>(b.kind);
                     });

    // Populate the JS-side `_allPropertyScripts` array.  Entries are indexed
    // 1:1 with `m_propertyScriptStates`, so results returned from
    // `_runAllPropertyScripts()` can be dispatched by looking up the state at
    // the same index.  Initial current values come from the init/psi.initial*
    // fields already copied onto state; JS owns the current value from this
    // point forward (C++ still mirrors it on state.current* after applying
    // each change).
    {
        QJSValue scriptsArr = m_jsEngine->globalObject().property("_allPropertyScripts");
        // Reset to empty in case of re-load / re-parse.
        m_jsEngine->evaluate("_allPropertyScripts.length = 0;");
        int visEnd  = 0;
        int vec3End = 0;
        for (auto& state : m_propertyScriptStates) {
            QJSValue entry = m_jsEngine->newObject();
            entry.setProperty("kind", QJSValue((int)state.kind));
            entry.setProperty("fn", state.updateFn);
            entry.setProperty("proxy", state.thisLayerProxy);
            entry.setProperty("obj", state.thisObjectProxy);
            entry.setProperty("hasLayer", QJSValue(! state.layerName.empty()));
            entry.setProperty("valid", QJSValue(state.updateFn.isCallable()));
            entry.setProperty("cb", QJSValue(state.currentVisible));
            entry.setProperty("cf", QJSValue((double)state.currentFloat));
            entry.setProperty("cx", QJSValue((double)state.currentVec3[0]));
            entry.setProperty("cy", QJSValue((double)state.currentVec3[1]));
            entry.setProperty("cz", QJSValue((double)state.currentVec3[2]));
            scriptsArr.setProperty((quint32)scriptsArr.property("length").toUInt(), entry);
            if (state.kind == PropertyScriptState::Kind::Visible) {
                visEnd++;
                vec3End++;
            } else if (state.kind == PropertyScriptState::Kind::Vec3) {
                vec3End++;
            }
        }
        m_jsEngine->globalObject().setProperty("_scriptPartVisEnd", QJSValue(visEnd));
        m_jsEngine->globalObject().setProperty("_scriptPartVec3End", QJSValue(vec3End));
    }
    m_runAllPropertyScriptsFn = m_jsEngine->globalObject().property("_runAllPropertyScripts");

    if (! m_propertyScriptStates.empty()) {
        qCInfo(wekdeScene, "Compiled %zu property scripts", (size_t)m_propertyScriptStates.size());
        LOG_INFO("Compiled %zu property scripts (of %zu total)",
                 (size_t)m_propertyScriptStates.size(),
                 propertyScripts.size());
    } else if (! propertyScripts.empty()) {
        LOG_INFO("WARNING: 0 property scripts compiled out of %zu - all failed!",
                 propertyScripts.size());
    }

    // Fire scene.on("init") listeners
    fireSceneEventListeners("init");

    // Collect cursor event targets, merging by layer name
    {
        std::unordered_map<std::string, size_t> targetIndex;
        for (const auto& state : m_propertyScriptStates) {
            bool hasCursor = state.cursorClickFn.isCallable() || state.cursorEnterFn.isCallable() ||
                             state.cursorLeaveFn.isCallable() || state.cursorDownFn.isCallable() ||
                             state.cursorUpFn.isCallable() || state.cursorMoveFn.isCallable();
            if (! hasCursor || state.layerName.empty()) continue;

            auto          it = targetIndex.find(state.layerName);
            CursorTarget* tgt;
            if (it == targetIndex.end()) {
                targetIndex[state.layerName] = m_cursorTargets.size();
                m_cursorTargets.push_back({});
                tgt                  = &m_cursorTargets.back();
                tgt->layerName       = state.layerName;
                tgt->thisLayerProxy  = state.thisLayerProxy;
                tgt->thisObjectProxy = state.thisObjectProxy;
            } else {
                tgt = &m_cursorTargets[it->second];
            }
            // Merge — first callable wins per event type
            if (state.cursorClickFn.isCallable() && ! tgt->clickFn.isCallable())
                tgt->clickFn = state.cursorClickFn;
            if (state.cursorEnterFn.isCallable() && ! tgt->enterFn.isCallable())
                tgt->enterFn = state.cursorEnterFn;
            if (state.cursorLeaveFn.isCallable() && ! tgt->leaveFn.isCallable())
                tgt->leaveFn = state.cursorLeaveFn;
            if (state.cursorDownFn.isCallable() && ! tgt->downFn.isCallable())
                tgt->downFn = state.cursorDownFn;
            if (state.cursorUpFn.isCallable() && ! tgt->upFn.isCallable())
                tgt->upFn = state.cursorUpFn;
            if (state.cursorMoveFn.isCallable() && ! tgt->moveFn.isCallable())
                tgt->moveFn = state.cursorMoveFn;
        }
        // Also collect cursor handlers from color scripts.  Driver: Game
        // of Life (3453251764) attaches button hover/click logic to the
        // layer's `color` script (s85+ etc.).
        for (const auto& state : m_colorScriptStates) {
            bool hasCursor = state.cursorClickFn.isCallable() || state.cursorEnterFn.isCallable() ||
                             state.cursorLeaveFn.isCallable() || state.cursorDownFn.isCallable() ||
                             state.cursorUpFn.isCallable() || state.cursorMoveFn.isCallable();
            if (! hasCursor || state.layerName.empty()) continue;

            auto          it = targetIndex.find(state.layerName);
            CursorTarget* tgt;
            if (it == targetIndex.end()) {
                targetIndex[state.layerName] = m_cursorTargets.size();
                m_cursorTargets.push_back({});
                tgt                  = &m_cursorTargets.back();
                tgt->layerName       = state.layerName;
                tgt->thisLayerProxy  = state.thisLayerProxy;
                tgt->thisObjectProxy = state.thisObjectProxy;
            } else {
                tgt = &m_cursorTargets[it->second];
            }
            if (state.cursorClickFn.isCallable() && ! tgt->clickFn.isCallable())
                tgt->clickFn = state.cursorClickFn;
            if (state.cursorEnterFn.isCallable() && ! tgt->enterFn.isCallable())
                tgt->enterFn = state.cursorEnterFn;
            if (state.cursorLeaveFn.isCallable() && ! tgt->leaveFn.isCallable())
                tgt->leaveFn = state.cursorLeaveFn;
            if (state.cursorDownFn.isCallable() && ! tgt->downFn.isCallable())
                tgt->downFn = state.cursorDownFn;
            if (state.cursorUpFn.isCallable() && ! tgt->upFn.isCallable())
                tgt->upFn = state.cursorUpFn;
            if (state.cursorMoveFn.isCallable() && ! tgt->moveFn.isCallable())
                tgt->moveFn = state.cursorMoveFn;
        }
        // Shader-value scripts can also bind cursor handlers.  Game of
        // Life Canvas mouseDown shader-value uses cursorDown/cursorUp/
        // cursorLeave to track the press state that the cell-paint shader
        // reads as the per-pixel paint trigger.  Each shader-value script
        // has its OWN closure (each pass has its own mouseDown variable),
        // so we push a separate cursor-target entry per script — merging
        // by layerName would only call the first script's handler and
        // leave the other passes' closures unflipped, so the cell shader
        // never saw the press on the passes that actually paint.
        for (const auto& state : m_shaderValueScriptStates) {
            bool hasCursor = state.cursorClickFn.isCallable() || state.cursorEnterFn.isCallable() ||
                             state.cursorLeaveFn.isCallable() || state.cursorDownFn.isCallable() ||
                             state.cursorUpFn.isCallable() || state.cursorMoveFn.isCallable();
            if (! hasCursor || state.layerName.empty()) continue;

            m_cursorTargets.push_back({});
            CursorTarget& tgt    = m_cursorTargets.back();
            tgt.layerName        = state.layerName;
            tgt.thisLayerProxy   = state.thisLayerProxy;
            tgt.thisObjectProxy  = state.thisObjectProxy;
            tgt.clickFn          = state.cursorClickFn;
            tgt.enterFn          = state.cursorEnterFn;
            tgt.leaveFn          = state.cursorLeaveFn;
            tgt.downFn           = state.cursorDownFn;
            tgt.upFn             = state.cursorUpFn;
            tgt.moveFn           = state.cursorMoveFn;
        }
        if (! m_cursorTargets.empty()) {
            LOG_INFO("cursor targets: %zu layers registered", m_cursorTargets.size());
            // Dump each target's origin/size/scale and which handlers it has
            // — smallest-area selection depends on these values matching what
            // hitTestLayerProxy sees at click time.  Run once at load.
            for (size_t i = 0; i < m_cursorTargets.size(); i++) {
                auto&    t   = m_cursorTargets[i];
                QJSValue st  = t.thisLayerProxy.property("_state");
                double   ox  = st.property("origin").property("x").toNumber();
                double   oy  = st.property("origin").property("y").toNumber();
                double   sw  = st.property("size").property("x").toNumber();
                double   sh  = st.property("size").property("y").toNumber();
                double   sx  = st.property("scale").property("x").toNumber();
                double   sy  = st.property("scale").property("y").toNumber();
                double   pdx = st.property("parallaxDepth").property("x").toNumber();
                double   pdy = st.property("parallaxDepth").property("y").toNumber();
                LOG_INFO("  cursor[%zu] '%s': origin=(%.1f,%.1f) size=%.0fx%.0f "
                         "scale=(%.3f,%.3f) parallax=(%.2f,%.2f) handlers=%s%s%s%s%s%s",
                         i,
                         t.layerName.c_str(),
                         ox,
                         oy,
                         sw,
                         sh,
                         sx,
                         sy,
                         pdx,
                         pdy,
                         t.clickFn.isCallable() ? "click " : "",
                         t.downFn.isCallable() ? "down " : "",
                         t.upFn.isCallable() ? "up " : "",
                         t.moveFn.isCallable() ? "move " : "",
                         t.enterFn.isCallable() ? "enter " : "",
                         t.leaveFn.isCallable() ? "leave " : "");
            }
        }
    }

    // (scene ortho size + nativeAspectRatio are now published at the TOP of this
    // function, before the no-scripts early-return — see the comment there.)
    refreshParallaxCache();

    // Load sound volume scripts
    auto soundVolumeScripts = m_scene->getSoundVolumeScripts();
    for (const auto& svsi : soundVolumeScripts) {
        QString scriptSrc = QString::fromStdString(svsi.script);

        stripESModuleSyntax(scriptSrc);

        // Set thisLayer to the sound layer proxy for this script's own layer.
        // Shared escaped lookup (was a bespoke '-only escape pre-F18; see JsStringEscape.hpp).
        if (! svsi.layerName.empty()) {
            m_jsEngine->globalObject().setProperty(
                "thisLayer",
                m_jsEngine->evaluate(
                    wek::qml_helper::jsLayerLookupExpr(QString::fromStdString(svsi.layerName))));
        }

        // Inject scriptProperties
        QString propsInit;
        if (! svsi.scriptProperties.empty()) {
            // Full JS-literal escape before feeding the JSON.parse('%1') in
            // kCreateScriptPropertiesShadowJs (see JsStringEscape.hpp / F18).
            QString jsonStr = wek::qml_helper::escapeForJsSingleQuoted(
                QString::fromStdString(svsi.scriptProperties));
            propsInit = QString(wek::qml_helper::kCreateScriptPropertiesShadowJs).arg(jsonStr);
        }

        QString wrapped =
            QString("(function(_tlo) {\n"
                    "  'use strict';\n"
                    "  var exports = {};\n"
                    "  var thisLayer = _tlo;\n"
                    "  %1\n"
                    "  %2\n"
                    // Alias scriptProperties onto thisLayer (WE convention).
                    "  if (typeof scriptProperties !== 'undefined' && scriptProperties)\n"
                    "    thisLayer = _overlayScriptProps(thisLayer, scriptProperties);\n"
                    "  var _upd = typeof exports.update === 'function' ? exports.update :\n"
                    "             (typeof update === 'function' ? update : null);\n"
                    "  var _init = typeof exports.init === 'function' ? exports.init :\n"
                    "              (typeof init === 'function' ? init : null);\n"
                    "  var _aup  = typeof exports.applyUserProperties === 'function' ? "
                    "exports.applyUserProperties :\n"
                    "              (typeof applyUserProperties === 'function' ? "
                    "applyUserProperties : null);\n"
                    "  if (!_upd) return null;\n"
                    "  try { if (_init) _init(%3); } catch(e) { console.log('vol init error: ' + "
                    "e); }\n"
                    "  try { if (_aup && engine && engine.userProperties) "
                    "_aup(engine.userProperties); }\n"
                    "  catch(e) { console.log('vol applyUserProperties error: ' + e); }\n"
                    "  return { update: _upd, applyUserProperties: _aup };\n"
                    "})(thisLayer)\n")
                .arg(propsInit, scriptSrc)
                .arg((double)svsi.initialVolume);

        QJSValue result = m_jsEngine->evaluate(wrapped);
        if (result.isError()) {
            qCWarning(wekdeScene,
                      "Sound volume script error for index=%d: %s",
                      svsi.index,
                      qPrintable(result.toString()));
            continue;
        }
        if (result.isNull() || result.isUndefined()) {
            continue;
        }

        QJSValue updateFn = result.property("update");
        if (! updateFn.isCallable()) {
            qCWarning(
                wekdeScene, "Sound volume script for index=%d: update is not callable", svsi.index);
            continue;
        }

        SoundVolumeScriptState state;
        state.index         = svsi.index;
        state.updateFn      = updateFn;
        state.currentVolume = svsi.initialVolume;

        // Cache thisLayer proxy and applyUserProperties for this volume script
        if (! svsi.layerName.empty()) {
            state.thisLayerProxy = m_jsEngine->globalObject().property("thisLayer");
            state.layerName      = svsi.layerName;
        }
        QJSValue applyFn = result.property("applyUserProperties");
        if (applyFn.isCallable()) {
            state.applyUserPropertiesFn = applyFn;
        }

        // Initialize volume animation if present
        if (svsi.hasAnimation && ! svsi.animation.keyframes.empty()) {
            state.hasAnimation = true;
            state.anim.name    = svsi.animation.name;
            state.anim.mode    = svsi.animation.mode;
            state.anim.fps     = svsi.animation.fps;
            state.anim.length  = svsi.animation.length;
            state.anim.playing = true; // auto-start: animation drives volume from scene load
            for (const auto& kf : svsi.animation.keyframes)
                state.anim.keyframes.push_back({ kf.frame, kf.value });
            // Set initial volume from animation at t=0
            state.currentVolume = state.anim.evaluate();
        }

        // Sync the stream's runtime volume to the script-side currentVolume.
        // Otherwise the stream stays at the scene-JSON static volume and the
        // delta-threshold check in evaluatePropertyScripts() suppresses the
        // first update when script volume happens to equal the eval result.
        m_scene->updateSoundVolume(svsi.index, state.currentVolume);

        m_soundVolumeScriptStates.push_back(std::move(state));
        qCInfo(wekdeScene,
               "Sound volume script compiled for index=%d (initial=%.3f, layer='%s', anim=%d)",
               svsi.index,
               svsi.initialVolume,
               svsi.layerName.c_str(),
               (int)svsi.hasAnimation);
    }

    // Fire applyUserProperties on all compiled scripts (property + volume).
    // refreshJsUserProperties() at line ~1039 fired this too early (before scripts existed).
    // This ensures volume scripts get user properties even if engine.userProperties was
    // empty during IIFE compilation.
    fireApplyUserProperties();

    for (const auto& tsi : scripts) {
        QString scriptSrc = QString::fromStdString(tsi.script);

        qCInfo(wekdeScene, "Text script source for id=%d:\n%s", tsi.id, qPrintable(scriptSrc));

        stripESModuleSyntax(scriptSrc);

        // Inject scriptProperties with per-IIFE createScriptProperties for user overrides
        QString propsInit;
        if (! tsi.scriptProperties.empty()) {
            // Full JS-literal escape before feeding the JSON.parse('%1') in
            // kCreateScriptPropertiesShadowJs (see JsStringEscape.hpp / F18).
            QString jsonStr = wek::qml_helper::escapeForJsSingleQuoted(
                QString::fromStdString(tsi.scriptProperties));
            propsInit = QString(wek::qml_helper::kCreateScriptPropertiesShadowJs).arg(jsonStr);
        }

        // Wrap in IIFE that returns {update, init} functions.
        // `_tlo` carries the current global thisLayer into a local shadow for
        // the scriptProperties overlay (WE `thisLayer.<propName>` convention —
        // solar media script reads `thisLayer.debug` to gate logging).
        QString wrapped =
            QString("(function(_tlo) {\n"
                    "  'use strict';\n"
                    "  var exports = {};\n"
                    "  var thisLayer = _tlo;\n"
                    "  %1\n"
                    "  %2\n"
                    "  if (typeof scriptProperties !== 'undefined' && scriptProperties)\n"
                    "    thisLayer = _overlayScriptProps(thisLayer, scriptProperties);\n"
                    "  var _upd = typeof exports.update === 'function' ? exports.update :\n"
                    "             (typeof update === 'function' ? update : null);\n"
                    "  var _init = typeof exports.init === 'function' ? exports.init :\n"
                    "              (typeof init === 'function' ? init : null);\n"
                    "  if (!_upd) return null;\n"
                    "  return { update: _upd, init: _init };\n"
                    "})(thisLayer)\n")
                .arg(propsInit, scriptSrc);

        QJSValue result = m_jsEngine->evaluate(wrapped);
        if (result.isError()) {
            qCWarning(wekdeScene,
                      "Text script error for id=%d: %s",
                      tsi.id,
                      qPrintable(result.toString()));
            continue;
        }
        if (result.isNull() || result.isUndefined()) {
            qCWarning(
                wekdeScene, "Text script for id=%d did not produce an update function", tsi.id);
            continue;
        }

        QJSValue updateFn = result.property("update");
        QJSValue initFn   = result.property("init");

        if (! updateFn.isCallable()) {
            qCWarning(wekdeScene, "Text script for id=%d: update is not callable", tsi.id);
            continue;
        }

        TextScriptState state;
        state.id          = tsi.id;
        state.updateFn    = updateFn;
        state.currentText = QString::fromStdString(tsi.initialValue);

        // Call init(value) if available
        if (initFn.isCallable()) {
            QJSValue initResult = callJsGuarded([&] {
                return initFn.call({ QJSValue(state.currentText) });
            });
            if (initResult.isError()) {
                qCWarning(wekdeScene,
                          "Text script init error id=%d: %s",
                          tsi.id,
                          qPrintable(initResult.toString()));
            } else if (initResult.isString()) {
                state.currentText = initResult.toString();
            }
        }

        m_textScriptStates.push_back(std::move(state));
        qCInfo(wekdeScene, "Text script compiled for id=%d", tsi.id);
    }

    // Check if scene.on("update") listeners were registered during script compilation
    bool hasUpdateListeners = m_hasSceneListenersFn.isCallable() &&
                              m_hasSceneListenersFn.call({ QJSValue("update") }).toBool();

    // Property scripts must run first — they populate shared.* that text/color scripts depend on
    if (! m_propertyScriptStates.empty() || ! m_soundVolumeScriptStates.empty() ||
        ! m_soundLayerStates.empty() || hasUpdateListeners) {
        // sub-frame physics opt-in.  Default: gate the property loop to
        // the render rate (script output is sampled only when the render thread
        // draws, so faster eval is wasted CPU).  A scene opts into >render-rate
        // stepping via the WEKDE_SCRIPT_HIGHRATE env override OR a hard workshop-
        // id allowlist (the workshop id is the scene.pkg's parent dir name, the
        // same derivation ensureLocalStorageLoaded() uses).  Seeded with 3body
        // (3509243656), whose chaotic 3-body integration the 8ms timer below was
        // written for.  Behavior is unchanged for every scene that does not ask.
        {
            const char* hr = std::getenv("WEKDE_SCRIPT_HIGHRATE");
            if (hr && hr[0] && hr[0] != '0') m_propertyHighRate = true;
            QString workshopId;
            if (m_source.isValid()) {
                const QString localPath = m_source.toLocalFile();
                if (! localPath.isEmpty()) workshopId = QFileInfo(localPath).dir().dirName();
            }
            static const std::unordered_set<std::string> kHighRateIds = {
                "3509243656", // 3body — chaotic 3-body, wants sub-frame stepping
            };
            if (! workshopId.isEmpty() && kHighRateIds.count(workshopId.toStdString()))
                m_propertyHighRate = true;
            if (m_propertyHighRate)
                LOG_INFO("Property loop: HIGH-RATE mode (sub-frame stepping, no frame gate)");
            else
                LOG_INFO("Property loop: render-frame-gated (~render rate, not 125Hz)");
        }
        m_propertyTimer = new QTimer(this);
        // PreciseTimer + 8ms for ~120Hz.  After the Vec3/slim-dirty/pool
        // optimization pass the per-tick cost dropped from ~42ms to ~5.7ms,
        // so we have budget to double the tick rate.  This quadruples the
        // simulated physics time per wall-second for scenes like 3body
        // (3509243656) whose chaotic 3-body takes sim-seconds to diverge.
        m_propertyTimer->setTimerType(Qt::PreciseTimer);
        m_propertyTimer->setInterval(8);
        connect(m_propertyTimer, &QTimer::timeout, this, &SceneObject::evaluatePropertyScripts);
        m_propertyTimer->start();

        // Run once immediately so shared.* is populated before text/color scripts
        evaluatePropertyScripts();
    }

    if (! m_textScriptStates.empty()) {
        m_textTimer = new QTimer(this);
        // 8ms poll → 125Hz max.  Eval is gated inside evaluateTextScripts on
        // the render-thread frame index, so a text script never runs more
        // often than the render thread renders.  This makes
        // `1000/(Date.now()-last)` style FPS counters report the real render
        // rate (Miku 3363252053 was stuck at "fps: 2" because the previous
        // 500ms cadence is what the script was actually measuring).
        m_textTimer->setTimerType(Qt::PreciseTimer);
        m_textTimer->setInterval(8);
        connect(m_textTimer, &QTimer::timeout, this, &SceneObject::evaluateTextScripts);
        m_textTimer->start();
        LOG_INFO("TextTimer started: %zu text scripts, interval=8ms (frame-gated)",
                 m_textScriptStates.size());

        // Run once immediately
        evaluateTextScripts();
    } else {
        LOG_INFO("TextTimer NOT started: no text scripts");
    }

    if (! m_colorScriptStates.empty()) {
        m_colorTimer = new QTimer(this);
        m_colorTimer->setInterval(33); // ~30Hz for smooth audio-reactive color
        connect(m_colorTimer, &QTimer::timeout, this, &SceneObject::evaluateColorScripts);
        m_colorTimer->start();

        // Run once immediately
        evaluateColorScripts();
    }

    // tell the render side whether SceneScripts registered audio
    // buffers, so the FFT gate (WPShaderValueUpdater) keeps Process() running
    // for script-only-audio scenes (no spectrum uniform, no reactive particle).
    // CRITICAL starvation guard.  Runs here at the end of setupTextScripts (on
    // firstFrame, so the scene is published) after every script's top-level
    // registerAudioBuffers has executed via the seed evals above.
    {
        QJSValue   engineObj      = m_jsEngine->globalObject().property("engine");
        QJSValue   regs           = engineObj.property("_audioRegs");
        const bool hasScriptAudio = regs.isArray() && regs.property("length").toInt() > 0;
        if (m_scene) m_scene->setHasScriptAudio(hasScriptAudio);
        if (hasScriptAudio) LOG_INFO("SceneScript registered audio buffers -> FFT gate enabled");
    }
}

void SceneObject::refreshAudioBuffers() {
    if (! m_jsEngine) return;

    auto analyzer = m_scene->audioAnalyzer();
    if (! analyzer || ! analyzer->HasData()) return;

    QJSValue engineObj = m_jsEngine->globalObject().property("engine");
    QJSValue audioRegs = engineObj.property("_audioRegs");
    if (! audioRegs.isArray()) return;

    // render-frame de-dup.  refreshAudioBuffers() is called from the
    // property loop (render-gated), the text loop (render-gated), and
    // the color loop (33Hz).  The analyzer only produces new spectrum data per
    // processed render frame, so collapse all callers to ONE rebuild per drawn
    // frame; a second call at the same frame index reuses the buffers already
    // filled this frame.  (Form A bulk-array-assignment from the spec was
    // dropped after review: the loop below already caches buf.left/right/average
    // ONCE per buffer and fills them in place, so replacing them with freshly
    // allocated arrays would add allocations + an extra structured store per
    // array AND break any script holding a `buf.left` reference, for zero
    // boundary-crossing reduction.  This de-dup gate is the real, dominant win.)
    {
        uint64_t f = m_scene->getFrameIdx();
        if (! audioRefreshShouldRun(f, m_lastAudioBufFrameIdx)) return;
        m_lastAudioBufFrameIdx = f;
    }

    int len = audioRegs.property("length").toInt();
    for (int r = 0; r < len; r++) {
        QJSValue buf        = audioRegs.property(r);
        int      resolution = buf.property("resolution").toInt();

        auto leftData  = analyzer->GetRawSpectrum(resolution, 0);
        auto rightData = analyzer->GetRawSpectrum(resolution, 1);

        QJSValue leftArr  = buf.property("left");
        QJSValue rightArr = buf.property("right");
        QJSValue avgArr   = buf.property("average");

        int n = (int)leftData.size();
        for (int i = 0; i < n; i++) {
            float l  = leftData[i];
            float rv = rightData[i];
            leftArr.setProperty(i, (double)l);
            rightArr.setProperty(i, (double)rv);
            avgArr.setProperty(i, (double)((l + rv) * 0.5f));
        }
    }
}

void SceneObject::evaluateTextScripts() {
    if (! m_jsEngine || m_textScriptStates.empty()) return;
    // A3-T2 — once the watchdog has disabled this wallpaper's text scripts (K
    // consecutive interrupts), stay idle (the text timer is also stopped).
    if (m_textScriptsDisabled) return;

    // Render-frame gate: only fire when the render thread has produced a new
    // frame since the previous eval.  The timer ticks at 125Hz but actual
    // eval rate is capped at the render rate, so `Date.now()` deltas between
    // calls equal real frametime.  Always allow the first eval (frame_idx
    // can be 0 if the render thread hasn't started yet, but the constructor
    // calls evaluateTextScripts() once to seed text content immediately).
    uint64_t curFrameIdx = m_scene->getFrameIdx();
    if (m_lastTextFrameIdx != 0 && curFrameIdx == m_lastTextFrameIdx) return;
    m_lastTextFrameIdx = curFrameIdx;

    // Refresh audio buffers before evaluating scripts
    refreshAudioBuffers();

    // Update engine globals.  `engine.frametime` reports the gated script-tick
    // delta (close to real frametime).  `engine.fps` comes from the render
    // thread's wall-clock FpsCounter and is the value scripts should poll for
    // accurate FPS readouts.
    qint64 nowMs       = m_runtimeTimer.elapsed();
    double runtimeSecs = nowMs / 1000.0;
    double frametime   = wallpaper::ComputeTickFrametime(nowMs, m_lastTextTickMs, 0.016, 2000);
    m_lastTextTickMs   = nowMs;
    QJSValue engineObj = m_jsEngine->globalObject().property("engine");
    engineObj.setProperty("runtime", runtimeSecs);
    engineObj.setProperty("frametime", frametime);
    engineObj.setProperty("fps", m_scene->getFps());
    // timeOfDay: 0.0 = midnight, 0.5 = noon, 1.0 = midnight
    QTime  now = QTime::currentTime();
    double tod = (now.hour() * 3600 + now.minute() * 60 + now.second()) / 86400.0;
    engineObj.setProperty("timeOfDay", tod);

    // Refresh cursor world position for scripts reading input.cursorWorldPosition
    {
        QJSValue inputObj = m_jsEngine->globalObject().property("input");
        QJSValue cwp      = inputObj.property("cursorWorldPosition");
        cwp.setProperty("x", (double)m_cursorSceneX);
        cwp.setProperty("y", (double)m_cursorSceneY);
    }

    static const bool s_textDiag = []() {
        const char* v = std::getenv("WEKDE_SCRIPT_DIAG");
        return v && v[0] && v[0] != '0';
    }();

    int        updated = 0, errors = 0;
    static int s_textDebugCount = 0;
    s_textDebugCount++;
    bool textInterrupted = false;
    for (auto& state : m_textScriptStates) {
        bool     wasInterrupted = false;
        QJSValue result         = callJsGuarded(
            [&] {
                return state.updateFn.call({ QJSValue(state.currentText) });
            },
            &wasInterrupted);
        textInterrupted = textInterrupted || wasInterrupted;
        if (result.isError()) {
            errors++;
            if (m_scriptDiag.textErroredIds.find(state.id) == m_scriptDiag.textErroredIds.end()) {
                m_scriptDiag.textErroredIds.insert(state.id);
                QString stack = result.property("stack").toString();
                int     line  = result.property("lineNumber").toInt();
                // Plain-C LOG_INFO matches property-script errors so they
                // surface in the journal too (qCWarning is invisible there).
                LOG_INFO("Text script RUNTIME ERROR id=%d: %s (line %d)\nSTACK: %s",
                         state.id,
                         qPrintable(result.toString()),
                         line,
                         qPrintable(stack));
                qCWarning(wekdeScene,
                          "Text script runtime error id=%d: %s",
                          state.id,
                          qPrintable(result.toString()));
            }
            continue;
        }
        QString newText = result.toString();
        // Always log first 5 evals so we can tell from plasma's journal whether
        // the text script is firing at all, and whether it produces the same
        // string each call (showing as "no update" with no re-raster).
        if (s_textDebugCount <= 5) {
            QString a = newText;
            a.replace('\n', '\\');
            QString b = state.currentText;
            b.replace('\n', '\\');
            LOG_INFO("TextEval[%d] id=%d new=\"%s\" cur=\"%s\" changed=%d",
                     s_textDebugCount,
                     state.id,
                     qPrintable(a.left(80)),
                     qPrintable(b.left(80)),
                     (int)(newText != state.currentText));
        }
        if (newText != state.currentText) {
            if (s_textDiag) {
                // Log the new value (truncated) — essential for blank-HUD
                // debugging where you can't see the text on screen but want
                // to know whether scripts are producing text at all.
                QString preview = newText;
                preview.replace('\n', '\\');
                if (preview.size() > 80) preview = preview.left(80) + "...";
                LOG_INFO("Text script id=%d update: \"%s\"", state.id, qPrintable(preview));
            }
            state.currentText = newText;
            m_scene->updateText(state.id, newText.toStdString());
            updated++;
        }
    }

    if (s_textDiag) {
        static int s_textTick = 0;
        s_textTick++;
        // Summary every 10 ticks (text timer is 500ms → every 5 seconds).
        if (s_textTick % 10 == 0) {
            LOG_INFO("DIAG text tick=%d total=%zu updated=%d errors=%d",
                     s_textTick,
                     m_textScriptStates.size(),
                     updated,
                     errors);
        }
    }

    // A3-T2 back-off: latch text scripts off after K consecutive interrupts so a
    // runaway text script stops re-interrupting (and pinning a CPU) every tick.
    applyInterruptBackoff(
        textInterrupted, m_consecutiveTextInterrupts, m_textScriptsDisabled, m_textTimer, "text");
}

void SceneObject::evaluateColorScripts() {
    if (! m_jsEngine) return;
    // A3-T2 — color + shader-value share this method/timer; once disabled, idle.
    if (m_colorScriptsDisabled) return;
    // Idle when there is nothing to evaluate OR the wallpaper is paused (F19).
    // pause() also stops m_colorTimer; this guards the setup-time seed eval and
    // any tick already queued when pause() ran.
    const bool hasStates = ! m_colorScriptStates.empty() || ! m_shaderValueScriptStates.empty();
    if (! scriptLoopShouldRun(hasStates, m_paused)) return;

    // A3-T2 — OR'd across the color + shader-value guarded calls; drives the
    // shared back-off latch at the tail.
    bool colorInterrupted = false;

    // Refresh audio buffers before evaluating scripts
    refreshAudioBuffers();

    // Update engine globals (color timer is 33ms but report real dt so the
    // value stays correct if the tick rate is ever changed).
    qint64 nowMs       = m_runtimeTimer.elapsed();
    double runtimeSecs = nowMs / 1000.0;
    double frametime   = wallpaper::ComputeTickFrametime(nowMs, m_lastColorTickMs, 0.033, 500);
    m_lastColorTickMs  = nowMs;
    QJSValue engineObj = m_jsEngine->globalObject().property("engine");
    engineObj.setProperty("runtime", runtimeSecs);
    engineObj.setProperty("frametime", frametime);
    engineObj.setProperty("fps", m_scene->getFps());

    QTime  now = QTime::currentTime();
    double tod = (now.hour() * 3600 + now.minute() * 60 + now.second()) / 86400.0;
    engineObj.setProperty("timeOfDay", tod);

    // Refresh cursor world position for scripts reading input.cursorWorldPosition
    {
        QJSValue inputObj = m_jsEngine->globalObject().property("input");
        QJSValue cwp      = inputObj.property("cursorWorldPosition");
        cwp.setProperty("x", (double)m_cursorSceneX);
        cwp.setProperty("y", (double)m_cursorSceneY);
    }

    for (auto& state : m_colorScriptStates) {
        // Pass current color as a full Vec3 so scripts can use both .x/.y/.z and .r/.g/.b.
        // Cached ctor .call() avoids a per-tick QJSEngine::evaluate() of a "Vec3(...)" string.
        QJSValue colorVal = m_vec3Fn.call({ QJSValue((double)state.currentColor[0]),
                                            QJSValue((double)state.currentColor[1]),
                                            QJSValue((double)state.currentColor[2]) });

        bool     wasInterrupted = false;
        QJSValue result         = callJsGuarded(
            [&] {
                return state.updateFn.call({ colorVal });
            },
            &wasInterrupted);
        colorInterrupted = colorInterrupted || wasInterrupted;
        if (result.isError()) {
            qCWarning(wekdeScene,
                      "Color script runtime error id=%d: %s",
                      state.id,
                      qPrintable(result.toString()));
            continue;
        }

        // Result is Vec3 {x, y, z} = RGB (r/g/b aliases also accepted as fallback)
        float r, g, b;
        if (result.isObject()) {
            QJSValue rx = result.property("x");
            r = (float)(rx.isUndefined() ? result.property("r").toNumber() : rx.toNumber());
            QJSValue gy = result.property("y");
            g = (float)(gy.isUndefined() ? result.property("g").toNumber() : gy.toNumber());
            QJSValue bz = result.property("z");
            b = (float)(bz.isUndefined() ? result.property("b").toNumber() : bz.toNumber());
        } else {
            // Skip silently — color scripts that depend on shared.* may return
            // undefined until property scripts populate the data
            continue;
        }

        // Periodic diagnostic logging (every ~3 seconds)
        static int evalCount = 0;
        if (++evalCount % 90 == 1) {
            qCInfo(wekdeScene, "Color script id=%d: rgb=(%.3f, %.3f, %.3f)", state.id, r, g, b);
        }

        // Only push update if color actually changed
        if (std::abs(r - state.currentColor[0]) > 0.001f ||
            std::abs(g - state.currentColor[1]) > 0.001f ||
            std::abs(b - state.currentColor[2]) > 0.001f) {
            state.currentColor = { r, g, b };
            m_scene->updateColor(state.id, r, g, b);
        }
    }

    // Shader-value script dispatch.  Each iteration packs the script's
    // last return value into the correct JS argument shape (scalar/Vec2/
    // Vec3/Vec4), runs update(), and pushes the result into the effect-
    // material drain so the renderer picks it up the next tick.  Errors
    // are deduped per (id, effectIdx, uniform) tuple so a broken script
    // doesn't flood the journal.
    for (auto& state : m_shaderValueScriptStates) {
        if (! state.updateFn.isCallable()) continue;
        QJSValue arg;
        if (state.argShape <= 1) {
            arg = QJSValue(state.cachedValue.empty() ? 0.0 : (double)state.cachedValue[0]);
        } else {
            // Cached ctor .call() — was a per-tick QJSEngine::evaluate() of a
            // "VecN(...)" string.  at() keeps the old zero-fill for the rare
            // first-tick case where cachedValue is shorter than argShape.
            auto at = [&](size_t i) -> QJSValue {
                return QJSValue(state.cachedValue.size() > i ? (double)state.cachedValue[i] : 0.0);
            };
            if (state.argShape == 2)
                arg = m_vec2Fn.call({ at(0), at(1) });
            else if (state.argShape == 3)
                arg = m_vec3Fn.call({ at(0), at(1), at(2) });
            else
                arg = m_vec4Fn.call({ at(0), at(1), at(2), at(3) });
        }

        bool     wasInterrupted = false;
        QJSValue result         = callJsGuarded(
            [&] {
                return state.updateFn.call({ arg });
            },
            &wasInterrupted);
        colorInterrupted = colorInterrupted || wasInterrupted;
        if (result.isError()) {
            int64_t tag = ((int64_t)state.id << 32) ^
                          ((int64_t)state.effectIdx << 24) ^
                          (int64_t)std::hash<std::string>{}(state.uniformName);
            if (m_scriptDiag.svErrored.find(tag) == m_scriptDiag.svErrored.end()) {
                m_scriptDiag.svErrored.insert(tag);
                LOG_INFO("sv-script error id=%d effect=%d uniform=%s: %s",
                         state.id, state.effectIdx, state.uniformName.c_str(),
                         qPrintable(result.toString()));
            }
            continue;
        }

        // Pack result back into a float vector.  Accept scalars, Vec2/3/4
        // objects, or plain arrays — same shape as
        // SceneObject::effectMaterialSetValue.
        std::vector<float> floats;
        if (result.isNumber()) {
            floats.push_back((float)result.toNumber());
        } else if (result.isArray()) {
            int n = result.property("length").toInt();
            for (int i = 0; i < n && i < 16; ++i) {
                QJSValue el = result.property(i);
                if (el.isNumber()) floats.push_back((float)el.toNumber());
            }
        } else if (result.isObject()) {
            const char* keys[] = { "x", "y", "z", "w" };
            for (int k = 0; k < 4; ++k) {
                QJSValue v = result.property(keys[k]);
                if (! v.isNumber()) break;
                floats.push_back((float)v.toNumber());
            }
        }
        if (floats.empty()) continue;

        // Compare-and-push: only dispatch when the value actually changed.
        bool changed = floats.size() != state.cachedValue.size();
        for (size_t i = 0; ! changed && i < floats.size(); ++i)
            if (std::abs(floats[i] - state.cachedValue[i]) > 0.0001f) changed = true;
        if (! changed) continue;
        state.cachedValue = floats;
        m_scene->updateEffectMaterialValue(state.id, state.effectIdx, state.uniformName,
                                           std::move(floats));
    }

    // A3-T2 back-off: color + shader-value share one latch (same method/timer).
    applyInterruptBackoff(colorInterrupted,
                          m_consecutiveColorInterrupts,
                          m_colorScriptsDisabled,
                          m_colorTimer,
                          "color");
}

QJSValue SceneObject::callJsGuarded(const std::function<QJSValue()>& fn, bool* outInterrupted) {
    if (outInterrupted) *outInterrupted = false;
    if (! m_jsEngine) return QJSValue();
    // Arm the off-thread deadline, run the author JS, then disarm.  A runaway
    // (`while(true)`) trips the watchdog, which calls setInterrupted(true) and
    // makes this call() unwind as a JS exception — caught here so the GUI-thread
    // tick loop continues instead of the desktop freezing.
    m_jsWatchdog.arm(m_jsEngine, m_jsWatchdogBudgetMs);
    QJSValue result;
    try {
        result = fn();
    } catch (const std::exception& e) {
        LOG_ERROR("guarded JS dispatch threw: %s", e.what());
    } catch (...) {
        LOG_ERROR("guarded JS dispatch threw (non-std)");
    }
    const bool fired = m_jsWatchdog.disarm();
    if (fired) {
        // Clear the interrupt latch so the NEXT guarded call can run; if the
        // script keeps overrunning, the watchdog re-fires and the caller's
        // back-off (consecutive-interrupt counter) eventually disables it.
        m_jsEngine->setInterrupted(false);
        if (outInterrupted) *outInterrupted = true;
    }
    return result;
}

bool SceneObject::applyInterruptBackoff(bool tickInterrupted, int& consecutive, bool& disabledFlag,
                                        QTimer* timer, const char* what) {
    if (! tickInterrupted) {
        consecutive = 0;
        return false;
    }
    consecutive++;
    if (scenebackend::shouldDisableAfterInterrupts(
            consecutive, scenebackend::JsWatchdog::kDisableAfterInterrupts) &&
        ! disabledFlag) {
        disabledFlag = true;
        if (timer) timer->stop();
        LOG_ERROR("%s script exceeded %lldms budget %d ticks running — disabling %s for this "
                  "wallpaper (it will stop updating)",
                  what,
                  (long long)m_jsWatchdogBudgetMs,
                  consecutive,
                  what);
        return true;
    }
    return false;
}

void SceneObject::evaluatePropertyScripts() {
    if (! m_jsEngine) return;
    // A3-T2 — once the watchdog has disabled this wallpaper's property scripts
    // (K consecutive interrupts), stay idle.  pause()/stop also halts the
    // timer; this belt covers the seed eval and any already-queued tick.
    if (m_propertyScriptsDisabled) return;
    // Idle when there is nothing to evaluate OR the wallpaper is paused (F19).
    // pause() also stops m_propertyTimer; this guards the setup-time seed eval
    // and any tick already queued when pause() ran.
    const bool hasStates = ! m_propertyScriptStates.empty() ||
                           ! m_soundVolumeScriptStates.empty() || ! m_soundLayerStates.empty();
    if (! scriptLoopShouldRun(hasStates, m_paused)) return;

    // render-frame gate (mirrors evaluateTextScripts above).  Unless
    // this wallpaper opted into sub-frame physics stepping, never evaluate
    // property scripts faster than the render thread draws — the script output
    // is sampled at the render rate, so extra ticks are wasted CPU (~76% of
    // ticks at 30fps, ~88% at 15fps).  The lastFrameIdx==0 branch keeps the
    // ctor's seed eval ungated so shared.* is populated before text/color
    // scripts run.  Counters below feed the diag dump.
    static uint64_t s_propEvalCount = 0, s_propSkipCount = 0;
    {
        uint64_t curFrameIdx = m_scene->getFrameIdx();
        if (! propertyTickShouldEval(m_propertyHighRate, curFrameIdx, m_lastPropertyFrameIdx)) {
            ++s_propSkipCount;
            return;
        }
        ++s_propEvalCount;
        m_lastPropertyFrameIdx = curFrameIdx;
    }

    // A3-T2 — OR'd across every guarded JS dispatch in this tick (animationEvent,
    // runAllPropertyScripts, the dirty-collect calls); drives the back-off
    // counter at the tail.  Declared up here because animationEvent fires first.
    bool tickInterrupted = false;

    // Periodic GC to prevent JS heap growth during long sessions
    {
        static int s_gc_counter = 0;
        if (++s_gc_counter % 900 == 0) { // every ~30s at 30Hz
            m_jsEngine->collectGarbage();
        }
    }

    // --- Per-phase timing probes ---------------------------------------------
    // Track cumulative microseconds per phase across ~90 ticks; dump under
    // WEKDE_SCRIPT_DIAG.  Zero overhead when diag is off (elapsed() is cheap;
    // the branch to accumulate is the only work).
    static const bool s_probeTimings = []() {
        const char* v = std::getenv("WEKDE_SCRIPT_DIAG");
        return v && v[0] && v[0] != '0';
    }();
    static qint64 s_t_prelude       = 0;
    static qint64 s_t_scripts       = 0;
    static qint64 s_t_dirtyCollect  = 0;
    static qint64 s_t_dirtyDispatch = 0;
    static qint64 s_t_sound         = 0;
    static qint64 s_t_scene         = 0;
    static qint64 s_t_total         = 0;
    QElapsedTimer probe;
    if (s_probeTimings) probe.start();
    auto probeMark = [&](qint64& accum) {
        if (s_probeTimings) {
            accum += probe.nsecsElapsed() / 1000; // microseconds
            probe.restart();
        }
    };

    // --- SceneScript diagnostic tick ---------------------------------------
    // Opt-in via WEKDE_SCRIPT_DIAG=1 env var.  Every ~90 ticks (~3s at 30Hz)
    // dumps a snapshot of the JS `shared` object + per-tick update / error
    // counts.  Cheap when disabled (one env read the first time, short
    // counter increment otherwise); intentionally noisy when on so that a
    // blank-screen bug like Three-Body (3509243656) exposes *what* the
    // scripts are actually computing.
    static const bool s_scriptDiag = []() {
        const char* v = std::getenv("WEKDE_SCRIPT_DIAG");
        return v && v[0] && v[0] != '0';
    }();
    static int s_diagTick       = 0;
    static int s_updatesVec3    = 0;
    static int s_updatesAlpha   = 0;
    static int s_updatesVisible = 0;
    static int s_errorsThisWin  = 0;
    if (s_scriptDiag) {
        s_diagTick++;
        if (s_diagTick == 1) {
            // Boot dump: counts of compiled scripts, so users see what the
            // scene actually registered vs. what they expect.
            int visCount = 0, alphaCount = 0, vec3Count = 0;
            for (auto& st : m_propertyScriptStates) {
                if (st.property == "visible")
                    visCount++;
                else if (st.property == "alpha")
                    alphaCount++;
                else
                    vec3Count++;
            }
            LOG_INFO("DIAG boot: property scripts total=%zu (visible=%d "
                     "alpha=%d vec3=%d) text=%zu color=%zu sound_vol=%zu "
                     "sound_layer=%zu",
                     m_propertyScriptStates.size(),
                     visCount,
                     alphaCount,
                     vec3Count,
                     m_textScriptStates.size(),
                     m_colorScriptStates.size(),
                     m_soundVolumeScriptStates.size(),
                     m_soundLayerStates.size());
        }
    }

    // One-shot debug JS eval from --js-eval (queued before engine was ready).
    if (! m_pendingJsEval.isEmpty()) {
        QJSValue r = m_jsEngine->evaluate(m_pendingJsEval);
        LOG_INFO("debugEvalJs deferred: '%s' -> %s",
                 qPrintable(m_pendingJsEval),
                 qPrintable(r.toString()));
        m_pendingJsEval.clear();
    }

    // Update engine globals.  frametime is the WALL-CLOCK dt since the
    // previous property tick (property timer fires every 8ms).  Scripts
    // that integrate motion as `x += v * engine.frametime` must see real
    // elapsed time — hardcoding a fixed value makes animation speed track
    // the timer rate, not real time, and caused a 4× speed regression in
    // dino_run-style wallpapers when the property timer was raised to 120Hz.
    qint64 nowMs         = m_runtimeTimer.elapsed();
    double runtimeSecs   = nowMs / 1000.0;
    double frametime     = wallpaper::ComputeTickFrametime(nowMs, m_lastPropertyTickMs, 0.008, 250);
    m_lastPropertyTickMs = nowMs;
    QJSValue engineObj   = m_jsEngine->globalObject().property("engine");
    engineObj.setProperty("runtime", runtimeSecs);
    engineObj.setProperty("frametime", frametime);
    engineObj.setProperty("fps", m_scene->getFps());
    engineObj.setProperty("frameCount", (double)++m_propFrameCount);

    QTime  now = QTime::currentTime();
    double tod = (now.hour() * 3600 + now.minute() * 60 + now.second()) / 86400.0;
    engineObj.setProperty("timeOfDay", tod);

    // Refresh cursor world position for scripts reading input.cursorWorldPosition
    {
        QJSValue inputObj = m_jsEngine->globalObject().property("input");
        QJSValue cwp      = inputObj.property("cursorWorldPosition");
        cwp.setProperty("x", (double)m_cursorSceneX);
        cwp.setProperty("y", (double)m_cursorSceneY);
    }

    // Refresh audio buffers in case property scripts use audio data
    refreshAudioBuffers();

    // Update sound layer isPlaying states from C++ before script evaluation.
    // Layers that had a JS command this tick (play/pause/stop) are marked
    // "dirty" in _soundPlayingStatesDirty; the shadow value already reflects
    // the commanded intent, so we DON'T overwrite them from C++ until C++
    // actually matches (stream transitioning can take a frame or two —
    // meanwhile anyPlaying()-style polling would see stale "still playing"
    // and misbehave).  Clear the dirty flag once the two agree.
    if (! m_soundLayerStates.empty()) {
        QJSValue engineObj2    = m_jsEngine->globalObject().property("engine");
        QJSValue playingStates = engineObj2.property("_soundPlayingStates");
        if (playingStates.isUndefined()) {
            playingStates = m_jsEngine->newObject();
            engineObj2.setProperty("_soundPlayingStates", playingStates);
        }
        QJSValue dirtyMap = engineObj2.property("_soundPlayingStatesDirty");
        if (dirtyMap.isUndefined()) {
            dirtyMap = m_jsEngine->newObject();
            engineObj2.setProperty("_soundPlayingStatesDirty", dirtyMap);
        }
        for (const auto& sls : m_soundLayerStates) {
            QString nameKey    = QString::fromStdString(sls.name);
            bool    cppPlaying = m_scene->soundLayerIsPlaying(sls.index);
            bool    jsDirty    = dirtyMap.property(nameKey).toBool();
            if (jsDirty) {
                bool shadow = playingStates.property(nameKey).toBool();
                if (shadow == cppPlaying) {
                    // C++ has caught up to the commanded state — clear dirty,
                    // resume tracking C++ from next tick onward.
                    dirtyMap.setProperty(nameKey, QJSValue(false));
                }
                // else: leave shadow alone, wait for C++ to catch up.
            } else {
                playingStates.setProperty(nameKey, cppPlaying);
            }
        }
    }

    // Cache Vec3 constructor for efficient argument creation
    QJSValue vec3Fn = m_jsEngine->globalObject().property("Vec3");

    // Drain puppet-animation keyframe events fired by the render thread
    // since the last tick, and dispatch each to the matching script's
    // animationEvent(event, value) handler.  Fired BEFORE update() so the
    // handler can mutate state (play sounds, toggle visibility) that update
    // then consumes on the same tick.
    if (m_scene) {
        auto events = m_scene->drainAnimationEvents();
        if (! events.empty()) {
            auto buildValue = [&](const PropertyScriptState& state) {
                if (state.property == "visible") return QJSValue(state.currentVisible);
                if (state.property == "alpha" ||
                    state.property.rfind("instanceoverride.", 0) == 0) {
                    return QJSValue((double)state.currentFloat);
                }
                return vec3Fn.call({ QJSValue((double)state.currentVec3[0]),
                                     QJSValue((double)state.currentVec3[1]),
                                     QJSValue((double)state.currentVec3[2]) });
            };
            for (const auto& evt : events) {
                bool delivered = false;
                for (auto& state : m_propertyScriptStates) {
                    if (state.id != evt.nodeId) continue;
                    if (! state.animationEventFn.isCallable()) continue;

                    if (! state.layerName.empty()) {
                        m_jsEngine->globalObject().setProperty("thisLayer", state.thisLayerProxy);
                        m_jsEngine->globalObject().setProperty("thisObject", state.thisObjectProxy);
                    }
                    QJSValue eventObj = m_jsEngine->newObject();
                    eventObj.setProperty("name", QString::fromStdString(evt.name));
                    eventObj.setProperty("frame", QJSValue(evt.frame));

                    bool     wasInterrupted = false;
                    QJSValue result         = callJsGuarded(
                        [&] {
                            return state.animationEventFn.call({ eventObj, buildValue(state) });
                        },
                        &wasInterrupted);
                    tickInterrupted = tickInterrupted || wasInterrupted;
                    if (result.isError()) {
                        QString stack = result.property("stack").toString();
                        int     line  = result.property("lineNumber").toInt();
                        LOG_INFO(
                            "animationEvent handler error id=%d name='%s': %s (line %d)\nSTACK: %s",
                            state.id,
                            evt.name.c_str(),
                            qPrintable(result.toString()),
                            line,
                            qPrintable(stack));
                    }
                    delivered = true;
                }
                // scene-level listeners so scripts on OTHER objects can listen
                QJSValue sceneEvt = m_jsEngine->newObject();
                sceneEvt.setProperty("name", QString::fromStdString(evt.name));
                sceneEvt.setProperty("frame", QJSValue(evt.frame));
                sceneEvt.setProperty("nodeId", QJSValue(evt.nodeId));
                fireSceneEventListeners("animationEvent", { sceneEvt });

                if (! delivered) {
                    LOG_INFO("animationEvent '%s' fired for node %d but no handler listening",
                             evt.name.c_str(),
                             evt.nodeId);
                }
            }
        }
    }

    probeMark(s_t_prelude);

    // Batched property-script dispatch.  ONE C++->JS call replaces the old
    // per-script loop: `_runAllPropertyScripts()` sets `thisLayer`, runs each
    // update fn with try/catch, compares the result against the stored
    // current value (threshold-gated), and packs ONLY the changed entries
    // into a flat stride-4 array [idx, v1, v2, v3, ...].  Errors are pushed
    // to `_scriptErrors` (stride 4: idx, message, line, stack) and drained
    // below.  Kind for each idx is known from `m_propertyScriptStates[idx].kind`.
    if (m_runAllPropertyScriptsFn.isCallable()) {
        using Kind              = PropertyScriptState::Kind;
        bool     wasInterrupted = false;
        QJSValue out            = callJsGuarded(
            [this] {
                return m_runAllPropertyScriptsFn.call();
            },
            &wasInterrupted);
        tickInterrupted = tickInterrupted || wasInterrupted;
        int total       = out.property("length").toInt();
        for (int i = 0; i < total; i += 4) {
            int idx = out.property(i).toInt();
            if (idx < 0 || idx >= (int)m_propertyScriptStates.size()) continue;
            auto& state = m_propertyScriptStates[idx];
            switch (state.kind) {
            case Kind::Visible: {
                bool v               = out.property(i + 1).toInt() != 0;
                state.currentVisible = v;
                m_scene->updateNodeVisible(state.id, v);
                if (s_scriptDiag) s_updatesVisible++;
                break;
            }
            case Kind::Alpha: {
                float v            = (float)out.property(i + 1).toNumber();
                state.currentFloat = v;
                m_scene->updateNodeAlpha(state.id, v);
                if (s_scriptDiag) s_updatesAlpha++;
                break;
            }
            case Kind::ParticleRate: {
                // Same wire shape as Alpha (single float), routed to the
                // particle subsystem's dynamic rate multiplier instead of
                // the g_UserAlpha uniform.
                float v            = (float)out.property(i + 1).toNumber();
                state.currentFloat = v;
                m_scene->updateParticleRate(state.id, v);
                if (s_scriptDiag) s_updatesAlpha++;
                break;
            }
            case Kind::Vec3: {
                float x           = (float)out.property(i + 1).toNumber();
                float y           = (float)out.property(i + 2).toNumber();
                float z           = (float)out.property(i + 3).toNumber();
                state.currentVec3 = { x, y, z };
                m_scene->updateNodeTransform(state.id, state.property, x, y, z);
                if (s_scriptDiag) s_updatesVec3++;
                break;
            }
            }
        }

        // Drain _scriptErrors (stride 4: idx, message, line, stack).  Same
        // dedup behavior as before: first 10 errors get full STACK logged,
        // subsequent runtime errors for the same id+kind are suppressed.
        QJSValue errors   = m_jsEngine->globalObject().property("_scriptErrors");
        int      errTotal = errors.property("length").toInt();
        for (int i = 0; i < errTotal; i += 4) {
            int idx = errors.property(i).toInt();
            if (idx < 0 || idx >= (int)m_propertyScriptStates.size()) continue;
            auto& state = m_propertyScriptStates[idx];
            if (s_scriptDiag) s_errorsThisWin++;
            int tag = state.id * 10 + (int)state.kind;
            if (m_scriptDiag.propErroredIds.find(tag) != m_scriptDiag.propErroredIds.end())
                continue;
            m_scriptDiag.propErroredIds.insert(tag);
            static int s_rt_err = 0;
            QString    msg      = errors.property(i + 1).toString();
            int        line     = errors.property(i + 2).toInt();
            QString    stack    = errors.property(i + 3).toString();
            if (++s_rt_err <= 10) {
                LOG_INFO("Property script RUNTIME ERROR id=%d prop=%s: %s (line %d)\nSTACK: %s",
                         state.id,
                         state.property.c_str(),
                         qPrintable(msg),
                         line,
                         qPrintable(stack));
            }
            qCWarning(wekdeScene,
                      "Property script error id=%d prop=%s: %s",
                      state.id,
                      state.property.c_str(),
                      qPrintable(msg));
        }
    }

    // Periodic summary every ~3s — tick counts + shared.* keys dump.  Cheap
    // to enumerate via QJSValue.property iteration (Object.keys under the
    // hood), but only done when diagnostics are on.
    if (s_scriptDiag && (s_diagTick % 90 == 0)) {
        QJSValue shared = m_jsEngine->globalObject().property("shared");
        QJSValue keysFn =
            m_jsEngine->evaluate("(function(o){var k=[]; for(var n in o) k.push(n); return k;})");
        QJSValue keys     = keysFn.call({ shared });
        int      keyCount = keys.property("length").toInt();
        QString  sample;
        sample.reserve(512);
        int shown = 0;
        for (int i = 0; i < keyCount && shown < 20; i++) {
            QString  k = keys.property(i).toString();
            QJSValue v = shared.property(k);
            QString  vs;
            if (v.isNumber())
                vs = QString::number(v.toNumber(), 'g', 4);
            else if (v.isBool())
                vs = v.toBool() ? "true" : "false";
            else if (v.isString()) {
                QString s = v.toString();
                if (s.size() > 32) s = s.left(32) + "...";
                vs = '"' + s + '"';
            } else if (v.isUndefined())
                vs = "undef";
            else
                vs = "<obj>";
            if (! sample.isEmpty()) sample += ", ";
            sample += k + "=" + vs;
            shown++;
        }
        LOG_INFO("DIAG tick=%d runtime=%.1fs shared[%d]: %s%s | "
                 "updates vec3=%d alpha=%d visible=%d errors=%d",
                 s_diagTick,
                 runtimeSecs,
                 keyCount,
                 qPrintable(sample),
                 keyCount > shown ? " ..." : "",
                 s_updatesVec3,
                 s_updatesAlpha,
                 s_updatesVisible,
                 s_errorsThisWin);
        s_updatesVec3 = s_updatesAlpha = s_updatesVisible = s_errorsThisWin = 0;
    }

    probeMark(s_t_scripts);

    // Fire scene.on("update") listeners — after IIFE updates, before dirty flush
    fireSceneEventListeners("update");

    // Flush dirty layer proxies — flat layout (see DIRTY_STRIDE in JS).
    // Each dirty layer uses 17 array slots; we read sequentially and
    // branch on the flags bitmask instead of doing separate boolean reads
    // for each dirty bit.
    constexpr int      DIRTY_STRIDE = 17;
    constexpr uint32_t F_ORIGIN = 1, F_SCALE = 2, F_ANGLES = 4, F_VISIBLE = 8, F_ALPHA = 16,
                       F_TEXT = 32, F_PSIZE = 64, F_CMDS = 128, F_EFX = 256;
    int dirtyLayerCount = 0;
    int dirtyLayerMiss  = 0;
    if (m_collectDirtyLayersFn.isCallable()) {
        bool     wasInterrupted = false;
        QJSValue updates        = callJsGuarded(
            [this] {
                return m_collectDirtyLayersFn.call();
            },
            &wasInterrupted);
        tickInterrupted = tickInterrupted || wasInterrupted;
        probeMark(s_t_dirtyCollect);
        int totalEntries = updates.property("length").toInt();
        dirtyLayerCount  = totalEntries / DIRTY_STRIDE;

        // Batch origin/scale/angles/alpha/visible updates so we take the
        // scene's property-update mutex once instead of per-field.
        static thread_local std::vector<wallpaper::SceneWallpaper::LayerBatchUpdate> s_batch;
        s_batch.clear();
        s_batch.reserve(dirtyLayerCount);
        constexpr uint32_t HOT_FLAGS = F_ORIGIN | F_SCALE | F_ANGLES | F_ALPHA | F_VISIBLE;

        // one-shot unresolved-name diagnostic.  Dirty entries are now
        // keyed by the JS-resolved integer id (slot 0), so the hot loop no longer
        // carries names; report any layer proxy whose name failed to resolve to
        // an id ONCE per load here (loudly), preserving the old unknown-name
        // warning without the per-tick string work.  Runs at the first non-empty
        // flush, by when touched proxies exist; lazily-created bad proxies are
        // still caught by the per-miss (-1) warning below.
        if (! m_scriptDiag.dirtyMissesScanned && totalEntries > 0) {
            m_scriptDiag.dirtyMissesScanned = true;
            QJSValue unresolved =
                m_jsEngine->evaluate("(function(){var u=[];if(typeof _layerList!=='undefined')"
                                     "for(var i=0;i<_layerList.length;i++){var L=_layerList[i];"
                                     "var id=L._state?L._state.id:L.id;"
                                     "if(typeof id!=='number')u.push(L.name);}return u;})()");
            int n = unresolved.property("length").toInt();
            for (int i = 0; i < n && i < 20; i++) {
                std::string nm = unresolved.property(i).toString().toStdString();
                m_scriptDiag.dirtyLayerMisses.insert(nm);
                qCWarning(wekdeScene,
                          "Layer '%s' has no resolved id in nodeNameToId (%zu "
                          "entries) — its dirty updates will be dropped",
                          nm.c_str(),
                          m_nodeNameToId.size());
            }
        }

        for (int base = 0; base < totalEntries; base += DIRTY_STRIDE) {
            // slot 0 is the JS-resolved integer id (echoed from
            // _layerNameToId at proxy creation), so no per-tick
            // QString->std::string materialization + nodeNameToId hash lookup.
            int32_t id = (int32_t)updates.property(base + 0).toInt();
            if (id < 0) {
                dirtyLayerMiss++;
                // The hot loop no longer has the name; unresolvable layers are
                // reported by name once at first flush above, so here we only
                // count + cap a generic warning.
                static int s_dirtyMissLog = 0;
                if (++s_dirtyMissLog <= 10)
                    qCWarning(wekdeScene,
                              "Dirty layer with unresolved id (-1) in dispatch; "
                              "see the first-flush unresolved-name warning");
                continue;
            }
            uint32_t flags = (uint32_t)updates.property(base + 1).toInt();

            if (flags & HOT_FLAGS) {
                wallpaper::SceneWallpaper::LayerBatchUpdate bu {};
                bu.id    = id;
                bu.flags = flags & HOT_FLAGS;
                if (flags & F_ORIGIN) {
                    bu.origin[0] = (float)updates.property(base + 2).toNumber();
                    bu.origin[1] = (float)updates.property(base + 3).toNumber();
                    bu.origin[2] = (float)updates.property(base + 4).toNumber();
                }
                if (flags & F_SCALE) {
                    bu.scale[0] = (float)updates.property(base + 5).toNumber();
                    bu.scale[1] = (float)updates.property(base + 6).toNumber();
                    bu.scale[2] = (float)updates.property(base + 7).toNumber();
                }
                if (flags & F_ANGLES) {
                    bu.angles[0] = (float)updates.property(base + 8).toNumber();
                    bu.angles[1] = (float)updates.property(base + 9).toNumber();
                    bu.angles[2] = (float)updates.property(base + 10).toNumber();
                }
                if (flags & F_ALPHA) {
                    bu.alpha = (float)updates.property(base + 11).toNumber();
                }
                if (flags & F_VISIBLE) {
                    bu.visible = updates.property(base + 12).toInt() != 0 ? 1 : 0;
                }
                s_batch.push_back(bu);
            }
            // Low-frequency fields stay on the individual-setter path — these
            // don't fire every tick so a per-call lock is harmless.
            if (flags & F_PSIZE) {
                m_scene->updateTextPointsize(id, (float)updates.property(base + 13).toNumber());
            }
            if (flags & F_TEXT) {
                m_scene->updateText(id, updates.property(base + 14).toString().toStdString());
            }
            if (flags & F_CMDS) {
                QJSValue cmds     = updates.property(base + 15);
                int      cmdCount = cmds.property("length").toInt();
                for (int c = 0; c < cmdCount; c++) {
                    QString cmd = cmds.property(c).toString();
                    if (! cmd.startsWith("panim_")) continue;
                    int sep = cmd.indexOf(':');
                    if (sep < 0) continue;
                    std::string an     = cmd.mid(sep + 1).toStdString();
                    QString     action = cmd.mid(6, sep - 6);
                    if (action == "play")
                        m_scene->propertyAnimPlay(id, an);
                    else if (action == "stop")
                        m_scene->propertyAnimStop(id, an);
                    else if (action == "pause")
                        m_scene->propertyAnimPause(id, an);
                }
            }
            if (flags & F_EFX) {
                QJSValue efxList = updates.property(base + 16);
                int      efxLen  = efxList.property("length").toInt();
                for (int e = 0; e < efxLen; e += 2) {
                    int  effIdx = efxList.property(e).toInt();
                    bool effVis = efxList.property(e + 1).toInt() != 0;
                    m_scene->updateEffectVisible(id, effIdx, effVis);
                }
            }
        }
        if (! s_batch.empty()) m_scene->applyLayerBatch(s_batch);
    }

    probeMark(s_t_dirtyDispatch);

    // Flush dirty sound layer proxies (play/stop/pause/volume commands)
    if (m_collectDirtySoundLayersFn.isCallable()) {
        bool     wasInterrupted = false;
        QJSValue soundUpdates   = callJsGuarded(
            [this] {
                return m_collectDirtySoundLayersFn.call();
            },
            &wasInterrupted);
        tickInterrupted      = tickInterrupted || wasInterrupted;
        int soundUpdateCount = soundUpdates.property("length").toInt();
        for (int i = 0; i < soundUpdateCount; i++) {
            QJSValue    entry = soundUpdates.property(i);
            std::string name  = entry.property("name").toString().toStdString();
            auto        it    = m_soundLayerNameToIndex.find(name);
            if (it == m_soundLayerNameToIndex.end()) continue;
            int32_t idx = it->second;

            // Process commands (play/stop/pause)
            QJSValue cmds     = entry.property("cmds");
            int      cmdCount = cmds.property("length").toInt();
            for (int c = 0; c < cmdCount; c++) {
                QString cmd = cmds.property(c).toString();
                if (cmd == "play") {
                    LOG_INFO("soundLayer.play '%s' idx=%d", name.c_str(), idx);
                    m_scene->soundLayerPlay(idx);
                } else if (cmd == "stop") {
                    LOG_INFO("soundLayer.stop '%s' idx=%d", name.c_str(), idx);
                    m_scene->soundLayerStop(idx);
                } else if (cmd == "pause") {
                    LOG_INFO("soundLayer.pause '%s' idx=%d", name.c_str(), idx);
                    m_scene->soundLayerPause(idx);
                } else if (cmd.startsWith("anim_")) {
                    // Animation control: anim_play:name, anim_pause:name, anim_stop:name
                    for (auto& sv : m_soundVolumeScriptStates) {
                        if (sv.layerName == name && sv.hasAnimation) {
                            if (cmd.startsWith("anim_play:")) {
                                sv.anim.playing = true;
                                LOG_INFO("anim_play: layer='%s' anim='%s'",
                                         name.c_str(),
                                         sv.anim.name.c_str());
                            } else if (cmd.startsWith("anim_pause:")) {
                                sv.anim.playing = false;
                            } else if (cmd.startsWith("anim_stop:")) {
                                sv.anim.playing = false;
                                sv.anim.time    = 0;
                            }
                            break;
                        }
                    }
                }
            }

            // Process volume changes
            QJSValue dirty = entry.property("dirty");
            if (dirty.property("volume").toBool()) {
                float vol = (float)entry.property("volume").toNumber();
                if (std::isnan(vol) || vol < 0.0f) vol = 0.0f;
                if (vol > 1.0f) vol = 1.0f;
                m_scene->soundLayerSetVolume(idx, vol);
            }
        }
    }

    probeMark(s_t_sound);

    // Flush dirty scene properties (bloom, clear color, camera, lighting)
    if (m_collectDirtySceneFn.isCallable()) {
        bool     wasInterrupted = false;
        QJSValue sceneUpdate    = callJsGuarded(
            [this] {
                return m_collectDirtySceneFn.call();
            },
            &wasInterrupted);
        tickInterrupted = tickInterrupted || wasInterrupted;
        if (! sceneUpdate.isNull() && ! sceneUpdate.isUndefined()) {
            QJSValue dirty = sceneUpdate.property("dirty");

            if (dirty.property("clearColor").toBool()) {
                QJSValue c = sceneUpdate.property("clearColor");
                m_scene->updateClearColor((float)c.property("x").toNumber(),
                                          (float)c.property("y").toNumber(),
                                          (float)c.property("z").toNumber());
            }
            if (dirty.property("bloomStrength").toBool())
                m_scene->updateBloomStrength(
                    (float)sceneUpdate.property("bloomStrength").toNumber());
            if (dirty.property("bloomThreshold").toBool())
                m_scene->updateBloomThreshold(
                    (float)sceneUpdate.property("bloomThreshold").toNumber());
            if (dirty.property("cameraFov").toBool())
                m_scene->updateCameraFov((float)sceneUpdate.property("cameraFov").toNumber());
            if (dirty.property("cameraEye").toBool() || dirty.property("cameraCenter").toBool() ||
                dirty.property("cameraUp").toBool()) {
                // Send full lookAt when any camera vector changes
                QJSValue e =
                    m_jsEngine->globalObject().property("_sceneState").property("cameraEye");
                QJSValue ct =
                    m_jsEngine->globalObject().property("_sceneState").property("cameraCenter");
                QJSValue u =
                    m_jsEngine->globalObject().property("_sceneState").property("cameraUp");
                m_scene->updateCameraLookAt((float)e.property("x").toNumber(),
                                            (float)e.property("y").toNumber(),
                                            (float)e.property("z").toNumber(),
                                            (float)ct.property("x").toNumber(),
                                            (float)ct.property("y").toNumber(),
                                            (float)ct.property("z").toNumber(),
                                            (float)u.property("x").toNumber(),
                                            (float)u.property("y").toNumber(),
                                            (float)u.property("z").toNumber());
            }
            if (dirty.property("ambientColor").toBool()) {
                QJSValue c = sceneUpdate.property("ambientColor");
                m_scene->updateAmbientColor((float)c.property("x").toNumber(),
                                            (float)c.property("y").toNumber(),
                                            (float)c.property("z").toNumber());
            }
            if (dirty.property("skylightColor").toBool()) {
                QJSValue c = sceneUpdate.property("skylightColor");
                m_scene->updateSkylightColor((float)c.property("x").toNumber(),
                                             (float)c.property("y").toNumber(),
                                             (float)c.property("z").toNumber());
            }

            // Light updates
            QJSValue lights     = sceneUpdate.property("lights");
            int      lightCount = lights.property("length").toInt();
            for (int i = 0; i < lightCount; i++) {
                QJSValue entry = lights.property(i);
                int32_t  idx   = entry.property("idx").toInt();
                QJSValue ld    = entry.property("dirty");
                if (ld.property("color").toBool()) {
                    QJSValue c = entry.property("color");
                    m_scene->updateLightColor(idx,
                                              (float)c.property("x").toNumber(),
                                              (float)c.property("y").toNumber(),
                                              (float)c.property("z").toNumber());
                }
                if (ld.property("radius").toBool())
                    m_scene->updateLightRadius(idx, (float)entry.property("radius").toNumber());
                if (ld.property("intensity").toBool())
                    m_scene->updateLightIntensity(idx,
                                                  (float)entry.property("intensity").toNumber());
                if (ld.property("position").toBool()) {
                    QJSValue p = entry.property("position");
                    m_scene->updateLightPosition(idx,
                                                 (float)p.property("x").toNumber(),
                                                 (float)p.property("y").toNumber(),
                                                 (float)p.property("z").toNumber());
                }
            }
        }
    }

    // Evaluate sound volume scripts (after visible scripts set shared.*)
    for (auto& svState : m_soundVolumeScriptStates) {
        if (! svState.updateFn.isCallable()) continue;

        // Set thisLayer for this volume script (same as property scripts)
        if (! svState.layerName.empty()) {
            m_jsEngine->globalObject().setProperty("thisLayer", svState.thisLayerProxy);
            m_jsEngine->globalObject().setProperty("thisObject", svState.thisLayerProxy);
        }

        // Advance volume animation and use animated value as base.
        // Clock source: render-thread scene time (m_scene->elapsingTime), so audio
        // stays locked to visual animations even if the render loop throttles.
        float baseVolume = svState.currentVolume;
        if (svState.hasAnimation) {
            double sceneTime = m_scene->getSceneTime();
            if (svState.anim.lastSceneTime < 0.0) {
                svState.anim.lastSceneTime = sceneTime;
            }
            double delta = sceneTime - svState.anim.lastSceneTime;
            if (delta < 0.0) delta = 0.0; // guard against scene reload resetting clock
            svState.anim.lastSceneTime = sceneTime;
            if (svState.anim.playing) {
                svState.anim.time += delta;
            }
            baseVolume = svState.anim.evaluate();
            // Log animation state periodically (every ~3s at 30Hz)
            static int animDiagCounter = 0;
            if (++animDiagCounter % 90 == 1) {
                LOG_INFO("volAnim[%d] '%s': playing=%d t=%.1f frame=%.1f val=%.3f",
                         svState.index,
                         svState.anim.name.c_str(),
                         (int)svState.anim.playing,
                         svState.anim.time,
                         (float)(svState.anim.time * svState.anim.fps),
                         baseVolume);
            }
        }

        // Guard the author volume script under the watchdog and fold its
        // interrupt into the property tick's back-off (tickInterrupted drives the
        // back-off latch) — a runaway setVolume(()=>{while(true){}}) is now
        // interrupted instead of freezing the GUI thread.
        bool     svInterrupted = false;
        QJSValue result        = callJsGuarded(
            [&] {
                return svState.updateFn.call({ QJSValue((double)baseVolume) });
            },
            &svInterrupted);
        tickInterrupted = tickInterrupted || svInterrupted;
        if (result.isError()) {
            if (m_scriptDiag.soundVolErrored.find(svState.index) ==
                m_scriptDiag.soundVolErrored.end()) {
                m_scriptDiag.soundVolErrored.insert(svState.index);
                qCWarning(wekdeScene,
                          "Sound volume script error index=%d: %s",
                          svState.index,
                          qPrintable(result.toString()));
            }
            continue;
        }

        if (result.isNumber()) {
            float newVol = (float)result.toNumber();
            if (newVol < 0.0f) newVol = 0.0f;
            if (newVol > 1.0f) newVol = 1.0f;
            if (std::abs(newVol - svState.currentVolume) > 0.001f) {
                svState.currentVolume = newVol;
                m_scene->updateSoundVolume(svState.index, newVol);
            }
        }
    }

    // Flush console.log buffer from scripts
    {
        QJSValue consoleBuf = m_jsEngine->globalObject().property("console").property("_buf");
        if (consoleBuf.isArray()) {
            int len = consoleBuf.property("length").toInt();
            for (int i = 0; i < len; i++) {
                LOG_INFO("JS console.log: %s", qPrintable(consoleBuf.property(i).toString()));
            }
            if (len > 0) {
                m_jsEngine->evaluate("console._buf = [];");
            }
        }
    }

    probeMark(s_t_scene);
    s_t_total +=
        (s_t_prelude + s_t_scripts + s_t_dirtyCollect + s_t_dirtyDispatch + s_t_sound + s_t_scene) -
        s_t_total;

    // Periodic diagnostic logging (~every 3 seconds at 30Hz)
    static int propEvalCount = 0;
    if (s_probeTimings && propEvalCount > 0 && propEvalCount % 90 == 0) {
        qint64 tot = s_t_prelude + s_t_scripts + s_t_dirtyCollect + s_t_dirtyDispatch + s_t_sound +
                     s_t_scene;
        double denom = tot > 0 ? (double)tot : 1.0;
        LOG_INFO("TIMING per-tick avg (90 ticks): total=%.2fms = prelude=%.2fms(%.0f%%) "
                 "scripts=%.2fms(%.0f%%) dirtyCollect=%.2fms(%.0f%%) "
                 "dispatch=%.2fms(%.0f%%) sound=%.2fms(%.0f%%) scene=%.2fms(%.0f%%)",
                 tot / 90000.0,
                 s_t_prelude / 90000.0,
                 100.0 * s_t_prelude / denom,
                 s_t_scripts / 90000.0,
                 100.0 * s_t_scripts / denom,
                 s_t_dirtyCollect / 90000.0,
                 100.0 * s_t_dirtyCollect / denom,
                 s_t_dirtyDispatch / 90000.0,
                 100.0 * s_t_dirtyDispatch / denom,
                 s_t_sound / 90000.0,
                 100.0 * s_t_sound / denom,
                 s_t_scene / 90000.0,
                 100.0 * s_t_scene / denom);
        s_t_prelude = s_t_scripts = s_t_dirtyCollect = 0;
        s_t_dirtyDispatch = s_t_sound = s_t_scene = s_t_total = 0;
    }
    if (++propEvalCount % 90 == 1) {
        int sharedCount = m_jsEngine->evaluate("Object.keys(shared).length").toInt();
        LOG_INFO("PROPEVAL[%d]: %zu states, shared=%d, dirty=%d (miss=%d), soundVol=%zu "
                 "| gate evals=%llu skips=%llu (highRate=%d)",
                 propEvalCount,
                 (size_t)m_propertyScriptStates.size(),
                 sharedCount,
                 dirtyLayerCount,
                 dirtyLayerMiss,
                 (size_t)m_soundVolumeScriptStates.size(),
                 (unsigned long long)s_propEvalCount,
                 (unsigned long long)s_propSkipCount,
                 (int)m_propertyHighRate);
        s_propEvalCount = s_propSkipCount = 0;
        qCInfo(wekdeScene,
               "Property scripts: %zu states, shared vars: %d, dirty layers: %d (miss: %d), sound "
               "vol: %zu",
               (size_t)m_propertyScriptStates.size(),
               sharedCount,
               dirtyLayerCount,
               dirtyLayerMiss,
               (size_t)m_soundVolumeScriptStates.size());
        // Dump sound volume states
        for (const auto& sv : m_soundVolumeScriptStates) {
            qCInfo(wekdeScene,
                   "  sound[%d] vol=%.3f callable=%d",
                   sv.index,
                   sv.currentVolume,
                   (int)sv.updateFn.isCallable());
        }
        // Dump key shared variables to verify simulation output
        QJSValue sharedObj = m_jsEngine->globalObject().property("shared");
        if (! sharedObj.isUndefined()) {
            QString dump;
            for (const char* key : { "p1x",
                                     "p1y",
                                     "p1z",
                                     "sunsize",
                                     "rotX",
                                     "rotY",
                                     "p3x",
                                     "p3y",
                                     "p3z",
                                     "p6x",
                                     "p6y",
                                     "musicse",
                                     "musicvolume",
                                     "volume",
                                     "songplays",
                                     "uiopacity",
                                     "playOnStart",
                                     "progress",
                                     // 2866203962 player UI visibility drivers
                                     "playerproximity",
                                     "enablePlayer",
                                     // 3509243656 3body state machine drivers
                                     "tj",
                                     "rst",
                                     "num",
                                     "auto",
                                     "dd",
                                     "cfw",
                                     "kt",
                                     "ylll",
                                     "universeCount",
                                     // MAIN update entry markers — set every
                                     // tick if MAIN is running
                                     "kg",
                                     "ktime",
                                     "rxz" }) {
                QJSValue v = sharedObj.property(key);
                if (! v.isUndefined()) {
                    dump += QString("%1=%2 ").arg(key).arg(v.toNumber(), 0, 'f', 4);
                }
            }
            if (! dump.isEmpty()) {
                LOG_INFO("Shared vars: %s", qPrintable(dump));
                qCInfo(wekdeScene, "Shared vars: %s", qPrintable(dump));
                // One-shot Math.random sanity probe — if always 0, the JS
                // engine's PRNG is broken (bodies can't get random init).
                if (! m_scriptDiag.mathRandomProbed) {
                    m_scriptDiag.mathRandomProbed = true;
                    QJSValue r = m_jsEngine->evaluate(
                        "Math.random().toFixed(6) + ',' + Math.random().toFixed(6) +"
                        "',' + Math.random().toFixed(6)");
                    LOG_INFO("Math.random probe: %s", qPrintable(r.toString()));
                }
            } else {
                LOG_INFO("Shared vars: (none of the expected keys found)");
            }
        }
    }

    // A3-T2 back-off — a clean tick resets the run; consecutive interrupts
    // accumulate.  Once the run reaches kDisableAfterInterrupts, disable this
    // wallpaper's property scripts (stop the timer, latch the gate, log once) so
    // a persistent runaway stops animating instead of perpetually losing one
    // tick (and pinning a CPU on the watchdog) every 8ms.
    // A3-T2 back-off (shared helper — see applyInterruptBackoff).  A clean tick
    // resets the run; once K consecutive interrupts disable this wallpaper's
    // property scripts, bail before chaining color eval.
    if (applyInterruptBackoff(tickInterrupted,
                              m_consecutivePropInterrupts,
                              m_propertyScriptsDisabled,
                              m_propertyTimer,
                              "property"))
        return; // do not chain color eval on a wallpaper we just disabled

    // Chain color/shader-value evaluation into the same tick so transient
    // signaling state (shared.brushEditor.hasChanged = true → consume → set
    // false next property tick) doesn't race the separate color timer.
    // Game of Life (3453251764) buttons set brushEditor.changed in cursorUp;
    // Canvas's visible property script promotes it to hasChanged on the
    // next property tick (and clears the next-next).  Buttons (color
    // scripts) refresh their selected state when they see hasChanged=true
    // — but with property at 8ms and color at 33ms cadence, color always
    // lands after hasChanged has been cleared, and tool selection never
    // visually updated.  Running color in the same tick removes the race.
    if (! m_colorScriptStates.empty() || ! m_shaderValueScriptStates.empty()) {
        evaluateColorScripts();
    }
}

void SceneObject::cleanupTextScripts() {
    // Fire destroy event on all scripts before cleanup
    fireDestroyEvent();

    // Flush any pending localStorage writes AND reset the loaded state so
    // a subsequent setSource (e.g. switching wallpapers without destroying
    // this QQuickItem) reloads the correct per-scene store instead of
    // serving the previous scene's data.
    if (m_lsGlobalDirty || m_lsScreenDirty) flushLocalStorage();
    m_lsLoaded = false;
    m_lsGlobal = {};
    m_lsScreen = {};
    m_lsSceneId.clear();
    // Stop + delete the localStorage debounce timer AFTER the flush above, or a
    // single-shot still pending from scheduleLocalStorageFlush() could fire
    // flushLocalStorage() against the half-torn-down state (m_jsEngine is
    // deleted at the tail of this function) on a wallpaper switch (F19).
    if (m_lsFlushTimer) {
        m_lsFlushTimer->stop();
        delete m_lsFlushTimer;
        m_lsFlushTimer = nullptr;
    }

    m_textScriptStates.clear();
    m_colorScriptStates.clear();
    m_shaderValueScriptStates.clear();
    m_propertyScriptStates.clear();
    m_soundVolumeScriptStates.clear();
    // A3-T2: clear the per-loop interrupt back-off so a switch to a healthy
    // wallpaper isn't pre-disabled by a previous one's runaway (Item 05).
    m_consecutiveTextInterrupts  = 0;
    m_textScriptsDisabled        = false;
    m_consecutiveColorInterrupts = 0;
    m_colorScriptsDisabled       = false;
    m_consecutivePropInterrupts  = 0;
    m_propertyScriptsDisabled    = false;
    // Item 15: clear per-load dedup / one-shot diagnostics so the next loaded
    // wallpaper (or a setSource reload) starts fresh and a recycled node id that
    // errored in the previous scene logs again.
    m_scriptDiag.clear();
    m_nodeNameToId.clear();
    m_collectDirtyLayersFn    = QJSValue();
    m_fireSceneEventFn        = QJSValue();
    m_hasSceneListenersFn     = QJSValue();
    m_runAllPropertyScriptsFn = QJSValue();
    m_vec2Fn                  = QJSValue();
    m_vec3Fn                  = QJSValue();
    m_vec4Fn                  = QJSValue();
    // Drain the JS-side script array so a reload starts fresh.  The engine
    // itself stays alive; just its cached references need to clear.
    if (m_jsEngine) {
        m_jsEngine->evaluate("if (typeof _allPropertyScripts !== 'undefined') "
                             "_allPropertyScripts.length = 0;");
    }
    m_soundLayerStates.clear();
    m_soundLayerNameToIndex.clear();
    m_collectDirtySoundLayersFn = QJSValue();
    m_cursorTargets.clear();
    m_hoveredLayers.clear();
    m_dragTarget.clear();
    // Stop + delete the hover-leave debounce timer and drop any pending leaves
    // (F19).  A single-shot still armed from hoverMoveEvent()/flushPendingLeaves()
    // would otherwise fire flushPendingLeaves() against stale m_pendingLeaves
    // entries after the script state and m_jsEngine are torn down on a wallpaper
    // switch — a use-after-teardown on the proxies the leave handlers call into.
    if (m_hoverLeaveTimer) {
        m_hoverLeaveTimer->stop();
        delete m_hoverLeaveTimer;
        m_hoverLeaveTimer = nullptr;
    }
    m_pendingLeaves.clear();
    if (m_textTimer) {
        m_textTimer->stop();
        delete m_textTimer;
        m_textTimer = nullptr;
    }
    if (m_colorTimer) {
        m_colorTimer->stop();
        delete m_colorTimer;
        m_colorTimer = nullptr;
    }
    if (m_propertyTimer) {
        m_propertyTimer->stop();
        delete m_propertyTimer;
        m_propertyTimer = nullptr;
    }
    if (m_timerBridge) {
        m_timerBridge->clearAll();
        delete m_timerBridge;
        m_timerBridge = nullptr;
    }
    // Stop + join the watchdog monitor thread BEFORE deleting the engine.  join()
    // blocks until any in-flight setInterrupted() has returned, so a watchdog
    // fire can never touch a freed QJSEngine (A3-T2 engine-pointer safety).
    m_jsWatchdog.stop();
    if (m_jsEngine) {
        delete m_jsEngine;
        m_jsEngine = nullptr;
    }
}

#include "SceneBackend.moc"
#include "moc_SceneTimerBridge.cpp"
