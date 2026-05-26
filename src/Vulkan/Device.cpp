#include "Device.hpp"

#include "Utils/Logging.h"
#include "GraphicsPipeline.hpp"

using namespace wallpaper::vulkan;

namespace
{

void EnumateDeviceExts(const vvk::PhysicalDevice& gpu, wallpaper::Set<std::string>& set) {
    std::vector<VkExtensionProperties> properties;
    VVK_CHECK_VOID_RE(gpu.EnumerateDeviceExtensionProperties(properties));
    for (auto& ext : properties) {
        set.insert(ext.extensionName);
    }
}

} // namespace

namespace wallpaper::vulkan::queue_selection
{
uint32_t FindUnifiedFamily(const std::vector<uint32_t>& graphics,
                           const std::vector<uint32_t>& present) {
    for (auto g : graphics) {
        for (auto p : present) {
            if (g == p) return g;
        }
    }
    return UINT32_MAX;
}
} // namespace wallpaper::vulkan::queue_selection

bool Device::CheckGPU(vvk::PhysicalDevice gpu, std::span<const Extension> exts,
                      VkSurfaceKHR surface) {
    std::vector<VkDeviceQueueCreateInfo> queues;
    auto                                 props = gpu.GetQueueFamilyProperties();

    // check queue
    bool has_graphics_queue { false };
    bool has_present_queue { false };
    uint index { 0 };
    for (auto& prop : props) {
        if (prop.queueFlags & VK_QUEUE_GRAPHICS_BIT) has_graphics_queue = true;
        if (surface) {
            bool ok { false };
            VVK_CHECK(gpu.GetSurfaceSupportKHR(index, surface, ok));
            if (ok) has_present_queue = true;
        }
        index++;
    };
    if (! has_graphics_queue) return false;
    if (surface && ! has_present_queue) return false;

    // check exts
    Set<std::string> extensions;
    EnumateDeviceExts(gpu, extensions);
    for (auto& ext : exts) {
        if (ext.required) {
            if (! exists(extensions, ext.name)) return false;
        }
    }
    return true;
}

std::vector<VkDeviceQueueCreateInfo> Device::ChooseDeviceQueue(VkSurfaceKHR surface) {
    std::vector<VkDeviceQueueCreateInfo> queues;

    auto props = m_gpu.GetQueueFamilyProperties();

    std::vector<uint32_t> graphic_indexs;
    uint32_t              index = 0;
    for (auto& prop : props) {
        if (prop.queueFlags & VK_QUEUE_GRAPHICS_BIT) graphic_indexs.push_back(index);
        index++;
    };
    // Default: first graphics family for both render and present.  When a
    // surface is provided we prefer a graphics family that ALSO supports
    // presentation — that lets the command pool, the render submit queue, and
    // the present queue all share a family.  Without this preference we'd
    // submit command buffers (allocated from the graphics-family pool) to a
    // present queue from a DIFFERENT family, which violates
    // VUID-vkQueueSubmit-pCommandBuffers-00074 on split-queue GPUs.
    m_graphics_queue.family_index = graphic_indexs.front();
    m_present_queue.family_index  = graphic_indexs.front();
    if (surface) {
        std::vector<uint32_t> present_indexs;
        index = 0;
        for (auto& prop : props) {
            (void)prop;
            bool ok { false };
            VVK_CHECK(m_gpu.GetSurfaceSupportKHR(index, surface, ok))
            if (ok) present_indexs.push_back(index);
            index++;
        };
        if (present_indexs.empty()) {
            LOG_ERROR("not find present queue");
        } else {
            const uint32_t unified =
                queue_selection::FindUnifiedFamily(graphic_indexs, present_indexs);
            if (unified != UINT32_MAX) {
                m_graphics_queue.family_index = unified;
                m_present_queue.family_index  = unified;
            } else {
                // No graphics-and-present family — proper handling needs a
                // second command pool from the present family + a queue-family
                // ownership transfer in FinPass.  Real desktop GPUs all expose
                // at least one graphics+present family, so this branch is a
                // best-effort fallback rather than a tested code path.
                LOG_ERROR("no graphics queue family supports surface presentation - "
                          "renderer may submit cross-family (VUID-vkQueueSubmit-00074)");
                m_present_queue.family_index = present_indexs.front();
            }
        }
    }
    const static float      defaultQueuePriority = 0.0f;
    VkDeviceQueueCreateInfo gi {
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = m_graphics_queue.family_index,
        .queueCount       = 1,
        .pQueuePriorities = &defaultQueuePriority,
    };
    queues.push_back(gi);
    if (m_present_queue.family_index != m_graphics_queue.family_index) {
        VkDeviceQueueCreateInfo pi {
            .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = m_present_queue.family_index,
            .queueCount       = 1,
            .pQueuePriorities = &defaultQueuePriority,
        };
        queues.push_back(pi);
    }
    return queues;
}

