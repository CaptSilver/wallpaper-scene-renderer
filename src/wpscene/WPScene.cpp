#include "WPScene.h"

using namespace wallpaper::wpscene;

bool Orthogonalprojection::FromJson(const nlohmann::json& json) {
    if(json.is_null()) return false;
	if(json.contains("auto")) {
		GET_JSON_NAME_VALUE(json, "auto", auto_);
	}
	else {
		GET_JSON_NAME_VALUE(json, "width", width);
		GET_JSON_NAME_VALUE(json, "height", height);
	}
    return true;
}

bool WPSceneCamera::FromJson(const nlohmann::json& json) {
    GET_JSON_NAME_VALUE(json, "center", center);
    GET_JSON_NAME_VALUE(json, "eye", eye);
    GET_JSON_NAME_VALUE(json, "up", up);
    if (json.contains("paths") && json.at("paths").is_array()) {
        for (auto& p : json.at("paths")) {
            if (p.is_string()) paths.push_back(p.get<std::string>());
        }
    }
    return true;
}

bool WPSceneGeneral::FromJson(const nlohmann::json& json) {
    GET_JSON_NAME_VALUE(json, "ambientcolor", ambientcolor);
    GET_JSON_NAME_VALUE(json, "skylightcolor", skylightcolor);
	GET_JSON_NAME_VALUE(json, "clearcolor", clearcolor);
	GET_JSON_NAME_VALUE(json, "cameraparallax", cameraparallax);
	GET_JSON_NAME_VALUE(json, "cameraparallaxamount", cameraparallaxamount);
	GET_JSON_NAME_VALUE(json, "cameraparallaxdelay", cameraparallaxdelay);
	GET_JSON_NAME_VALUE(json, "cameraparallaxmouseinfluence", cameraparallaxmouseinfluence);
	GET_JSON_NAME_VALUE_NOWARN(json, "zoom", zoom);
	GET_JSON_NAME_VALUE_NOWARN(json, "fov", fov);
	GET_JSON_NAME_VALUE_NOWARN(json, "nearz", nearz);
	GET_JSON_NAME_VALUE_NOWARN(json, "farz", farz);
	GET_JSON_NAME_VALUE_NOWARN(json, "hdr", hdr);
	GET_JSON_NAME_VALUE_NOWARN(json, "bloom", bloom);
	GET_JSON_NAME_VALUE_NOWARN(json, "bloomstrength", bloomstrength);
	GET_JSON_NAME_VALUE_NOWARN(json, "bloomthreshold", bloomthreshold);
	GET_JSON_NAME_VALUE_NOWARN(json, "bloomhdrstrength", bloomhdrstrength);
	GET_JSON_NAME_VALUE_NOWARN(json, "bloomhdrthreshold", bloomhdrthreshold);
	GET_JSON_NAME_VALUE_NOWARN(json, "camerashake", camerashake);
	GET_JSON_NAME_VALUE_NOWARN(json, "camerashakeamplitude", camerashakeamplitude);
	GET_JSON_NAME_VALUE_NOWARN(json, "camerashakespeed", camerashakespeed);
	GET_JSON_NAME_VALUE_NOWARN(json, "camerashakeroughness", camerashakeroughness);
	GET_JSON_NAME_VALUE_NOWARN(json, "camerafade", camerafade);
    if(json.contains("orthogonalprojection")) {
        const auto& ortho = json.at("orthogonalprojection");
        if(ortho.is_null())
            isOrtho = false;
        else {
            isOrtho = true;
            orthogonalprojection.FromJson(ortho);
        }
    } else {
        isOrtho = false;
    }
    return true;
}

bool WPScene::FromJson(const nlohmann::json& json) {
    if(json.contains("camera")) {
        camera.FromJson(json.at("camera"));
    } else {
        LOG_ERROR("scene no camera");
        return false;
    }
    if(json.contains("general")) {
        general.FromJson(json.at("general"));
    } else {
        LOG_ERROR("scene no genera data");
        return false;
    }
    return true; 
}
