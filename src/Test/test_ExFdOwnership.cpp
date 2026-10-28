#include <doctest.h>

#include "glExtra.hpp"
#include "Vulkan/VulkanExSwapchain.hpp"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

using wallpaper::ExHandle;
using wallpaper::vulkan::VulkanExHandle;
using wallpaper::vulkan::VulkanExSwapchain;

namespace
{

// A real fd to hand around; the other end of the pipe is closed straight away.
int makeFd() {
    int p[2];
    REQUIRE(pipe(p) == 0);
    close(p[1]);
    return p[0];
}

bool isOpen(int fd) { return fcntl(fd, F_GETFD) != -1 || errno != EBADF; }

// An exported image as CreateExSwapchain would build it, minus the Vulkan
// objects: only the fds matter here.
std::array<VulkanExHandle, 3> exportedImages(std::array<int, 9>& fds) {
    std::array<VulkanExHandle, 3> handles;
    for (int i = 0; i < 3; ++i) {
        handles[i].image.fd = fds[i * 3] = makeFd();
        handles[i].render_done_fd = fds[i * 3 + 1] = makeFd();
        handles[i].release_fd = fds[i * 3 + 2] = makeFd();
    }
    return handles;
}

} // namespace

TEST_SUITE("ExHandle fd ownership") {
    // GL takes ownership of an fd it imports, so the caller must not close it
    // afterwards; one genExTexture cannot import is still the caller's, and
    // genExTexture closes it rather than leaving it to a caller that can no
    // longer tell the two apart.
    TEST_CASE("genExTexture closes an fd it cannot import") {
        GlExtra  glex;
        ExHandle handle;
        handle.fd    = makeFd();
        handle.size  = 0; // not importable
        const int fd = handle.fd;

        CHECK(glex.genExTexture(handle) == 0);
        CHECK(handle.fd == -1);
        CHECK_FALSE(isOpen(fd));
    }

    TEST_CASE("a swapchain closes the fds of images nobody imported") {
        std::array<int, 9> fds {};
        {
            VulkanExSwapchain chain(exportedImages(fds), VkExtent2D { 4, 4 });
        }
        for (int fd : fds) CHECK_FALSE(isOpen(fd));
    }

    TEST_CASE("a swapchain leaves alone the fds an importer took") {
        std::array<int, 9> fds {};
        {
            VulkanExSwapchain chain(exportedImages(fds), VkExtent2D { 4, 4 });
            // The consumer imports whatever it is handed and marks it taken.
            ExHandle* taken = chain.eatFrame([](ExHandle*) {
            });
            CHECK(taken == nullptr); // nothing published yet
            chain.renderFrame();
            taken = chain.eatFrame([](ExHandle*) {
            });
            REQUIRE(taken != nullptr);
            taken->fd = taken->render_done_fd = taken->release_fd = -1;
        }
        int still_open = 0;
        for (int fd : fds) still_open += isOpen(fd) ? 1 : 0;
        CHECK(still_open == 3); // the taken image's three, now the importer's
        for (int fd : fds)
            if (isOpen(fd)) close(fd);
    }
}
