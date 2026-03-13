#pragma once
#include <list>
#include <vector>
#include <memory>
#include <Eigen/Dense>
#include "SceneMesh.h"
#include "SceneCamera.h"

#include "Core/Literals.hpp"
#include "Core/NoCopyMove.hpp"

namespace wallpaper
{

class SceneNode : NoCopy, NoMove {
public:
    SceneNode()
        : m_name(),
          m_dirty(true),
          m_translate(Eigen::Vector3f::Zero()),
          m_scale { 1.0f, 1.0f, 1.0f },
          m_rotation(Eigen::Vector3f::Zero()) {}
    SceneNode(const Eigen::Vector3f& translate, const Eigen::Vector3f& scale,
              const Eigen::Vector3f& rotation, const std::string& name = "")
        : m_name(name),
          m_dirty(true),
          m_translate(translate),
          m_scale(scale),
          m_rotation(rotation) {};

    const auto& Camera() const { return m_cameraName; }
    void        SetCamera(const std::string& name) { m_cameraName = name; }
    void        AddMesh(std::shared_ptr<SceneMesh> mesh) { m_mesh = mesh; }
    void        AppendChild(std::shared_ptr<SceneNode> sub) {
               sub->m_parent = this;
               m_children.push_back(sub);
    }
    Eigen::Matrix4d GetLocalTrans() const;

    const auto& Translate() const { return m_translate; }
    const auto& Scale() const { return m_scale; }
    const auto& Rotation() const { return m_rotation; }
    void        SetTranslate(Eigen::Vector3f v) { m_translate = v; }
    void        SetScale(Eigen::Vector3f v) { m_scale = v; }
    void        SetRotation(Eigen::Vector3f v) { m_rotation = v; }

    void CopyTrans(const SceneNode& node) {
        m_translate = node.m_translate;
        m_scale     = node.m_scale;
        m_rotation  = node.m_rotation;
    }

    // Copy local transform AND inherit the same parent, so UpdateTrans()
    // produces the same world transform as the source node.
    void CopyTransWithParent(const SceneNode& node) {
        CopyTrans(node);
        m_parent = node.m_parent;
        m_dirty  = true;
    }

    // Inherit only the parent pointer from another node, keeping local
    // transform unchanged.  Used by ResolveEffect so the last effect node
    // picks up the parent-group transform without overwriting its saved
    // local transform (which may differ from the live world-node).
    void InheritParent(const SceneNode& node) {
        m_parent = node.m_parent;
        m_dirty  = true;
    }

    // Set the parent node directly for world transform chaining.
    void SetParent(SceneNode* parent) {
        m_parent = parent;
        m_dirty  = true;
    }

    // Set a pre-computed world transform, bypassing the parent chain.
    // Used by proxy nodes that have their transform baked at parse time.
    void SetWorldTransform(const Eigen::Matrix4d& t) {
        m_trans = t;
        m_dirty = false;
    }

    // update self modle trans (will update parent before)
    void            UpdateTrans();
    Eigen::Matrix4d ModelTrans() const { return m_trans; };

    SceneMesh* Mesh() { return m_mesh.get(); }
    bool       HasMaterial() const { return m_mesh && m_mesh->Material() != nullptr; };

    const auto& GetChildren() const { return m_children; }
    auto&       GetChildren() { return m_children; }

    i32& ID() { return m_id; }

    bool IsOffscreen() const { return m_offscreen; }
    void SetOffscreen(bool v) { m_offscreen = v; }

    bool IsVisible() const {
        if (! m_visible) return false;
        // Effect nodes inherit visibility from their owner (the image object node)
        if (m_visibilityOwner && ! m_visibilityOwner->m_visible) return false;
        return true;
    }
    void SetVisible(bool v) { m_visible = v; }

    // Set the owner node whose visibility this node inherits (for effect chain nodes)
    void SetVisibilityOwner(SceneNode* owner) { m_visibilityOwner = owner; }

private:
    // mark self and all children
    void MarkTransDirty();

    i32         m_id { -1 };
    bool        m_offscreen { false };
    bool        m_visible { true };
    SceneNode*  m_visibilityOwner { nullptr };
    std::string m_name;

    bool            m_dirty;
    Eigen::Matrix4d m_trans;

    Eigen::Vector3f m_translate { 0.0f, 0.0f, 0.0f };
    Eigen::Vector3f m_scale { 1.0f, 1.0f, 1.0f };
    Eigen::Vector3f m_rotation { 0.0f, 0.0f, 0.0f };

    std::shared_ptr<SceneMesh> m_mesh;

    // specific a camera not active, used for image effect
    std::string m_cameraName;

    SceneNode* m_parent { nullptr };

    std::list<std::shared_ptr<SceneNode>> m_children;
};
} // namespace wallpaper
