#pragma once
#include <vector>
#include <list>
#include <memory>
#include <cstdint>
#include <string>
#include "Core/Literals.hpp"
#include "Type.hpp"

namespace wallpaper
{

class SceneNode;
class SceneMesh;

struct SceneImageEffectNode {
    std::string                output; // render target
    std::shared_ptr<SceneNode> sceneNode;
};

struct SceneImageEffect {
    enum class CmdType
    {
        Copy,
        Swap,
    };
    struct Command {
        CmdType     cmd { CmdType::Copy };
        std::string dst;
        std::string src;
        i32         afterpos { 0 }; // start at 1, 0 for begin at all
    };
    std::vector<Command>            commands;
    std::list<SceneImageEffectNode> nodes;
};

class SceneImageEffectLayer {
public:
    SceneImageEffectLayer(SceneNode* node, float w, float h, std::string_view pingpong_a,
                          std::string_view pingpong_b);

    void AddEffect(const std::shared_ptr<SceneImageEffect>& node) { m_effects.push_back(node); }
    std::size_t EffectCount() const { return m_effects.size(); }
    auto&       GetEffect(std::size_t index) { return m_effects.at(index); }
    const auto& FirstTarget() const { return m_pingpong_a; }
    SceneMesh&  FinalMesh() const { return *m_final_mesh; }
    SceneNode&  FinalNode() const { return *m_final_node; }
    void        SetFinalBlend(BlendMode m) { m_final_blend = m; }
    // When true, ResolveEffect writes the final output to a per-node offscreen RT instead of
    // SpecTex_Default, so invisible dependency nodes don't composite into the main scene.
    void        SetOffscreen(bool v) { m_is_offscreen = v; }
    bool        IsOffscreen() const { return m_is_offscreen; }

    // When true, the last effect node inherits the parent pointer from the
    // world node so its world transform includes the parent-group transform.
    void SetInheritParent(bool v) { m_inherit_parent = v; }
    bool InheritParent() const { return m_inherit_parent; }

    // When true (compose layer with config.passthrough), the effect chain
    // starts from the current scene output (_rt_default) instead of the
    // layer's own mesh.  The base mesh rendering is replaced by a copy.
    void SetPassthrough(bool v) { m_passthrough = v; }
    bool IsPassthrough() const { return m_passthrough; }

    void ResolveEffect(const SceneMesh& defualt_mesh, std::string_view effect_cam);

private:
    SceneNode*  m_worldNode;
    std::string m_pingpong_a;
    std::string m_pingpong_b;

    bool fullscreen { false };
    bool m_is_offscreen { false };
    bool m_inherit_parent { false };
    bool m_passthrough { false };
    //    std::vector<float> m_size;
    std::unique_ptr<SceneMesh> m_final_mesh;
    std::unique_ptr<SceneNode> m_final_node;
    BlendMode                  m_final_blend;

    std::vector<std::shared_ptr<SceneImageEffect>> m_effects;
};
} // namespace wallpaper
