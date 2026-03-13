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
            node->initVulkan(width()*window()->devicePixelRatio(), height()*window()->devicePixelRatio(), m_hdrOutput);

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
    auto colorScripts = m_scene->getColorScripts();
    auto propertyScripts = m_scene->getPropertyScripts();
    if (scripts.empty() && colorScripts.empty() && propertyScripts.empty()) return;

    m_jsEngine = new QJSEngine(this);

    // Provide a minimal 'engine' global with runtime and timeOfDay
    m_runtimeTimer.start();
    QJSValue engineObj = m_jsEngine->newObject();
    engineObj.setProperty("frametime", 0.5);   // ~500ms timer
    engineObj.setProperty("runtime", 0.0);
    engineObj.setProperty("timeOfDay", 0.0);
    engineObj.setProperty("userProperties", m_jsEngine->newObject());
    m_jsEngine->globalObject().setProperty("engine", engineObj);

    // Provide the 'shared' global for inter-script data sharing.
    // All scripts in the scene can read/write to this object.
    m_jsEngine->globalObject().setProperty("shared", m_jsEngine->newObject());

    // Provide a minimal 'console' object for scripts that call console.log()
    m_jsEngine->evaluate(
        "var console = { log: function() {}, warn: function() {}, error: function() {} };\n"
    );

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

    // Screen resolution and input stubs for property scripts
    m_jsEngine->evaluate(
        "engine.screenResolution = { x: 1920, y: 1080 };\n"
        "var input = { cursorWorldPosition: { x: 0, y: 0 } };\n"
        "function Vec3(x, y, z) {\n"
        "  var v = { x: x||0, y: y||0, z: z||0 };\n"
        "  v.multiply = function(s) { return Vec3(v.x*s, v.y*s, v.z*s); };\n"
        "  v.add = function(o) { return Vec3(v.x+o.x, v.y+o.y, v.z+o.z); };\n"
        "  v.subtract = function(o) { return Vec3(v.x-o.x, v.y-o.y, v.z-o.z); };\n"
        "  v.length = function() { return Math.sqrt(v.x*v.x+v.y*v.y+v.z*v.z); };\n"
        "  v.normalize = function() { var l=v.length()||1; return Vec3(v.x/l,v.y/l,v.z/l); };\n"
        "  v.copy = function() { return Vec3(v.x, v.y, v.z); };\n"
        "  v.dot = function(o) { return v.x*o.x+v.y*o.y+v.z*o.z; };\n"
        "  v.cross = function(o) { return Vec3(v.y*o.z-v.z*o.y, v.z*o.x-v.x*o.z, v.x*o.y-v.y*o.x); };\n"
        "  v.negate = function() { return Vec3(-v.x,-v.y,-v.z); };\n"
        "  return v;\n"
        "}\n"
        "var localStorage = {\n"
        "  get: function(key) { return undefined; },\n"
        "  set: function(key, value) {}\n"
        "};\n"
    );

    // engine.openUserShortcut stub
    m_jsEngine->evaluate("engine.openUserShortcut = function(name) {};\n");

    // createScriptProperties() — WE SceneScript API for declaring user-configurable properties
    // Returns a chainable builder: createScriptProperties().addSlider({name,value,...}).addCheckbox(...)
    // After chaining, the result object has properties accessible by name (e.g. scriptProperties.mode)
    m_jsEngine->evaluate(
        "function createScriptProperties() {\n"
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
        "  return builder;\n"
        "}\n"
    );

    // Inject layer initial states from scene parsing
    {
        std::string initJson = m_scene->getLayerInitialStatesJson();
        if (!initJson.empty()) {
            QString escaped = QString::fromStdString(initJson);
            escaped.replace("\\", "\\\\");
            escaped.replace("'", "\\'");
            m_jsEngine->evaluate(
                QString("var _layerInitStates = JSON.parse('%1');\n").arg(escaped));
        } else {
            m_jsEngine->evaluate("var _layerInitStates = {};\n");
        }
    }

    // thisScene.getLayer() and thisLayer infrastructure
    // Layer proxies use Object.defineProperty for dirty tracking
    m_jsEngine->evaluate(
        "var _layerCache = {};\n"
        "function _makeLayerProxy(name) {\n"
        "  var init = _layerInitStates[name];\n"
        "  var _s = init ? {\n"
        "    origin: {x:init.o[0], y:init.o[1], z:init.o[2]},\n"
        "    scale: {x:init.s[0], y:init.s[1], z:init.s[2]},\n"
        "    angles: {x:init.a[0], y:init.a[1], z:init.a[2]},\n"
        "    visible: init.v, alpha: 1.0,\n"
        "    text: '', name: name, _dirty: {}\n"
        "  } : { origin: {x:0,y:0,z:0}, scale: {x:1,y:1,z:1},\n"
        "        angles: {x:0,y:0,z:0}, visible: true, alpha: 1.0,\n"
        "        text: '', name: name, _dirty: {} };\n"
        "  var p = {};\n"
        "  Object.defineProperty(p, 'name', { get: function(){return _s.name;}, enumerable:true });\n"
        "  Object.defineProperty(p, 'debug', { get: function(){return undefined;}, enumerable:true });\n"
        "  var vec3Props = ['origin','scale','angles'];\n"
        "  for (var i=0; i<vec3Props.length; i++) {\n"
        "    (function(prop){\n"
        "      Object.defineProperty(p, prop, {\n"
        "        get: function(){ return _s[prop]; },\n"
        "        set: function(v){ _s[prop] = v; _s._dirty[prop] = true; },\n"
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
        "  p.play = function(){};\n"
        "  p._state = _s;\n"
        "  return p;\n"
        "}\n"
        "var thisScene = {\n"
        "  getLayer: function(name) {\n"
        "    if (!_layerCache[name]) _layerCache[name] = _makeLayerProxy(name);\n"
        "    return _layerCache[name];\n"
        "  }\n"
        "};\n"
        "var thisLayer = null;\n"
        "function _collectDirtyLayers() {\n"
        "  var updates = [];\n"
        "  for (var name in _layerCache) {\n"
        "    var s = _layerCache[name]._state;\n"
        "    var d = s._dirty;\n"
        "    var keys = Object.keys(d);\n"
        "    if (keys.length === 0) continue;\n"
        "    updates.push({ name: name, dirty: d,\n"
        "      origin: s.origin, scale: s.scale, angles: s.angles,\n"
        "      visible: s.visible, alpha: s.alpha });\n"
        "    s._dirty = {};\n"
        "  }\n"
        "  return updates;\n"
        "}\n"
    );

    // Store reference to _collectDirtyLayers for C++ calls
    m_collectDirtyLayersFn = m_jsEngine->globalObject().property("_collectDirtyLayers");

    // Get node name→id map for thisScene.getLayer() dispatch
    m_nodeNameToId = m_scene->getNodeNameToIdMap();

    // Audio resolution constants
    engineObj.setProperty("AUDIO_RESOLUTION_16", 16);
    engineObj.setProperty("AUDIO_RESOLUTION_32", 32);
    engineObj.setProperty("AUDIO_RESOLUTION_64", 64);

    // engine.registerAudioBuffers(resolution) — implemented as native C++ callback
    {
        // Store 'this' pointer for the closure; safe because cleanupTextScripts() removes timer
        auto* self = this;
        QJSValue regFn = m_jsEngine->evaluate(
            "(function(resolution) {\n"
            "  resolution = resolution || 64;\n"
            "  var n = Math.min(Math.max(resolution, 16), 64);\n"
            "  // Round to nearest valid: 16, 32, or 64\n"
            "  if (n <= 24) n = 16;\n"
            "  else if (n <= 48) n = 32;\n"
            "  else n = 64;\n"
            "  var buf = { left: [], right: [], average: [], resolution: n };\n"
            "  for (var i = 0; i < n; i++) { buf.left.push(0); buf.right.push(0); buf.average.push(0); }\n"
            "  // Store registration ID for C++ side to find\n"
            "  if (!engine._audioRegs) engine._audioRegs = [];\n"
            "  buf._regIdx = engine._audioRegs.length;\n"
            "  engine._audioRegs.push(buf);\n"
            "  return buf;\n"
            "})\n"
        );
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
        "  return { hsv2rgb: hsv2rgb, rgb2hsv: rgb2hsv };\n"
        "})();\n"
    );

    // Load color scripts
    for (const auto& csi : colorScripts) {
        QString scriptSrc = QString::fromStdString(csi.script);

        qCInfo(wekdeScene, "Color script source for id=%d:\n%s",
               csi.id, qPrintable(scriptSrc));

        // Strip 'use strict';
        scriptSrc.replace(QRegularExpression("^\\s*['\"]use strict['\"];?\\s*", QRegularExpression::MultilineOption), "");

        // Strip import statements (we provide WEColor as a global)
        scriptSrc.replace(QRegularExpression("^\\s*import\\s+.*?from\\s+['\"].*?['\"];?\\s*$", QRegularExpression::MultilineOption), "");

        // Strip 'export ' keyword
        scriptSrc.replace(QRegularExpression("\\bexport\\s+function\\b"), "function");
        scriptSrc.replace(QRegularExpression("\\bexport\\s+var\\b"), "var");
        scriptSrc.replace(QRegularExpression("\\bexport\\s+let\\b"), "let");
        scriptSrc.replace(QRegularExpression("\\bexport\\s+const\\b"), "const");

        // Inject scriptProperties with per-IIFE createScriptProperties for user overrides
        QString propsInit;
        if (!csi.scriptProperties.empty()) {
            QString jsonStr = QString::fromStdString(csi.scriptProperties);
            jsonStr.replace("\\", "\\\\");
            jsonStr.replace("'", "\\'");
            propsInit = QString(
                "var _storedProps = JSON.parse('%1');\n"
                "function createScriptProperties() {\n"
                "  var b = {};\n"
                "  function ap(def) {\n"
                "    var n = def.name || def.n;\n"
                "    if (n) {\n"
                "      if (n in _storedProps) {\n"
                "        var sp = _storedProps[n];\n"
                "        b[n] = (typeof sp === 'object' && sp !== null && 'value' in sp) ? sp.value : sp;\n"
                "      } else { b[n] = def.value; }\n"
                "    }\n"
                "    return b;\n"
                "  }\n"
                "  b.addCheckbox=ap; b.addSlider=ap; b.addCombo=ap;\n"
                "  b.addText=ap; b.addColor=ap; b.addFile=ap; b.addDirectory=ap;\n"
                "  return b;\n"
                "}\n"
            ).arg(jsonStr);
        }

        // Wrap in IIFE
        QString wrapped = QString(
            "(function() {\n"
            "  'use strict';\n"
            "  var exports = {};\n"
            "  %1\n"  // scriptProperties override
            "  %2\n"  // script body
            "  var _upd = typeof exports.update === 'function' ? exports.update :\n"
            "             (typeof update === 'function' ? update : null);\n"
            "  if (!_upd) return null;\n"
            "  return { update: _upd };\n"
            "})()\n"
        ).arg(propsInit, scriptSrc);

        QJSValue result = m_jsEngine->evaluate(wrapped);
        if (result.isError()) {
            qCWarning(wekdeScene, "Color script error for id=%d: %s",
                       csi.id, qPrintable(result.toString()));
            continue;
        }
        if (result.isNull() || result.isUndefined()) {
            qCWarning(wekdeScene, "Color script for id=%d did not produce an update function", csi.id);
            continue;
        }

        QJSValue updateFn = result.property("update");
        if (!updateFn.isCallable()) {
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

        // Strip 'use strict', imports, exports
        scriptSrc.replace(QRegularExpression("^\\s*['\"]use strict['\"];?\\s*", QRegularExpression::MultilineOption), "");
        scriptSrc.replace(QRegularExpression("^\\s*import\\s+.*?from\\s+['\"].*?['\"];?\\s*$", QRegularExpression::MultilineOption), "");
        scriptSrc.replace(QRegularExpression("\\bexport\\s+function\\b"), "function");
        scriptSrc.replace(QRegularExpression("\\bexport\\s+var\\b"), "var");
        scriptSrc.replace(QRegularExpression("\\bexport\\s+let\\b"), "let");
        scriptSrc.replace(QRegularExpression("\\bexport\\s+const\\b"), "const");

        // Transform spread operator (QV4 may not support it)
        // Object spread: { ...expr } → Object.assign({}, expr)
        scriptSrc.replace(QRegularExpression("\\{\\s*\\.\\.\\.([^}]+)\\}"), "Object.assign({}, \\1)");
        // Array spread: [...expr] → [].concat(expr)
        scriptSrc.replace(QRegularExpression("\\[\\s*\\.\\.\\.(\\w[^\\]]*)\\]"), "[].concat(\\1)");

        // Inject scriptProperties and per-IIFE createScriptProperties that merges stored values
        QString propsInit;
        if (!psi.scriptProperties.empty()) {
            QString jsonStr = QString::fromStdString(psi.scriptProperties);
            jsonStr.replace("\\", "\\\\");
            jsonStr.replace("'", "\\'");
            // Shadow global createScriptProperties with one that uses stored property values
            propsInit = QString(
                "var _storedProps = JSON.parse('%1');\n"
                "function createScriptProperties() {\n"
                "  var b = {};\n"
                "  function ap(def) {\n"
                "    var n = def.name || def.n;\n"
                "    if (n) {\n"
                "      if (n in _storedProps) {\n"
                "        var sp = _storedProps[n];\n"
                "        b[n] = (typeof sp === 'object' && sp !== null && 'value' in sp) ? sp.value : sp;\n"
                "      } else { b[n] = def.value; }\n"
                "    }\n"
                "    return b;\n"
                "  }\n"
                "  b.addCheckbox=ap; b.addSlider=ap; b.addCombo=ap;\n"
                "  b.addText=ap; b.addColor=ap; b.addFile=ap; b.addDirectory=ap;\n"
                "  return b;\n"
                "}\n"
            ).arg(jsonStr);
        }

        // Set thisLayer before compilation so closures can capture it
        if (!psi.layerName.empty()) {
            m_jsEngine->globalObject().setProperty(
                "thisLayer",
                m_jsEngine->evaluate(
                    QString("thisScene.getLayer('%1')").arg(
                        QString::fromStdString(psi.layerName))));
        }

        // Wrap in IIFE returning {update, init}
        // Scripts that only use thisScene.getLayer() side effects may not have update()
        QString wrapped = QString(
            "(function() {\n"
            "  'use strict';\n"
            "  var exports = {};\n"
            "  %1\n"
            "  %2\n"
            "  var _upd = typeof exports.update === 'function' ? exports.update :\n"
            "             (typeof update === 'function' ? update : null);\n"
            "  var _init = typeof exports.init === 'function' ? exports.init :\n"
            "              (typeof init === 'function' ? init : null);\n"
            "  return { update: _upd, init: _init };\n"
            "})()\n"
        ).arg(propsInit, scriptSrc);

        QJSValue result = m_jsEngine->evaluate(wrapped);
        if (result.isError()) {
            qCWarning(wekdeScene, "Property script error for id=%d prop=%s: %s",
                       psi.id, psi.property.c_str(), qPrintable(result.toString()));
            continue;
        }
        if (result.isNull() || result.isUndefined()) {
            continue;
        }

        QJSValue updateFn = result.property("update");
        QJSValue initFn = result.property("init");

        // Scripts with neither update nor init are useless
        if (!updateFn.isCallable() && !initFn.isCallable()) {
            continue;
        }

        PropertyScriptState state;
        state.id             = psi.id;
        state.property       = psi.property;
        state.layerName      = psi.layerName;
        state.updateFn       = updateFn;
        state.initFn         = initFn;
        state.currentVisible = psi.initialVisible;
        state.currentVec3    = psi.initialVec3;
        state.currentFloat   = psi.initialFloat;

        // Cache layer proxy for thisLayer (avoids evaluate per frame)
        if (!psi.layerName.empty()) {
            state.thisLayerProxy = m_jsEngine->evaluate(
                QString("thisScene.getLayer('%1')").arg(
                    QString::fromStdString(psi.layerName)));
        }

        // Call init(value) if available
        if (initFn.isCallable()) {
            // Set thisLayer for init call
            if (!psi.layerName.empty()) {
                m_jsEngine->globalObject().setProperty("thisLayer", state.thisLayerProxy);
            }
            QJSValue initVal;
            if (psi.property == "visible") {
                initVal = QJSValue(psi.initialVisible);
            } else if (psi.property == "alpha") {
                initVal = QJSValue((double)psi.initialFloat);
            } else {
                initVal = m_jsEngine->evaluate(
                    QString("Vec3(%1,%2,%3)")
                        .arg(psi.initialVec3[0], 0, 'g', 9)
                        .arg(psi.initialVec3[1], 0, 'g', 9)
                        .arg(psi.initialVec3[2], 0, 'g', 9));
            }
            QJSValue initResult = initFn.call({ initVal });
            if (initResult.isError()) {
                qCWarning(wekdeScene, "Property script init error id=%d prop=%s: %s",
                           psi.id, psi.property.c_str(), qPrintable(initResult.toString()));
            }
        }

        m_propertyScriptStates.push_back(std::move(state));
    }
    if (!m_propertyScriptStates.empty()) {
        qCInfo(wekdeScene, "Compiled %zu property scripts", (size_t)m_propertyScriptStates.size());
    }

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

    if (!m_colorScriptStates.empty()) {
        m_colorTimer = new QTimer(this);
        m_colorTimer->setInterval(33); // ~30Hz for smooth audio-reactive color
        connect(m_colorTimer, &QTimer::timeout, this, &SceneObject::evaluateColorScripts);
        m_colorTimer->start();

        // Run once immediately
        evaluateColorScripts();
    }

    if (!m_propertyScriptStates.empty()) {
        m_propertyTimer = new QTimer(this);
        m_propertyTimer->setInterval(33); // ~30Hz for smooth orbital animation
        connect(m_propertyTimer, &QTimer::timeout, this, &SceneObject::evaluatePropertyScripts);
        m_propertyTimer->start();

        // Run once immediately
        evaluatePropertyScripts();
    }
}

