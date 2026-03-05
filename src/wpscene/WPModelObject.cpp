#include "WPModelObject.h"
#include "Utils/Logging.h"
#include "Fs/VFS.h"

using namespace wallpaper::wpscene;

bool WPModelObject::FromJson(const nlohmann::json& json, fs::VFS&) {
    GET_JSON_NAME_VALUE(json, "model", model);
    GET_JSON_NAME_VALUE_NOWARN(json, "id", id);
    GET_JSON_NAME_VALUE_NOWARN(json, "name", name);
    GET_JSON_NAME_VALUE_NOWARN(json, "visible", visible);
    GET_JSON_NAME_VALUE_NOWARN(json, "parent", parent_id);
    GET_JSON_NAME_VALUE_NOWARN(json, "origin", origin);
    GET_JSON_NAME_VALUE_NOWARN(json, "scale", scale);
    GET_JSON_NAME_VALUE_NOWARN(json, "angles", angles);
    LOG_INFO("model object id=%d name='%s' model='%s'", id, name.c_str(), model.c_str());
    return true;
}
