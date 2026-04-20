#include "SceneBackend.hpp"
#include "SceneCursorHitTest.h"
#include "HoverLeaveDebounce.h"
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
#include <atomic>
#include <array>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <QtCore/QRegularExpression>
#include <QtCore/QTime>

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

#define _Q_INFO(fmt, ...) qCInfo(wekdeScene, fmt, __VA_ARGS__)

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
        : m_texture(nullptr),
          m_scene(scene),
          m_enable_valid(valid),
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
        _Q_INFO("Destroy texnode", "");
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
    _Q_INFO("Destroy sceneobject", "");
}

void SceneObject::resizeFb() {
    int w = (int)this->width();
    int h = (int)this->height();
    fireResizeScreen(w, h);
}

QSGNode* SceneObject::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    TextureNode* node = static_cast<TextureNode*>(oldNode);
    if (! node) {
        node = new TextureNode(window(), m_scene, m_enable_valid, [this](QQuickWindow* window) {
            return (QSGTexture*)nullptr;
        });
        if (node->initGl()) {
            node->initVulkan(width() * window()->devicePixelRatio(),
                             height() * window()->devicePixelRatio(),
                             m_hdrOutput);

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
    // Escape single quotes and backslashes for safe embedding in a JS string literal.
    // JSON values use double quotes so single quotes only appear inside string values.
    QString json = m_userProperties;
    json.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    json.replace(QLatin1Char('\''), QStringLiteral("\\'"));
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
    // Derive engine.colorScheme from the schemecolor user property if Vec3 is available.
    // schemecolor is stored as a space-separated float string e.g. "0.5 0.1 0.9".
    // Only update if Vec3 exists (may be called before Vec3 is defined on first init).
    m_jsEngine->evaluate("(function(){"
                         "if (typeof Vec3 === 'undefined') return;"
                         "var sc = engine.userProperties.schemecolor;"
                         "if (sc !== undefined && sc !== null)"
                         "  engine.colorScheme = Vec3.fromString(sc);"
                         "})()");

    // Fire applyUserProperties event on all property scripts that define it.
    // WE SceneScript passes an object with {propertyName: {value: newValue}} entries.
    fireApplyUserProperties();
}

void SceneObject::fireSceneEventListeners(const QString& eventName, const QJSValueList& args) {
    if (! m_fireSceneEventFn.isCallable()) return;
    QJSValue has = m_hasSceneListenersFn.call({ QJSValue(eventName) });
    if (! has.toBool()) return;
    QJSValueList callArgs;
    callArgs << QJSValue(eventName);
    callArgs.append(args);
    m_fireSceneEventFn.call(callArgs);
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
        }

        QJSValue result = state.applyUserPropertiesFn.call({ propsArg });
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
        }

        QJSValue result = svState.applyUserPropertiesFn.call({ propsArg });
        if (result.isError()) {
            LOG_INFO("applyUserProperties error vol index=%d: %s",
                     svState.index,
                     qPrintable(result.toString()));
        }
    }

    fireSceneEventListeners("applyUserProperties", { propsArg });
}

void SceneObject::fireDestroyEvent() {
    if (! m_jsEngine || m_propertyScriptStates.empty()) return;

    for (auto& state : m_propertyScriptStates) {
        if (! state.destroyFn.isCallable()) continue;

        if (! state.layerName.empty()) {
            m_jsEngine->globalObject().setProperty("thisLayer", state.thisLayerProxy);
        }

        QJSValue result = state.destroyFn.call({});
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
        }

        QJSValue result = state.resizeScreenFn.call({ QJSValue(width), QJSValue(height) });
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
        s.mediaPlaybackChangedFn.call({ event });
    }
    fireSceneEventListeners("mediaPlaybackChanged", { event });
}

void SceneObject::mediaPropertiesChanged(const QString& title, const QString& artist,
                                         const QString& albumTitle, const QString& albumArtist,
                                         const QString& genres) {
    if (! m_jsEngine) return;
    QJSValue event = m_jsEngine->newObject();
    event.setProperty("title", title);
    event.setProperty("artist", artist);
    event.setProperty("albumTitle", albumTitle);
    event.setProperty("albumArtist", albumArtist);
    event.setProperty("genres", genres);
    event.setProperty("contentType", QJSValue("media"));
    for (auto& s : m_propertyScriptStates) {
        if (! s.mediaPropertiesChangedFn.isCallable()) continue;
        if (! s.layerName.empty())
            m_jsEngine->globalObject().setProperty("thisLayer", s.thisLayerProxy);
        s.mediaPropertiesChangedFn.call({ event });
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
        s.mediaThumbnailChangedFn.call({ event });
    }
    fireSceneEventListeners("mediaThumbnailChanged", { event });
}

void SceneObject::mediaTimelineChanged(double position, double duration) {
    if (! m_jsEngine) return;
    QJSValue event = m_jsEngine->newObject();
    event.setProperty("position", position);
    event.setProperty("duration", duration);
    for (auto& s : m_propertyScriptStates) {
        if (! s.mediaTimelineChangedFn.isCallable()) continue;
        if (! s.layerName.empty())
            m_jsEngine->globalObject().setProperty("thisLayer", s.thisLayerProxy);
        s.mediaTimelineChangedFn.call({ event });
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
        s.mediaStatusChangedFn.call({ event });
    }
    fireSceneEventListeners("mediaStatusChanged", { event });
}

