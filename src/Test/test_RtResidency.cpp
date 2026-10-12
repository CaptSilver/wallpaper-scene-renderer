#include <doctest.h>

#include "VulkanRender/RtResidency.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>

using namespace wallpaper::vulkan;

// Keys for the pass analyses: a render target is named by its slot id, a depth
// or MSAA image by its VkImage handle.  The two kinds must never compare equal,
// and the default key must be the null every PassCacheAlias / PassMerge
// template compares against.
TEST_SUITE("RtResidency::ResidencyKey") {
    TEST_CASE("the default key is null and compares equal to another default key") {
        ResidencyKey a, b;
        CHECK(a == b);
        CHECK(a.kind == ResidencyKey::Kind::None);
        CHECK(a == ResidencyKey {});
    }
    TEST_CASE("a slot key and a handle key with the same value never compare equal") {
        CHECK_FALSE(ResidencyKey::ForSlot(7) == ResidencyKey::ForHandle(7));
        CHECK_FALSE(ResidencyKey::ForSlot(0) == ResidencyKey {});
        CHECK(ResidencyKey::ForSlot(7) == ResidencyKey::ForSlot(7));
        CHECK_FALSE(ResidencyKey::ForSlot(7) == ResidencyKey::ForSlot(8));
    }
    TEST_CASE("a null handle gives the null key, so unbound slots stay invisible to the analyses") {
        CHECK(ResidencyKey::ForHandle(0) == ResidencyKey {});
        CHECK(KeyForImage(static_cast<std::uintptr_t>(0)) == ResidencyKey {});
        CHECK(KeyForImage(static_cast<std::uintptr_t>(0x1234)) == ResidencyKey::ForHandle(0x1234));
    }
    TEST_CASE("a pointer handle maps through its address and a null pointer is the null key") {
        CHECK(KeyForImage(static_cast<int*>(nullptr)) == ResidencyKey {});
        CHECK(KeyForImage(reinterpret_cast<int*>(std::uintptr_t { 0x1234 })) ==
              ResidencyKey::ForHandle(0x1234));
    }
    TEST_CASE("keys hash into an unordered_set without collapsing kinds") {
        std::unordered_set<ResidencyKey> set;
        set.insert(ResidencyKey::ForSlot(1));
        set.insert(ResidencyKey::ForHandle(1));
        set.insert(ResidencyKey::ForSlot(1));
        CHECK(set.size() == 2);
        CHECK(set.count(ResidencyKey::ForSlot(1)) == 1);
        CHECK(set.count(ResidencyKey::ForHandle(1)) == 1);
        CHECK(set.count(ResidencyKey {}) == 0);
    }
}

