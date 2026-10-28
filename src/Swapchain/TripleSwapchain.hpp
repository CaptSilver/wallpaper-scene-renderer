#pragma once
#include <atomic>
#include <cstdint>
#include "Core/NoCopyMove.hpp"

namespace wallpaper
{

// What the next render into a TripleSwapchain's in-progress image waits on
// before writing it.
enum class ExInprogressWait
{
    None,            // never presented, never published since: nothing pending
    Release,         // the consumer presented it: wait for its release
    DrainRenderDone, // published and dropped unseen: its render-done is still
                     // signalled and has to be consumed before the next render
                     // signals it again
};

// Three images passed between the render thread (renderFrame) and the
// consumer (eatFrame, the Qt render thread).  Two low bits of the ready
// pointer say how the image in it got there, so each side hands an image over
// and says what state it is in with a single exchange:
//
//   kUnconsumed     the render thread published it and nobody has eaten it.
//                   As a flag written separately from the exchange, an eat
//                   landing between the two took the new frame and left the
//                   flag set, and the next eat swapped back an image two
//                   frames older than the one on screen.
//   kReleasePending the consumer stopped presenting it and has already issued
//                   the release signal for it.
//
// The GPU work on either side is ordered by a binary semaphore pair per image:
// render-done (signalled by the submit that renders the image, waited by the
// consumer once per image eatFrame hands it) and release (signalled by the
// consumer when the image leaves the screen, waited by the next submit that
// renders into it).  inprogressWait() says which of them the next render into
// getInprogress() has to wait on.
//
// The release signal has to be issued before the image can reach the render
// thread, whose wait needs a signal already pending.  eatFrame calls `retire`
// before its exchange for that reason, and the render thread only learns the
// image is free from its own exchange, which orders it after the retire.
template<typename T>
class TripleSwapchain : NoCopy, NoMove {
    static_assert(alignof(T) >= 4, "the two low pointer bits carry the handoff state");

public:
    using InprogressWait = ExInprogressWait;

    virtual ~TripleSwapchain() = default;

    // Returns the newest published image, or nullptr when nothing new arrived
    // since the last eat.  A non-null image must have its render-done waited
    // on before it is sampled.  `retire(outgoing)` runs first, with the image
    // that stops being presented, and must issue that image's release signal.
    template<typename Retire>
    T* eatFrame(Retire&& retire) {
        // Only renderFrame sets kUnconsumed and only this clears it, so a set
        // bit seen here is still set at the exchange.
        if (! (bits(ready().load()) & kUnconsumed)) return nullptr;
        T* outgoing = presented();
        retire(outgoing);
        presented() = untagged(ready().exchange(tagged(outgoing, kReleasePending)));
        return presented();
    }
    void renderFrame() {
        T* previous      = ready().exchange(tagged(inprogress(), kUnconsumed));
        inprogress()     = untagged(previous);
        m_inprogressWait = (bits(previous) & kUnconsumed)       ? InprogressWait::DrainRenderDone
                           : (bits(previous) & kReleasePending) ? InprogressWait::Release
                                                                : InprogressWait::None;
    }
    T*             getInprogress() { return inprogress(); }
    InprogressWait inprogressWait() const { return m_inprogressWait; }

    virtual std::uint32_t width() const  = 0;
    virtual std::uint32_t height() const = 0;

protected:
    TripleSwapchain() = default;

    virtual std::atomic<T*>& presented()  = 0;
    virtual std::atomic<T*>& ready()      = 0;
    virtual std::atomic<T*>& inprogress() = 0;

private:
    static constexpr std::uintptr_t kUnconsumed     = 1;
    static constexpr std::uintptr_t kReleasePending = 2;
    static constexpr std::uintptr_t kTagBits        = kUnconsumed | kReleasePending;

    static T* tagged(T* p, std::uintptr_t tag) {
        return reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(p) | tag);
    }
    static T* untagged(T* p) {
        return reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(p) & ~kTagBits);
    }
    static std::uintptr_t bits(T* p) { return reinterpret_cast<std::uintptr_t>(p) & kTagBits; }

    // Render-thread state only: set by renderFrame, read before the next render.
    InprogressWait m_inprogressWait { InprogressWait::None };
};

} // namespace wallpaper