void SceneObject::refreshAudioBuffers() {
    if (!m_jsEngine) return;

    auto analyzer = m_scene->audioAnalyzer();
    if (!analyzer || !analyzer->HasData()) return;

    QJSValue engineObj = m_jsEngine->globalObject().property("engine");
    QJSValue audioRegs = engineObj.property("_audioRegs");
    if (!audioRegs.isArray()) return;

    int len = audioRegs.property("length").toInt();
    for (int r = 0; r < len; r++) {
        QJSValue buf = audioRegs.property(r);
        int resolution = buf.property("resolution").toInt();

        auto leftData  = analyzer->GetRawSpectrum(resolution, 0);
        auto rightData = analyzer->GetRawSpectrum(resolution, 1);

        QJSValue leftArr  = buf.property("left");
        QJSValue rightArr = buf.property("right");
        QJSValue avgArr   = buf.property("average");

        int n = (int)leftData.size();
        for (int i = 0; i < n; i++) {
            float l = leftData[i];
            float rv = rightData[i];
            leftArr.setProperty(i, (double)l);
            rightArr.setProperty(i, (double)rv);
            avgArr.setProperty(i, (double)((l + rv) * 0.5f));
        }
    }
}

void SceneObject::evaluateTextScripts() {
    if (!m_jsEngine || m_textScriptStates.empty()) return;

    // Refresh audio buffers before evaluating scripts
    refreshAudioBuffers();

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

void SceneObject::evaluateColorScripts() {
    if (!m_jsEngine || m_colorScriptStates.empty()) return;

    // Refresh audio buffers before evaluating scripts
    refreshAudioBuffers();

    // Update engine globals
    double runtimeSecs = m_runtimeTimer.elapsed() / 1000.0;
    QJSValue engineObj = m_jsEngine->globalObject().property("engine");
    engineObj.setProperty("runtime", runtimeSecs);
    engineObj.setProperty("frametime", 0.033); // ~30Hz timer interval

    QTime now = QTime::currentTime();
    double tod = (now.hour() * 3600 + now.minute() * 60 + now.second()) / 86400.0;
    engineObj.setProperty("timeOfDay", tod);

    for (auto& state : m_colorScriptStates) {
        // Pass current color as Vec3 {x, y, z}
        QJSValue colorVal = m_jsEngine->newObject();
        colorVal.setProperty("x", (double)state.currentColor[0]);
        colorVal.setProperty("y", (double)state.currentColor[1]);
        colorVal.setProperty("z", (double)state.currentColor[2]);

        QJSValue result = state.updateFn.call({ colorVal });
        if (result.isError()) {
            qCWarning(wekdeScene, "Color script runtime error id=%d: %s",
                       state.id, qPrintable(result.toString()));
            continue;
        }

        // Result is Vec3 {x, y, z} = RGB
        float r, g, b;
        if (result.isObject()) {
            r = (float)result.property("x").toNumber();
            g = (float)result.property("y").toNumber();
            b = (float)result.property("z").toNumber();
        } else {
            // Skip silently — color scripts that depend on shared.* may return
            // undefined until property scripts populate the data
            continue;
        }

        // Periodic diagnostic logging (every ~3 seconds)
        static int evalCount = 0;
        if (++evalCount % 90 == 1) {
            qCInfo(wekdeScene, "Color script id=%d: rgb=(%.3f, %.3f, %.3f)",
                   state.id, r, g, b);
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
    if (!m_jsEngine || m_propertyScriptStates.empty()) return;

    // Update engine globals
    double runtimeSecs = m_runtimeTimer.elapsed() / 1000.0;
    QJSValue engineObj = m_jsEngine->globalObject().property("engine");
    engineObj.setProperty("runtime", runtimeSecs);
    engineObj.setProperty("frametime", 0.033); // ~30Hz

    QTime now = QTime::currentTime();
    double tod = (now.hour() * 3600 + now.minute() * 60 + now.second()) / 86400.0;
    engineObj.setProperty("timeOfDay", tod);

    // Refresh audio buffers in case property scripts use audio data
    refreshAudioBuffers();

    // Cache Vec3 constructor for efficient argument creation
    QJSValue vec3Fn = m_jsEngine->globalObject().property("Vec3");

    // Evaluate in order: visible first (computes shared.*), then vec3 props, then alpha
    for (int pass = 0; pass < 3; pass++) {
        for (auto& state : m_propertyScriptStates) {
            bool isVisible = (state.property == "visible");
            bool isAlpha   = (state.property == "alpha");
            bool isVec3    = !isVisible && !isAlpha;

            if (pass == 0 && !isVisible) continue;
            if (pass == 1 && !isVec3) continue;
            if (pass == 2 && !isAlpha) continue;

            // Set thisLayer for this script's context (cached proxy)
            if (!state.layerName.empty()) {
                m_jsEngine->globalObject().setProperty("thisLayer", state.thisLayerProxy);
            }

            if (!state.updateFn.isCallable()) continue;

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
                static std::unordered_set<int> erroredIds;
                if (erroredIds.find(state.id * 100 + pass) == erroredIds.end()) {
                    erroredIds.insert(state.id * 100 + pass);
                    qCWarning(wekdeScene, "Property script error id=%d prop=%s: %s",
                               state.id, state.property.c_str(), qPrintable(result.toString()));
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
                    }
                }
            } else if (isAlpha) {
                if (result.isNumber()) {
                    float newVal = (float)result.toNumber();
                    if (std::abs(newVal - state.currentFloat) > 0.001f) {
                        state.currentFloat = newVal;
                        m_scene->updateNodeAlpha(state.id, newVal);
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
                }
            }
        }
    }

    // Flush dirty layer proxies from thisScene.getLayer() side effects
    int dirtyLayerCount = 0;
    if (m_collectDirtyLayersFn.isCallable()) {
        QJSValue updates = m_collectDirtyLayersFn.call();
        dirtyLayerCount = updates.property("length").toInt();
        for (int i = 0; i < dirtyLayerCount; i++) {
            QJSValue entry = updates.property(i);
            std::string name = entry.property("name").toString().toStdString();
            auto it = m_nodeNameToId.find(name);
            if (it == m_nodeNameToId.end()) continue;
            int32_t id = it->second;

            QJSValue dirty = entry.property("dirty");

            if (dirty.property("origin").toBool()) {
                QJSValue o = entry.property("origin");
                m_scene->updateNodeTransform(id, "origin",
                    (float)o.property("x").toNumber(),
                    (float)o.property("y").toNumber(),
                    (float)o.property("z").toNumber());
            }
            if (dirty.property("scale").toBool()) {
                QJSValue s = entry.property("scale");
                m_scene->updateNodeTransform(id, "scale",
                    (float)s.property("x").toNumber(),
                    (float)s.property("y").toNumber(),
                    (float)s.property("z").toNumber());
            }
            if (dirty.property("angles").toBool()) {
                QJSValue a = entry.property("angles");
                m_scene->updateNodeTransform(id, "angles",
                    (float)a.property("x").toNumber(),
                    (float)a.property("y").toNumber(),
                    (float)a.property("z").toNumber());
            }
            if (dirty.property("visible").toBool()) {
                m_scene->updateNodeVisible(id, entry.property("visible").toBool());
            }
            if (dirty.property("alpha").toBool()) {
                m_scene->updateNodeAlpha(id, (float)entry.property("alpha").toNumber());
            }
        }
    }

    // Periodic diagnostic logging (~every 3 seconds at 30Hz)
    static int propEvalCount = 0;
    if (++propEvalCount % 90 == 1) {
        int sharedCount = m_jsEngine->evaluate("Object.keys(shared).length").toInt();
        qCInfo(wekdeScene, "Property scripts: %zu states, shared vars: %d, dirty layers: %d",
               (size_t)m_propertyScriptStates.size(), sharedCount, dirtyLayerCount);
    }
}

void SceneObject::cleanupTextScripts() {
    m_textScriptStates.clear();
    m_colorScriptStates.clear();
    m_propertyScriptStates.clear();
    m_nodeNameToId.clear();
    m_collectDirtyLayersFn = QJSValue();
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
    if (m_jsEngine) {
        delete m_jsEngine;
        m_jsEngine = nullptr;
    }
}

#include "SceneBackend.moc"
