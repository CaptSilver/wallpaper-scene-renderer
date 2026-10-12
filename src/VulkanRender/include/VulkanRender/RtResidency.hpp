#pragma once
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "Vulkan/RetireRing.hpp"

// The key type the pass analyses use to name a render target independently of
// its VkImage: the slot id for render targets, the Vulkan handle for everything
// else.  It is a tagged struct because Vulkan leaves no free bit in a
// non-dispatchable handle to tell the two kinds apart.  Header-only so the
// doctest suite can exercise it without a Vulkan device.
namespace wallpaper::vulkan
{

// Scene-lifetime identity of a render target; see TextureCache::RtSlot.
using SlotId = std::uint32_t;

// What the pass analyses (cache safety, redraw propagation, render pass
// merging) key on.  A render target is named by its slot, so the key stays the
// same while the slot's VkImage comes and goes; a depth or MSAA image, which is
// never released, is named by its handle.  A tagged struct rather than a bit
// packed into a handle: Vulkan does not promise that non-dispatchable handles
// leave any bit free.
struct ResidencyKey {
    enum class Kind : std::uint8_t
    {
        None,
        Slot,
        Handle,
    };
    Kind          kind { Kind::None };
    std::uint64_t value { 0 };

    static ResidencyKey ForSlot(SlotId id) { return { Kind::Slot, id }; }
    // A null handle is the null key, so an unbound texture slot never matches.
    static ResidencyKey ForHandle(std::uint64_t handle) {
        return handle == 0 ? ResidencyKey {} : ResidencyKey { Kind::Handle, handle };
    }
    bool operator==(const ResidencyKey&) const = default;
};

// VkImage / VkImageView are pointers on 64-bit builds; the tests pass plain
// integers.
template<typename VkH>
ResidencyKey KeyForImage(VkH handle) {
    if constexpr (std::is_pointer_v<VkH>)
        return ResidencyKey::ForHandle(
            static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle)));
    else
        return ResidencyKey::ForHandle(static_cast<std::uint64_t>(handle));
}

// Which of a pass's texture slots it samples this frame: its material's own
// textures, or one of the chain buffers a hidden effect copies through.
enum class DemandInputs : std::uint8_t
{
    None,
    Own,
    PassthroughA,
    PassthroughB,
};

// A pass's part of the demand walk that is fixed at compile: what it writes
// and the slots behind each kind of read.
struct PassDemandStatic {
    ResidencyKey              out;
    bool                      out_managed { false };
    std::vector<ResidencyKey> own_inputs; // the material's own textures
    ResidencyKey              passthrough_a;
    ResidencyKey              passthrough_b;
};

// What changes per frame: whether the pass draws, whether its cached bytes are
// in use, and which of its reads apply.
struct PassDemandFrame {
    bool         draws { false };
    bool         cached { false };
    DemandInputs inputs { DemandInputs::None };
};

struct DemandResult {
    std::vector<bool> needed; // indexed by slot id
    int               iterations { 0 };
};

// Whether a pass's output is wanted this frame: an unmanaged output, or one
// that is not a slot, always is; a managed slot is when the walk marked it.
// A slot id past the vector is not needed.
inline bool PassOutputNeeded(const PassDemandStatic& s, const std::vector<bool>& needed) {
    if (! s.out_managed || s.out.kind != ResidencyKey::Kind::Slot) return true;
    return s.out.value < needed.size() && needed[s.out.value];
}