// Demand: walking the passes last to first, a pass is active when its output is
// unmanaged or needed, and an active pass that draws (or is cached) needs the
// slots it samples.  Slot ids index the result.
TEST_SUITE("RtResidency::ComputeFrameDemand") {
    using K = ResidencyKey;
    static PassDemandStatic writes(SlotId out, std::vector<K> own = {}) {
        PassDemandStatic s;
        s.out         = K::ForSlot(out);
        s.out_managed = true;
        s.own_inputs  = std::move(own);
        return s;
    }
    static PassDemandStatic composites(std::vector<K> own) {
        PassDemandStatic s; // writes _rt_default: unmanaged, always active
        s.out         = K::ForHandle(0xDEF);
        s.out_managed = false;
        s.own_inputs  = std::move(own);
        return s;
    }
    static PassDemandFrame draws() { return { true, false, DemandInputs::Own }; }
    static PassDemandFrame skipped() { return { false, false, DemandInputs::None }; }

    TEST_CASE("a hidden layer needs nothing, its clear owner included") {
        // base (clears A), effect (A -> B), terminal (B -> default): all hidden.
        // The base owns A's clear, so it records without drawing (DecideHideable:
        // records, draws=false, inputs None); the others drop their record.
        std::vector<PassDemandStatic> s { writes(0),
                                          writes(1, { K::ForSlot(0) }),
                                          composites({ K::ForSlot(1) }) };
        std::vector<PassDemandFrame>  f { { false, false, DemandInputs::None },
                                          skipped(),
                                          skipped() };
        auto                          r = ComputeFrameDemand(s, f, 2);
        CHECK(r.needed == std::vector<bool> { false, false });
    }
    TEST_CASE("a visible layer whose effects are all hidden needs only its first ping-pong") {
        std::vector<PassDemandStatic> s { writes(0),
                                          writes(1, { K::ForSlot(0) }),
                                          composites({ K::ForSlot(1) }) };
        auto                          terminal = s[2];
        terminal.passthrough_a                 = K::ForSlot(0);
        terminal.passthrough_b                 = K::ForSlot(1);
        s[2]                                   = terminal;
        std::vector<PassDemandFrame> f { draws(),
                                         skipped(),
                                         { true, false, DemandInputs::PassthroughA } };
        auto                         r = ComputeFrameDemand(s, f, 2);
        CHECK(r.needed == std::vector<bool> { true, false });
    }
    TEST_CASE(
        "an offscreen producer that draws keeps its whole chain, whether the layer was hidden "
        "at load or not") {
        // A layer hidden in the scene file but routed offscreen keeps its node
        // visible (the parser leaves m_visible set for it), so its passes draw
        // and every slot of the chain is needed while a visible layer reads it.
        // producer base (A), producer terminal (A -> offscreen 2), link copy (2 -> 3),
        // compose reads 3 into default.  Slot 1 is unused.
        std::vector<PassDemandStatic> s { writes(0),
                                          writes(2, { K::ForSlot(0) }),
                                          writes(3, { K::ForSlot(2) }),
                                          composites({ K::ForSlot(3) }) };
        std::vector<PassDemandFrame>  f { draws(), draws(), draws(), draws() };
        auto                          r = ComputeFrameDemand(s, f, 4);
        CHECK(r.needed == std::vector<bool> { true, false, true, true });
    }
    TEST_CASE("an offscreen producer whose passes are skipped at runtime loses its chain, and a "
              "hidden compose idles the link copy") {
        std::vector<PassDemandStatic> s { writes(0),
                                          writes(2, { K::ForSlot(0) }),
                                          writes(3, { K::ForSlot(2) }),
                                          composites({ K::ForSlot(3) }) };
        // producer runtime-hidden (its passes drop their record): the compose
        // still samples 3 and the copy still fills 3 from 2, so both stay;
        // nothing draws into 2 this frame, so the ping-pong 0 that fed it goes idle
        std::vector<PassDemandFrame> f { skipped(), skipped(), draws(), draws() };
        auto                         r = ComputeFrameDemand(s, f, 4);
        CHECK(r.needed == std::vector<bool> { false, false, true, true });
        // compose hidden: the copy and the whole producer chain go idle
        f = { draws(), draws(), draws(), skipped() };
        r = ComputeFrameDemand(s, f, 4);
        CHECK(r.needed == std::vector<bool> { false, false, false, false });
    }
    TEST_CASE("a backward read settles within three walks") {
        // reader of slot 0 ordered before its writer, which itself samples slot 1
        std::vector<PassDemandStatic> s { composites({ K::ForSlot(0) }),
                                          writes(1),
                                          writes(0, { K::ForSlot(1) }) };
        std::vector<PassDemandFrame>  f { draws(), draws(), draws() };
        auto                          r = ComputeFrameDemand(s, f, 2);
        CHECK(r.needed == std::vector<bool> { true, true });
        // walk 1 finds the reader's slot, walk 2 the writer's input, walk 3 sees no change
        CHECK(r.iterations == 3);
    }
    TEST_CASE("a slot aliased between a hidden and a visible layer stays needed") {
        // slot 0 written by both layers' base passes; only the second layer's reader draws
        std::vector<PassDemandStatic> s {
            writes(0), composites({ K::ForSlot(0) }), writes(0), composites({ K::ForSlot(0) })
        };
        std::vector<PassDemandFrame> f { skipped(), skipped(), draws(), draws() };
        auto                         r = ComputeFrameDemand(s, f, 1);
        CHECK(r.needed == std::vector<bool> { true });
    }
    TEST_CASE("merged-group members share the output, so they agree") {
        std::vector<PassDemandStatic> s { writes(0, { K::ForSlot(1) }),
                                          writes(0, { K::ForSlot(2) }),
                                          writes(0, { K::ForSlot(3) }),
                                          composites({ K::ForSlot(0) }) };
        std::vector<PassDemandFrame>  f { draws(), draws(), draws(), draws() };
        auto                          r = ComputeFrameDemand(s, f, 4);
        CHECK(r.needed == std::vector<bool> { true, true, true, true });
        f[3] = skipped();
        r    = ComputeFrameDemand(s, f, 4);
        CHECK(r.needed == std::vector<bool> { false, false, false, false });
    }
    TEST_CASE("a cached pass keeps the slots its bytes depend on") {
        std::vector<PassDemandStatic> s { writes(0),
                                          writes(1, { K::ForSlot(0) }),
                                          composites({ K::ForSlot(1) }) };
        std::vector<PassDemandFrame>  f { draws(), { false, true, DemandInputs::Own }, draws() };
        auto                          r = ComputeFrameDemand(s, f, 2);
        CHECK(r.needed == std::vector<bool> { true, true });
    }
    TEST_CASE("handle keys and the null key are ignored, and slot ids past the count are too") {
        std::vector<PassDemandStatic> s { composites({ K::ForHandle(0x77), K {}, K::ForSlot(9) }) };
        std::vector<PassDemandFrame>  f { draws() };
        auto                          r = ComputeFrameDemand(s, f, 2);
        CHECK(r.needed == std::vector<bool> { false, false });
    }
    TEST_CASE("an unprepared pass contributes nothing") {
        std::vector<PassDemandStatic> s { writes(0), PassDemandStatic {} };
        std::vector<PassDemandFrame>  f { draws(), PassDemandFrame {} };
        auto                          r = ComputeFrameDemand(s, f, 1);
        CHECK(r.needed == std::vector<bool> { false });
    }
    TEST_CASE("a managed output past the slot count is inactive, not a read past the end") {
        std::vector<PassDemandStatic> s { writes(5, { K::ForSlot(0) }) };
        std::vector<PassDemandFrame>  f { draws() };
        auto                          r = ComputeFrameDemand(s, f, 1);
        CHECK(r.needed == std::vector<bool> { false });
    }
}

