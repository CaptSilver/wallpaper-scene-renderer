#include <iostream>
#include <set>
#include <fstream>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <atomic>
#include "arg.hpp"
#include "SceneWallpaper.hpp"
#include "SceneWallpaperSurface.hpp"

#include "Utils/Platform.hpp"

using namespace std;

atomic<bool> renderCall(false);

struct UserData {
    wallpaper::SceneWallpaper* psw { nullptr };

    uint16_t width;
    uint16_t height;
};

extern "C" {
void framebuffer_size_callback(GLFWwindow*, int width, int height) {}

void key_callback(GLFWwindow* win, int key, int scancode, int action, int mods) {
    if (action != GLFW_PRESS) return;
    UserData* data = static_cast<UserData*>(glfwGetWindowUserPointer(win));
    // Number keys 1-9 → switch lucyrebecca to that value at runtime
    if (key >= GLFW_KEY_1 && key <= GLFW_KEY_9) {
        std::string val = std::to_string(key - GLFW_KEY_0);
        std::string json = "{\"lucyrebecca\":\"" + val + "\"}";
        std::cout << "Runtime user prop change: " << json << std::endl;
        data->psw->setPropertyString(wallpaper::PROPERTY_USER_PROPS, json);
    }
}

void mouse_button_callback(GLFWwindow* win, int button, int action, int mods) {
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        UserData* data = static_cast<UserData*>(glfwGetWindowUserPointer(win));
        // data->psw->setPropertyString(wallpaper::PROPERTY_SOURCE,
    }
}

void cursor_position_callback(GLFWwindow* win, double xpos, double ypos) {
    UserData* data = static_cast<UserData*>(glfwGetWindowUserPointer(win));
    data->psw->mouseInput(xpos / data->width, ypos / data->height);
}
}

void updateCallback() {
    renderCall = true;
    glfwPostEmptyEvent();
}

int main(int argc, char** argv) {
    argparse::ArgumentParser program("scene-viewer");
    setAndParseArg(program, argc, argv);
    auto [w_width, w_height] = program.get<Resolution>(OPT_RESOLUTION);

    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(w_width, w_height, "WP", nullptr, nullptr);

    UserData data;
    data.width  = w_width;
    data.height = w_height;

    wallpaper::RenderInitInfo info;
    info.enable_valid_layer = program.get<bool>(OPT_VALID_LAYER);
    info.hdr_content        = program.get<bool>(OPT_HDR);
    info.width              = w_width;
    info.height             = w_height;

    auto& sf_info = info.surface_info;
    {
        uint32_t glfwExtCount = 0;
        auto     exts         = glfwGetRequiredInstanceExtensions(&glfwExtCount);
        for (int i = 0; i < glfwExtCount; i++) {
            sf_info.instanceExts.emplace_back(exts[i]);
        }

        sf_info.createSurfaceOp = [window](VkInstance inst, VkSurfaceKHR* surface) {
            return glfwCreateWindowSurface(inst, window, NULL, surface);
        };
    }

    if (window == nullptr) {
        std::cout << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }

    auto* psw = new wallpaper::SceneWallpaper();
    data.psw  = psw;

    psw->init();
    psw->initVulkan(info);
    psw->setPropertyString(wallpaper::PROPERTY_ASSETS, program.get<std::string>(ARG_ASSETS));
    psw->setPropertyString(wallpaper::PROPERTY_SOURCE, program.get<std::string>(ARG_SCENE));
    psw->setPropertyBool(wallpaper::PROPERTY_GRAPHIVZ, program.get<bool>(OPT_GRAPHVIZ));
    auto fps_val = program.get<int32_t>(OPT_FPS);
    if (fps_val < 5) fps_val = 60; // default to 60fps
    psw->setPropertyInt32(wallpaper::PROPERTY_FPS, fps_val);

    std::string cache_path = program.get<std::string>(OPT_CACHE_PATH);
    if (cache_path.empty()) cache_path = wallpaper::platform::GetCachePath("wescene-renderer");
    psw->setPropertyString(wallpaper::PROPERTY_CACHE_PATH, cache_path);

    std::string user_props = program.get<std::string>(OPT_USER_PROPS);
    if (!user_props.empty()) {
        psw->setPropertyString(wallpaper::PROPERTY_USER_PROPS, user_props);
    }

    glfwSetWindowUserPointer(window, &data);

    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetKeyCallback(window, key_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetCursorPosCallback(window, cursor_position_callback);

    // Start the render loop (frame timer)
    psw->play();

    while (! glfwWindowShouldClose(window)) {
        glfwPollEvents();
    }
    delete psw;
    // wgl.Clear();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
