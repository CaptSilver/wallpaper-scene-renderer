# WekMullInstrument.cmake — shared Mull mutation-instrumentation plumbing.
#
# Single source of truth for the -fpass-plugin option block that makes Mull
# embed mutants into a target's object code, and for resolving MULL_PLUGIN_PATH
# itself (system-installed mull-ir-frontend-N first, FetchMull.cmake's
# auto-download otherwise).  Included from the submodule root CMakeLists.txt
# only: Mull mutation testing here is submodule-only, since the parent's
# tests/CMakeLists.txt compiles first-party sources directly into each test
# executable rather than linking a static library, so it has nothing this file
# needs to reach.
#
# Must run BEFORE add_subdirectory(third_party) / add_subdirectory(src): those
# add the library targets wek_apply_mull_instrumentation() gets called on
# (Utils, Looper, Particle, Audio, RenderGraph, Scene, Timer, Vulkan), and
# MULL_PLUGIN_PATH has to already be resolved by the time they run — the old
# resolution block ran AFTER add_subdirectory(src), which is exactly why the
# libraries it defines had never been instrumented: only
# src/Test/CMakeLists.txt's own target_compile_options ever added the flag,
# because src/Test is added after this block, not before it.

# No include guard here: the header comment above already makes this a
# single-include file (submodule root CMakeLists.txt only, never the parent),
# so a guard would just be ceremony with nothing to protect against.  Add one
# back if a second include site ever shows up.
if(MUTATION_TESTING AND NOT BUILD_TESTS)
    message(FATAL_ERROR "MUTATION_TESTING requires BUILD_TESTS=ON")
endif()

if(MUTATION_TESTING)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "MUTATION_TESTING requires Clang (found ${CMAKE_CXX_COMPILER_ID})")
    endif()

    if(NOT MULL_PLUGIN_PATH)
        # Try system-installed Mull first.
        execute_process(
            COMMAND ${CMAKE_CXX_COMPILER} --version
            OUTPUT_VARIABLE _wek_mull_clang_ver_out OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(REGEX MATCH "clang version ([0-9]+)" _ "${_wek_mull_clang_ver_out}")
        set(_wek_mull_clang_maj "${CMAKE_MATCH_1}")
        file(GLOB _wek_mull_candidates
            /usr/lib/mull-ir-frontend-${_wek_mull_clang_maj}
            /usr/lib64/mull-ir-frontend-${_wek_mull_clang_maj})
        if(_wek_mull_candidates)
            list(GET _wek_mull_candidates 0 MULL_PLUGIN_PATH)
            find_program(MULL_RUNNER mull-runner-${_wek_mull_clang_maj})
        endif()
    endif()

    if(NOT MULL_PLUGIN_PATH)
        # Auto-download from GitHub releases.  Resolved via
        # CMAKE_CURRENT_LIST_DIR (this file's own directory), not the
        # caller's CMAKE_CURRENT_SOURCE_DIR, so it works regardless of
        # where this module is included from.
        include(${CMAKE_CURRENT_LIST_DIR}/FetchMull.cmake)
    endif()

    message(STATUS "Mull plugin: ${MULL_PLUGIN_PATH}")
    if(MULL_RUNNER)
        message(STATUS "Mull runner: ${MULL_RUNNER}")
    endif()
endif()

# wek_apply_mull_instrumentation(<target>) — append the same -fpass-plugin
# option block src/Test/CMakeLists.txt has always used for the test executable
# itself, so a first-party library gets embedded mutants too. No-op when
# MUTATION_TESTING is off, so it's always safe to call unconditionally at
# every first-party library site (mirrors wek_apply_sanitizers()).
#
# INTERFACE libraries (wpFs: headers only, no compiled TUs) are skipped —
# there is nothing for the plugin to instrument, and target_compile_options on
# an INTERFACE library needs the INTERFACE keyword instead of PRIVATE.
function(wek_apply_mull_instrumentation _target)
    if(NOT MUTATION_TESTING)
        return()
    endif()
    if(NOT TARGET ${_target})
        message(WARNING "wek_apply_mull_instrumentation: '${_target}' is not a target")
        return()
    endif()
    get_target_property(_wek_mull_type ${_target} TYPE)
    if(_wek_mull_type STREQUAL "INTERFACE_LIBRARY")
        return()
    endif()
    target_compile_options(${_target} PRIVATE
        -fpass-plugin=${MULL_PLUGIN_PATH}
        -g -grecord-command-line
        -DWP_SUPPRESS_DEBUG_LOGGING
    )
endfunction()