// Last pass to first: a pass is active when its output is unmanaged or needed,
// and an active pass that draws or is cached needs what it samples.  A reader
// ordered before its writer is only reached after the writer was judged, so
// the walk repeats until nothing changes; demand only grows, so it ends.
// statics and frames are parallel; a size mismatch is a caller bug, and in a
// release build every slot is reported needed rather than guessing which
// passes the missing entries would have kept alive.
inline DemandResult ComputeFrameDemand(std::span<const PassDemandStatic> statics,
                                       std::span<const PassDemandFrame>  frames,
                                       std::size_t                       slot_count) {
    DemandResult r;
    assert(statics.size() == frames.size());
    if (statics.size() != frames.size()) {
        r.needed.assign(slot_count, true);
        return r;
    }
    r.needed.assign(slot_count, false);
    auto mark = [&](const ResidencyKey& k, bool& changed) {
        if (k.kind != ResidencyKey::Kind::Slot || k.value >= slot_count) return;
        if (! r.needed[k.value]) {
            r.needed[k.value] = true;
            changed           = true;
        }
    };
    for (;;) {
        ++r.iterations;
        bool changed = false;
        for (std::size_t i = statics.size(); i-- > 0;) {
            const PassDemandStatic& s = statics[i];
            const PassDemandFrame&  f = frames[i];
            if (! PassOutputNeeded(s, r.needed) || ! (f.draws || f.cached)) continue;
            switch (f.inputs) {
            case DemandInputs::None: break;
            case DemandInputs::Own:
                for (const ResidencyKey& k : s.own_inputs) mark(k, changed);
                break;
            case DemandInputs::PassthroughA: mark(s.passthrough_a, changed); break;
            case DemandInputs::PassthroughB: mark(s.passthrough_b, changed); break;
            }
        }
        if (! changed) break;
    }
    return r;
}

// A fresh image is cleared when something could read it before this frame's
// first write: a reader ordered ahead of the writer, a first writer that loads
// instead of clearing, or no writer at all.
inline bool NeedsInitialClear(bool has_writer, bool first_writer_clears, bool has_backward_reader) {
    return ! has_writer || ! first_writer_clears || has_backward_reader;
}

// Per slot id, whether an image attached to it must be cleared before the pass
// loop runs.  Decided from the graph once at compile: the first writer in pass
// order owns the CLEAR load op, and a reader at or before the last writer's
// position reads the previous frame's contents.
inline std::vector<bool> ComputeClearOnAttach(std::span<const PassDemandStatic> statics,
                                              const std::vector<bool>&          clears_output,
                                              std::size_t                       slot_count) {
    constexpr std::size_t    none = static_cast<std::size_t>(-1);
    std::vector<std::size_t> first_writer(slot_count, none), last_writer(slot_count, none);
    for (std::size_t i = 0; i < statics.size(); ++i) {
        const ResidencyKey& out = statics[i].out;
        if (out.kind != ResidencyKey::Kind::Slot || out.value >= slot_count) continue;
        if (first_writer[out.value] == none) first_writer[out.value] = i;
        last_writer[out.value] = i;
    }
    std::vector<bool> backward(slot_count, false);
    auto              note = [&](const ResidencyKey& k, std::size_t reader) {
        if (k.kind != ResidencyKey::Kind::Slot || k.value >= slot_count) return;
        if (last_writer[k.value] != none && last_writer[k.value] >= reader)
            backward[k.value] = true;
    };
    for (std::size_t i = 0; i < statics.size(); ++i) {
        for (const ResidencyKey& k : statics[i].own_inputs) note(k, i);
        note(statics[i].passthrough_a, i);
        note(statics[i].passthrough_b, i);
    }
    std::vector<bool> out(slot_count);
    for (std::size_t s = 0; s < slot_count; ++s) {
        const bool has_writer = first_writer[s] != none;
        const bool clears =
            has_writer && first_writer[s] < clears_output.size() && clears_output[first_writer[s]];
        out[s] = NeedsInitialClear(has_writer, clears, backward[s]);
    }
    return out;
}

enum class SlotAction : std::uint8_t
{
    None,
    Create,
    Keep,
    Release,
};

// Per managed slot, per frame.  The idle counter is the hysteresis: a script
// toggling a layer every frame never reaches the hold, a layer hidden for the
// evening is released once it does.  `hold` 0 releases everything idle at
// once (used when a create just failed).
inline SlotAction DecideSlotAction(bool resident, bool needed, std::uint32_t& idle_frames,
                                   std::uint32_t hold, bool release_enabled) {
    if (! resident) return needed ? SlotAction::Create : SlotAction::None;
    if (needed) {
        idle_frames = 0;
        return SlotAction::Keep;
    }
    ++idle_frames;
    if (release_enabled && idle_frames >= hold) return SlotAction::Release;
    return SlotAction::None;
}

