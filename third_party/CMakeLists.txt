set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

add_subdirectory(glad)

add_library (eigen INTERFACE)
add_library (Eigen3::Eigen ALIAS eigen)
target_include_directories(eigen INTERFACE Eigen)

add_subdirectory(nlohmann EXCLUDE_FROM_ALL)

set(SPIRV_REFLECT_STATIC_LIB ON)
set(SPIRV_REFLECT_EXECUTABLE OFF)
set(SPIRV_REFLECT_BUILD_TESTS OFF)
set(SPIRV_REFLECT_EXAMPLES OFF)
# Block upstream's install(TARGETS spirv-reflect-static ...) rule. Today the
# directory-level EXCLUDE_FROM_ALL on the add_subdirectory below also blocks it,
# but pinning OFF gives defense-in-depth: if anyone ever drops the
# EXCLUDE_FROM_ALL (e.g., to expose spirv-reflect-static for a unit test target),
# the static lib + header still won't deposit under the install prefix.
set(SPIRV_REFLECT_INSTALL OFF)
add_subdirectory(SPIRV-Reflect EXCLUDE_FROM_ALL)

set(SKIP_GLSLANG_INSTALL ON)
set(ENABLE_GLSLANG_BINARIES OFF)
set(ENABLE_SPVREMAPPER OFF)
set(ENABLE_GLSLANG_JS OFF)
set(ENABLE_HLSL OFF)
set(ENABLE_OPT OFF)
add_subdirectory(glslang EXCLUDE_FROM_ALL)

if(BUILD_TESTS)
    # RapidCheck — property-based testing companion to doctest. Header-heavy
    # template library; only built when BUILD_TESTS is ON. EXCLUDE_FROM_ALL
    # means only the test binary pulls it in transitively. The extras/doctest
    # INTERFACE target (gated by RC_ENABLE_DOCTEST) is left OFF: its header
    # depends on doctest internals that postdate the vendored snapshot under
    # third_party/nlohmann/tests/thirdparty/doctest/.  rc::check + a doctest
    # CHECK wrapper covers the same use case without the header drift.
    add_subdirectory(rapidcheck EXCLUDE_FROM_ALL)
endif()
