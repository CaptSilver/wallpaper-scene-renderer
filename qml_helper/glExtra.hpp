#pragma once
#include <memory>
#include <array>
#include <cstdint>

#include <span>
#include "ExSwapchain.hpp"

class QOpenGLContext;
class QOffscreenSurface;

class GlExtra {
public:
    GlExtra();
    ~GlExtra();
    bool init(void* get_proc_address(const char*));
    uint genExTexture(wallpaper::ExHandle&);
    void deleteTexture(uint);

    // GL_EXT_semaphore_fd, needed to take the renderer's semaphores.
    bool hasSemaphoreFd() const;
    // Imports in the current context and takes ownership of `fd` (sets -1).
    // 0 on failure.
    uint importSemaphore(int& fd);
    // Order this context's later use of `tex` after the semaphore's signal.
    // `tex` 0 = no texture to transition (one never imported here).
    void waitSemaphore(uint sem, uint tex);
    // Signal once this context's earlier use of `tex` is done, and flush so the
    // signal is submitted before anything can wait on it.
    void signalSemaphore(uint sem, uint tex);
    void deleteSemaphore(uint sem);

    std::span<const std::uint8_t> uuid() const;
    wallpaper::TexTiling          tiling() const;

private:
    class impl;
    std::unique_ptr<impl> pImpl;

    bool inited { false };

    wallpaper::TexTiling m_tiling { wallpaper::TexTiling::OPTIMAL };

    // Shared GL 4.2+ context for external memory import
    QOpenGLContext*    m_shared_ctx { nullptr };
    QOffscreenSurface* m_surface { nullptr };
    bool               m_use_shared_ctx { false };
    bool               m_is_low_gl { false };
};
