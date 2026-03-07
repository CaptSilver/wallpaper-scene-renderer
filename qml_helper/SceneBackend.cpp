#include "SceneBackend.hpp"

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
#include <QSGTexture>
#endif

#include <clocale>
#include <atomic>
#include <array>
#include <functional>
#include <QtCore/QRegularExpression>
#include <QtCore/QTime>

#include "glExtra.hpp"
#include "SceneWallpaper.hpp"
#include "SceneWallpaperSurface.hpp"
#include "Type.hpp"
#include "Utils/Platform.hpp"
#include <cstdio>
#include <qobjectdefs.h>
#include <unistd.h>

using namespace scenebackend;

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
    void initVulkan(uint16_t w, uint16_t h) {
        wallpaper::RenderInitInfo info;
        info.enable_valid_layer = m_enable_valid;
        info.offscreen          = true;
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
    QSize size;
    size.setWidth(this->width());
    size.setHeight(this->height());
}

QSGNode* SceneObject::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    TextureNode* node = static_cast<TextureNode*>(oldNode);
    if (! node) {
        node = new TextureNode(window(), m_scene, m_enable_valid, [this](QQuickWindow* window) {
            return (QSGTexture*)nullptr;
        });
        if (node->initGl()) {
            node->initVulkan(width()*window()->devicePixelRatio(), height()*window()->devicePixelRatio());

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

QString SceneObject::userProperties() const { return m_userProperties; }

void SceneObject::setUserProperties(const QString& value) {
    if (m_userProperties == value) return;
    m_userProperties = value;
    SET_PROPERTY(String, wallpaper::PROPERTY_USER_PROPS, value.toStdString());
    Q_EMIT userPropertiesChanged();
}

void SceneObject::play() { m_scene->play(); }
void SceneObject::pause() { m_scene->pause(); }

bool SceneObject::vulkanValid() const { return m_enable_valid; }
void SceneObject::enableVulkanValid() { m_enable_valid = true; }
void SceneObject::enableGenGraphviz() { SET_PROPERTY(Bool, wallpaper::PROPERTY_GRAPHIVZ, true); }

void SceneObject::setAcceptMouse(bool value) {
    if (value)
        setAcceptedMouseButtons(Qt::LeftButton);
    else
        setAcceptedMouseButtons(Qt::NoButton);
}

void SceneObject::setAcceptHover(bool value) { setAcceptHoverEvents(value); }

void SceneObject::mousePressEvent(QMouseEvent* event) {}
void SceneObject::mouseMoveEvent(QMouseEvent* event) {
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    auto pos = event->position();
#else
    auto pos = event->localPos();
#endif
    m_scene->mouseInput(pos.x() / width(), pos.y() / height());
}

void SceneObject::hoverMoveEvent(QHoverEvent* event) {
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    auto pos = event->position();
#else
    auto pos = event->posF();
#endif
    m_scene->mouseInput(pos.x() / width(), pos.y() / height());
}

std::string SceneObject::GetDefaultCachePath() {
    return wallpaper::platform::GetCachePath(CACHE_DIR);
}

void SceneObject::setupTextScripts() {
    cleanupTextScripts();

    auto scripts = m_scene->getTextScripts();
    if (scripts.empty()) return;

    m_jsEngine = new QJSEngine(this);

    // Provide a minimal 'engine' global with runtime and timeOfDay
    m_runtimeTimer.start();
    QJSValue engineObj = m_jsEngine->newObject();
    engineObj.setProperty("frametime", 0.5);   // ~500ms timer
    engineObj.setProperty("runtime", 0.0);
    engineObj.setProperty("timeOfDay", 0.0);
    engineObj.setProperty("userProperties", m_jsEngine->newObject());
    m_jsEngine->globalObject().setProperty("engine", engineObj);

    // Engine method stubs
    m_jsEngine->evaluate(
        "engine.isDesktopDevice = function() { return true; };\n"
        "engine.isMobileDevice = function() { return false; };\n"
        "engine.isWallpaper = function() { return true; };\n"
        "engine.isScreensaver = function() { return false; };\n"
        "engine.isRunningInEditor = function() { return false; };\n"
        "engine.isPortrait = function() { return false; };\n"
        "engine.isLandscape = function() { return true; };\n"
    );

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
        "    addSlider: function(o) { _props[o.name] = o.value !== undefined ? o.value : 0; return builder; },\n"
        "    addCheckbox: function(o) { _props[o.name] = o.value !== undefined ? o.value : false; return builder; },\n"
        "    addCombo: function(o) { _props[o.name] = o.value !== undefined ? o.value : (o.options && o.options.length > 0 ? o.options[0].value : 0); return builder; },\n"
        "    addTextInput: function(o) { _props[o.name] = o.value !== undefined ? o.value : ''; return builder; },\n"
        "    addText: function(o) { _props[o.name] = o.value !== undefined ? o.value : ''; return builder; },\n"
        "    addColor: function(o) { _props[o.name] = o.value !== undefined ? o.value : '0 0 0'; return builder; },\n"
        "    addFile: function(o) { _props[o.name] = o.value !== undefined ? o.value : ''; return builder; },\n"
        "    finish: function() { return _props; }\n"
        "  };\n"
        "  return builder;\n"
        "}\n"
    );

    for (const auto& tsi : scripts) {
        QString scriptSrc = QString::fromStdString(tsi.script);

        qCInfo(wekdeScene, "Text script source for id=%d:\n%s",
               tsi.id, qPrintable(scriptSrc));

        // Strip 'use strict'; — it's inside our IIFE anyway
        scriptSrc.replace(QRegularExpression("^\\s*['\"]use strict['\"];?\\s*", QRegularExpression::MultilineOption), "");

        // Strip 'export ' keyword (QJSEngine doesn't support ES modules)
        scriptSrc.replace(QRegularExpression("\\bexport\\s+function\\b"), "function");
        scriptSrc.replace(QRegularExpression("\\bexport\\s+var\\b"), "var");
        scriptSrc.replace(QRegularExpression("\\bexport\\s+let\\b"), "let");
        scriptSrc.replace(QRegularExpression("\\bexport\\s+const\\b"), "const");

        // Wrap in IIFE that returns {update, init} functions
        QString wrapped = QString(
            "(function() {\n"
            "  'use strict';\n"
            "  var exports = {};\n"
            "  %1\n"
            "  var _upd = typeof exports.update === 'function' ? exports.update :\n"
            "             (typeof update === 'function' ? update : null);\n"
            "  var _init = typeof exports.init === 'function' ? exports.init :\n"
            "              (typeof init === 'function' ? init : null);\n"
            "  if (!_upd) return null;\n"
            "  return { update: _upd, init: _init };\n"
            "})()\n"
        ).arg(scriptSrc);

        QJSValue result = m_jsEngine->evaluate(wrapped);
        if (result.isError()) {
            qCWarning(wekdeScene, "Text script error for id=%d: %s",
                       tsi.id, qPrintable(result.toString()));
            continue;
        }
        if (result.isNull() || result.isUndefined()) {
            qCWarning(wekdeScene, "Text script for id=%d did not produce an update function", tsi.id);
            continue;
        }

        QJSValue updateFn = result.property("update");
        QJSValue initFn = result.property("init");

        if (!updateFn.isCallable()) {
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
                qCWarning(wekdeScene, "Text script init error id=%d: %s",
                           tsi.id, qPrintable(initResult.toString()));
            } else if (initResult.isString()) {
                state.currentText = initResult.toString();
            }
        }

        m_textScriptStates.push_back(std::move(state));
        qCInfo(wekdeScene, "Text script compiled for id=%d", tsi.id);
    }

    if (!m_textScriptStates.empty()) {
        m_textTimer = new QTimer(this);
        m_textTimer->setInterval(500); // evaluate twice per second
        connect(m_textTimer, &QTimer::timeout, this, &SceneObject::evaluateTextScripts);
        m_textTimer->start();

        // Run once immediately
        evaluateTextScripts();
    }
}

