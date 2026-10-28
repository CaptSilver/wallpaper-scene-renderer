#include <doctest.h>

#include "Vulkan/Parameters.hpp"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

using wallpaper::vulkan::ExImageParameters;

namespace
{

int makeFd() {
    int p[2];
    REQUIRE(pipe(p) == 0);
    close(p[1]);
    return p[0];
}

bool isOpen(int fd) { return fcntl(fd, F_GETFD) != -1 || errno != EBADF; }

// Puts a pipe on fd 0 for the duration, so a test can hand an
// ExImageParameters the one valid fd number that used to mean "none".
// Restores the process's stdin afterwards whatever the test did with fd 0.
struct PipeOnStdin {
    int saved { dup(0) };
    PipeOnStdin() {
        REQUIRE(saved >= 0);
        const int fd = makeFd();
        REQUIRE(dup2(fd, 0) == 0);
        close(fd);
    }
    ~PipeOnStdin() {
        dup2(saved, 0);
        close(saved);
    }
};

} // namespace

TEST_SUITE("ExImageParameters fd ownership") {
    TEST_CASE("destruction closes the exported fd it still holds") {
        const int fd = makeFd();
        {
            ExImageParameters image;
            image.fd = fd;
        }
        CHECK_FALSE(isOpen(fd));
    }

    TEST_CASE("destruction closes fd 0 too, which is a real fd and not 'none'") {
        PipeOnStdin stdin_pipe;
        {
            ExImageParameters image;
            image.fd = 0;
        }
        CHECK_FALSE(isOpen(0));
    }

    TEST_CASE("an image that never exported an fd closes nothing") {
        const int bystander = makeFd();
        {
            ExImageParameters image;
            CHECK(image.fd == -1);
        }
        CHECK(isOpen(bystander));
        close(bystander);
    }

    TEST_CASE("moving out leaves no fd behind, so only the destination closes it") {
        const int fd = makeFd();
        {
            ExImageParameters source;
            source.fd = fd;
            ExImageParameters moved(std::move(source));
            CHECK(source.fd == -1);
            CHECK(moved.fd == fd);
            // both die here; only `moved` may close the fd, and a moved-from
            // source that still held it would close it a second time
        }
        CHECK_FALSE(isOpen(fd));
    }

    TEST_CASE("move-assigning over an image closes the fd it held") {
        const int         replaced = makeFd();
        const int         incoming = makeFd();
        ExImageParameters dest;
        dest.fd = replaced;
        ExImageParameters source;
        source.fd = incoming;

        dest = std::move(source);
        CHECK_FALSE(isOpen(replaced));
        CHECK(isOpen(incoming));
        CHECK(dest.fd == incoming);
        CHECK(source.fd == -1);
    }

    TEST_CASE("move-assigning over an image holding fd 0 closes it") {
        PipeOnStdin       stdin_pipe;
        const int         incoming = makeFd();
        ExImageParameters dest;
        dest.fd = 0;
        ExImageParameters source;
        source.fd = incoming;

        dest = std::move(source);
        CHECK_FALSE(isOpen(0));
        CHECK(dest.fd == incoming);
    }

    TEST_CASE("move-assigning an image onto itself keeps its fd open") {
        const int         fd = makeFd();
        ExImageParameters image;
        image.fd    = fd;
        auto& alias = image; // avoids -Wself-move on a direct self-assignment
        image       = std::move(alias);
        CHECK(isOpen(fd));
        CHECK(image.fd == fd);
    }
}
