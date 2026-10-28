#include <doctest.h>

#include "glExtra.hpp"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

using wallpaper::ExHandle;

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

} // namespace

TEST_SUITE("genExTexture fd ownership") {
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
}
