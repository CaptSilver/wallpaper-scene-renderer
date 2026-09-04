#include <doctest.h>

#include "VulkanRender/FrameFenceCycle.hpp"
#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>

using wallpaper::vulkan::FrameFenceCycleOps;
using wallpaper::vulkan::runFrameFenceCycle;

namespace
{

// Stand-in for one slot's VkFence, created signaled like the real frame
// fences.  A wait retires when the fence is signaled or has a submit behind
// it; waiting on one that was reset with nothing submitted can never retire,
// and waitFenceWithRetry turns that into DEVICE_LOST after its retry budget,
// so that is what this reports.
struct FakeFence {
    bool signaled { true };
    bool submitted { false };

    VkResult wait() {
        if (submitted) {
            submitted = false;
            signaled  = true;
        }
        return signaled ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
    }
    VkResult reset() {
        signaled = false;
        return VK_SUCCESS;
    }
    VkResult submit() {
        submitted = true;
        return VK_SUCCESS;
    }
};

constexpr std::size_t kFramesInFlight = 2;

// The slot ring the swapchain draw loop runs on: the frame index picks the
// slot, and the fence in that slot is what the next frame landing there waits
// on.
struct FrameRing {
    std::array<FakeFence, kFramesInFlight> fences {};
    std::uint64_t                          frame_index { 0 };

    // `records` false stands for any of the pre-submit exits — the swapchain
    // needing a recreate, an OUT_OF_DATE acquire after a window resize, a
    // fatal acquire.
    VkResult drawFrame(bool records, bool& submitted) {
        FakeFence& fence = fences[frame_index % kFramesInFlight];
        return runFrameFenceCycle(FrameFenceCycleOps {
                                      .wait_fence =
                                          [&fence]() {
                                              return fence.wait();
                                          },
                                      .record =
                                          [records]() {
                                              return records;
                                          },
                                      .reset_fence =
                                          [&fence]() {
                                              return fence.reset();
                                          },
                                      .submit =
                                          [&fence]() {
                                              return fence.submit();
                                          },
                                  },
                                  frame_index,
                                  submitted);
    }
};

} // namespace

TEST_SUITE("VulkanRender frame fence cycle") {
    TEST_CASE("a frame abandoned before submit leaves its slot waitable for the retry") {
        FrameRing ring;
        bool      submitted = true;

        CHECK(ring.drawFrame(false, submitted) == VK_SUCCESS);
        CHECK_FALSE(submitted);

        // The abandoned frame did not advance the index, so the retry lands on
        // the same slot and waits on the same fence.
        CHECK(ring.drawFrame(true, submitted) == VK_SUCCESS);
        CHECK(submitted);
    }

    TEST_CASE("a run of abandoned frames keeps retrying the same slot") {
        // The viewer's failure mode was a recreate that re-armed itself every
        // time round, so the skip has to stay survivable indefinitely.
        FrameRing ring;
        bool      submitted = true;

        for (int i = 0; i < 8; ++i) {
            CAPTURE(i);
            CHECK(ring.drawFrame(false, submitted) == VK_SUCCESS);
            CHECK_FALSE(submitted);
            CHECK(ring.frame_index == 0);
        }

        CHECK(ring.drawFrame(true, submitted) == VK_SUCCESS);
        CHECK(submitted);
    }

    TEST_CASE("a submitted frame moves to the next slot and comes back to a retired fence") {
        FrameRing ring;
        bool      submitted = false;

        for (std::uint64_t frame = 0; frame < 6; ++frame) {
            CAPTURE(frame);
            CHECK(ring.drawFrame(true, submitted) == VK_SUCCESS);
            CHECK(submitted);
            CHECK(ring.frame_index == frame + 1);
        }
    }

    TEST_CASE("a failed wait stops the frame before it touches the fence") {
        bool          reset_called = false, submit_called = false, recorded = false;
        std::uint64_t frame_index = 7;
        bool          submitted   = true;

        VkResult r = runFrameFenceCycle(FrameFenceCycleOps {
                                            .wait_fence =
                                                []() {
                                                    return VK_ERROR_DEVICE_LOST;
                                                },
                                            .record =
                                                [&recorded]() {
                                                    recorded = true;
                                                    return true;
                                                },
                                            .reset_fence =
                                                [&reset_called]() {
                                                    reset_called = true;
                                                    return VK_SUCCESS;
                                                },
                                            .submit =
                                                [&submit_called]() {
                                                    submit_called = true;
                                                    return VK_SUCCESS;
                                                },
                                        },
                                        frame_index,
                                        submitted);
        CHECK(r == VK_ERROR_DEVICE_LOST);
        CHECK_FALSE(recorded);
        CHECK_FALSE(reset_called);
        CHECK_FALSE(submit_called);
        CHECK_FALSE(submitted);
        CHECK(frame_index == 7);
    }

    TEST_CASE("a failed reset does not submit") {
        bool          submit_called = false;
        std::uint64_t frame_index   = 3;
        bool          submitted     = true;

        VkResult r = runFrameFenceCycle(FrameFenceCycleOps {
                                            .wait_fence =
                                                []() {
                                                    return VK_SUCCESS;
                                                },
                                            .record =
                                                []() {
                                                    return true;
                                                },
                                            .reset_fence =
                                                []() {
                                                    return VK_ERROR_OUT_OF_HOST_MEMORY;
                                                },
                                            .submit =
                                                [&submit_called]() {
                                                    submit_called = true;
                                                    return VK_SUCCESS;
                                                },
                                        },
                                        frame_index,
                                        submitted);
        CHECK(r == VK_ERROR_OUT_OF_HOST_MEMORY);
        CHECK_FALSE(submit_called);
        CHECK_FALSE(submitted);
        CHECK(frame_index == 3);
    }

    TEST_CASE("a failed submit reports the error and leaves the index put") {
        std::uint64_t frame_index = 3;
        bool          submitted   = true;

        VkResult r = runFrameFenceCycle(FrameFenceCycleOps {
                                            .wait_fence =
                                                []() {
                                                    return VK_SUCCESS;
                                                },
                                            .record =
                                                []() {
                                                    return true;
                                                },
                                            .reset_fence =
                                                []() {
                                                    return VK_SUCCESS;
                                                },
                                            .submit =
                                                []() {
                                                    return VK_ERROR_DEVICE_LOST;
                                                },
                                        },
                                        frame_index,
                                        submitted);
        CHECK(r == VK_ERROR_DEVICE_LOST);
        CHECK_FALSE(submitted);
        CHECK(frame_index == 3);
    }

    TEST_CASE("SUBOPTIMAL from a step is success, not a failure") {
        // The draw loop already presents through a suboptimal swapchain; the
        // fence cycle must not treat it as a reason to drop the frame.
        std::uint64_t frame_index = 0;
        bool          submitted   = false;

        VkResult r = runFrameFenceCycle(FrameFenceCycleOps {
                                            .wait_fence =
                                                []() {
                                                    return VK_SUBOPTIMAL_KHR;
                                                },
                                            .record =
                                                []() {
                                                    return true;
                                                },
                                            .reset_fence =
                                                []() {
                                                    return VK_SUCCESS;
                                                },
                                            .submit =
                                                []() {
                                                    return VK_SUBOPTIMAL_KHR;
                                                },
                                        },
                                        frame_index,
                                        submitted);
        CHECK(r == VK_SUCCESS);
        CHECK(submitted);
        CHECK(frame_index == 1);
    }
}