// About two seconds at 72 Hz; tuned from the Persona 5 measurements.
constexpr std::uint32_t kDefaultHoldFrames = 150;
// 2 keeps "release" from meaning "every frame"; 100000 is about 23 minutes
// at 72 Hz, past which the setting is effectively "never".
constexpr std::uint32_t kMinHoldFrames = 2;
constexpr std::uint32_t kMaxHoldFrames = 100000;

// WEKDE_RT_RELEASE_FRAMES: a whole decimal number is clamped into range (so
// "0" and "1" mean 2, and an absurd value means the cap), and anything that
// is not one falls back to the default.  Clamping rather than rejecting:
// someone typing a value wants it honoured as far as it can be.
inline std::uint32_t ParseHoldFrames(const char* env) {
    // strtoull would skip leading whitespace and accept a sign; require a digit first.
    if (env == nullptr || ! std::isdigit(static_cast<unsigned char>(env[0])))
        return kDefaultHoldFrames;
    char* end                  = nullptr;
    errno                      = 0;
    const unsigned long long v = std::strtoull(env, &end, 10);
    if (end == env || *end != '\0') return kDefaultHoldFrames;
    if (errno == ERANGE) return kMaxHoldFrames;
    return static_cast<std::uint32_t>(
        std::clamp<unsigned long long>(v, kMinHoldFrames, kMaxHoldFrames));
}

// Out-of-memory backoff: a minute's worth of frames per failure, at most an
// hour between attempts.
inline std::uint64_t RetryAfter(std::uint64_t frame, std::uint32_t streak) {
    return frame + std::min<std::uint64_t>(60ull * streak, 3600ull);
}
inline bool CanRetryCreate(std::uint64_t frame, std::uint64_t retry_after) {
    return frame >= retry_after;
}

// GPU microseconds between two vkCmdWriteTimestamp queries.  The counter is
// only `valid_bits` wide, so the difference is taken in that width (a wrap
// between the two writes then cancels), then scaled by the device's tick.
inline double TimestampDeltaUs(std::uint64_t begin, std::uint64_t end, std::uint32_t valid_bits,
                               float period_ns) {
    if (valid_bits == 0) return 0.0;
    const std::uint64_t mask  = valid_bits >= 64 ? ~0ull : ((1ull << valid_bits) - 1ull);
    const std::uint64_t ticks = (end - begin) & mask;
    return static_cast<double>(ticks) * static_cast<double>(period_ns) / 1000.0;
}

// Which drawing passes would sample a slot that has no image (a create
// failed).  Such a pass is treated as hidden for the frame.  Only the pass
// that samples the absent slot is blocked, not the readers of its output: those
// see stale or cleared bytes (never undefined ones) until the retry succeeds.
inline std::vector<bool> ComputeBlockedPasses(std::span<const PassDemandStatic> statics,
                                              std::span<const PassDemandFrame>  frames,
                                              const std::vector<bool>&          resident) {
    const std::size_t n = std::min(statics.size(), frames.size());
    std::vector<bool> blocked(n, false);
    auto              absent = [&](const ResidencyKey& k) {
        return k.kind == ResidencyKey::Kind::Slot &&
               (k.value >= resident.size() || ! resident[k.value]);
    };
    for (std::size_t i = 0; i < n; ++i) {
        const PassDemandStatic& s = statics[i];
        const PassDemandFrame&  f = frames[i];
        if (! f.draws) continue;
        switch (f.inputs) {
        case DemandInputs::None: break;
        case DemandInputs::Own:
            for (const ResidencyKey& k : s.own_inputs) blocked[i] = blocked[i] || absent(k);
            break;
        case DemandInputs::PassthroughA: blocked[i] = absent(s.passthrough_a); break;
        case DemandInputs::PassthroughB: blocked[i] = absent(s.passthrough_b); break;
        }
    }
    return blocked;
}

} // namespace wallpaper::vulkan

template<>
struct std::hash<wallpaper::vulkan::ResidencyKey> {
    std::size_t operator()(const wallpaper::vulkan::ResidencyKey& k) const noexcept {
        return std::hash<std::uint64_t> {}(k.value) ^
               (static_cast<std::size_t>(k.kind) << (sizeof(std::size_t) * 8 - 3));
    }
};