// A fresh image is cleared only when something could read it before this
// frame's first write: a reader ordered ahead of the writer, a first writer
// that loads instead of clearing, or no writer at all.
TEST_SUITE("RtResidency::ComputeClearOnAttach") {
    using K = ResidencyKey;
    static PassDemandStatic pass(K out, std::vector<K> own = {}) {
        PassDemandStatic s;
        s.out         = out;
        s.out_managed = out.kind == K::Kind::Slot;
        s.own_inputs  = std::move(own);
        return s;
    }
    TEST_CASE("the rule itself") {
        CHECK_FALSE(NeedsInitialClear(true, true, false));
        CHECK(NeedsInitialClear(true, false, false));
        CHECK(NeedsInitialClear(true, true, true));
        CHECK(NeedsInitialClear(false, true, false));
    }
    TEST_CASE("a slot whose first writer clears and whose readers follow needs no clear") {
        std::vector<PassDemandStatic> s { pass(K::ForSlot(0)),
                                          pass(K::ForHandle(1), { K::ForSlot(0) }) };
        std::vector<bool>             clears { true, false };
        CHECK(ComputeClearOnAttach(s, clears, 1) == std::vector<bool> { false });
    }
    TEST_CASE("a reader ordered before the writer forces a clear") {
        std::vector<PassDemandStatic> s { pass(K::ForHandle(1), { K::ForSlot(0) }),
                                          pass(K::ForSlot(0)) };
        std::vector<bool>             clears { false, true };
        CHECK(ComputeClearOnAttach(s, clears, 1) == std::vector<bool> { true });
    }
    TEST_CASE("a first writer that loads forces a clear") {
        std::vector<PassDemandStatic> s { pass(K::ForSlot(0)), pass(K::ForSlot(0)) };
        std::vector<bool>             clears { false, false };
        CHECK(ComputeClearOnAttach(s, clears, 1) == std::vector<bool> { true });
    }
    TEST_CASE("a slot nobody writes is cleared, a slot nobody reads or writes too") {
        std::vector<PassDemandStatic> s { pass(K::ForHandle(1), { K::ForSlot(0) }) };
        std::vector<bool>             clears { false };
        CHECK(ComputeClearOnAttach(s, clears, 2) == std::vector<bool> { true, true });
    }
    TEST_CASE("passthrough reads count as reads") {
        PassDemandStatic terminal = pass(K::ForHandle(1));
        terminal.passthrough_b    = K::ForSlot(0);
        std::vector<PassDemandStatic> s { terminal, pass(K::ForSlot(0)) };
        std::vector<bool>             clears { false, true };
        CHECK(ComputeClearOnAttach(s, clears, 1) == std::vector<bool> { true });
    }
    TEST_CASE("an unprepared pass is not a writer: the first prepared writer decides, and if it "
              "loads the slot is cleared") {
        // buildResidencyTables leaves an unprepared pass as a default entry
        // (null out key).  Its clears_output of true must not count.
        std::vector<PassDemandStatic> s { PassDemandStatic {},
                                          pass(K::ForSlot(0)),
                                          pass(K::ForHandle(1), { K::ForSlot(0) }) };
        std::vector<bool>             clears { true, false, false };
        CHECK(ComputeClearOnAttach(s, clears, 1) == std::vector<bool> { true });
        clears[1] = true;
        CHECK(ComputeClearOnAttach(s, clears, 1) == std::vector<bool> { false });
    }
}

