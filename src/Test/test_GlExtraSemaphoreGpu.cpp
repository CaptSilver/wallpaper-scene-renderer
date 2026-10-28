#include <doctest.h>

#include "glExtra.hpp"

#include <glad/glad.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <vulkan/vulkan.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <span>
#include <string_view>
#include <unistd.h>
#include <vector>

// GlExtra's semaphore import against a real driver.  Software Mesa cannot
// stand in: llvmpipe has no GL_EXT_semaphore and lavapipe cannot export an
// opaque-fd semaphore, so these need a GPU and only run with WEK_GPU_TESTS=1.
// The GL context is surfaceless EGL (no window, no display) and the Vulkan
// device is the one whose UUID matches it, as the renderer picks its own.

namespace
{

bool gpuTestsEnabled() {
    const char* v = std::getenv("WEK_GPU_TESTS");
    return v && std::string_view(v) == "1";
}

bool isOpen(int fd) { return fcntl(fd, F_GETFD) != -1 || errno != EBADF; }

int makePipeFd() {
    int p[2];
    REQUIRE(pipe(p) == 0);
    close(p[1]);
    return p[0];
}

// One surfaceless GL context made current for the fixture's lifetime.
struct SurfacelessGl {
    void*      lib { nullptr };
    EGLDisplay dpy { EGL_NO_DISPLAY };
    EGLContext ctx { EGL_NO_CONTEXT };

    PFNEGLGETPROCADDRESSPROC getProc { nullptr };

    bool open() {
        lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
        if (! lib) return false;
        getProc = reinterpret_cast<PFNEGLGETPROCADDRESSPROC>(dlsym(lib, "eglGetProcAddress"));
        auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            getProc ? getProc("eglGetPlatformDisplayEXT") : nullptr);
        auto initialize = reinterpret_cast<PFNEGLINITIALIZEPROC>(dlsym(lib, "eglInitialize"));
        auto bindApi    = reinterpret_cast<PFNEGLBINDAPIPROC>(dlsym(lib, "eglBindAPI"));
        auto createCtx  = reinterpret_cast<PFNEGLCREATECONTEXTPROC>(dlsym(lib, "eglCreateContext"));
        auto makeCurrent = reinterpret_cast<PFNEGLMAKECURRENTPROC>(dlsym(lib, "eglMakeCurrent"));
        if (! getPlatformDisplay || ! initialize || ! bindApi || ! createCtx || ! makeCurrent)
            return false;
        dpy = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        if (dpy == EGL_NO_DISPLAY || ! initialize(dpy, nullptr, nullptr)) return false;
        if (! bindApi(EGL_OPENGL_API)) return false;
        ctx = createCtx(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, nullptr);
        return ctx != EGL_NO_CONTEXT &&
               makeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx) == EGL_TRUE;
    }

    ~SurfacelessGl() {
        if (! lib) return;
        auto makeCurrent = reinterpret_cast<PFNEGLMAKECURRENTPROC>(dlsym(lib, "eglMakeCurrent"));
        auto destroyCtx =
            reinterpret_cast<PFNEGLDESTROYCONTEXTPROC>(dlsym(lib, "eglDestroyContext"));
        auto terminate = reinterpret_cast<PFNEGLTERMINATEPROC>(dlsym(lib, "eglTerminate"));
        if (dpy != EGL_NO_DISPLAY) {
            if (makeCurrent) makeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (ctx != EGL_NO_CONTEXT && destroyCtx) destroyCtx(dpy, ctx);
            if (terminate) terminate(dpy);
        }
        dlclose(lib);
    }
};

SurfacelessGl* g_gl = nullptr;
void*          glProc(const char* name) { return reinterpret_cast<void*>(g_gl->getProc(name)); }

// A Vulkan device on the GPU behind the GL context, able to export binary
// semaphores as opaque fds the way CreateExSwapchain does.
struct ExportingVulkan {
    void*      lib { nullptr };
    VkInstance inst { VK_NULL_HANDLE };
    VkDevice   dev { VK_NULL_HANDLE };

    PFN_vkGetInstanceProcAddr gipa { nullptr };
    PFN_vkCreateSemaphore     createSemaphore { nullptr };
    PFN_vkDestroySemaphore    destroySemaphore { nullptr };
    PFN_vkGetSemaphoreFdKHR   getSemaphoreFd { nullptr };
    std::vector<VkSemaphore>  semaphores;

    template<typename F>
    F inst_fn(const char* name) {
        return reinterpret_cast<F>(gipa(inst, name));
    }

    bool open(std::span<const std::uint8_t> gl_uuid) {
        lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (! lib) return false;
        gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
        if (! gipa) return false;
        auto                    createInstance = inst_fn<PFN_vkCreateInstance>("vkCreateInstance");
        const VkApplicationInfo app { .sType      = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                      .apiVersion = VK_API_VERSION_1_1 };
        const VkInstanceCreateInfo ici { .sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                         .pApplicationInfo = &app };
        if (! createInstance || createInstance(&ici, nullptr, &inst) != VK_SUCCESS) return false;

        auto enumerate = inst_fn<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
        auto props2 = inst_fn<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2");
        auto queues = inst_fn<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            "vkGetPhysicalDeviceQueueFamilyProperties");
        auto     createDevice = inst_fn<PFN_vkCreateDevice>("vkCreateDevice");
        auto     gdpa         = inst_fn<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        uint32_t n            = 0;
        enumerate(inst, &n, nullptr);
        std::vector<VkPhysicalDevice> gpus(n);
        enumerate(inst, &n, gpus.data());

