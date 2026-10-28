#include <doctest.h>

#include "Vulkan/VulkanExSwapchain.hpp"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

using wallpaper::ExHandle;
using wallpaper::vulkan::VulkanExHandle;
using wallpaper::vulkan::VulkanExSwapchain;

namespace
{

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

TEST_SUITE("VulkanExSwapchain fd ownership") {
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
            CHECK(chain.eatFrame([](ExHandle*) {
            }) == nullptr); // nothing published yet
            chain.renderFrame();
            ExHandle* taken = chain.eatFrame([](ExHandle*) {
            });
            REQUIRE(taken != nullptr);
            // The consumer imports whatever it is handed and marks it taken.
            taken->fd = taken->render_done_fd = taken->release_fd = -1;
        }
        int still_open = 0;
        for (int fd : fds) still_open += isOpen(fd) ? 1 : 0;
        CHECK(still_open == 3); // the taken image's three, now the importer's
        for (int fd : fds)
            if (isOpen(fd)) close(fd);
    }

    TEST_CASE("the memory fd moves to the handle, so the image no longer holds it") {
        std::array<int, 9>            fds {};
        std::array<VulkanExHandle, 3> handles = exportedImages(fds);
        VulkanExSwapchain             chain(std::move(handles), VkExtent2D { 4, 4 });
        for (const auto& h : chain.handles()) {
            CHECK(h.image.fd == -1);
            CHECK(h.render_done_fd == -1);
            CHECK(h.release_fd == -1);
        }
    }

    TEST_CASE("a swapchain built without semaphores hands images over unsynchronised") {
        std::array<int, 9> fds {};
        VulkanExSwapchain  chain(exportedImages(fds), VkExtent2D { 4, 4 });
        CHECK_FALSE(chain.interopSync());
        CHECK(chain.inprogressRenderDone() == VK_NULL_HANDLE);
        CHECK(chain.inprogressRelease() == VK_NULL_HANDLE);
    }

    // CreateExSwapchain drops the handles it built when one semaphore fails to
    // export; the fds already exported are only freed by the handle itself.
    TEST_CASE("an image handle never given to a swapchain closes its semaphore fds") {
        const int render_done = makeFd();
        const int release     = makeFd();
        {
            VulkanExHandle h;
            h.render_done_fd = render_done;
            h.release_fd     = release;
        }
        CHECK_FALSE(isOpen(render_done));
        CHECK_FALSE(isOpen(release));
    }

    TEST_CASE("move-assigning over an image handle closes the semaphore fds it held") {
        const int      old_done = makeFd(), old_release = makeFd();
        const int      new_done = makeFd(), new_release = makeFd();
        VulkanExHandle dest;
        dest.render_done_fd = old_done;
        dest.release_fd     = old_release;
        VulkanExHandle source;
        source.render_done_fd = new_done;
        source.release_fd     = new_release;

        dest = std::move(source);
        CHECK_FALSE(isOpen(old_done));
        CHECK_FALSE(isOpen(old_release));
        CHECK(dest.render_done_fd == new_done);
        CHECK(dest.release_fd == new_release);
        CHECK(source.render_done_fd == -1);
        CHECK(source.release_fd == -1);
    }

    TEST_CASE("move-assigning an image handle onto itself keeps its semaphore fds") {
        const int      done = makeFd(), release = makeFd();
        VulkanExHandle h;
        h.render_done_fd = done;
        h.release_fd     = release;
        auto& alias      = h; // avoids -Wself-move on a direct self-assignment
        h                = std::move(alias);
        CHECK(isOpen(done));
        CHECK(isOpen(release));
        CHECK(h.render_done_fd == done);
        CHECK(h.release_fd == release);
    }
}
