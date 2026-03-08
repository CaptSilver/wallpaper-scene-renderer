#include "SceneImageEffectLayer.h"
#include "SceneNode.h"

#include "SpecTexs.hpp"
#include "Core/StringHelper.hpp"
#include "Utils/Logging.h"

#include <cassert>

using namespace wallpaper;

SceneImageEffectLayer::SceneImageEffectLayer(SceneNode* node, float w, float h,
                                             std::string_view pingpong_a,
                                             std::string_view pingpong_b)
    : m_worldNode(node),
      m_pingpong_a(pingpong_a),
      m_pingpong_b(pingpong_b),
      m_final_mesh(std::make_unique<SceneMesh>()),
      m_final_node(std::make_unique<SceneNode>()) {};

void SceneImageEffectLayer::ResolveEffect(const SceneMesh& default_mesh,
                                          std::string_view effect_cam) {
    std::string_view ppong_a = m_pingpong_a, ppong_b = m_pingpong_b;
    auto             swap_pp = [&ppong_a, &ppong_b]() {
        std::swap(ppong_a, ppong_b);
    };
    auto default_node = SceneNode();

    SceneImageEffectNode* last_output { nullptr };
    for (auto& eff : m_effects) {
        for (auto& cmd : eff->commands) {
            if (sstart_with(cmd.src, WE_EFFECT_PPONG_PREFIX_A)) cmd.src = ppong_a;

            if (sstart_with(cmd.dst, WE_EFFECT_PPONG_PREFIX_A)) cmd.dst = ppong_a;
        }
        for (auto it = eff->nodes.begin(); it != eff->nodes.end(); it++) {
            if (sstart_with(it->output, WE_EFFECT_PPONG_PREFIX_B) ||
                it->output == SpecTex_Default) {
                it->output  = ppong_b;
                last_output = &(*it);
            }

            assert(it->sceneNode->HasMaterial());

            auto& material = *(it->sceneNode->Mesh()->Material());
            {
                material.blenmode = BlendMode::Normal;
                it->sceneNode->SetCamera(effect_cam.data());
                it->sceneNode->CopyTrans(default_node);
                it->sceneNode->Mesh()->ChangeMeshDataFrom(default_mesh);
            }

            auto& texs = material.textures;
            std::replace_if(
                texs.begin(),
                texs.end(),
                [](auto& t) {
                    return sstart_with(t, WE_EFFECT_PPONG_PREFIX_A);
                },
                ppong_a);
        }
        swap_pp();
    }
    if (last_output != nullptr) {
        // Offscreen nodes write to a per-node RT to avoid compositing into the main scene;
        // their output is still accessible via id_link_map for dependent effects.
        last_output->output = m_is_offscreen
                                  ? GenOffscreenRT(m_worldNode->ID())
                                  : std::string(SpecTex_Default);
        auto& mesh     = *(last_output->sceneNode->Mesh());
        auto& material = *mesh.Material();
        {
            material.blenmode = m_final_blend;
            if (m_is_offscreen) {
                // Force Normal blend so the first write uses DONT_CARE load op
                // instead of LOAD on an uninitialized render target (Vulkan UB).
                material.blenmode = BlendMode::Normal;
                // Offscreen nodes render their final effect into a dedicated RT.
                // Use the "effect" camera (2x2 ortho at origin) with the default
                // mesh and identity transform so the output fills the entire RT.
                // Cannot use the layer camera here because it has the imgEffect
                // attached, which would cause ToGraphPass to recursively re-process
                // the same effect chain.  The global camera would clip content
                // since offscreen positions are outside its visible area.
                last_output->sceneNode->SetCamera(std::string(effect_cam));
                last_output->sceneNode->CopyTrans(default_node);
                mesh.ChangeMeshDataFrom(default_mesh);
            } else if (m_passthrough) {
                // Passthrough compose layers capture the background and run
                // effects (blend, fisheye, opacity mask) on it.  The final
                // output uses the compose layer's world-space position so the
                // result appears at the correct location and size on screen.
                // The opacity/planet-mask effect clips to alpha=0 outside the
                // globe, so Normal blend leaves the rest of the screen intact.
                last_output->sceneNode->SetCamera(std::string());
                last_output->sceneNode->CopyTrans(*m_final_node);
                if (m_inherit_parent) {
                    last_output->sceneNode->InheritParent(*m_worldNode);
                }
                mesh.ChangeMeshDataFrom(*m_final_mesh);
            } else {
                last_output->sceneNode->SetCamera(std::string());
                // Copy the saved local transform from m_final_node
                // (m_worldNode may have been reset to identity for non-compose layers).
                last_output->sceneNode->CopyTrans(*m_final_node);
                // When the image layer has a parent group, inherit that parent so
                // UpdateTrans() chains the group transform into the world matrix.
                if (m_inherit_parent) {
                    last_output->sceneNode->InheritParent(*m_worldNode);
                }
                mesh.ChangeMeshDataFrom(*m_final_mesh);
            }
        }
        auto& t = m_final_node->Translate();
        LOG_INFO("ResolveEffect final: output='%.*s' blend=%d (actual=%d) passthrough=%d "
                 "inherit_parent=%d offscreen=%d translate=(%.1f,%.1f,%.1f) worldNode_id=%d",
                 (int)last_output->output.size(), last_output->output.data(),
                 (int)m_final_blend, (int)material.blenmode, m_passthrough,
                 m_inherit_parent, m_is_offscreen,
                 t[0], t[1], t[2], m_worldNode->ID());
    }
}