void SceneObject::play() { m_scene->play(); }
void SceneObject::pause() { m_scene->pause(); }

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
    // Fire cursorDown on the smallest-hit layer with downFn.
    int downFired = 0;
    if (downIdx >= 0) {
        auto& target = m_cursorTargets[downIdx];
        m_jsEngine->globalObject().setProperty("thisLayer", target.thisLayerProxy);
        QJSValue r = target.downFn.call({ ev });
        downFired  = 1;
        if (r.isError())
            LOG_INFO(
                "cursorDown error on '%s': %s", target.layerName.c_str(), qPrintable(r.toString()));
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
            QJSValue r = target.downFn.call({ ev });
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
        QJSValue r = target.clickFn.call({ ev });
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

    // cursorUp: fire only on the drag target chosen at press (matches the
    // layer that received cursorDown).  When there was no hit target at
    // press (dino_run "tap anywhere" fallback), fan out to mirror whatever
    // cursorDown did — otherwise game-control state machines get stuck.
    QJSValue ev      = makeCursorEvent(m_jsEngine, sceneX, sceneY);
    int      upFired = 0;
    if (! m_dragTarget.empty()) {
        for (auto& target : m_cursorTargets) {
            if (target.layerName != m_dragTarget) continue;
            if (! target.upFn.isCallable()) break;
            m_jsEngine->globalObject().setProperty("thisLayer", target.thisLayerProxy);
            QJSValue r = target.upFn.call({ ev });
            upFired    = 1;
            if (r.isError())
                LOG_INFO("cursorUp error on '%s': %s",
                         target.layerName.c_str(),
                         qPrintable(r.toString()));
            break;
        }
    } else {
        for (auto& target : m_cursorTargets) {
            if (! target.upFn.isCallable()) continue;
            m_jsEngine->globalObject().setProperty("thisLayer", target.thisLayerProxy);
            QJSValue r = target.upFn.call({ ev });
            upFired++;
            if (r.isError())
                LOG_INFO("cursorUp error on '%s': %s",
                         target.layerName.c_str(),
                         qPrintable(r.toString()));
        }
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
            QJSValue ev = makeCursorEvent(m_jsEngine, sceneX, sceneY);
            QJSValue r  = target.moveFn.call({ ev });
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
            QJSValue ev = makeCursorEvent(m_jsEngine, sceneX, sceneY);
            QJSValue r  = target.enterFn.call({ ev });
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
                QJSValue ev = makeCursorEvent(m_jsEngine, m_cursorSceneX, m_cursorSceneY);
                target.leaveFn.call({ ev });
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

    // Expose SceneObject to JS as __sceneBridge so layer proxies can call its
    // Q_INVOKABLE methods (videoXxx, ...).  Parent is `this`, lifetime is tied
    // to the QJSEngine which we own; newQObject wraps without taking ownership
    // (QJSEngine::ObjectOwnership::CppOwnership by default for child QObjects).
    m_jsEngine->globalObject().setProperty("__sceneBridge", m_jsEngine->newQObject(this));

    // Provide a minimal 'engine' global with runtime and timeOfDay
    m_runtimeTimer.start();
    QJSValue engineObj = m_jsEngine->newObject();
    engineObj.setProperty("frametime", 0.5); // ~500ms timer
    engineObj.setProperty("runtime", 0.0);
    engineObj.setProperty("timeOfDay", 0.0);
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
    m_timerBridge =
        new SceneTimerBridge(m_jsEngine, this, [this](int id, bool error, const QString& msg) {
            if (error) {
                LOG_INFO("Timer callback error (id=%d): %s", id, qPrintable(msg));
            }
            flushJsConsole(m_jsEngine, "timer");
        });
    m_jsEngine->globalObject().setProperty("_timerBridge", m_jsEngine->newQObject(m_timerBridge));
    m_jsEngine->evaluate("function setTimeout(fn, delay)  { return _timerBridge.createTimer(fn, "
                         "delay || 0, false); }\n"
                         "function setInterval(fn, delay) { return _timerBridge.createTimer(fn, "
                         "delay || 0, true); }\n"
                         "function clearTimeout(id)  { _timerBridge.clearTimer(id); }\n"
                         "function clearInterval(id) { _timerBridge.clearTimer(id); }\n");

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
    m_jsEngine->evaluate(
        "var input = { cursorWorldPosition: { x: 0, y: 0 },\n"
        "  cursorScreenPosition: { x: 0, y: 0 },\n"
        "  cursorLeftDown: false };\n"
        // Vec2 factory — 2D vector utility matching WE's Vec2 class
        "function Vec2(x, y) {\n"
        "  if (typeof x === 'string') { var p=x.trim().split(/\\s+/); x=parseFloat(p[0])||0; "
        "y=parseFloat(p[1])||0; }\n"
        "  else if (x && typeof x === 'object') { y=x.y||0; x=x.x||0; }\n"
        "  else if (y === undefined) { y = x||0; x = x||0; }\n"
        "  var v = { x: x||0, y: y||0 };\n"
        "  v.copy = function() { return Vec2(v.x, v.y); };\n"
        "  v.length = function() { return Math.sqrt(v.x*v.x+v.y*v.y); };\n"
        "  v.lengthSqr = function() { return v.x*v.x+v.y*v.y; };\n"
        "  v.normalize = function() { var l=v.length()||1; return Vec2(v.x/l,v.y/l); };\n"
        "  v.add = function(o) { return Vec2(v.x+o.x, v.y+o.y); };\n"
        "  v.subtract = function(o) { return Vec2(v.x-o.x, v.y-o.y); };\n"
        "  v.multiply = function(s) { return typeof s==='object'? Vec2(v.x*s.x,v.y*s.y): "
        "Vec2(v.x*s,v.y*s); };\n"
        "  v.divide = function(s) { return typeof s==='object'? Vec2(v.x/s.x,v.y/s.y): "
        "Vec2(v.x/s,v.y/s); };\n"
        "  v.dot = function(o) { return v.x*o.x+v.y*o.y; };\n"
        "  v.perpendicular = function() { return Vec2(-v.y, v.x); };\n"
        "  v.reflect = function(n) { var d=2*v.dot(n); return Vec2(v.x-d*n.x, v.y-d*n.y); };\n"
        "  v.mix = function(o,t) { return Vec2(v.x+(o.x-v.x)*t, v.y+(o.y-v.y)*t); };\n"
        "  v.equals = function(o) { return v.x===o.x && v.y===o.y; };\n"
        "  v.toString = function() { return v.x+' '+v.y; };\n"
        "  v.min = function(o) { return Vec2(Math.min(v.x,o.x),Math.min(v.y,o.y)); };\n"
        "  v.max = function(o) { return Vec2(Math.max(v.x,o.x),Math.max(v.y,o.y)); };\n"
        "  v.abs = function() { return Vec2(Math.abs(v.x),Math.abs(v.y)); };\n"
        "  v.sign = function() { return Vec2(Math.sign(v.x),Math.sign(v.y)); };\n"
        "  v.round = function() { return Vec2(Math.round(v.x),Math.round(v.y)); };\n"
        "  v.floor = function() { return Vec2(Math.floor(v.x),Math.floor(v.y)); };\n"
        "  v.ceil = function() { return Vec2(Math.ceil(v.x),Math.ceil(v.y)); };\n"
        "  return v;\n"
        "}\n"
        "Vec2.fromString = function(s) { var p=String(s).trim().split(/\\s+/); return "
        "Vec2(parseFloat(p[0])||0,parseFloat(p[1])||0); };\n"
        "Vec2.lerp = function(a,b,t) { return Vec2(a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t); };\n"
        // Vec3 — prototype-based class so methods are shared across
        // instances.  The old impl closed over ~24 methods + 3 defineProperty
        // calls per Vec3(), which ran tens of thousands of times per tick in
        // scenes like 3body (3509243656) with 1200 trail points.  Prototype
        // dispatch keeps construction to a single {x,y,z} allocation.
        "function Vec3(x, y, z) {\n"
        "  if (!(this instanceof Vec3)) {\n"
        "    var v = Object.create(Vec3.prototype);\n"
        "    Vec3.call(v, x, y, z);\n"
        "    return v;\n"
        "  }\n"
        "  if (typeof x === 'string') {\n"
        "    var p = x.trim().split(/\\s+/);\n"
        "    this.x = parseFloat(p[0])||0;\n"
        "    this.y = parseFloat(p[1])||0;\n"
        "    this.z = parseFloat(p[2])||0;\n"
        "  } else if (x && typeof x === 'object') {\n"
        "    this.x = x.x||0; this.y = x.y||0; this.z = x.z||0;\n"
        "  } else {\n"
        "    this.x = x||0; this.y = y||0; this.z = z||0;\n"
        "  }\n"
        "}\n"
        "Vec3.prototype.multiply  = function(s) { return Vec3(this.x*s, this.y*s, this.z*s); };\n"
        "Vec3.prototype.add       = function(o) { return Vec3(this.x+o.x, this.y+o.y, this.z+o.z); "
        "};\n"
        "Vec3.prototype.subtract  = function(o) { return Vec3(this.x-o.x, this.y-o.y, this.z-o.z); "
        "};\n"
        "Vec3.prototype.length    = function() { return "
        "Math.sqrt(this.x*this.x+this.y*this.y+this.z*this.z); };\n"
        "Vec3.prototype.normalize = function() { var l=this.length()||1; return "
        "Vec3(this.x/l,this.y/l,this.z/l); };\n"
        "Vec3.prototype.copy      = function() { return Vec3(this.x, this.y, this.z); };\n"
        "Vec3.prototype.dot       = function(o) { return this.x*o.x+this.y*o.y+this.z*o.z; };\n"
        "Vec3.prototype.cross     = function(o) { return Vec3(this.y*o.z-this.z*o.y, "
        "this.z*o.x-this.x*o.z, this.x*o.y-this.y*o.x); };\n"
        "Vec3.prototype.negate    = function() { return Vec3(-this.x,-this.y,-this.z); };\n"
        "Vec3.prototype.divide    = function(s) { return Vec3(this.x/s, this.y/s, this.z/s); };\n"
        "Vec3.prototype.lerp      = function(o, t) { return Vec3(this.x+(o.x-this.x)*t, "
        "this.y+(o.y-this.y)*t, this.z+(o.z-this.z)*t); };\n"
        "Vec3.prototype.distance  = function(o) { var dx=this.x-o.x,dy=this.y-o.y,dz=this.z-o.z; "
        "return Math.sqrt(dx*dx+dy*dy+dz*dz); };\n"
        "Vec3.prototype.lengthSqr = function() { return this.x*this.x+this.y*this.y+this.z*this.z; "
        "};\n"
        "Vec3.prototype.reflect   = function(n) { var d=2*this.dot(n); return Vec3(this.x-d*n.x, "
        "this.y-d*n.y, this.z-d*n.z); };\n"
        "Vec3.prototype.mix       = function(o,t) { return Vec3(this.x+(o.x-this.x)*t, "
        "this.y+(o.y-this.y)*t, this.z+(o.z-this.z)*t); };\n"
        "Vec3.prototype.equals    = function(o) { return this.x===o.x && this.y===o.y && "
        "this.z===o.z; };\n"
        "Vec3.prototype.toString  = function() { return this.x+' '+this.y+' '+this.z; };\n"
        "Vec3.prototype.min       = function(o) { return "
        "Vec3(Math.min(this.x,o.x),Math.min(this.y,o.y),Math.min(this.z,o.z)); };\n"
        "Vec3.prototype.max       = function(o) { return "
        "Vec3(Math.max(this.x,o.x),Math.max(this.y,o.y),Math.max(this.z,o.z)); };\n"
        "Vec3.prototype.abs       = function() { return "
        "Vec3(Math.abs(this.x),Math.abs(this.y),Math.abs(this.z)); };\n"
        "Vec3.prototype.sign      = function() { return "
        "Vec3(Math.sign(this.x),Math.sign(this.y),Math.sign(this.z)); };\n"
        "Vec3.prototype.round     = function() { return "
        "Vec3(Math.round(this.x),Math.round(this.y),Math.round(this.z)); };\n"
        "Vec3.prototype.floor     = function() { return "
        "Vec3(Math.floor(this.x),Math.floor(this.y),Math.floor(this.z)); };\n"
        "Vec3.prototype.ceil      = function() { return "
        "Vec3(Math.ceil(this.x),Math.ceil(this.y),Math.ceil(this.z)); };\n"
        // In-place setter — lets hot-path scripts reuse a Vec3 instead of
        // allocating a fresh object every update.  Returns `this` to chain.
        "Vec3.prototype.set       = function(x, y, z) { this.x=x||0; this.y=y||0; this.z=z||0; "
        "return this; };\n"
        // r/g/b aliases for color-vector usage (shared on prototype).
        "Object.defineProperty(Vec3.prototype,'r',{get:function(){return "
        "this.x;},set:function(v){this.x=v;},enumerable:true});\n"
        "Object.defineProperty(Vec3.prototype,'g',{get:function(){return "
        "this.y;},set:function(v){this.y=v;},enumerable:true});\n"
        "Object.defineProperty(Vec3.prototype,'b',{get:function(){return "
        "this.z;},set:function(v){this.z=v;},enumerable:true});\n"
        // Vec3.fromString: parses a WE color string "r g b" (space-separated floats)
        "Vec3.fromString = function(s) {\n"
        "  var p = String(s).trim().split(/\\s+/);\n"
        "  return Vec3(parseFloat(p[0])||0, parseFloat(p[1])||0, parseFloat(p[2])||0);\n"
        "};\n"
        // Vec3.lerp(a, b, t): static linear interpolation between two Vec3 values
        "Vec3.lerp = function(a, b, t) { return Vec3(a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t, "
        "a.z+(b.z-a.z)*t); };\n"
        // Safe String.match: return empty array instead of null (prevents null.forEach crashes)
        "var _origMatch = String.prototype.match;\n"
        "String.prototype.match = function(re) { return _origMatch.call(this, re) || []; };\n"
        "var localStorage = (function() {\n"
        "  var _store = {};\n"
        "  return {\n"
        "    LOCATION_GLOBAL: 0, LOCATION_SCREEN: 1,\n"
        "    get: function(key, loc) { return _store.hasOwnProperty(key) ? _store[key] : "
        "undefined; },\n"
        "    set: function(key, value, loc) { _store[key] = value; },\n"
        "    remove: function(key, loc) { delete _store[key]; },\n"
        "    'delete': function(key, loc) { delete _store[key]; },\n"
        "    clear: function(loc) { _store = {}; }\n"
        "  };\n"
        "})();\n");

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
        // WEVector module: 2D angle↔vector conversion
        "var WEVector = {\n"
        "  angleVector2: function(angle) { return Vec2(Math.cos(angle), Math.sin(angle)); },\n"
        "  vectorAngle2: function(dir) { return Math.atan2(dir.y, dir.x); }\n"
        "};\n");

    // engine.colorScheme: the wallpaper's scheme/accent color as a Vec3 (r/g/b).
    // Default white (1,1,1); refreshJsUserProperties() overrides it from schemecolor.
    m_jsEngine->evaluate("engine.colorScheme = Vec3(1,1,1);\n");

    // Populate engine.userProperties with defaults from project.json first,
    // then apply QML-side overrides (if any). Must run AFTER Vec3 is defined
    // so colorScheme derivation works.
    {
        std::string defaultsJson = m_scene->getUserPropertiesJson();
        if (! defaultsJson.empty() && defaultsJson != "{}") {
            QString escaped = QString::fromStdString(defaultsJson);
            escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
            escaped.replace(QLatin1Char('\''), QStringLiteral("\\'"));
            m_jsEngine->evaluate(QString("(function(){"
                                         "var p=JSON.parse('%1');"
                                         "var up=engine.userProperties;"
                                         "for(var k in p) up[k]=p[k];"
                                         "})()")
                                     .arg(escaped));
        }
    }
    refreshJsUserProperties();

    // engine.openUserShortcut stub
    m_jsEngine->evaluate("engine.openUserShortcut = function(name) {};\n");

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
    m_jsEngine->evaluate("function createScriptProperties() {\n"
                         "  var builder = {};\n"
                         "  function addProp(def) {\n"
                         "    var n = def.name || def.n;\n"
                         "    if (n) builder[n] = def.value;\n"
                         "    return builder;\n"
                         "  }\n"
                         "  builder.addCheckbox = addProp;\n"
                         "  builder.addSlider = addProp;\n"
                         "  builder.addCombo = addProp;\n"
                         "  builder.addText = addProp;\n"
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
            QString escaped = QString::fromStdString(initJson);
            escaped.replace("\\", "\\\\");
            escaped.replace("'", "\\'");
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
    m_jsEngine->evaluate(
        "var _layerCache = {};\n"
        // Pool-layer fast path: for slots whose name starts with __pool_,
        // skip the defineProperty-based proxy entirely and return a plain
        // object.  Scripts mutate origin/scale/angles/alpha/visible directly
        // (no setter overhead).  _collectDirtyLayers snapshots and compares
        // at tick boundary.  Cuts ~200k per-second setter invocations for
        // 1200-slot trail scenes like 3body.
        "function _makePoolLayerProxy(name) {\n"
        "  var init = _layerInitStates[name];\n"
        "  var p = {\n"
        "    _pool: true,\n"
        "    name: name,\n"
        "    origin: init ? Vec3(init.o[0], init.o[1], init.o[2]) : Vec3(0,0,0),\n"
        "    scale:  init ? Vec3(init.s[0], init.s[1], init.s[2]) : Vec3(1,1,1),\n"
        "    angles: init ? Vec3(init.a[0], init.a[1], init.a[2]) : Vec3(0,0,0),\n"
        "    alpha: 1.0, visible: init ? init.v : true,\n"
        "    color: Vec3(1,1,1), text: '', pointsize: 0,\n"
        "    __asset: null\n"
        "  };\n"
        // Snapshot used by _collectDirtyLayers to detect changes.
        "  p._prev = { ox: p.origin.x, oy: p.origin.y, oz: p.origin.z,\n"
        "              sx: p.scale.x, sy: p.scale.y, sz: p.scale.z,\n"
        "              ax: p.angles.x, ay: p.angles.y, az: p.angles.z,\n"
        "              alpha: p.alpha, visible: p.visible };\n"
        "  return p;\n"
        "}\n"
        "function _makeLayerProxy(name) {\n"
        "  if (name.charCodeAt(0) === 95 && name.charCodeAt(1) === 95 &&\n"
        "      name.charCodeAt(2) === 112 && name.charCodeAt(3) === 111 &&\n"
        "      name.charCodeAt(4) === 111 && name.charCodeAt(5) === 108) {\n"
        "    return _makePoolLayerProxy(name);\n"
        "  }\n"
        "  var init = _layerInitStates[name];\n"
        "  var _s = init ? {\n"
        "    origin: Vec3(init.o[0], init.o[1], init.o[2]),\n"
        "    scale:  Vec3(init.s[0], init.s[1], init.s[2]),\n"
        "    angles: Vec3(init.a[0], init.a[1], init.a[2]),\n"
        "    size: init.sz ? {x:init.sz[0], y:init.sz[1]} : {x:0, y:0},\n"
        "    parallaxDepth: init.pd ? {x:init.pd[0], y:init.pd[1]} : {x:0, y:0},\n"
        "    visible: init.v, alpha: 1.0,\n"
        "    text: '', pointsize: init.ps || 0,\n"
        "    name: name, _dirty: {}, _cmds: []\n"
        "  } : { origin: Vec3(0,0,0), scale: Vec3(1,1,1),\n"
        "        angles: Vec3(0,0,0), size: {x:0, y:0},\n"
        "        parallaxDepth: {x:0, y:0},\n"
        "        visible: true, alpha: 1.0,\n"
        "        text: '', pointsize: 0,\n"
        "        name: name, _dirty: {}, _cmds: [] };\n"
        "  var p = {};\n"
        "  Object.defineProperty(p, 'name', { get: function(){return _s.name;}, enumerable:true "
        "});\n"
        "  Object.defineProperty(p, 'debug', { get: function(){return undefined;}, enumerable:true "
        "});\n"
        // Getters return a fresh Vec3 copy each call — matches WE semantics where
        // `let o = thisLayer.origin; o.x = 5;` mutates a snapshot, and only an
        // explicit `thisLayer.origin = o` writes back.  Without this, scripts that
        // cache `initialPosition = thisLayer.origin` end up sharing a reference
        // with `marioOrigin = thisLayer.origin`, so `isInAir` checks always fail.
        "  var vec3Props = ['origin','scale','angles'];\n"
        "  for (var i=0; i<vec3Props.length; i++) {\n"
        "    (function(prop){\n"
        "      Object.defineProperty(p, prop, {\n"
        // Getter still copies — scripts that do
        //   initialPos = thisLayer.origin; marioPos = thisLayer.origin;
        // rely on these being independent snapshots; otherwise a later
        // mutation to one affects the other.  See the proxy-aliasing
        // gotcha note in dynamic-asset-pool.md.
        "        get: function(){ var s = _s[prop]; return Vec3(s.x, s.y, s.z); },\n"
        // Setter: skip the defensive Vec3 copy.  Scripts overwhelmingly do
        //   thisLayer.origin = Vec3(x, y, z)
        // with a fresh allocation — the old impl then allocated ANOTHER Vec3
        // to copy from that one, an alloc storm in hot paths like 3body's
        // 1200-trail updateTrailAppearance.  Stored value is what the script
        // passed; if it later mutates that reference, that's scripts-own
        // aliasing behavior (matches WE).
        "        set: function(v){ _s[prop] = v ? (v instanceof Vec3 ? v : Vec3(v.x||0, v.y||0, "
        "v.z||0)) : Vec3(0,0,0);\n"
        "                          _s._dirty[prop] = true; },\n"
        "        enumerable: true\n"
        "      });\n"
        "    })(vec3Props[i]);\n"
        "  }\n"
        "  var scalarProps = ['visible','alpha'];\n"
        "  for (var j=0; j<scalarProps.length; j++) {\n"
        "    (function(prop){\n"
        "      Object.defineProperty(p, prop, {\n"
        "        get: function(){ return _s[prop]; },\n"
        "        set: function(v){ _s[prop] = v; _s._dirty[prop] = true; },\n"
        "        enumerable: true\n"
        "      });\n"
        "    })(scalarProps[j]);\n"
        "  }\n"
        "  Object.defineProperty(p, 'text', {\n"
        "    get: function(){ return _s.text; },\n"
        "    set: function(v){ _s.text = v; _s._dirty.text = true; },\n"
        "    enumerable: true\n"
        "  });\n"
        "  Object.defineProperty(p, 'pointsize', {\n"
        "    get: function(){ return _s.pointsize; },\n"
        "    set: function(v){ _s.pointsize = v; _s._dirty.pointsize = true;\n"
        "                      _s._dirty.text = true; },\n"
        "    enumerable: true\n"
        "  });\n"
        "  Object.defineProperty(p, 'size', {\n"
        "    get: function(){ return _s.size; },\n"
        "    enumerable: true\n"
        "  });\n"
        // opacity: WE alias for alpha.  Used by wallpaper 2866203962's
        // playervolume.cursorUp script to hide the '100%' percentage text
        // via `percentageLayer.opacity = 0`; without this alias the assign
        // silently dropped into a plain JS property and the old bitmap stuck.
        "  Object.defineProperty(p, 'opacity', {\n"
        "    get: function(){ return _s.alpha; },\n"
        "    set: function(v){ _s.alpha = v; _s._dirty.alpha = true; },\n"
        "    enumerable: true\n"
        "  });\n"
        // solid: WE property that toggles whether a layer participates in
        // cursor hit-testing.  Tracked on _state so hitTestLayerProxy (C++
        // side) can honor it.  Defaults to true — the proxy's _state.solid
        // stays undefined until explicitly set, which hitTestLayerProxy
        // treats as 'on' via the same undefined→true convention.
        "  Object.defineProperty(p, 'solid', {\n"
        "    get: function(){ return _s.solid === false ? false : true; },\n"
        "    set: function(v){ _s.solid = !!v; },\n"
        "    enumerable: true\n"
        "  });\n"
        "  p.play = function(){};\n"
        "  p.stop = function(){};\n"
        "  p.pause = function(){};\n"
        "  p.isPlaying = function(){ return false; };\n"
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
        // getVideoTexture(): returns IVideoTexture proxy bound to this layer.
        // Methods delegate to SceneObject Q_INVOKABLEs via the __sceneBridge
        // global.  Rate is cached JS-side (libmpv readback would race with
        // writes issued the same tick).  Safe on layers with no video — the
        // bridge returns 0/no-op defaults.
        "  p.getVideoTexture = function() {\n"
        "    var name = _s.name;\n"
        "    var _rate = 1.0;\n"
        "    var o = {\n"
        "      getCurrentTime: function(){ return __sceneBridge.videoGetCurrentTime(name); },\n"
        "      setCurrentTime: function(t){ __sceneBridge.videoSetCurrentTime(name, +t || 0); },\n"
        "      isPlaying: function(){ return !!__sceneBridge.videoIsPlaying(name); },\n"
        "      play:  function(){ __sceneBridge.videoPlay(name); },\n"
        "      pause: function(){ __sceneBridge.videoPause(name); },\n"
        "      stop:  function(){ __sceneBridge.videoStop(name); }\n"
        "    };\n"
        "    Object.defineProperty(o, 'duration', {\n"
        "      get: function(){ return __sceneBridge.videoGetDuration(name); }, enumerable:true "
        "});\n"
        "    Object.defineProperty(o, 'rate', {\n"
        "      get: function(){ return _rate; },\n"
        "      set: function(v){ _rate = +v || 1.0; __sceneBridge.videoSetRate(name, _rate); },\n"
        "      enumerable:true });\n"
        "    return o;\n"
        "  };\n"
        // Animation layer stub: getAnimationLayer(index|name) returns an IAnimationLayer proxy.
        // Scripts control puppet animation layers (play, pause, rate, blend, frame).
        "  if (!_s._aniLayers) _s._aniLayers = {};\n"
        "  p.getAnimationLayerCount = function(){ return Object.keys(_s._aniLayers).length || 1; "
        "};\n"
        "  p.getAnimationLayer = function(idx) {\n"
        "    var key = String(idx);\n"
        "    if (_s._aniLayers[key]) return _s._aniLayers[key];\n"
        "    var al = { rate: 1, blend: 1, visible: true, frameCount: 60, _frame: 0, _playing: "
        "false,\n"
        "      play: function(){ al._playing = true; },\n"
        "      pause: function(){ al._playing = false; },\n"
        "      stop: function(){ al._playing = false; al._frame = 0; },\n"
        "      isPlaying: function(){ return al._playing; },\n"
        "      getFrame: function(){ return al._frame; },\n"
        "      setFrame: function(f){ al._frame = f; }\n"
        "    };\n"
        "    _s._aniLayers[key] = al;\n"
        "    return al;\n"
        "  };\n"
        // Named keyframe animation controller for image-layer properties
        // (alpha.animation etc).  Matches the WE IAnimation shape used by
        // scripts: getAnimation(name).play()/stop()/pause()/isPlaying.
        "  if (!_s._namedAnims) _s._namedAnims = {};\n"
        "  p.getAnimation = function(animName) {\n"
        "    if (_s._namedAnims[animName]) return _s._namedAnims[animName];\n"
        "    var ctrl = { _playing: false, _name: animName,\n"
        "      play:  function(){ this._playing = true;  _s._cmds.push('panim_play:'  + animName); "
        "_s._dirty._cmds = true; },\n"
        "      pause: function(){ this._playing = false; _s._cmds.push('panim_pause:' + animName); "
        "_s._dirty._cmds = true; },\n"
        "      stop:  function(){ this._playing = false; _s._cmds.push('panim_stop:'  + animName); "
        "_s._dirty._cmds = true; },\n"
        "      isPlaying: function(){ return this._playing; }\n"
        "    };\n"
        "    _s._namedAnims[animName] = ctrl;\n"
        "    return ctrl;\n"
        "  };\n"
        // getEffect(name) — returns effect proxy with dirty-tracked visible property
        "  if (!_s._effCache) _s._effCache = {};\n"
        "  p.getEffect = function(ename) {\n"
        "    if (_s._effCache[ename]) return _s._effCache[ename];\n"
        "    var efxList = init ? init.efx : null;\n"
        "    if (!efxList) return null;\n"
        "    var idx = -1;\n"
        "    for (var i = 0; i < efxList.length; i++) {\n"
        "      if (efxList[i] === ename) { idx = i; break; }\n"
        "    }\n"
        "    if (idx < 0) return null;\n"
        "    var es = { visible: true, name: ename, _idx: idx };\n"
        "    var ep = {};\n"
        "    Object.defineProperty(ep, 'name', { get: function(){ return es.name; }, enumerable: "
        "true });\n"
        "    Object.defineProperty(ep, 'visible', {\n"
        "      get: function(){ return es.visible; },\n"
        "      set: function(v){ es.visible = v;\n"
        "        if (!_s._efxDirty) _s._efxDirty = [];\n"
        "        _s._efxDirty.push(idx, v ? 1 : 0); },\n"
        "      enumerable: true\n"
        "    });\n"
        "    _s._effCache[ename] = ep;\n"
        "    return ep;\n"
        "  };\n"
        "  p.getEffectCount = function() {\n"
        "    return (init && init.efx) ? init.efx.length : 0;\n"
        "  };\n"
        "  p._state = _s;\n"
        "  return p;\n"
        "}\n"
        // Null-safe proxy: returned when getLayer() can't find a layer.
        // All setters are no-ops, no dirty tracking. Prevents TypeError on missing layers.
        "var _nullProxy = (function() {\n"
        "  var _s = { origin:Vec3(0,0,0), scale:Vec3(1,1,1),\n"
        "    angles:Vec3(0,0,0), size:{x:0,y:0},\n"
        "    visible:false, alpha:0, text:'', name:'', _dirty:{} };\n"
        "  var p = {};\n"
        "  Object.defineProperty(p, 'name', {get:function(){return '';}, enumerable:true});\n"
        "  Object.defineProperty(p, 'debug', {get:function(){return undefined;}, "
        "enumerable:true});\n"
        "  var vec3Props = ['origin','scale','angles'];\n"
        "  for (var i=0; i<vec3Props.length; i++) {\n"
        "    (function(prop){\n"
        "      Object.defineProperty(p, prop, {\n"
        "        get: function(){ var s = _s[prop]; return Vec3(s.x, s.y, s.z); },\n"
        "        set: function(v){},\n"
        "        enumerable: true\n"
        "      });\n"
        "    })(vec3Props[i]);\n"
        "  }\n"
        "  var scalarProps = ['visible','alpha'];\n"
        "  for (var j=0; j<scalarProps.length; j++) {\n"
        "    (function(prop){\n"
        "      Object.defineProperty(p, prop, {\n"
        "        get: function(){return _s[prop];}, set: function(v){},\n"
        "        enumerable: true\n"
        "      });\n"
        "    })(scalarProps[j]);\n"
        "  }\n"
        "  Object.defineProperty(p, 'text', {get:function(){return '';}, set:function(v){}, "
        "enumerable:true});\n"
        "  Object.defineProperty(p, 'size', {get:function(){return _s.size;}, enumerable:true});\n"
        "  p.play = function(){}; p.stop = function(){};\n"
        "  p.pause = function(){}; p.isPlaying = function(){return false;};\n"
        "  p.getTextureAnimation = function(){\n"
        "    return { rate:0, frameCount:1, _frame:0,\n"
        "      getFrame:function(){return this._frame;}, setFrame:function(f){},\n"
        "      play:function(){}, pause:function(){}, stop:function(){},\n"
        "      isPlaying:function(){return false;}\n"
        "    };\n"
        "  };\n"
        // Empty-layer proxy: getVideoTexture returns functional object bound to
        // the layer name. Bridge returns 0/no-op for non-video layers.
        "  p.getVideoTexture = function() {\n"
        "    var name = _s.name;\n"
        "    var _rate = 1.0;\n"
        "    var o = {\n"
        "      getCurrentTime: function(){ return __sceneBridge.videoGetCurrentTime(name); },\n"
        "      setCurrentTime: function(t){ __sceneBridge.videoSetCurrentTime(name, +t || 0); },\n"
        "      isPlaying: function(){ return !!__sceneBridge.videoIsPlaying(name); },\n"
        "      play:  function(){ __sceneBridge.videoPlay(name); },\n"
        "      pause: function(){ __sceneBridge.videoPause(name); },\n"
        "      stop:  function(){ __sceneBridge.videoStop(name); }\n"
        "    };\n"
        "    Object.defineProperty(o, 'duration', {\n"
        "      get: function(){ return __sceneBridge.videoGetDuration(name); }, enumerable:true "
        "});\n"
        "    Object.defineProperty(o, 'rate', {\n"
        "      get: function(){ return _rate; },\n"
        "      set: function(v){ _rate = +v || 1.0; __sceneBridge.videoSetRate(name, _rate); },\n"
        "      enumerable:true });\n"
        "    return o;\n"
        "  };\n"
        "  p.getAnimationLayerCount = function(){ return 0; };\n"
        "  p.getAnimationLayer = function(idx){\n"
        "    return { rate:0, blend:0, visible:false, frameCount:0, _frame:0, _playing:false,\n"
        "      play:function(){}, pause:function(){}, stop:function(){},\n"
        "      isPlaying:function(){return false;}, getFrame:function(){return 0;}, "
        "setFrame:function(f){}\n"
        "    };\n"
        "  };\n"
        "  p.getAnimation = function(animName){\n"
        "    return { _playing: false, _name: animName,\n"
        "      play:function(){}, pause:function(){}, stop:function(){},\n"
        "      isPlaying:function(){return false;}\n"
        "    };\n"
        "  };\n"
        "  p.getEffect = function(name) { return { name: name||'', visible: false }; };\n"
        "  p.getEffectCount = function() { return 0; };\n"
        "  p._state = _s;\n"
        "  return p;\n"
        "})();\n"
        "var thisScene = {\n"
        "  getLayer: function(name) {\n"
        "    if (_layerCache[name]) return _layerCache[name];\n"
        "    if (!_layerInitStates[name]) return null;\n"
        "    _layerCache[name] = _makeLayerProxy(name);\n"
        "    return _layerCache[name];\n"
        "  }\n"
        "};\n"
        "var _sceneListeners = {};\n"
        "thisScene.on = function(eventName, callback) {\n"
        "  if (typeof eventName !== 'string' || typeof callback !== 'function') return;\n"
        "  if (!_sceneListeners[eventName]) _sceneListeners[eventName] = [];\n"
        "  _sceneListeners[eventName].push(callback);\n"
        "};\n"
        "thisScene.off = function(eventName, callback) {\n"
        "  if (!_sceneListeners[eventName]) return;\n"
        "  if (typeof callback === 'function') {\n"
        "    _sceneListeners[eventName] = _sceneListeners[eventName].filter(\n"
        "      function(cb) { return cb !== callback; });\n"
        "  } else { delete _sceneListeners[eventName]; }\n"
        "};\n"
        "function _fireSceneEvent(eventName) {\n"
        "  var listeners = _sceneListeners[eventName];\n"
        "  if (!listeners || listeners.length === 0) return 0;\n"
        "  var args = Array.prototype.slice.call(arguments, 1);\n"
        "  var count = 0;\n"
        "  for (var i = 0; i < listeners.length; i++) {\n"
        "    try { listeners[i].apply(null, args); count++; }\n"
        "    catch(e) { console.log('scene.on(' + eventName + ') error: ' + e.message); }\n"
        "  }\n"
        "  return count;\n"
        "}\n"
        "function _hasSceneListeners(eventName) {\n"
        "  var l = _sceneListeners[eventName];\n"
        "  return l && l.length > 0;\n"
        "}\n"
        "var scene = thisScene;\n"
        "var thisLayer = null;\n"
        // createLayer pops a hidden pool node; destroyLayer returns it.
        // Two calling conventions:
        //   1. engine.registerAsset('path') → { __asset: 'path' }; then
        //      createLayer(thatAsset).  Used by dino_run etc.
        //   2. Object literal: createLayer({image: 'path', origin:..., ...}).
        //      WE's real API; pre-scan in WPSceneParser seeds the pool for
        //      any paths it finds.  Extra keys (origin/scale/angles/alpha/
        //      visible/color) are applied to the rented layer after it's
        //      popped, so trail-style scripts that rely on "initial
        //      position from literal" behave as on WE.
        // Fallback: if asset wasn't pre-scanned or pool exhausted, return a
        // throwaway stub so scripts don't crash (but the "layer" renders
        // nothing).
        // Helper: apply object-literal properties to a rented layer.
        "function _applyLayerLiteral(layer, asset) {\n"
        "  if (asset && !asset.__asset) {\n"
        "    if (asset.origin) layer.origin = asset.origin;\n"
        "    if (asset.scale)  layer.scale  = asset.scale;\n"
        "    if (asset.angles) layer.angles = asset.angles;\n"
        "    if ('alpha' in asset) layer.alpha = asset.alpha;\n"
        "    if (asset.color) layer.color = asset.color;\n"
        "    layer.visible = ('visible' in asset) ? !!asset.visible : true;\n"
        "  } else {\n"
        "    layer.visible = true;\n"
        "  }\n"
        "}\n"
        "thisScene.createLayer = function(asset) {\n"
        "  var path = asset && (asset.__asset || asset.image);\n"
        "  var pool = path && engine._assetPools[path];\n"
        "  if (!engine._assetLive[path]) engine._assetLive[path] = [];\n"
        "  var live = engine._assetLive[path];\n"
        "  var name = null;\n"
        "  if (pool && pool.length > 0) {\n"
        "    name = pool.shift();\n"
        "  } else if (live.length > 0) {\n"
        // LRU recycle: reuse the oldest live slot.  Scripts that call
        // createLayer in a loop without destroyLayer (3body MAIN, dino_run
        // coins, etc.) would otherwise exhaust the 8-slot pool and hit a
        // stub.  Recycling keeps createLayer always returning a real node.
        "    name = live.shift();\n"
        "    console.log('createLayer LRU-recycle: path=' + path + ' name=' + name);\n"
        "  }\n"
        "  if (name) {\n"
        "    var layer = thisScene.getLayer(name);\n"
        "    if (layer && layer !== _nullProxy) {\n"
        "      layer.__asset = path;\n"
        "      _applyLayerLiteral(layer, asset);\n"
        "      live.push(name);\n"
        "      console.log('createLayer OK: path=' + path + ' name=' + name +\n"
        "                  ' pool.free=' + (pool ? pool.length : 0) +\n"
        "                  ' live=' + live.length);\n"
        "      return layer;\n"
        "    }\n"
        "    console.log('createLayer: pool layer ' + name + ' not a real proxy');\n"
        "  }\n"
        // Pool never registered for this path — pre-scan missed it.  Loudly
        // report so the caller can extend the scan (see feedback_no_stubs).
        "  console.log('createLayer NO-POOL: path=' + path +\n"
        "              ' (pre-scan did not register this asset)');\n"
        "  return { __stub: true, __asset: path,\n"
        "    origin: Vec3(0,0,0), scale: Vec3(1,1,1), angles: Vec3(0,0,0),\n"
        "    alpha: 1.0, visible: true, text: '',\n"
        "    getAnimation: function(n) {\n"
        "      return { play:function(){}, stop:function(){}, pause:function(){},\n"
        "               isPlaying:function(){ return false; } }; } };\n"
        "};\n"
        "thisScene.destroyLayer = function(layer) {\n"
        "  if (!layer || layer.__stub) return;\n"
        "  layer.visible = false;\n"
        "  var path = layer.__asset;\n"
        "  if (!path) return;\n"
        "  var live = engine._assetLive[path];\n"
        "  if (live && layer.name) {\n"
        "    var idx = live.indexOf(layer.name);\n"
        "    if (idx >= 0) live.splice(idx, 1);\n"
        "  }\n"
        "  if (engine._assetPools[path] && layer.name)\n"
        "    engine._assetPools[path].push(layer.name);\n"
        "};\n"
        "thisScene.sortLayer    = function(layer, index) { /* noop */ };\n"
        "thisScene.getLayerIndex = function(name) {\n"
        "  var l = thisScene.getLayer(name);\n"
        "  return l && typeof l._index === 'number' ? l._index : 0;\n"
        "};\n"
        // _collectDirtyLayers returns a FLAT array.  Each dirty layer
        // contributes a fixed 17-entry block:
        //   [0]  name (string)
        //   [1]  flags (int bitmask): 1=origin 2=scale 4=angles 8=visible
        //        16=alpha 32=text 64=pointsize 128=cmds 256=efx
        //   [2-4]  origin x,y,z
        //   [5-7]  scale x,y,z
        //   [8-10] angles x,y,z
        //   [11] alpha (float)
        //   [12] visible (0 or 1)
        //   [13] pointsize (float)
        //   [14] text (string) or null
        //   [15] cmds (array of strings) or null
        //   [16] efxList (array of [idx,visible] tuples) or null
        // The old version returned nested objects — C++ did ~30 QJSValue
        // property reads per layer.  Flat layout cuts that to 17, and most
        // values short-circuit to null when their flag isn't set.
        "var DIRTY_STRIDE = 17;\n"
        "var F_ORIGIN=1, F_SCALE=2, F_ANGLES=4, F_VISIBLE=8, F_ALPHA=16,\n"
        "    F_TEXT=32, F_PSIZE=64, F_CMDS=128, F_EFX=256;\n"
        "function _collectDirtyLayers() {\n"
        "  var out = [];\n"
        "  for (var name in _layerCache) {\n"
        "    var layer = _layerCache[name];\n"
        "    if (layer._pool) {\n"
        // Pool fast path: plain properties, snapshot-compare for dirty.
        "      var prev = layer._prev;\n"
        "      var o = layer.origin, sc = layer.scale, a = layer.angles;\n"
        "      var flags = 0;\n"
        "      if (o.x !== prev.ox || o.y !== prev.oy || o.z !== prev.oz)\n"
        "        { flags |= F_ORIGIN; prev.ox = o.x; prev.oy = o.y; prev.oz = o.z; }\n"
        "      if (sc.x !== prev.sx || sc.y !== prev.sy || sc.z !== prev.sz)\n"
        "        { flags |= F_SCALE; prev.sx = sc.x; prev.sy = sc.y; prev.sz = sc.z; }\n"
        "      if (a.x !== prev.ax || a.y !== prev.ay || a.z !== prev.az)\n"
        "        { flags |= F_ANGLES; prev.ax = a.x; prev.ay = a.y; prev.az = a.z; }\n"
        "      if (layer.alpha !== prev.alpha)\n"
        "        { flags |= F_ALPHA; prev.alpha = layer.alpha; }\n"
        "      if (layer.visible !== prev.visible)\n"
        "        { flags |= F_VISIBLE; prev.visible = layer.visible; }\n"
        "      if (flags === 0) continue;\n"
        "      out.push(\n"
        "        name, flags,\n"
        "        o.x, o.y, o.z,\n"
        "        sc.x, sc.y, sc.z,\n"
        "        a.x, a.y, a.z,\n"
        "        layer.alpha, layer.visible ? 1 : 0, 0,\n"
        "        null, null, null);\n"
        "      continue;\n"
        "    }\n"
        "    var s = layer._state;\n"
        "    var d = s._dirty;\n"
        "    var cmds = s._cmds;\n"
        "    var efxList = s._efxDirty;\n"
        "    var flags = 0;\n"
        "    if (d.origin)    flags |= F_ORIGIN;\n"
        "    if (d.scale)     flags |= F_SCALE;\n"
        "    if (d.angles)    flags |= F_ANGLES;\n"
        "    if (d.visible)   flags |= F_VISIBLE;\n"
        "    if (d.alpha)     flags |= F_ALPHA;\n"
        "    if (d.text)      flags |= F_TEXT;\n"
        "    if (d.pointsize) flags |= F_PSIZE;\n"
        "    if (cmds && cmds.length) flags |= F_CMDS;\n"
        "    if (efxList && efxList.length) flags |= F_EFX;\n"
        "    if (flags === 0) continue;\n"
        "    var o = s.origin, sc = s.scale, a = s.angles;\n"
        "    out.push(\n"
        "      name, flags,\n"
        "      o.x, o.y, o.z,\n"
        "      sc.x, sc.y, sc.z,\n"
        "      a.x, a.y, a.z,\n"
        "      s.alpha, s.visible ? 1 : 0, s.pointsize,\n"
        "      (flags & F_TEXT) ? s.text : null,\n"
        "      (flags & F_CMDS) ? cmds.slice() : null,\n"
        "      (flags & F_EFX) ? efxList.slice() : null);\n"
        "    s._dirty = {};\n"
        "    if (flags & F_CMDS) s._cmds = [];\n"
        "    if (flags & F_EFX)  s._efxDirty = [];\n"
        "  }\n"
        "  return out;\n"
        "}\n");

    // Store references to JS dispatch functions for C++ calls
    m_collectDirtyLayersFn = m_jsEngine->globalObject().property("_collectDirtyLayers");
    m_fireSceneEventFn     = m_jsEngine->globalObject().property("_fireSceneEvent");
    m_hasSceneListenersFn  = m_jsEngine->globalObject().property("_hasSceneListeners");

    // Scene-level property control (bloom, clear color, camera, lighting)
    {
        std::string sceneJson = m_scene->getSceneInitialStateJson();
        if (! sceneJson.empty()) {
            QString escaped = QString::fromStdString(sceneJson);
            escaped.replace("\\", "\\\\");
            escaped.replace("'", "\\'");
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

                QString nameEsc = QString::fromStdString(sl.name);
                nameEsc.replace("'", "\\'");
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

    // engine.registerAudioBuffers(resolution) — implemented as native C++ callback
    {
        // Store 'this' pointer for the closure; safe because cleanupTextScripts() removes timer
        auto*    self  = this;
        QJSValue regFn = m_jsEngine->evaluate(
            "(function(resolution) {\n"
            "  resolution = resolution || 64;\n"
            "  var n = Math.min(Math.max(resolution, 16), 64);\n"
            "  // Round to nearest valid: 16, 32, or 64\n"
            "  if (n <= 24) n = 16;\n"
            "  else if (n <= 48) n = 32;\n"
            "  else n = 64;\n"
            "  var buf = { left: [], right: [], average: [], resolution: n };\n"
            "  for (var i = 0; i < n; i++) { buf.left.push(0); buf.right.push(0); "
            "buf.average.push(0); }\n"
            "  // Store registration ID for C++ side to find\n"
            "  if (!engine._audioRegs) engine._audioRegs = [];\n"
            "  buf._regIdx = engine._audioRegs.length;\n"
            "  engine._audioRegs.push(buf);\n"
            "  return buf;\n"
            "})\n");
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

    // WEColor module: hsv2rgb, rgb2hsv for color scripts
    m_jsEngine->evaluate(
        "var WEColor = (function() {\n"
        "  function hsv2rgb(hsv) {\n"
        "    var h = hsv.x, s = hsv.y, v = hsv.z;\n"
        "    var i = Math.floor(h * 6);\n"
        "    var f = h * 6 - i;\n"
        "    var p = v * (1 - s);\n"
        "    var q = v * (1 - f * s);\n"
        "    var t = v * (1 - (1 - f) * s);\n"
        "    var r, g, b;\n"
        "    switch (i % 6) {\n"
        "      case 0: r = v; g = t; b = p; break;\n"
        "      case 1: r = q; g = v; b = p; break;\n"
        "      case 2: r = p; g = v; b = t; break;\n"
        "      case 3: r = p; g = q; b = v; break;\n"
        "      case 4: r = t; g = p; b = v; break;\n"
        "      case 5: r = v; g = p; b = q; break;\n"
        "    }\n"
        "    return { x: r, y: g, z: b };\n"
        "  }\n"
        "  function rgb2hsv(rgb) {\n"
        "    var r = rgb.x, g = rgb.y, b = rgb.z;\n"
        "    var max = Math.max(r, g, b), min = Math.min(r, g, b);\n"
        "    var h, s, v = max;\n"
        "    var d = max - min;\n"
        "    s = max === 0 ? 0 : d / max;\n"
        "    if (max === min) { h = 0; }\n"
        "    else {\n"
        "      switch (max) {\n"
        "        case r: h = (g - b) / d + (g < b ? 6 : 0); break;\n"
        "        case g: h = (b - r) / d + 2; break;\n"
        "        case b: h = (r - g) / d + 4; break;\n"
        "      }\n"
        "      h /= 6;\n"
        "    }\n"
        "    return { x: h, y: s, z: v };\n"
        "  }\n"
        "  function normalizeColor(rgb) { return { x: rgb.x/255, y: rgb.y/255, z: rgb.z/255 }; }\n"
        "  function expandColor(rgb) { return { x: rgb.x*255, y: rgb.y*255, z: rgb.z*255 }; }\n"
        "  return { hsv2rgb: hsv2rgb, rgb2hsv: rgb2hsv,\n"
        "           normalizeColor: normalizeColor, expandColor: expandColor };\n"
        "})();\n");

    // Load color scripts
    for (const auto& csi : colorScripts) {
        QString scriptSrc = QString::fromStdString(csi.script);

        qCInfo(wekdeScene, "Color script source for id=%d:\n%s", csi.id, qPrintable(scriptSrc));

        stripESModuleSyntax(scriptSrc);

        // Inject scriptProperties with per-IIFE createScriptProperties for user overrides
        QString propsInit;
        if (! csi.scriptProperties.empty()) {
            QString jsonStr = QString::fromStdString(csi.scriptProperties);
            jsonStr.replace("\\", "\\\\");
            jsonStr.replace("'", "\\'");
            propsInit =
                QString(
                    "var _storedProps = JSON.parse('%1');\n"
                    "function createScriptProperties() {\n"
                    "  var b = {};\n"
                    "  function ap(def) {\n"
                    "    var n = def.name || def.n;\n"
                    "    if (n) {\n"
                    "      if (n in _storedProps) {\n"
                    "        var sp = _storedProps[n];\n"
                    "        // WE scene.json lets a scriptProperty bind to a\n"
                    "        // user property via `user: '<name>'`.  Resolve from\n"
                    "        // engine.userProperties first, fall back to the\n"
                    "        // stored `value`, then the script's own default.\n"
                    "        // Wallpaper 2866203962's Player Options binds\n"
                    "        // `enableplayer` to user prop `ui` with fallback\n"
                    "        // `false`; without the user-resolve step the UI\n"
                    "        // always faded because shared.enablePlayer=false.\n"
                    "        if (typeof sp === 'object' && sp !== null) {\n"
                    "          if ('user' in sp && typeof engine !== 'undefined' &&\n"
                    "              engine.userProperties && sp.user in engine.userProperties) {\n"
                    "            b[n] = engine.userProperties[sp.user];\n"
                    "          } else if ('value' in sp) {\n"
                    "            b[n] = sp.value;\n"
                    "          } else {\n"
                    "            b[n] = def.value;\n"
                    "          }\n"
                    "        } else {\n"
                    "          b[n] = sp;\n"
                    "        }\n"
                    "      } else { b[n] = def.value; }\n"
                    "    }\n"
                    "    return b;\n"
                    "  }\n"
                    "  b.addCheckbox=ap; b.addSlider=ap; b.addCombo=ap;\n"
                    "  b.addText=ap; b.addColor=ap; b.addFile=ap; b.addDirectory=ap;\n"
                    "  b.finish=function(){return b;};\n"
                    "  return b;\n"
                    "}\n")
                    .arg(jsonStr);
        }

        // Wrap in IIFE
        QString wrapped =
            QString("(function() {\n"
                    "  'use strict';\n"
                    "  var exports = {};\n"
                    "  %1\n" // scriptProperties override
                    "  %2\n" // script body
                    "  var _upd = typeof exports.update === 'function' ? exports.update :\n"
                    "             (typeof update === 'function' ? update : null);\n"
                    "  if (!_upd) return null;\n"
                    "  return { update: _upd };\n"
                    "})()\n")
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
        state.id           = csi.id;
        state.updateFn     = updateFn;
        state.currentColor = csi.initialColor;
        m_colorScriptStates.push_back(std::move(state));
        qCInfo(wekdeScene, "Color script compiled for id=%d", csi.id);
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
            QString jsonStr = QString::fromStdString(psi.scriptProperties);
            jsonStr.replace("\\", "\\\\");
            jsonStr.replace("'", "\\'");
            // Shadow global createScriptProperties with one that uses stored property values
            propsInit =
                QString(
                    "var _storedProps = JSON.parse('%1');\n"
                    "function createScriptProperties() {\n"
                    "  var b = {};\n"
                    "  function ap(def) {\n"
                    "    var n = def.name || def.n;\n"
                    "    if (n) {\n"
                    "      if (n in _storedProps) {\n"
                    "        var sp = _storedProps[n];\n"
                    "        // WE scene.json lets a scriptProperty bind to a\n"
                    "        // user property via `user: '<name>'`.  Resolve from\n"
                    "        // engine.userProperties first, fall back to the\n"
                    "        // stored `value`, then the script's own default.\n"
                    "        // Wallpaper 2866203962's Player Options binds\n"
                    "        // `enableplayer` to user prop `ui` with fallback\n"
                    "        // `false`; without the user-resolve step the UI\n"
                    "        // always faded because shared.enablePlayer=false.\n"
                    "        if (typeof sp === 'object' && sp !== null) {\n"
                    "          if ('user' in sp && typeof engine !== 'undefined' &&\n"
                    "              engine.userProperties && sp.user in engine.userProperties) {\n"
                    "            b[n] = engine.userProperties[sp.user];\n"
                    "          } else if ('value' in sp) {\n"
                    "            b[n] = sp.value;\n"
                    "          } else {\n"
                    "            b[n] = def.value;\n"
                    "          }\n"
                    "        } else {\n"
                    "          b[n] = sp;\n"
                    "        }\n"
                    "      } else { b[n] = def.value; }\n"
                    "    }\n"
                    "    return b;\n"
                    "  }\n"
                    "  b.addCheckbox=ap; b.addSlider=ap; b.addCombo=ap;\n"
                    "  b.addText=ap; b.addColor=ap; b.addFile=ap; b.addDirectory=ap;\n"
                    "  b.finish=function(){return b;};\n"
                    "  return b;\n"
                    "}\n")
                    .arg(jsonStr);
        }

        // Set thisLayer before compilation so closures can capture it
        if (! psi.layerName.empty()) {
            m_jsEngine->globalObject().setProperty(
                "thisLayer",
                m_jsEngine->evaluate(QString("thisScene.getLayer('%1')")
                                         .arg(QString::fromStdString(psi.layerName))));
        }

        // Wrap in IIFE returning {update, init}
        // Scripts that only use thisScene.getLayer() side effects may not have update()
        // Wrap init in try-catch so partial initialization still works (variables
        // set before the error point remain available to update)
        QString wrapped =
            QString(
                "(function() {\n"
                "  'use strict';\n"
                "  var exports = {};\n"
                "  %1\n"
                "  %2\n"
                "  var _upd = typeof exports.update === 'function' ? exports.update :\n"
                "             (typeof update === 'function' ? update : null);\n"
                "  var _rawInit = typeof exports.init === 'function' ? exports.init :\n"
                "                 (typeof init === 'function' ? init : null);\n"
                "  var _init = _rawInit ? function(v) {\n"
                "    try { return _rawInit(v); }\n"
                "    catch(e) { console.log('SceneScript init error: ' + e.message); }\n"
                "  } : null;\n"
                "  var _rawUpd = _upd;\n"
                "  if (_rawUpd) _upd = function(v) {\n"
                "    try { return _rawUpd(v); }\n"
                "    catch(e) { console.log('update error: ' + e.message + ' line=' + "
                "e.lineNumber); return v; }\n"
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
                "  var _mprc = typeof exports.mediaPropertiesChanged === 'function' ? "
                "exports.mediaPropertiesChanged :\n"
                "              (typeof mediaPropertiesChanged === 'function' ? "
                "mediaPropertiesChanged : null);\n"
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
                "})()\n")
                .arg(propsInit, scriptSrc);

        QJSValue result = m_jsEngine->evaluate(wrapped);
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
        state.id                       = psi.id;
        state.property                 = psi.property;
        state.layerName                = psi.layerName;
        state.updateFn                 = updateFn;
        state.initFn                   = initFn;
        state.cursorClickFn            = cursorClickFn;
        state.cursorEnterFn            = cursorEnterFn;
        state.cursorLeaveFn            = cursorLeaveFn;
        state.cursorDownFn             = cursorDownFn;
        state.cursorUpFn               = cursorUpFn;
        state.cursorMoveFn             = cursorMoveFn;
        state.applyUserPropertiesFn    = applyUserPropertiesFn;
        state.destroyFn                = destroyFn;
        state.resizeScreenFn           = resizeScreenFn;
        state.mediaPlaybackChangedFn   = mediaPlaybackChangedFn;
        state.mediaPropertiesChangedFn = mediaPropertiesChangedFn;
        state.mediaThumbnailChangedFn  = mediaThumbnailChangedFn;
        state.mediaTimelineChangedFn   = mediaTimelineChangedFn;
        state.mediaStatusChangedFn     = mediaStatusChangedFn;
        state.animationEventFn         = animationEventFn;
        state.currentVisible           = psi.initialVisible;
        state.currentVec3              = psi.initialVec3;
        state.currentFloat             = psi.initialFloat;

        // Cache layer proxy for thisLayer (avoids evaluate per frame)
        if (! psi.layerName.empty()) {
            state.thisLayerProxy = m_jsEngine->evaluate(
                QString("thisScene.getLayer('%1')").arg(QString::fromStdString(psi.layerName)));
        }

        // Call init(value) if available
        if (initFn.isCallable()) {
            // Set thisLayer for init call
            if (! psi.layerName.empty()) {
                m_jsEngine->globalObject().setProperty("thisLayer", state.thisLayerProxy);
            }
            QJSValue initVal;
            if (psi.property == "visible") {
                initVal = QJSValue(psi.initialVisible);
            } else if (psi.property == "alpha") {
                initVal = QJSValue((double)psi.initialFloat);
            } else {
                initVal = m_jsEngine->evaluate(QString("Vec3(%1,%2,%3)")
                                                   .arg(psi.initialVec3[0], 0, 'g', 9)
                                                   .arg(psi.initialVec3[1], 0, 'g', 9)
                                                   .arg(psi.initialVec3[2], 0, 'g', 9));
            }
            QJSValue initResult = initFn.call({ initVal });
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
                tgt                 = &m_cursorTargets.back();
                tgt->layerName      = state.layerName;
                tgt->thisLayerProxy = state.thisLayerProxy;
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

    // Store scene ortho size for cursorClick hit-testing
    {
        auto orthoSize = m_scene->getOrthoSize();
        m_sceneOrthoW  = (float)orthoSize[0];
        m_sceneOrthoH  = (float)orthoSize[1];
    }
    refreshParallaxCache();

    // Load sound volume scripts
    auto soundVolumeScripts = m_scene->getSoundVolumeScripts();
    for (const auto& svsi : soundVolumeScripts) {
        QString scriptSrc = QString::fromStdString(svsi.script);

        stripESModuleSyntax(scriptSrc);

        // Set thisLayer to the sound layer proxy for this script's own layer
        if (! svsi.layerName.empty()) {
            QString nameEsc = QString::fromStdString(svsi.layerName);
            nameEsc.replace("'", "\\'");
            m_jsEngine->globalObject().setProperty(
                "thisLayer",
                m_jsEngine->evaluate(QString("thisScene.getLayer('%1')").arg(nameEsc)));
        }

        // Inject scriptProperties
        QString propsInit;
        if (! svsi.scriptProperties.empty()) {
            QString jsonStr = QString::fromStdString(svsi.scriptProperties);
            jsonStr.replace("\\", "\\\\");
            jsonStr.replace("'", "\\'");
            propsInit =
                QString(
                    "var _storedProps = JSON.parse('%1');\n"
                    "function createScriptProperties() {\n"
                    "  var b = {};\n"
                    "  function ap(def) {\n"
                    "    var n = def.name || def.n;\n"
                    "    if (n) {\n"
                    "      if (n in _storedProps) {\n"
                    "        var sp = _storedProps[n];\n"
                    "        // WE scene.json lets a scriptProperty bind to a\n"
                    "        // user property via `user: '<name>'`.  Resolve from\n"
                    "        // engine.userProperties first, fall back to the\n"
                    "        // stored `value`, then the script's own default.\n"
                    "        // Wallpaper 2866203962's Player Options binds\n"
                    "        // `enableplayer` to user prop `ui` with fallback\n"
                    "        // `false`; without the user-resolve step the UI\n"
                    "        // always faded because shared.enablePlayer=false.\n"
                    "        if (typeof sp === 'object' && sp !== null) {\n"
                    "          if ('user' in sp && typeof engine !== 'undefined' &&\n"
                    "              engine.userProperties && sp.user in engine.userProperties) {\n"
                    "            b[n] = engine.userProperties[sp.user];\n"
                    "          } else if ('value' in sp) {\n"
                    "            b[n] = sp.value;\n"
                    "          } else {\n"
                    "            b[n] = def.value;\n"
                    "          }\n"
                    "        } else {\n"
                    "          b[n] = sp;\n"
                    "        }\n"
                    "      } else { b[n] = def.value; }\n"
                    "    }\n"
                    "    return b;\n"
                    "  }\n"
                    "  b.addCheckbox=ap; b.addSlider=ap; b.addCombo=ap;\n"
                    "  b.addText=ap; b.addColor=ap; b.addFile=ap; b.addDirectory=ap;\n"
                    "  b.finish=function(){return b;};\n"
                    "  return b;\n"
                    "}\n")
                    .arg(jsonStr);
        }

        QString wrapped =
            QString("(function() {\n"
                    "  'use strict';\n"
                    "  var exports = {};\n"
                    "  %1\n"
                    "  %2\n"
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
                    "})()\n")
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
            QString jsonStr = QString::fromStdString(tsi.scriptProperties);
            jsonStr.replace("\\", "\\\\");
            jsonStr.replace("'", "\\'");
            propsInit = QString("var _storedProps = JSON.parse('%1');\n"
                                "function createScriptProperties() {\n"
                                "  var b = {};\n"
                                "  function ap(def) {\n"
                                "    var n = def.name || def.n;\n"
                                "    if (n) {\n"
                                "      if (n in _storedProps) {\n"
                                "        var sp = _storedProps[n];\n"
                                "        b[n] = (typeof sp === 'object' && sp !== null && 'value' "
                                "in sp) ? sp.value : sp;\n"
                                "      } else { b[n] = def.value; }\n"
                                "    }\n"
                                "    return b;\n"
                                "  }\n"
                                "  b.addCheckbox=ap; b.addSlider=ap; b.addCombo=ap;\n"
                                "  b.addText=ap; b.addColor=ap; b.addFile=ap; b.addDirectory=ap;\n"
                                "  b.addTextInput=ap;\n"
                                "  b.finish=function(){return b;};\n"
                                "  return b;\n"
                                "}\n")
                            .arg(jsonStr);
        }

        // Wrap in IIFE that returns {update, init} functions
        QString wrapped =
            QString("(function() {\n"
                    "  'use strict';\n"
                    "  var exports = {};\n"
                    "  %1\n"
                    "  %2\n"
                    "  var _upd = typeof exports.update === 'function' ? exports.update :\n"
                    "             (typeof update === 'function' ? update : null);\n"
                    "  var _init = typeof exports.init === 'function' ? exports.init :\n"
                    "              (typeof init === 'function' ? init : null);\n"
                    "  if (!_upd) return null;\n"
                    "  return { update: _upd, init: _init };\n"
                    "})()\n")
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
            QJSValue initResult = initFn.call({ QJSValue(state.currentText) });
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
        m_textTimer->setInterval(500); // evaluate twice per second
        connect(m_textTimer, &QTimer::timeout, this, &SceneObject::evaluateTextScripts);
        m_textTimer->start();

        // Run once immediately
        evaluateTextScripts();
    }

    if (! m_colorScriptStates.empty()) {
        m_colorTimer = new QTimer(this);
        m_colorTimer->setInterval(33); // ~30Hz for smooth audio-reactive color
        connect(m_colorTimer, &QTimer::timeout, this, &SceneObject::evaluateColorScripts);
        m_colorTimer->start();

        // Run once immediately
        evaluateColorScripts();
    }
}

void SceneObject::refreshAudioBuffers() {
    if (! m_jsEngine) return;

    auto analyzer = m_scene->audioAnalyzer();
    if (! analyzer || ! analyzer->HasData()) return;

    QJSValue engineObj = m_jsEngine->globalObject().property("engine");
    QJSValue audioRegs = engineObj.property("_audioRegs");
    if (! audioRegs.isArray()) return;

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

    // Refresh audio buffers before evaluating scripts
    refreshAudioBuffers();

    // Update engine globals
    double   runtimeSecs = m_runtimeTimer.elapsed() / 1000.0;
    QJSValue engineObj   = m_jsEngine->globalObject().property("engine");
    engineObj.setProperty("runtime", runtimeSecs);
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

    int updated = 0, errors = 0;
    for (auto& state : m_textScriptStates) {
        QJSValue result = state.updateFn.call({ QJSValue(state.currentText) });
        if (result.isError()) {
            errors++;
            static std::unordered_set<int> textErroredIds;
            if (textErroredIds.find(state.id) == textErroredIds.end()) {
                textErroredIds.insert(state.id);
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
}

void SceneObject::evaluateColorScripts() {
    if (! m_jsEngine || m_colorScriptStates.empty()) return;

    // Refresh audio buffers before evaluating scripts
    refreshAudioBuffers();

    // Update engine globals
    double   runtimeSecs = m_runtimeTimer.elapsed() / 1000.0;
    QJSValue engineObj   = m_jsEngine->globalObject().property("engine");
    engineObj.setProperty("runtime", runtimeSecs);
    engineObj.setProperty("frametime", 0.033); // ~30Hz timer interval

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
        // Pass current color as a full Vec3 so scripts can use both .x/.y/.z and .r/.g/.b
        QString colorJs = QString("Vec3(%1,%2,%3)")
                              .arg((double)state.currentColor[0], 0, 'g', 8)
                              .arg((double)state.currentColor[1], 0, 'g', 8)
                              .arg((double)state.currentColor[2], 0, 'g', 8);
        QJSValue colorVal = m_jsEngine->evaluate(colorJs);

        QJSValue result = state.updateFn.call({ colorVal });
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
}

void SceneObject::evaluatePropertyScripts() {
    if (! m_jsEngine) return;
    if (m_propertyScriptStates.empty() && m_soundVolumeScriptStates.empty() &&
        m_soundLayerStates.empty())
        return;

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

    // Update engine globals
    double   runtimeSecs = m_runtimeTimer.elapsed() / 1000.0;
    QJSValue engineObj   = m_jsEngine->globalObject().property("engine");
    engineObj.setProperty("runtime", runtimeSecs);
    engineObj.setProperty("frametime", 0.033); // ~30Hz

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
                if (state.property == "alpha") return QJSValue((double)state.currentFloat);
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
                    }
                    QJSValue eventObj = m_jsEngine->newObject();
                    eventObj.setProperty("name", QString::fromStdString(evt.name));
                    eventObj.setProperty("frame", QJSValue(evt.frame));

                    QJSValue result = state.animationEventFn.call({ eventObj, buildValue(state) });
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

    // Evaluate in order: visible first (computes shared.*), then vec3 props, then alpha
    for (int pass = 0; pass < 3; pass++) {
        for (auto& state : m_propertyScriptStates) {
            bool isVisible = (state.property == "visible");
            bool isAlpha   = (state.property == "alpha");
            bool isVec3    = ! isVisible && ! isAlpha;

            if (pass == 0 && ! isVisible) continue;
            if (pass == 1 && ! isVec3) continue;
            if (pass == 2 && ! isAlpha) continue;

            // Set thisLayer for this script's context (cached proxy)
            if (! state.layerName.empty()) {
                m_jsEngine->globalObject().setProperty("thisLayer", state.thisLayerProxy);
            }

            if (! state.updateFn.isCallable()) continue;

            QJSValue arg;
            if (isVisible) {
                arg = QJSValue(state.currentVisible);
            } else if (isAlpha) {
                arg = QJSValue((double)state.currentFloat);
            } else {
                // Call Vec3() function directly (cheaper than evaluate)
                arg = vec3Fn.call({ QJSValue((double)state.currentVec3[0]),
                                    QJSValue((double)state.currentVec3[1]),
                                    QJSValue((double)state.currentVec3[2]) });
            }

            QJSValue result = state.updateFn.call({ arg });
            if (result.isError()) {
                if (s_scriptDiag) s_errorsThisWin++;
                static std::unordered_set<int> erroredIds;
                if (erroredIds.find(state.id * 100 + pass) == erroredIds.end()) {
                    erroredIds.insert(state.id * 100 + pass);
                    static int s_rt_err = 0;
                    if (++s_rt_err <= 10) {
                        QString stack = result.property("stack").toString();
                        int     line  = result.property("lineNumber").toInt();
                        LOG_INFO(
                            "Property script RUNTIME ERROR id=%d prop=%s: %s (line %d)\nSTACK: %s",
                            state.id,
                            state.property.c_str(),
                            qPrintable(result.toString()),
                            line,
                            qPrintable(stack));
                    }
                    qCWarning(wekdeScene,
                              "Property script error id=%d prop=%s: %s",
                              state.id,
                              state.property.c_str(),
                              qPrintable(result.toString()));
                }
                continue;
            }

            // Process return values (some scripts return values directly)
            if (isVisible) {
                if (result.isBool()) {
                    bool newVal = result.toBool();
                    if (newVal != state.currentVisible) {
                        state.currentVisible = newVal;
                        m_scene->updateNodeVisible(state.id, newVal);
                        if (s_scriptDiag) s_updatesVisible++;
                    }
                }
            } else if (isAlpha) {
                if (result.isNumber()) {
                    float newVal = (float)result.toNumber();
                    if (std::abs(newVal - state.currentFloat) > 0.001f) {
                        state.currentFloat = newVal;
                        m_scene->updateNodeAlpha(state.id, newVal);
                        if (s_scriptDiag) s_updatesAlpha++;
                    }
                }
            } else if (result.isObject() && result.hasProperty("x")) {
                float x = (float)result.property("x").toNumber();
                float y = (float)result.property("y").toNumber();
                float z = (float)result.property("z").toNumber();
                if (std::abs(x - state.currentVec3[0]) > 0.0001f ||
                    std::abs(y - state.currentVec3[1]) > 0.0001f ||
                    std::abs(z - state.currentVec3[2]) > 0.0001f) {
                    state.currentVec3 = { x, y, z };
                    m_scene->updateNodeTransform(state.id, state.property, x, y, z);
                    if (s_scriptDiag) s_updatesVec3++;
                }
            }
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
        QJSValue updates = m_collectDirtyLayersFn.call();
        probeMark(s_t_dirtyCollect);
        int totalEntries = updates.property("length").toInt();
        dirtyLayerCount  = totalEntries / DIRTY_STRIDE;

        // Batch origin/scale/angles/alpha/visible updates so we take the
        // scene's property-update mutex once instead of per-field.
        static thread_local std::vector<wallpaper::SceneWallpaper::LayerBatchUpdate> s_batch;
        s_batch.clear();
        s_batch.reserve(dirtyLayerCount);
        constexpr uint32_t HOT_FLAGS = F_ORIGIN | F_SCALE | F_ANGLES | F_ALPHA | F_VISIBLE;

        for (int base = 0; base < totalEntries; base += DIRTY_STRIDE) {
            std::string name = updates.property(base + 0).toString().toStdString();
            auto        it   = m_nodeNameToId.find(name);
            if (it == m_nodeNameToId.end()) {
                dirtyLayerMiss++;
                static std::unordered_set<std::string> loggedMisses;
                if (loggedMisses.find(name) == loggedMisses.end()) {
                    loggedMisses.insert(name);
                    qCWarning(wekdeScene,
                              "Dirty layer '%s' not found in nodeNameToId "
                              "(%zu entries)",
                              name.c_str(),
                              m_nodeNameToId.size());
                }
                continue;
            }
            int32_t  id    = it->second;
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
        QJSValue soundUpdates     = m_collectDirtySoundLayersFn.call();
        int      soundUpdateCount = soundUpdates.property("length").toInt();
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
        QJSValue sceneUpdate = m_collectDirtySceneFn.call();
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

        QJSValue result = svState.updateFn.call({ QJSValue((double)baseVolume) });
        if (result.isError()) {
            static std::unordered_set<int> erroredIndices;
            if (erroredIndices.find(svState.index) == erroredIndices.end()) {
                erroredIndices.insert(svState.index);
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
        LOG_INFO("PROPEVAL[%d]: %zu states, shared=%d, dirty=%d (miss=%d), soundVol=%zu",
                 propEvalCount,
                 (size_t)m_propertyScriptStates.size(),
                 sharedCount,
                 dirtyLayerCount,
                 dirtyLayerMiss,
                 (size_t)m_soundVolumeScriptStates.size());
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
                static bool probed = false;
                if (! probed) {
                    probed     = true;
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
}

void SceneObject::cleanupTextScripts() {
    // Fire destroy event on all scripts before cleanup
    fireDestroyEvent();

    m_textScriptStates.clear();
    m_colorScriptStates.clear();
    m_propertyScriptStates.clear();
    m_soundVolumeScriptStates.clear();
    m_nodeNameToId.clear();
    m_collectDirtyLayersFn = QJSValue();
    m_fireSceneEventFn     = QJSValue();
    m_hasSceneListenersFn  = QJSValue();
    m_soundLayerStates.clear();
    m_soundLayerNameToIndex.clear();
    m_collectDirtySoundLayersFn = QJSValue();
    m_cursorTargets.clear();
    m_hoveredLayers.clear();
    m_dragTarget.clear();
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
    if (m_jsEngine) {
        delete m_jsEngine;
        m_jsEngine = nullptr;
    }
}

#include "SceneBackend.moc"
#include "moc_SceneTimerBridge.cpp"
