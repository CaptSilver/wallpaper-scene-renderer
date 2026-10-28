#pragma once
#include "ExSwapchain.hpp"

#include <unistd.h>

namespace wallpaper
{

// The fds of an exported image (its memory and its two semaphores) have one
// owner at a time, and -1 means that holder has none.  0 is a real fd.

inline bool HoldsFd(int fd) { return fd >= 0; }

// Closes `fd` if it is held and leaves -1, for a holder giving up an fd that
// nobody took over.
inline void CloseHeldFd(int& fd) {
    if (HoldsFd(fd)) close(fd);
    fd = -1;
}

// Whether GL can import the handle's memory: it needs the fd and its size.
inline bool ExHandleImportable(const ExHandle& handle) {
    return HoldsFd(handle.fd) && handle.size != 0;
}

} // namespace wallpaper
