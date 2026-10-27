cmake_minimum_required(VERSION 3.14)
project(wescene-renderer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
# Declared here, not in src/, because src/Test is added from this file too:
# sibling directory scopes do not share variables, and the test target
# compiles SystemFontFallback.cpp directly so it needs its own link.
pkg_check_modules(FONTCONFIG REQUIRED fontconfig)


option(BUILD_TESTS "Build backend_scene unit tests" OFF)
option(BUILD_TOOLS "Build pkg inspection / extraction tools (wp-pkg)" OFF)
option(BUILD_FUZZERS "Build libFuzzer harnesses (requires Clang + sanitizers)" OFF)
option(MUTATION_TESTING "Enable Mull mutation testing instrumentation (requires Clang)" OFF)
option(COVERAGE "Enable Clang source-based coverage instrumentation (requires Clang)" OFF)
option(PROFILING "Enable lightweight scoped-timer profiler (SceneProfiler.h)" OFF)

# Sanitizer plumbing (WEK_SANITIZE option + wek_apply_sanitizers helper).
# Same canonical file the parent includes; the guard inside makes it idempotent
# when the parent has already declared the option.  Lets the submodule be
# sanitized on its own:  cmake -B b -S src/backend_scene -DWEK_SANITIZE=thread.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/WekSanitize.cmake)

# Warning-flag plumbing (WEK_WERROR option + wek_warn_opts).  Same canonical
# file the parent includes; the guard makes it idempotent.  Provides
# WEK_WERROR_FLAGS, appended to the renderer `warn_opts` in src/CMakeLists.txt,
# and wek_warn_opts for the wescene-renderer-qml bridge below.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/WekWarnings.cmake)

# Release-time hardening plumbing (WEK_HARDEN + WEK_LTO + WEK_USE_LLD).  Same
# canonical-file pattern as WekWarnings / WekSanitize above.  Provides
# wek_harden_opts + wek_harden_link_opts for the wescene-renderer-qml bridge
# below; the option declarations are visible to standalone-submodule
# configures too.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/WekHardening.cmake)

# Mull mutation-instrumentation plumbing (resolves MULL_PLUGIN_PATH, provides
# wek_apply_mull_instrumentation).  Included here, before add_subdirectory(src)
# below, because that call defines the library targets the function gets
# called on (Utils, Looper, Particle, Audio, RenderGraph, Scene, Timer,
# Vulkan) — MULL_PLUGIN_PATH must already be resolved by the time they run.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/WekMullInstrument.cmake)

# Translator fingerprint salting the on-disk SPIR-V cache key.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/WekShaderFingerprint.cmake)

if(PROFILING)
    # Applied before add_subdirectory so every backend target compiles the
    # WEK_PROFILE_SCOPE macros to real timers instead of ((void)0).
    add_compile_definitions(WEK_PROFILING=1)
    message(STATUS "Scene profiler: enabled (WEK_PROFILING=1)")
endif()

if(COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "COVERAGE requires Clang (found ${CMAKE_CXX_COMPILER_ID})")
    endif()
    # Applied here (before add_subdirectory) so it reaches all project targets;
    # third_party gets filtered at report time via -ignore-filename-regex.
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping -g -O0)
    add_link_options(-fprofile-instr-generate)
    # Serialize instrumented links (the whole renderer maps into each -O0 -g
    # link — the RSS spike that OOM'd 30 GB boxes).  A Ninja link pool caps
    # concurrent links; compiles stay wide.  Ninja-only (the RAM-bounded -j in
    # tools/scripts/preflight.sh is the Make backstop).
    set(COVERAGE_LINK_JOBS 2 CACHE STRING "Max concurrent links in the COVERAGE build")
    set_property(GLOBAL PROPERTY JOB_POOLS cov_link_pool=${COVERAGE_LINK_JOBS})
    set(CMAKE_JOB_POOL_LINK cov_link_pool)
endif()

add_subdirectory(third_party)
add_subdirectory(src)

if(BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(BUILD_FUZZERS AND NOT BUILD_TESTS)
    message(FATAL_ERROR "BUILD_FUZZERS requires BUILD_TESTS=ON")
endif()

# MUTATION_TESTING's own BUILD_TESTS check and MULL_PLUGIN_PATH resolution now
# live in cmake/WekMullInstrument.cmake, included above — before
# add_subdirectory(src), not after, so the libraries it defines can be
# instrumented too (see that file's header comment for why the old ordering
# left them out).

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(src/Test)
endif()

if(BUILD_QML)
    find_package(Qt6 COMPONENTS Gui Quick Qml REQUIRED)

    set(CMAKE_AUTOMOC ON) 
    set(CMAKE_AUTORCC ON) 
    set(CMAKE_AUTOUIC ON)

    add_library(${PROJECT_NAME}-qml
    STATIC
        qml_helper/SceneBackend.cpp
        qml_helper/SceneScriptBridge.h
        qml_helper/SceneTimerBridge.h
        qml_helper/MediaThumbnailCrossfade.cpp
        qml_helper/glExtra.cpp
    )
    target_link_libraries(${PROJECT_NAME}-qml
    PUBLIC
        Qt::Quick
        Qt::Gui
        Qt::Qml
        glad
        ${PROJECT_NAME}
    PRIVATE
        wpTimer
    )
    target_include_directories(${PROJECT_NAME}-qml PUBLIC qml_helper
        PRIVATE src/Audio/include src/Scene/include)
    # Vendored headers go in as -isystem so upstream's warnings are not reported
    # against the bridge, which builds with -Wall -Wextra.
    target_include_directories(${PROJECT_NAME}-qml SYSTEM PUBLIC third_party)

    # The QML bridge (SceneBackend.cpp ~2200 lines) is dlopen'd into plasmashell
    # via the plugin .so but previously carried NO warning flags.  Apply the
    # conservative wek_warn_opts (-Wall -Wextra, +/-Werror under WEK_WERROR).
    # Not the full -Wconversion family: the bridge marshals QJSEngine doubles
    # <-> C++ ints heavily and that would drown the actionable warnings.
    target_compile_options(${PROJECT_NAME}-qml PRIVATE ${wek_warn_opts} ${wek_harden_opts})
    target_link_options(${PROJECT_NAME}-qml PRIVATE ${wek_harden_link_opts})

    set(CMAKE_AUTOMOC OFF)
    set(CMAKE_AUTORCC OFF)
    set(CMAKE_AUTOUIC OFF)
endif()

# Scoped -Werror gate for the audited-clean shippable targets.  Same canonical
# file the parent includes.  Standalone-submodule configures only have two of
# the four targets (wescene-renderer-qml, wpParticle); the per-target if(TARGET)
# guard inside the helper makes that safe.  No-op when -DWEK_WERROR=ON (full
# audit covers these targets already).
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/WekWerrorScoped.cmake)
