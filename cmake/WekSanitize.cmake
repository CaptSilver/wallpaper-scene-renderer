# WekSanitize.cmake — shared sanitizer plumbing for test/dev builds.
#
# Single source of truth, included from THREE places so the WEK_SANITIZE option
# behaves identically whether the build is configured from the parent project
# (-S .) or the submodule standalone (-S src/backend_scene):
#   * root        CMakeLists.txt           — include(src/backend_scene/cmake/WekSanitize.cmake)
#   * submodule   src/backend_scene/CMakeLists.txt — include(cmake/WekSanitize.cmake)
#   * main tests  tests/CMakeLists.txt     — include(../src/backend_scene/cmake/WekSanitize.cmake)
# (mirrors how FetchMull.cmake is referenced cross-tree.)
#
# It declares the WEK_SANITIZE cache STRING (idempotent — guarded so multiple
# includes don't clobber a user-set value) and provides wek_apply_sanitizers()
# which appends the -fsanitize flags to a target's compile + link options.
#
# Design: applied PER TARGET (the test binaries), not project-wide. This keeps
# instrumentation scoped to the suites we run under a sanitizer and avoids
# touching the shippable plugin .so / renderer libs. ASAN/UBSAN want uniform
# instrumentation across a binary's own TUs — every source of each test target
# is compiled with the same flag, and the libraries those targets statically
# link (wpScene, wpUtils, …) are separate translation units; partial-library
# instrumentation is a known, tolerated sanitizer caveat (third_party stays
# uninstrumented too, same as the COVERAGE precedent).

# Declare the option once. The `WEK_SANITIZE_DECLARED` guard makes re-includes
# from the three sites a no-op rather than re-running the parse/validation.
if(NOT DEFINED WEK_SANITIZE_DECLARED)
    set(WEK_SANITIZE "" CACHE STRING
        "Sanitizers for test/dev builds: comma list of address,undefined,thread \
(e.g. 'address,undefined' or 'thread'). address+thread are mutually exclusive. \
Clang-only. Empty (default) => no sanitizer flags.")

    set(WEK_SANITIZE_DECLARED ON CACHE INTERNAL "WekSanitize.cmake has run")

    if(WEK_SANITIZE)
        # Clang-only — match the COVERAGE / MUTATION / BUILD_FUZZERS precedent.
        if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            message(FATAL_ERROR
                "WEK_SANITIZE requires Clang (found ${CMAKE_CXX_COMPILER_ID})")
        endif()

        # Normalise the comma list to a CMake list for validation.
        string(REPLACE "," ";" _wek_san_list "${WEK_SANITIZE}")
        list(REMOVE_DUPLICATES _wek_san_list)

        # Validate each token and enforce the address/thread mutual exclusion.
        set(_wek_san_have_address OFF)
        set(_wek_san_have_thread  OFF)
        foreach(_san IN LISTS _wek_san_list)
            if(_san STREQUAL "address" OR _san STREQUAL "leak")
                set(_wek_san_have_address ON)
            elseif(_san STREQUAL "thread")
                set(_wek_san_have_thread ON)
            elseif(_san STREQUAL "undefined")
                # ok with either
            else()
                message(FATAL_ERROR
                    "WEK_SANITIZE: unknown sanitizer '${_san}' \
(accepted: address, undefined, thread)")
            endif()
        endforeach()
        if(_wek_san_have_address AND _wek_san_have_thread)
            message(FATAL_ERROR
                "WEK_SANITIZE: 'thread' is mutually exclusive with 'address'/'leak' \
(got '${WEK_SANITIZE}') — run TSAN and ASAN as separate builds.")
        endif()

        # Build the flag list once. -fno-omit-frame-pointer + -g for readable
        # sanitizer stacks; mirrors the fuzzer flag set (_fuzz_san_opts).
        #
        # NOTE: deliberately NOT adding -fno-sanitize-recover here. Whether a
        # finding aborts (gate) or prints-and-continues (survey) is left to the
        # RUNTIME *SAN_OPTIONS the caller sets:
        #   * gate   (TSAN / future CI): TSAN_OPTIONS=halt_on_error=1 / UBSAN_OPTIONS=halt_on_error=1
        #   * survey (the non-fatal ASAN/UBSAN preflight leg): halt_on_error=0
        # Baking -fno-sanitize-recover=all in would force UBSAN to abort on the
        # first violation, hiding the rest — bad for the survey use-case.
        set(WEK_SANITIZE_FLAGS
            -fsanitize=${WEK_SANITIZE}
            -fno-omit-frame-pointer
            -g
            CACHE INTERNAL "Resolved -fsanitize flag list for wek_apply_sanitizers()")

        message(STATUS "WEK_SANITIZE: ${WEK_SANITIZE} (flags: ${WEK_SANITIZE_FLAGS})")
    endif()
endif()

# wek_apply_sanitizers(<target>) — append the resolved sanitizer flags to the
# target's compile + link options. No-op when WEK_SANITIZE is empty, so it is
# always safe to call unconditionally at every test-target site.
function(wek_apply_sanitizers _target)
    if(NOT WEK_SANITIZE)
        return()
    endif()
    if(NOT TARGET ${_target})
        message(WARNING "wek_apply_sanitizers: '${_target}' is not a target")
        return()
    endif()
    target_compile_options(${_target} PRIVATE ${WEK_SANITIZE_FLAGS})
    target_link_options(${_target}    PRIVATE ${WEK_SANITIZE_FLAGS})
endfunction()
