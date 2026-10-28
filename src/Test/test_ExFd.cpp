#include <doctest.h>

#include "Swapchain/ExFd.hpp"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

using wallpaper::CloseHeldFd;
using wallpaper::ExHandle;
using wallpaper::ExHandleImportable;
using wallpaper::HoldsFd;

namespace
{

int makeFd() {
    int p[2];
    REQUIRE(pipe(p) == 0);
    close(p[1]);
    return p[0];
}

bool isOpen(int fd) { return fcntl(fd, F_GETFD) != -1 || errno != EBADF; }

// Puts a pipe on fd 0 for the duration, restoring stdin afterwards.
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

TEST_SUITE("ExFd") {
    TEST_CASE("any non-negative fd is held, 0 included; -1 is none") {
        CHECK(HoldsFd(0));
        CHECK(HoldsFd(7));
        CHECK_FALSE(HoldsFd(-1));
    }

    TEST_CASE("closing a held fd closes it and leaves -1") {
        int       fd   = makeFd();
        const int copy = fd;
        CloseHeldFd(fd);
        CHECK(fd == -1);
        CHECK_FALSE(isOpen(copy));
    }

    TEST_CASE("closing fd 0 closes it like any other") {
        PipeOnStdin stdin_pipe;
        int         fd = 0;
        CloseHeldFd(fd);
        CHECK(fd == -1);
        CHECK_FALSE(isOpen(0));
    }

    TEST_CASE("closing when no fd is held does nothing") {
        int fd = -1;
        CloseHeldFd(fd);
        CHECK(fd == -1);
    }

    TEST_CASE("an image is importable with an fd and a size") {
        ExHandle h;
        h.fd   = 3;
        h.size = 4096;
        CHECK(ExHandleImportable(h));
        h.fd = 0;
        CHECK(ExHandleImportable(h));
    }

    TEST_CASE("an image without an fd or without a size is not importable") {
        ExHandle no_fd;
        no_fd.fd   = -1;
        no_fd.size = 4096;
        CHECK_FALSE(ExHandleImportable(no_fd));

        ExHandle no_size;
        no_size.fd   = 3;
        no_size.size = 0;
        CHECK_FALSE(ExHandleImportable(no_size));
    }
}