// Whether a pass's output is wanted this frame: the same test the demand walk
// uses to decide a pass is active, exposed so the pass can be told.
TEST_SUITE("RtResidency::PassOutputNeeded") {
    using K = ResidencyKey;
    TEST_CASE("an unmanaged output is always needed") {
        PassDemandStatic s;
        s.out         = K::ForSlot(0);
        s.out_managed = false;
        CHECK(PassOutputNeeded(s, std::vector<bool> { false }));
    }
    TEST_CASE("a pass with no slot output is always needed") {
        PassDemandStatic s;
        s.out         = K::ForHandle(5);
        s.out_managed = true;
        CHECK(PassOutputNeeded(s, std::vector<bool> {}));
    }
    TEST_CASE("a managed output follows the demand vector") {
        PassDemandStatic s;
        s.out         = K::ForSlot(1);
        s.out_managed = true;
        CHECK(PassOutputNeeded(s, std::vector<bool> { false, true }));
        CHECK_FALSE(PassOutputNeeded(s, std::vector<bool> { true, false }));
    }
    TEST_CASE("a managed slot id past the demand vector is not needed") {
        PassDemandStatic s;
        s.out         = K::ForSlot(4);
        s.out_managed = true;
        CHECK_FALSE(PassOutputNeeded(s, std::vector<bool> { true, true }));
    }
    TEST_CASE("the demand walk agrees: an inactive reader marks nothing") {
        PassDemandStatic w;
        w.out         = K::ForSlot(0);
        w.out_managed = true;
        w.own_inputs  = { K::ForSlot(1) };
        std::vector<PassDemandStatic> s { w };
        std::vector<PassDemandFrame>  f { { true, false, DemandInputs::Own } };
        auto                          r = ComputeFrameDemand(s, f, 2);
        CHECK_FALSE(PassOutputNeeded(s[0], r.needed));
        CHECK(r.needed == std::vector<bool> { false, false });
    }
}

TEST_SUITE("RtResidency::DecideSlotAction") {
    TEST_CASE("absent and needed creates; absent and idle does nothing") {
        std::uint32_t idle = 0;
        CHECK(DecideSlotAction(false, true, idle, 150, true) == SlotAction::Create);
        CHECK(DecideSlotAction(false, false, idle, 150, true) == SlotAction::None);
        CHECK(idle == 0);
    }
    TEST_CASE("a slot toggled every frame never releases") {
        std::uint32_t idle = 0;
        for (int f = 0; f < 1000; ++f) {
            const bool needed = f % 2 == 0;
            auto       a      = DecideSlotAction(true, needed, idle, 150, true);
            CHECK(a != SlotAction::Release);
            CHECK(idle <= 1);
        }
    }
    TEST_CASE("idle for hold - 1 frames then needed restarts the count") {
        std::uint32_t idle = 0;
        for (int f = 0; f < 149; ++f)
            CHECK(DecideSlotAction(true, false, idle, 150, true) == SlotAction::None);
        CHECK(idle == 149);
        CHECK(DecideSlotAction(true, true, idle, 150, true) == SlotAction::Keep);
        CHECK(idle == 0);
    }
    TEST_CASE("release happens at exactly the hold") {
        std::uint32_t idle   = 0;
        SlotAction    last   = SlotAction::None;
        int           frames = 0;
        while (last != SlotAction::Release) {
            last = DecideSlotAction(true, false, idle, 150, true);
            ++frames;
        }
        CHECK(frames == 150);
    }
    TEST_CASE("with release disabled an idle slot is kept forever") {
        std::uint32_t idle = 0;
        for (int f = 0; f < 5000; ++f)
            CHECK(DecideSlotAction(true, false, idle, 150, false) == SlotAction::None);
    }
    TEST_CASE(
        "hold zero releases every unneeded resident slot at once (the out-of-memory collapse)") {
        std::uint32_t idle = 0;
        CHECK(DecideSlotAction(true, false, idle, 0, true) == SlotAction::Release);
    }
    TEST_CASE(
        "released at R, needed at R+1: a new image is created, the old one is still retiring") {
        std::uint32_t idle = 149;
        CHECK(DecideSlotAction(true, false, idle, 150, true) == SlotAction::Release);
        // the caller moved the image out; the slot is now absent.  Resetting idle_frames on
        // attach is the caller's job (detail::attachImage), not this function's.
        CHECK(DecideSlotAction(false, true, idle, 150, true) == SlotAction::Create);
    }
}

