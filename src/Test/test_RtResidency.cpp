#include <doctest.h>

#include "VulkanRender/RtResidency.hpp"

#include <cstdint>
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
        CHECK(r.iterations <= 3);
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
