set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

add_subdirectory(glad)

add_library (eigen INTERFACE)
add_library (Eigen3::Eigen ALIAS eigen)
# SYSTEM: Eigen is vendored and its headers are pulled into first-party TUs, so
# without -isystem every diagnostic upstream trips is attributed to whichever of
# our files included it -- and wpParticle carries -Werror on any clang build.
# clang 23 added -Wunused-template to what it reports here and broke the build on
# a compiler bump alone, with no change of ours.
target_include_directories(eigen SYSTEM INTERFACE Eigen)

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

# Everything vendored above exposes its headers to first-party TUs, and upstream
# projects do not mark their own interface includes as system.  Without this, a
# diagnostic that any of them trips is reported against whichever of our files
# pulled the header in -- and several of our targets carry -Werror.  Eigen has
# SYSTEM applied at its own target above; the add_subdirectory ones have to be
# retrofitted here because we do not patch vendored CMake.
#
# INTERFACE_SYSTEM_INCLUDE_DIRECTORIES rather than the SYSTEM target property,
# which needs CMake 3.25 and the floor here is 3.22.
foreach(_vendored glslang glslang-default-resource-limits SPIRV OSDependent
                  MachineIndependent GenericCodeGen spirv-reflect-static
                  nlohmann_json glad rapidcheck)
    if(TARGET ${_vendored})
        get_target_property(_inc ${_vendored} INTERFACE_INCLUDE_DIRECTORIES)
        if(_inc)
            set_target_properties(${_vendored} PROPERTIES
                INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_inc}")
        endif()
    endif()
endforeach()