TEST_SUITE("RtResidency::hold window and backoff") {
    TEST_CASE("WEKDE_RT_RELEASE_FRAMES: a number is clamped to [2, 100000], anything else is 150") {
        CHECK(ParseHoldFrames(nullptr) == 150);
        CHECK(ParseHoldFrames("") == 150);
        CHECK(ParseHoldFrames("abc") == 150);
        CHECK(ParseHoldFrames("12x") == 150);
        CHECK(ParseHoldFrames("-5") == 150);
        CHECK(ParseHoldFrames(" -5") == 150);
        CHECK(ParseHoldFrames("+5") == 150);
        CHECK(ParseHoldFrames(" 5") == 150);
        CHECK(ParseHoldFrames("\t7") == 150);
        CHECK(ParseHoldFrames("0") == 2); // never "release every frame": 2 is the floor
        CHECK(ParseHoldFrames("1") == 2);
        CHECK(ParseHoldFrames("2") == 2);
        CHECK(ParseHoldFrames("600") == 600);
        CHECK(ParseHoldFrames("100000") == 100000);
        CHECK(ParseHoldFrames("1000000000") == 100000);
        CHECK(ParseHoldFrames("99999999999999999999") == 100000); // overflow still lands on the cap
        CHECK(kDefaultHoldFrames == 150);
    }
    TEST_CASE("the retry frame grows with the streak and caps at 3600 frames out") {
        CHECK(RetryAfter(1000, 1) == 1060);
        CHECK(RetryAfter(1000, 2) == 1120);
        CHECK(RetryAfter(1000, 60) == 4600);
        CHECK(RetryAfter(1000, 61) == 4600);
        CHECK(RetryAfter(1000, 100000) == 4600);
        CHECK_FALSE(CanRetryCreate(1059, 1060));
        CHECK(CanRetryCreate(1060, 1060));
        CHECK(CanRetryCreate(0, 0));
    }
}

TEST_SUITE("RtResidency::RetireRing") {
    TEST_CASE("an entry released at R is destroyed at R+2 with two frames in flight, not before") {
        RetireRing<std::string> ring;
        ring.push("img", 10);
        CHECK(ring.collect(10, 2).empty());
        CHECK(ring.collect(11, 2).empty());
        auto out = ring.collect(12, 2);
        REQUIRE(out.size() == 1);
        CHECK(out[0] == "img");
        CHECK(ring.size() == 0);
    }
    TEST_CASE("collecting the same frame twice (an abandoned frame) destroys nothing new and "
              "nothing early") {
        RetireRing<std::string> ring;
        ring.push("a", 10);
        ring.push("b", 11);
        CHECK(ring.collect(11, 2).empty());
        CHECK(ring.collect(11, 2).empty());
        auto out = ring.collect(12, 2);
        REQUIRE(out.size() == 1);
        CHECK(out[0] == "a");
        CHECK(ring.collect(12, 2).empty());
        CHECK(ring.collect(13, 2).size() == 1);
    }
    TEST_CASE("drain returns everything regardless of stamp") {
        RetireRing<std::string> ring;
        ring.push("a", 10);
        ring.push("b", 10000);
        auto out = ring.drain();
        CHECK(out.size() == 2);
        CHECK(ring.size() == 0);
    }
}

