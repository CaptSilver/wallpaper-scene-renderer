#include "Scene.h"

#include "Fs/VFS.h"
#include "Interface/IImageParser.h"
#include "Interface/IShaderValueUpdater.h"
#include "Particle/ParticleSystem.h"

#include <nlohmann/json.hpp>

namespace wallpaper
{

Scene::Scene()
    : sceneGraph(std::make_shared<SceneNode>()),
      paritileSys(std::make_unique<ParticleSystem>(*this)) {}
Scene::~Scene() = default;

std::string Scene::SerializeLayerInitialStates() const {
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [name, lis] : layerInitialStates) {
        auto entry =
            nlohmann::json { { "o", { lis.origin[0], lis.origin[1], lis.origin[2] } },
                             { "s", { lis.scale[0], lis.scale[1], lis.scale[2] } },
                             { "a", { lis.angles[0], lis.angles[1], lis.angles[2] } },
                             { "v", lis.visible },
                             { "sz", { lis.size[0], lis.size[1] } },
                             { "pd", { lis.parallaxDepth[0], lis.parallaxDepth[1] } },
                             // World origin/scale baked at parse time —
                             // hit-test uses these so clicks on parented
                             // buttons reach the right layer.
                             { "wo", { lis.worldOrigin[0], lis.worldOrigin[1], lis.worldOrigin[2] } },
                             { "ws", { lis.worldScale[0], lis.worldScale[1], lis.worldScale[2] } } };
        // Effects metadata for SceneScript getEffect()
        auto eit = layerEffectNames.find(name);
        if (eit != layerEffectNames.end()) {
            entry["efx"] = eit->second;
        }
        // Node id for setParent bridge dispatch.
        auto idit = nodeNameToId.find(name);
        if (idit != nodeNameToId.end()) {
            entry["id"] = idit->second;
            // Parent layer name (for deferred linkup of _parent / _children).
            //
            // PREFERRED PATH: JSON-declared parent ID (scene.json's `parent`
            // field).  Set at parse time and untouched by effect-RT or
            // composelayer node rewiring.  Floating Cat (3367988661) ships
            //   "Background"        parent=169 (Text Container)
            //   "Settings Container" parent=169
            // but the runtime SceneNode for Background lives under several
            // effect-RT wrappers and `SceneNode::Parent()` walks straight up
            // to the scene root — `Background.pn` resolved to nothing and
            // scripts crashed on `parent.scale.x`.
            //
            // FALLBACK: live SceneNode parent walk, skipping unnamed
            // intermediate nodes.  Used when the JSON didn't declare a
            // `parent` (script-created layers, dynamic-pool slots,
            // effect-chain roots).
            bool        pn_resolved = false;
            const auto& nodeId      = idit->second;
            auto        jpit        = jsonParentId.find(nodeId);
            if (jpit != jsonParentId.end()) {
                // Find the parent ID's name.
                for (const auto& [pname, pid] : nodeNameToId) {
                    if (pid == jpit->second) {
                        entry["pn"] = pname;
                        pn_resolved = true;
                        break;
                    }
                }
            }
            if (! pn_resolved) {
                auto nbit = nodeById.find(nodeId);
                if (nbit != nodeById.end() && nbit->second) {
                    SceneNode* parent_raw = nbit->second->Parent();
                    while (parent_raw && parent_raw != sceneGraph.get()) {
                        bool resolved = false;
                        for (const auto& [pname, pid] : nodeNameToId) {
                            auto pnit = nodeById.find(pid);
                            if (pnit != nodeById.end() && pnit->second == parent_raw) {
                                entry["pn"] = pname;
                                resolved = true;
                                break;
                            }
                        }
                        if (resolved) break;
                        parent_raw = parent_raw->Parent();
                    }
                }
            }
            // Text-layer style seeds for SceneScript thisLayer.font /
            // .horizontalalign / .verticalalign / .alignment reads before any
            // assignment.  Only emitted for layers that actually have a text
            // info entry.
            for (const auto& tl : textLayers) {
                if (tl.id != idit->second) continue;
                if (! tl.halign.empty())   entry["halign"] = tl.halign;
                if (! tl.valign.empty())   entry["valign"] = tl.valign;
                if (! tl.fontName.empty()) entry["font"]   = tl.fontName;
                break;
            }
        }
        j[name] = std::move(entry);
    }
    j["_ortho"] = { ortho[0], ortho[1] };
    if (! assetPools.empty()) {
        nlohmann::json pools = nlohmann::json::object();
        for (const auto& [path, names] : assetPools) {
            pools[path] = names;
        }
        j["_assetPools"] = std::move(pools);
    }
    return j.dump();
}

} // namespace wallpaper