void SceneObject::evaluateTextScripts() {
    if (!m_jsEngine || m_textScriptStates.empty()) return;

    // Update engine globals
    double runtimeSecs = m_runtimeTimer.elapsed() / 1000.0;
    QJSValue engineObj = m_jsEngine->globalObject().property("engine");
    engineObj.setProperty("runtime", runtimeSecs);
    // timeOfDay: 0.0 = midnight, 0.5 = noon, 1.0 = midnight
    QTime now = QTime::currentTime();
    double tod = (now.hour() * 3600 + now.minute() * 60 + now.second()) / 86400.0;
    engineObj.setProperty("timeOfDay", tod);

    for (auto& state : m_textScriptStates) {
        QJSValue result = state.updateFn.call({ QJSValue(state.currentText) });
        if (result.isError()) {
            qCWarning(wekdeScene, "Text script runtime error id=%d: %s",
                       state.id, qPrintable(result.toString()));
            continue;
        }
        QString newText = result.toString();
        if (newText != state.currentText) {
            state.currentText = newText;
            m_scene->updateText(state.id, newText.toStdString());
        }
    }
}

void SceneObject::cleanupTextScripts() {
    m_textScriptStates.clear();
    if (m_textTimer) {
        m_textTimer->stop();
        delete m_textTimer;
        m_textTimer = nullptr;
    }
    if (m_jsEngine) {
        delete m_jsEngine;
        m_jsEngine = nullptr;
    }
}

#include "SceneBackend.moc"