// When a create failed, the passes that would sample the absent slot are
// treated as hidden for the frame: nothing draws from a slot with no image,
// and the opaque 1x1 fallback is never drawn in a buffer's place.
TEST_SUITE("RtResidency::ComputeBlockedPasses") {
    using K = ResidencyKey;
    static PassDemandStatic reader(std::vector<K> own, K a = {}, K b = {}) {
        PassDemandStatic s;
        s.out           = K::ForHandle(0xDEF);
        s.own_inputs    = std::move(own);
        s.passthrough_a = a;
        s.passthrough_b = b;
        return s;
    }
    TEST_CASE("a drawing pass with an absent own input is blocked; a resident one is not") {
        std::vector<PassDemandStatic> s { reader({ K::ForSlot(0) }), reader({ K::ForSlot(1) }) };
        std::vector<PassDemandFrame>  f { { true, false, DemandInputs::Own },
                                          { true, false, DemandInputs::Own } };
        std::vector<bool>             resident { false, true };
        CHECK(ComputeBlockedPasses(s, f, resident) == std::vector<bool> { true, false });
    }
    TEST_CASE("every pass is judged, not just the first") {
        // Only the second pass samples the absent slot.
        std::vector<PassDemandStatic> s { reader({ K::ForSlot(0) }), reader({ K::ForSlot(1) }) };
        std::vector<PassDemandFrame>  f { { true, false, DemandInputs::Own },
                                          { true, false, DemandInputs::Own } };
        std::vector<bool>             resident { true, false };
        CHECK(ComputeBlockedPasses(s, f, resident) == std::vector<bool> { false, true });
    }
    TEST_CASE("only the buffer actually sampled counts for a passthrough") {
        std::vector<PassDemandStatic> s { reader({}, K::ForSlot(0), K::ForSlot(1)) };
        std::vector<bool>             resident { true, false };
        std::vector<PassDemandFrame>  a { { true, false, DemandInputs::PassthroughA } };
        std::vector<PassDemandFrame>  b { { true, false, DemandInputs::PassthroughB } };
        CHECK(ComputeBlockedPasses(s, a, resident) == std::vector<bool> { false });
        CHECK(ComputeBlockedPasses(s, b, resident) == std::vector<bool> { true });
    }
    TEST_CASE("a pass that does not draw is never blocked, and handle keys never block") {
        std::vector<PassDemandStatic> s { reader({ K::ForSlot(0), K::ForHandle(0x9) }) };
        std::vector<PassDemandFrame>  f { { false, false, DemandInputs::None } };
        std::vector<bool>             resident { false };
        CHECK(ComputeBlockedPasses(s, f, resident) == std::vector<bool> { false });
        f[0]        = { true, false, DemandInputs::Own };
        resident[0] = true;
        CHECK(ComputeBlockedPasses(s, f, resident) == std::vector<bool> { false });
    }
}

TEST_SUITE("RtResidency::TimestampDeltaUs") {
    TEST_CASE("a 64-bit counter at 1 ns per tick reads straight through") {
        CHECK(TimestampDeltaUs(1000, 1000 + 2500, 64, 1.0f) == doctest::Approx(2.5));
    }
    TEST_CASE("the device tick period scales the result") {
        CHECK(TimestampDeltaUs(0, 100, 64, 40.0f) == doctest::Approx(4.0)); // 100 ticks x 40 ns
    }
    TEST_CASE("a narrower counter that wrapped between the two reads still gives the right delta") {
        const std::uint64_t top = (1ull << 36) - 10;
        CHECK(TimestampDeltaUs(top, 5, 36, 1000.0f) == doctest::Approx(15.0)); // 15 ticks x 1 us
    }
    TEST_CASE("a queue without timestamps reports zero") {
        CHECK(TimestampDeltaUs(0, 1000, 0, 1.0f) == 0.0);
    }
}

