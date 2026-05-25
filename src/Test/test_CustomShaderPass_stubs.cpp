// Test-only stubs for the per-pass RT dump globals defined in
// VulkanRender.cpp.  CustomShaderPass.cpp references them through extern
// declarations inside its execute() body; the headless test binary compiles
// the .cpp in to get the ctor/dtor/desc copy but never calls execute(), so
// the stubs only have to satisfy the linker — they are never read.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace vvk
{
class CommandBuffer;
}

namespace wallpaper::vulkan
{

struct Device;
struct PassDumpEntry;

bool                        g_pass_dump_active  = false;
std::vector<PassDumpEntry>* g_pass_dump_entries = nullptr;
Device const*               g_pass_dump_device  = nullptr;

void g_pass_dump_record(const vvk::CommandBuffer& /*cmd*/, VkImage /*image*/, VkFormat /*format*/,
                        uint32_t /*w*/, uint32_t /*h*/, const std::string& /*shader*/,
                        const std::string& /*output*/, int32_t /*node_id*/) {}

} // namespace wallpaper::vulkan
