# WekWarnings.cmake — shared warning-flag plumbing for first-party targets.
#
# Single source of truth, included from the same places as WekSanitize.cmake so
# the WEK_WERROR option + the `wek_warn_opts` list behave identically whether the
# build is configured from the parent project (-S .) or the submodule standalone
# (-S src/backend_scene):
#   * root        CMakeLists.txt                   — include(src/backend_scene/cmake/WekWarnings.cmake)
#   * submodule   src/backend_scene/CMakeLists.txt — include(cmake/WekWarnings.cmake)
# (mirrors how WekSanitize.cmake / FetchMull.cmake are referenced cross-tree.)
#
# It declares:
#   * option(WEK_WERROR ...) — OFF by default. ON appends -Werror to the
#     FIRST-PARTY warn lists only. third_party never receives these flags (it is
#     added via add_subdirectory(third_party) with its own flags and pulled in
#     with include_directories(SYSTEM ...)), so -Werror is excluded from it BY
#     CONSTRUCTION — we deliberately do NOT use a global add_compile_options.
#   * wek_warn_opts (CACHE INTERNAL) — the conservative warning set for the
#     shippable / dlopen'd targets that previously had NO warning flags at all:
#     the plugin .so, backend_mpv, the wescene-renderer-qml bridge, and
#     wpParticle. -Wall -Wextra only — the -Wconversion / -Wsign-conversion
#     family is intentionally NOT here (it is noisy on the Qt/QJSEngine bridge
#     and on FileHelper's int<->size_t arithmetic; the renderer libs opt into it
#     via the separate `warn_opts` in src/backend_scene/src/CMakeLists.txt).
#   * WEK_WERROR_FLAGS (CACHE INTERNAL) — the -Werror tail to append onto the
#     submodule's richer `warn_opts`.  Even under WEK_WERROR the signed/unsigned
#     diagnostic family stays WARNING-ONLY (-Wno-error=conversion /
#     -Wno-error=sign-conversion / -Wno-error=sign-compare) because that family
#     is noisy and the project tracks it as informational.
#
# No DEFINED-guard around the derived values: `option()` is idempotent, and the
# derived CACHE INTERNAL vars are *recomputed every configure* (INTERNAL forces
# the cache value) so toggling -DWEK_WERROR on a pre-existing build dir takes
# effect — a stale guard would otherwise pin the first run's flag list.  The two
# include sites (root + submodule) just recompute identically within a configure.

option(WEK_WERROR "Treat first-party warnings as errors (-Werror; sign/conversion family stays warning-only)" OFF)

# The -Werror tail.  Empty unless WEK_WERROR is ON.  The signed/unsigned
# diagnostic family (-Wconversion, -Wsign-conversion, -Wsign-compare) stays
# WARNING-only (not error) under -Werror: it is noisy across the renderer (heavy
# size_t<->int arithmetic) and the project tracks it as informational, which
# matches the "ride -Werror on -Wall -Wextra only" policy.
set(_wek_werror_flags "")
if(WEK_WERROR)
    if(NOT (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU"))
        message(WARNING
            "WEK_WERROR set but compiler is '${CMAKE_CXX_COMPILER_ID}' \
(expected Clang/GNU) — -Werror not applied.")
    else()
        set(_wek_werror_flags
            -Werror
            -Wno-error=conversion
            -Wno-error=sign-conversion
            -Wno-error=sign-compare
            # GCC reports double->float narrowing under -Wfloat-conversion and
            # does not cascade the -Wno-error=conversion group exempt to it the
            # way clang does, so name it explicitly to keep the conversion
            # family warning-only on both compilers.
            -Wno-error=float-conversion)
        message(STATUS "WEK_WERROR: ON — first-party warnings are errors "
                       "(sign/conversion family stays warning-only)")
    endif()
endif()
set(WEK_WERROR_FLAGS "${_wek_werror_flags}"
    CACHE INTERNAL "-Werror tail appended to first-party warn lists when WEK_WERROR=ON")

# Conservative warning set for the previously-unwarned shippable targets.
# -Wall -Wextra plus the -Werror tail (empty unless WEK_WERROR=ON).
set(wek_warn_opts -Wall -Wextra ${WEK_WERROR_FLAGS}
    CACHE INTERNAL "Warning flags for the shippable plugin/mpv/bridge/particle targets")
