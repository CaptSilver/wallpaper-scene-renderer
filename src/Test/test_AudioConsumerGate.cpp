#include <doctest.h>
#include "WPShaderValueUpdater.hpp"

#include <map>

using namespace wallpaper;

// The per-frame audio FFT (AudioAnalyzer::Process) + emit-rate scan
// run iff the scene consumes audio.  audioConsumerPredicate is the union of the
// three consumer kinds; WPShaderValueUpdater::hasAudioConsumer() is literally
// this predicate over its three member flags, so testing the pure function
// covers the shipped gate logic without constructing the (vtable-bearing,
// Vulkan-adjacent) updater.
TEST_SUITE("Audio consumer predicate") {
    TEST_CASE("false only when NO consumer of any kind") {
        CHECK(audioConsumerPredicate(false, false, false) == false);
    }
    TEST_CASE("a spectrum uniform alone makes the scene a consumer") {
        CHECK(audioConsumerPredicate(true, false, false) == true);
    }
    TEST_CASE("a reactive particle alone makes the scene a consumer") {
        CHECK(audioConsumerPredicate(false, true, false) == true);
    }
    TEST_CASE("CRITICAL: script audio (_audioRegs) alone makes the scene a consumer") {
        // A script-only-audio scene (registerAudioBuffers, no uniform, no
        // reactive particle) MUST still run Process() — else refreshAudioBuffers
        // feeds zeros and the visualizer is dead.  This is the spec's explicit
        // starvation guard, wired in via SceneWallpaper::setHasScriptAudio.
        CHECK(audioConsumerPredicate(false, false, true) == true);
    }
    TEST_CASE("any combination of consumers is true") {
        CHECK(audioConsumerPredicate(true, true, false) == true);
        CHECK(audioConsumerPredicate(true, false, true) == true);
        CHECK(audioConsumerPredicate(false, true, true) == true);
        CHECK(audioConsumerPredicate(true, true, true) == true);
    }
}

namespace
{
// Minimal stand-in matching the only interface anyAudioReactive touches
// (it->second->IsAudioReactive()); avoids pulling in the full ParticleSubSystem.
struct MockSub {
    bool reactive;
    bool IsAudioReactive() const { return reactive; }
};
} // namespace

TEST_SUITE("anyAudioReactive aggregation") {
    TEST_CASE("false when no subsystem is reactive") {
        MockSub                 a { false }, b { false };
        std::map<int, MockSub*> m { { 1, &a }, { 2, &b } };
        CHECK(anyAudioReactive(m.begin(), m.end()) == false);
    }
    TEST_CASE("true when any subsystem is reactive") {
        MockSub                 a { false }, b { true };
        std::map<int, MockSub*> m { { 1, &a }, { 2, &b } };
        CHECK(anyAudioReactive(m.begin(), m.end()) == true);
    }
    TEST_CASE("null entries are skipped (no deref crash)") {
        MockSub                 b { true };
        std::map<int, MockSub*> withNullThenReactive { { 1, nullptr }, { 2, &b } };
        CHECK(anyAudioReactive(withNullThenReactive.begin(), withNullThenReactive.end()) == true);
        std::map<int, MockSub*> onlyNull { { 1, nullptr } };
        CHECK(anyAudioReactive(onlyNull.begin(), onlyNull.end()) == false);
    }
    TEST_CASE("empty map is not a consumer") {
        std::map<int, MockSub*> empty;
        CHECK(anyAudioReactive(empty.begin(), empty.end()) == false);
    }
}
