#pragma once
#include <atomic>
#include <cstdint>
#include "Core/NoCopyMove.hpp"

namespace wallpaper
{

// Three images passed between the render thread (renderFrame) and the
// consumer (eatFrame, the Qt render thread).  "ready holds a frame nobody has
// eaten yet" lives in the low bit of the ready pointer itself, so publishing
// a frame and marking it new is one exchange.  As two separate writes, an eat
// landing between them took the new frame and left the flag set, and the next
// eat then swapped back an image two frames older than the one on screen.
template<typename T>
class TripleSwapchain : NoCopy, NoMove {
    static_assert(alignof(T) >= 2, "the low pointer bit carries the unconsumed flag");

public:
    virtual ~TripleSwapchain() = default;

    T* eatFrame() {
        // Only renderFrame sets the bit and only this clears it, so a set bit
        // seen here is still set at the exchange.
        if (! isUnconsumed(ready().load())) return nullptr;
        presented() = untagged(ready().exchange(presented()));
        return presented();
    }
    void renderFrame() { inprogress() = untagged(ready().exchange(tagged(inprogress()))); }
    T*   getInprogress() { return inprogress(); }

    virtual std::uint32_t width() const  = 0;
    virtual std::uint32_t height() const = 0;

protected:
    TripleSwapchain() = default;

    virtual std::atomic<T*>& presented()  = 0;
    virtual std::atomic<T*>& ready()      = 0;
    virtual std::atomic<T*>& inprogress() = 0;

private:
    static constexpr std::uintptr_t kUnconsumed = 1;

    static T* tagged(T* p) {
        return reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(p) | kUnconsumed);
    }
    static T* untagged(T* p) {
        return reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(p) & ~kUnconsumed);
    }
    static bool isUnconsumed(T* p) { return reinterpret_cast<std::uintptr_t>(p) & kUnconsumed; }
};

} // namespace wallpaper