bool Device::Create(Instance& inst, std::span<const Extension> exts, VkExtent2D extent,
                    Device& device) {
    device.dld      = vvk::DeviceDispatch { inst.inst().Dispatch() };
    device.m_gpu    = inst.gpu();
    device.m_limits = inst.gpu().GetProperties().limits;
    device.set_out_extent(extent);

    Set<std::string> tested_exts;
    {
        EnumateDeviceExts(inst.gpu(), device.m_extensions);
        for (auto& ext : exts) {
            bool ok = device.supportExt(ext.name);
            if (ok) tested_exts.insert(std::string(ext.name));
            if (ext.required && ! ok) {
                LOG_ERROR("required vulkan device extension \"%s\" is not supported",
                          ext.name.data());
                return false;
            }
        }
    }
    std::vector<const char*> tested_exts_c { tested_exts.size() };
    std::transform(
        tested_exts.begin(), tested_exts.end(), tested_exts_c.begin(), [](const auto& s) {
            return s.c_str();
        });
    // Enable device features
    VkPhysicalDeviceFeatures2KHR features2 {
        .sType    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR,
        .pNext    = nullptr,
        .features = {},
    };
    {
        VkPhysicalDeviceFeatures2KHR supported {
            .sType    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR,
            .pNext    = nullptr,
            .features = {},
        };
        inst.gpu().GetFeatures2KHR(supported);
        if (supported.features.samplerAnisotropy) {
            features2.features.samplerAnisotropy = VK_TRUE;
            LOG_INFO("Anisotropic filtering enabled (max %.0f)",
                     device.m_limits.maxSamplerAnisotropy);
        }
        if (supported.features.geometryShader) {
            features2.features.geometryShader = VK_TRUE;
            LOG_INFO("Geometry shader feature enabled");
        }
    }

    // Probe whether VK_FORMAT_D32_SFLOAT supports sampled-image usage with
    // optimal tiling.  This invariant gates the depth-sample path used by
    // the volumetric chain (and any future SSAO/DOF).  On modern desktop
    // GPUs (RADV, NVIDIA, Intel) the bit is set; on lavapipe and some
    // mobile/headless drivers it is not, in which case downstream code
    // emits a depth-to-color resolve fallback.
    {
        const VkFormatProperties fmt_props = inst.gpu().GetFormatProperties(VK_FORMAT_D32_SFLOAT);
        device.m_d32_sampleable =
            (fmt_props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
        LOG_INFO("D32_SFLOAT optimal-tiling sampled-image support: %s",
                 device.m_d32_sampleable ? "yes" : "no");
    }

    bool rq_surface = ! inst.offscreen();
    VVK_CHECK_BOOL_RE(vvk::Device::Create(device.m_device,
                                          *device.m_gpu,
                                          device.ChooseDeviceQueue(*inst.surface()),
                                          tested_exts_c,
                                          &features2,
                                          device.dld));

    // VK_CHECK_RESULT_BOOL_RE(CreateDevice(inst, device.ChooseDeviceQueue(inst.surface()),
    // tested_exts_c, &device.m_device));

    device.m_graphics_queue.handle = device.m_device.GetQueue(device.m_graphics_queue.family_index);
    device.m_present_queue.handle  = device.m_device.GetQueue(device.m_present_queue.family_index);

    if (rq_surface) {
        if (! Swapchain::Create(device, *inst.surface(), extent, device.m_swapchain)) {
            LOG_ERROR("create swapchain failed");
            return false;
        }
    }
    {
        // No TRANSIENT_BIT: the per-frame render command buffers live for the
        // whole session (1+kFramesInFlight), so the "short-lived" hint would
        // mislead the driver allocator.  RESET_COMMAND_BUFFER_BIT plus the
        // CB.Begin(ONE_TIME_SUBMIT) call sites already provide per-CB reset.
        VkCommandPoolCreateInfo info { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                       .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                       .queueFamilyIndex = device.m_graphics_queue.family_index };
        VVK_CHECK_BOOL_RE(device.m_device.CreateCommandPool(info, device.m_command_pool));
    }
    {
        VmaAllocatorCreateInfo allocatorInfo = {};
        allocatorInfo.vulkanApiVersion       = WP_VULKAN_VERSION;
        allocatorInfo.physicalDevice         = *device.m_gpu;
        allocatorInfo.device                 = *device.m_device;
        allocatorInfo.instance               = *inst.inst();
        VVK_CHECK_BOOL_RE(vvk::CreateVmaAllocator(allocatorInfo, device.m_allocator));
    }
    device.m_tex_cache = std::make_unique<TextureCache>(device);
    return true;
}

VkDeviceSize Device::GetUsage() const {
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
    vmaGetHeapBudgets(*m_allocator, budgets);
    VkDeviceSize total = 0;
    for (auto& b : budgets) total += b.usage;
    return total;
}

void Device::Destroy() { VVK_CHECK(m_device.WaitIdle()); }

Device::Device(): m_tex_cache(std::make_unique<TextureCache>(*this)) {}
Device::~Device() {};

bool Device::supportExt(std::string_view name) const { return exists(m_extensions, name); }