// A slot id equal to the table size is one past the last entry.  The guards are
// checked directly (IndexInRange) so the boundary needs no out-of-range read,
// and once more through each caller with ordinary tables.
TEST_SUITE("RtResidency::slot ids at the table size") {
    using K = ResidencyKey;
    static std::vector<bool> tableOf(std::size_t n) { return std::vector<bool>(n, true); }
    TEST_CASE("IndexInRange accepts up to the last entry and nothing past it") {
        CHECK(IndexInRange(0, 3));
        CHECK(IndexInRange(2, 3));
        CHECK_FALSE(IndexInRange(3, 3));
        CHECK_FALSE(IndexInRange(4, 3));
        CHECK_FALSE(IndexInRange(0, 0));
        CHECK_FALSE(IndexInRange(std::numeric_limits<std::uint64_t>::max(), 3));
    }
    TEST_CASE("SignedIndexInRange also refuses negative indices") {
        CHECK(SignedIndexInRange(0, 3));
        CHECK(SignedIndexInRange(2, 3));
        CHECK_FALSE(SignedIndexInRange(3, 3));
        CHECK_FALSE(SignedIndexInRange(-1, 3));
        CHECK_FALSE(SignedIndexInRange(-1, 0));
    }
    static PassDemandStatic pass(K out, bool managed, std::vector<K> own = {}) {
        PassDemandStatic s;
        s.out         = out;
        s.out_managed = managed;
        s.own_inputs  = std::move(own);
        return s;
    }

    TEST_CASE("PassOutputNeeded: the slot after the last entry is not needed") {
        const auto s = pass(K::ForSlot(2), true);
        CHECK_FALSE(PassOutputNeeded(s, tableOf(2)));
        CHECK(PassOutputNeeded(s, tableOf(3)));
    }
    TEST_CASE("ComputeFrameDemand: a read of the slot after the last one is ignored") {
        std::vector<PassDemandStatic> s { pass(K::ForHandle(0xDEF), false, { K::ForSlot(2) }) };
        std::vector<PassDemandFrame>  f { { true, false, DemandInputs::Own } };
        auto                          r = ComputeFrameDemand(s, f, 2);
        CHECK(r.needed == std::vector<bool> { false, false });
        CHECK(r.iterations == 1); // nothing was marked, so the first walk is the last
    }
    TEST_CASE("ComputeClearOnAttach: a writer or reader of the slot after the last is ignored") {
        std::vector<PassDemandStatic> s { pass(K::ForSlot(1), true),
                                          pass(K::ForHandle(0xDEF), false, { K::ForSlot(1) }) };
        std::vector<bool>             clears { true, false };
        // Only slot 0 exists; it has no writer, so it alone is cleared.
        CHECK(ComputeClearOnAttach(s, clears, 1) == std::vector<bool> { true });
    }
    TEST_CASE(
        "ComputeClearOnAttach: a first writer past the end of the clear list does not clear") {
        std::vector<PassDemandStatic> s { pass(K::ForHandle(0xDEF), false),
                                          pass(K::ForSlot(0), true) };
        // The writer is pass 1 but the list has one entry: its clear flag is unknown, so the
        // slot is cleared on attach.
        CHECK(ComputeClearOnAttach(s, tableOf(1), 1) == std::vector<bool> { true });
    }
    TEST_CASE("ComputeClearOnAttach: a pass that reads the slot it writes reads last frame") {
        // The reader sits at the last writer's own position: it samples what the slot held
        // before this frame's write, so the attach clear is needed even though the writer
        // clears its own output.
        std::vector<PassDemandStatic> self { pass(K::ForSlot(0), true, { K::ForSlot(0) }) };
        std::vector<bool>             clears { true };
        CHECK(ComputeClearOnAttach(self, clears, 1) == std::vector<bool> { true });
        std::vector<PassDemandStatic> plain { pass(K::ForSlot(0), true) };
        CHECK(ComputeClearOnAttach(plain, clears, 1) == std::vector<bool> { false });
    }
    TEST_CASE("ComputeBlockedPasses: a slot after the last resident entry is absent") {
        std::vector<PassDemandStatic> s { pass(K::ForHandle(0xDEF), false, { K::ForSlot(2) }) };
        std::vector<PassDemandFrame>  f { { true, false, DemandInputs::Own } };
        CHECK(ComputeBlockedPasses(s, f, tableOf(2)) == std::vector<bool> { true });
        CHECK(ComputeBlockedPasses(s, f, tableOf(3)) == std::vector<bool> { false });
    }
}

