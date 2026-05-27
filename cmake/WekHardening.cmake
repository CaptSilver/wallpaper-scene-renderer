# WekHardening.cmake — shared release-time hardening + ThinLTO plumbing for
# first-party targets.
#
# Single source of truth, included from the same places as WekWarnings.cmake
# and WekSanitize.cmake so the options behave identically whether the build is
# configured from the parent project (-S .) or the submodule standalone
# (-S src/backend_scene):
#   * root        CMakeLists.txt                   — include(src/backend_scene/cmake/WekHardening.cmake)
#   * submodule   src/backend_scene/CMakeLists.txt — include(cmake/WekHardening.cmake)
#
# Declares:
#   * option(WEK_HARDEN)   — release-time hardening for first-party targets.
#                            OFF by default to keep the developer "just build
#                            it fast" path unchanged; packagers/users opt in
#                            via -DWEK_HARDEN=ON.  Distro builds (RPM via
#                            %{optflags}, Debian via dpkg-buildflags) already
#                            inject most of these at the env level; this
#                            option is the source-build path.
#   * option(WEK_LTO)      — ThinLTO via CMake's INTERPROCEDURAL_OPTIMIZATION.
#                            OFF by default (link-time +; opt-in for releases).
#   * option(WEK_USE_LLD)  — prefer ld.lld for Clang.  ON by default; probed
#                            (falls back to system linker when ld.lld absent).
#   * wek_harden_opts      (CACHE INTERNAL) — compile-flag list, applied per
#                            target via target_compile_options(<tgt> PRIVATE
#                            ${wek_harden_opts}).  Empty unless WEK_HARDEN=ON.
#   * wek_harden_link_opts (CACHE INTERNAL) — link-flag list, applied per
#                            target via target_link_options.  Empty unless
#                            WEK_HARDEN=ON.
#
# NEVER add these via a global add_compile_options — third_party/ is pulled
# in via include_directories(SYSTEM ...) with relaxed flags, and tightening
# them here would silently break some upstream headers (FreeType, glslang)
# that don't compile cleanly under _FORTIFY_SOURCE.  The per-target wiring
# at each shippable CMakeLists is the supported pattern.

option(WEK_HARDEN  "Apply release-time hardening flags (stack-protector-strong, _FORTIFY_SOURCE=2, RELRO+now, cf-protection)" OFF)
option(WEK_LTO     "Enable ThinLTO via CMake INTERPROCEDURAL_OPTIMIZATION on Release/RelWithDebInfo (link time +)"            OFF)
option(WEK_USE_LLD "Prefer ld.lld when the compiler is Clang (probed; falls back to default linker if absent)"                ON)

set(_harden_compile "")
set(_harden_link    "")

# Sanitizers and FORTIFY_SOURCE conflict at runtime (ASan re-implements many
# of the libc wrappers FORTIFY hooks into).  Short-circuit hardening when a
# sanitizer leg is active so the two opt-ins don't trip each other.
if(WEK_HARDEN AND NOT WEK_SANITIZE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        list(APPEND _harden_compile
            -fstack-protector-strong
            -D_FORTIFY_SOURCE=2
            -fcf-protection=full)
        list(APPEND _harden_link
            -Wl,-z,relro
            -Wl,-z,now
            -Wl,-z,noexecstack)
        message(STATUS "WEK_HARDEN: ON -- stack-protector-strong, "
                       "_FORTIFY_SOURCE=2, RELRO+now, cf-protection=full")
    else()
        message(WARNING
            "WEK_HARDEN set but compiler is '${CMAKE_CXX_COMPILER_ID}' "
            "(expected Clang/GNU) -- hardening flags not applied.")
    endif()
elseif(WEK_HARDEN AND WEK_SANITIZE)
    message(STATUS
        "WEK_HARDEN: short-circuited because WEK_SANITIZE=${WEK_SANITIZE} "
        "(FORTIFY_SOURCE + sanitizers conflict at runtime)")
endif()

set(wek_harden_opts      "${_harden_compile}"
    CACHE INTERNAL "Hardening compile flags for first-party shippable targets")
set(wek_harden_link_opts "${_harden_link}"
    CACHE INTERNAL "Hardening link flags for first-party shippable targets")

# ----- ThinLTO -----------------------------------------------------------------
# Drive CMake's INTERPROCEDURAL_OPTIMIZATION when WEK_LTO is requested.  Use
# check_ipo_supported so a toolchain that doesn't actually support IPO emits
# a clear warning instead of a late link error.
if(WEK_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg)
    if(_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
        message(STATUS "WEK_LTO: ON -- ThinLTO active "
                       "(CMAKE_INTERPROCEDURAL_OPTIMIZATION=TRUE)")
    else()
        message(WARNING "WEK_LTO=ON but the toolchain reports IPO unsupported: ${_ipo_msg}")
    endif()
endif()

# ----- Linker ------------------------------------------------------------------
# ld.lld is significantly faster than BFD on incremental rebuilds and supports
# every flag in wek_harden_link_opts.  Probe before adding -fuse-ld=lld so a
# host without lld silently falls back to the default linker.
if(WEK_USE_LLD AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    find_program(_wek_lld_path NAMES ld.lld lld)
    if(_wek_lld_path)
        add_link_options(-fuse-ld=lld)
        message(STATUS "WEK_USE_LLD: ON -- using ${_wek_lld_path}")
    else()
        message(STATUS "WEK_USE_LLD: ON but ld.lld not found in PATH -- falling back to default linker")
    endif()
endif()
