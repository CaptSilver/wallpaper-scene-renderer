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
                             { "pd", { lis.parallaxDepth[0], lis.parallaxDepth[1] } } };
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
            // Resolve via nodeById → SceneNode → Parent() → reverse-lookup
            // the parent's name in nodeNameToId.  Skip the unnamed scene
            // root and any parent that isn't itself a named layer
            // (effect-RT internal nodes are filtered this way).
            auto nbit = nodeById.find(idit->second);
            if (nbit != nodeById.end() && nbit->second) {
                SceneNode* parent_raw = nbit->second->Parent();
                if (parent_raw && parent_raw != sceneGraph.get()) {
                    for (const auto& [pname, pid] : nodeNameToId) {
                        auto pnit = nodeById.find(pid);
                        if (pnit != nodeById.end() && pnit->second == parent_raw) {
                            entry["pn"] = pname;
                            break;
                        }
                    }
                }
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