// A failed create bumps the slot's streak and schedules the retry; only the
// first failure of a streak earns a log line.
TEST_SUITE("RtResidency::NoteCreateFailureFor") {
    TEST_CASE("the first failure starts a streak of one and backs off a minute") {
        const auto v = NoteCreateFailureFor(0, 1000);
        CHECK(v.streak == 1);
        CHECK(v.retry_after == 1060);
        CHECK(v.first_of_streak);
    }
    TEST_CASE("later failures lengthen the backoff and stay quiet") {
        const auto v = NoteCreateFailureFor(2, 1000);
        CHECK(v.streak == 3);
        CHECK(v.retry_after == 1000 + 180);
        CHECK_FALSE(v.first_of_streak);
    }
    TEST_CASE("the backoff caps at an hour and the streak saturates instead of wrapping") {
        const auto v = NoteCreateFailureFor(1000, 5);
        CHECK(v.retry_after == 5 + 3600);
        const auto top = NoteCreateFailureFor(std::numeric_limits<std::uint32_t>::max(), 0);
        CHECK(top.streak == std::numeric_limits<std::uint32_t>::max());
        CHECK_FALSE(top.first_of_streak);
    }
}

// What the DIAG line reports for the frames since the previous one.
TEST_SUITE("RtResidency::ResidencyWindow") {
    TEST_CASE("counts add up and the step cost keeps a sum and a maximum") {
        ResidencyWindow w;
        w.add(1, 0, 0, 40);
        w.add(0, 2, 1, 900);
        w.add(0, 0, 0, 60);
        CHECK(w.created == 1);
        CHECK(w.released == 2);
        CHECK(w.failed == 1);
        CHECK(w.steps == 3);
        CHECK(w.step_us_sum == 1000);
        CHECK(w.step_us_max == 900);
    }
    TEST_CASE("a fresh window is empty, so resetting is assigning one") {
        ResidencyWindow w;
        w.add(5, 5, 5, 5);
        w = ResidencyWindow {};
        CHECK(w.created == 0);
        CHECK(w.steps == 0);
        CHECK(w.step_us_sum == 0);
        CHECK(w.step_us_max == 0);
    }
}

// The per-frame walk reuses caller-owned buffers, so an idle frame allocates
// nothing; the result must match the allocating form.
TEST_SUITE("RtResidency::walk buffer reuse") {
    using K = ResidencyKey;
    static PassDemandStatic writer(SlotId out, std::vector<K> own = {}) {
        PassDemandStatic s;
        s.out         = K::ForSlot(out);
        s.out_managed = true;
        s.own_inputs  = std::move(own);
        return s;
    }
    TEST_CASE("ComputeFrameDemandInto matches ComputeFrameDemand and keeps its capacity") {
        std::vector<PassDemandStatic> s { writer(0),
                                          writer(1, { K::ForSlot(0) }),
                                          PassDemandStatic {
                                              K::ForHandle(1), false, { K::ForSlot(1) }, {}, {} } };
        std::vector<PassDemandFrame>  f(3, PassDemandFrame { true, false, DemandInputs::Own });
        DemandResult                  r;
        ComputeFrameDemandInto(s, f, 2, r);
        CHECK(r.needed == ComputeFrameDemand(s, f, 2).needed);
        CHECK(r.iterations == ComputeFrameDemand(s, f, 2).iterations);
        const auto cap = r.needed.capacity();
        f[2].draws     = false;
        ComputeFrameDemandInto(s, f, 2, r);
        CHECK(r.needed == std::vector<bool> { false, false }); // stale true bits are cleared
        CHECK(r.needed.capacity() == cap);
    }
    TEST_CASE("ComputeBlockedPassesInto matches ComputeBlockedPasses and keeps its capacity") {
        std::vector<PassDemandStatic> s { PassDemandStatic {
            K::ForHandle(1), false, { K::ForSlot(0) }, {}, {} } };
        std::vector<PassDemandFrame>  f { { true, false, DemandInputs::Own } };
        std::vector<bool>             blocked;
        ComputeBlockedPassesInto(s, f, std::vector<bool> { false }, blocked);
        CHECK(blocked == std::vector<bool> { true });
        const auto cap = blocked.capacity();
        ComputeBlockedPassesInto(s, f, std::vector<bool> { true }, blocked);
        CHECK(blocked == std::vector<bool> { false });
        CHECK(blocked.capacity() == cap);
        CHECK(ComputeBlockedPasses(s, f, std::vector<bool> { false }) ==
              std::vector<bool> { true });
    }
}
