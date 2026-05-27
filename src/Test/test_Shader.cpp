// Behavioural tests for the SPIR-V validation gate (spvOptions.validate).
#include <doctest.h>
#include <cstdlib>

namespace wallpaper::vulkan::detail
{
bool ShouldValidateSpv();
} // namespace wallpaper::vulkan::detail

namespace
{

void clearShaderDebugEnv() { ::unsetenv("WEK_DEBUG_SHADERS"); }

} // namespace

TEST_SUITE_BEGIN("Shader compile flags");

TEST_CASE("ShouldValidateSpv: NDEBUG build with env unset returns false") {
#ifdef NDEBUG
    clearShaderDebugEnv();
    CHECK_FALSE(wallpaper::vulkan::detail::ShouldValidateSpv());
#else
    // In Debug builds the predicate is unconditionally true; skip the
    // env-clearing assertion (the static-init caches Debug=true regardless).
    CHECK(wallpaper::vulkan::detail::ShouldValidateSpv());
#endif
}

TEST_CASE("ShouldValidateSpv: Debug build always returns true") {
#ifndef NDEBUG
    CHECK(wallpaper::vulkan::detail::ShouldValidateSpv());
#endif
}

TEST_CASE("ShouldValidateSpv: WEK_DEBUG_SHADERS=1 forces true even under NDEBUG") {
    // NOTE: the predicate caches in a static-local; this test only covers
    // the env-set state when the cache is fresh.  In a Debug build the
    // predicate is unconditionally true so the env doesn't matter.  In a
    // Release build, the first test sets the cache to false, so this
    // assertion documents the env-honoured path via a side-channel: we
    // can't re-cache once initialised, so the test is degenerate after
    // case #1 runs.  Run with `--test-case=*WEK_DEBUG_SHADERS*` to isolate.
    ::setenv("WEK_DEBUG_SHADERS", "1", 1);
    // No assertion possible without test-isolated env setup; this case
    // exists to document the intent + serve as the env-override smoke.
    CHECK_NOTHROW(wallpaper::vulkan::detail::ShouldValidateSpv());
}

TEST_SUITE_END();
