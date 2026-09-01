#pragma once

#include <QObject>
#include <QJSValue>
#include <QString>

namespace scenebackend
{

class SceneObject;

/// The only C++ object untrusted SceneScript ever sees, installed as the
/// `__sceneBridge` global.
///
/// Scene property/text/colour scripts are lifted verbatim out of Workshop
/// scene.json and compiled into the same QJSEngine that holds this global, so
/// whatever it reflects is granted to hostile content.  QJSEngine::newQObject
/// reflects a QObject's ENTIRE metaobject — every Q_INVOKABLE, public slot and
/// writable Q_PROPERTY, all the way up the base classes.  Wrapping SceneObject
/// (a QQuickItem) therefore handed scripts 156 enumerable names, including the
/// standalone-viewer debug console (requestScreenshot / requestPassDump /
/// debugEvalJs / simulate*), every writable scene property, and QQuickItem's own
/// `parent` and `grabToImage`.
///
/// So the boundary object is this one instead: its metaobject IS the allowlist.
/// The surface is defined by addition — adding a Q_INVOKABLE to SceneObject
/// cannot widen what scripts can reach, only editing this class can.  Each
/// method forwards to the owning SceneObject and nothing else; there are
/// deliberately no Q_PROPERTYs, so no script assignment can reach a C++ setter.
///
/// Logging here would have to be Qt's qWarning/qInfo rather than the project
/// LOG_* macros — this header compiles into scenescript_tests, which links only
/// Qt6::Core+Qml and not wpUtils.  In practice the forwarders log nothing;
/// SceneObject already logs on its side.
class SceneScriptBridge : public QObject {
    Q_OBJECT
public:
    /// `owner` doubles as the QObject parent so Qt hands the wrapper
    /// CppOwnership — a parentless QObject passed to newQObject would get
    /// JavaScriptOwnership and be deleted during the engine's own teardown.
    explicit SceneScriptBridge(SceneObject* owner);

    // Every forwarder tolerates a null owner so the surface can be pinned in a
    // unit test without standing up a SceneObject.

    Q_INVOKABLE void materialSetValue(const QString& layerName, const QString& name,
                                      const QJSValue& value);
    Q_INVOKABLE void effectMaterialSetValue(const QString& layerName, int effectIdx,
                                            const QString& name, const QJSValue& value);
    Q_INVOKABLE void setLayerSpriteFrame(const QString& layerName, bool wantsManual, int frameIdx);
    Q_INVOKABLE QJSValue getLayerSpriteInfo(const QString& layerName) const;
    Q_INVOKABLE void     setTextStyle(const QString& layerName, const QString& halign,
                                      const QString& valign, const QString& fontName);
    Q_INVOKABLE QJSValue getLayerWorldTransform(const QString& layerName) const;
    Q_INVOKABLE int      getBoneIndex(const QString& layerName, const QString& boneName) const;
    Q_INVOKABLE void     setLayerParent(int childId, int parentId);
    Q_INVOKABLE void     sortLayer(int childId, int targetIndex);
    Q_INVOKABLE void     openUserShortcut(const QString& name);

    Q_INVOKABLE QJSValue lsGet(int loc, const QString& key);
    Q_INVOKABLE void     lsSet(int loc, const QString& key, const QJSValue& value);
    Q_INVOKABLE void     lsRemove(int loc, const QString& key);
    Q_INVOKABLE void     lsClear(int loc);

    Q_INVOKABLE double videoGetCurrentTime(const QString& layerName) const;
    Q_INVOKABLE double videoGetDuration(const QString& layerName) const;
    Q_INVOKABLE bool   videoIsPlaying(const QString& layerName) const;
    Q_INVOKABLE void   videoPlay(const QString& layerName);
    Q_INVOKABLE void   videoPause(const QString& layerName);
    Q_INVOKABLE void   videoStop(const QString& layerName);
    Q_INVOKABLE void   videoSetCurrentTime(const QString& layerName, double t);
    Q_INVOKABLE void   videoSetRate(const QString& layerName, double rate);

private:
    /// Also this object's QObject parent, so it outlives the bridge.
    SceneObject* m_owner;
};

} // namespace scenebackend