        for (VkPhysicalDevice gpu : gpus) {
            VkPhysicalDeviceIDProperties id { .sType =
                                                  VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
            VkPhysicalDeviceProperties2 p { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                            .pNext = &id };
            props2(gpu, &p);
            if (gl_uuid.size() != VK_UUID_SIZE ||
                std::memcmp(id.deviceUUID, gl_uuid.data(), VK_UUID_SIZE) != 0)
                continue;
            uint32_t qn = 0;
            queues(gpu, &qn, nullptr);
            if (qn == 0) return false;
            const float             prio = 1.0f;
            VkDeviceQueueCreateInfo qci { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                          .queueFamilyIndex = 0,
                                          .queueCount       = 1,
                                          .pQueuePriorities = &prio };
            const char*             exts[] = { VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME };
            VkDeviceCreateInfo      dci { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                          .queueCreateInfoCount    = 1,
                                          .pQueueCreateInfos       = &qci,
                                          .enabledExtensionCount   = 1,
                                          .ppEnabledExtensionNames = exts };
            if (createDevice(gpu, &dci, nullptr, &dev) != VK_SUCCESS) return false;
            createSemaphore =
                reinterpret_cast<PFN_vkCreateSemaphore>(gdpa(dev, "vkCreateSemaphore"));
            destroySemaphore =
                reinterpret_cast<PFN_vkDestroySemaphore>(gdpa(dev, "vkDestroySemaphore"));
            getSemaphoreFd =
                reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(gdpa(dev, "vkGetSemaphoreFdKHR"));
            return createSemaphore && destroySemaphore && getSemaphoreFd;
        }
        return false; // no Vulkan device behind this GL context
    }

    int exportSemaphoreFd() {
        VkExportSemaphoreCreateInfo export_info {
            .sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
        };
        VkSemaphoreCreateInfo ci { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                   .pNext = &export_info };
        VkSemaphore           sem { VK_NULL_HANDLE };
        REQUIRE(createSemaphore(dev, &ci, nullptr, &sem) == VK_SUCCESS);
        semaphores.push_back(sem);
        VkSemaphoreGetFdInfoKHR gfi { .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
                                      .semaphore = sem,
                                      .handleType =
                                          VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT };
        int                     fd = -1;
        REQUIRE(getSemaphoreFd(dev, &gfi, &fd) == VK_SUCCESS);
        return fd;
    }

    ~ExportingVulkan() {
        if (dev != VK_NULL_HANDLE) {
            for (VkSemaphore s : semaphores) destroySemaphore(dev, s, nullptr);
            inst_fn<PFN_vkDestroyDevice>("vkDestroyDevice")(dev, nullptr);
        }
        if (inst != VK_NULL_HANDLE)
            inst_fn<PFN_vkDestroyInstance>("vkDestroyInstance")(inst, nullptr);
        if (lib) dlclose(lib);
    }
};

// Everything the cases share; `ready` false means the machine cannot run them.
struct GpuInterop {
    SurfacelessGl   gl;
    GlExtra         glex;
    ExportingVulkan vk;
    bool            ready { false };

    GpuInterop() {
        if (! gl.open()) return;
        g_gl = &gl;
        if (! glex.init(glProc) || ! glex.hasSemaphoreFd()) return;
        ready = vk.open(glex.uuid());
    }
};

} // namespace

TEST_SUITE("GlExtra semaphore import (real GPU, WEK_GPU_TESTS=1)") {
    TEST_CASE("a semaphore the renderer exported imports, and GL takes its fd") {
        if (! gpuTestsEnabled()) return;
        GpuInterop gpu;
        REQUIRE_MESSAGE(gpu.ready, "no GPU with GL_EXT_semaphore_fd and a matching Vulkan device");
        int        fd  = gpu.vk.exportSemaphoreFd();
        const uint sem = gpu.glex.importSemaphore(fd);
        CHECK(sem != 0);
        CHECK(fd == -1);
        gpu.glex.deleteSemaphore(sem);
    }

    // Drivers differ here: radeonsi accepts any fd at import (no GL error,
    // glIsSemaphoreEXT true) and closes it, while a driver that validates the
    // fd fails the import and leaves it open, so importSemaphore closes it.
    // Either way the caller is left holding nothing and no fd stays open.
    TEST_CASE("an fd that is not a semaphore is taken either way, never leaked") {
        if (! gpuTestsEnabled()) return;
        GpuInterop gpu;
        REQUIRE_MESSAGE(gpu.ready, "no GPU with GL_EXT_semaphore_fd and a matching Vulkan device");
        int        fd   = makePipeFd();
        const int  copy = fd;
        const uint sem  = gpu.glex.importSemaphore(fd);
        CHECK(fd == -1);
        CHECK_FALSE(isOpen(copy));
        gpu.glex.deleteSemaphore(sem);
    }

    TEST_CASE("a GL error left by earlier calls does not fail a good import") {
        if (! gpuTestsEnabled()) return;
        GpuInterop gpu;
        REQUIRE_MESSAGE(gpu.ready, "no GPU with GL_EXT_semaphore_fd and a matching Vulkan device");
        glEnable(0xFFFF); // GL_INVALID_ENUM, left pending
        int        fd  = gpu.vk.exportSemaphoreFd();
        const uint sem = gpu.glex.importSemaphore(fd);
        CHECK(sem != 0);
        CHECK(fd == -1);
        gpu.glex.deleteSemaphore(sem);
    }
}
