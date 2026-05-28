# WekWerrorScoped.cmake — scoped -Werror for the audited-clean shippable targets.
#
# CLAUDE.md inventories four shippable / dlopen'd targets that are -Wall -Wextra
# clean and therefore safe to treat warnings-as-errors for on every push:
#   * WallpaperEngineKde       — the plugin .so loaded into plasmashell
#   * mpvbackend               — video-backend static lib linked into the .so
#   * wescene-renderer-qml     — QML bridge to the scene renderer
#   * wpParticle               — particle subsystem (renderer-libs sibling)
# The wider renderer libs (VulkanRender / Vulkan / RenderGraph / Scene / Audio)
# still trip -Wall/-Wextra and need a separate audit pass before -Werror can
# gate; until then, the project-wide -DWEK_WERROR=ON path is the opt-in audit
# leg (WekWarnings.cmake) and this helper applies -Werror to JUST the four
# audited-clean targets as a default-gate fatal step.
#
# When -DWEK_WERROR=ON is set explicitly, this helper is a no-op so the audit
# build does not double-apply -Werror; the audit's full-project -Werror already
# covers every target.  The signed/unsigned diagnostic family stays warning-
# only here as well, matching the WEK_WERROR policy.
#
# Include from the SAME canonical site (root CMakeLists.txt) the WekWarnings /
# WekSanitize files come from, AFTER add_subdirectory(src) so all four targets
# already exist when the foreach() runs.

if(NOT WEK_WERROR
   AND (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU"))
    set(_wek_werror_scoped_targets
        WallpaperEngineKde
        mpvbackend
        wescene-renderer-qml
        wpParticle)
    set(_wek_werror_scoped_flags
        -Werror
        -Wno-error=conversion
        -Wno-error=sign-conversion
        -Wno-error=sign-compare
        # rpmbuild injects gcc-flavour `-specs=...` hardening + annobin flags
        # the host toolchain pipeline doesn't recognise.  Under clang those
        # silently warn as -Wunused-command-line-argument; -Werror would then
        # promote that to a fatal during `rpmbuild`.  Keep the diagnostic
        # warning-only so the RPM build still proceeds while a -Wall regression
        # in our own code remains gating.
        -Wno-error=unused-command-line-argument)
    foreach(_t IN LISTS _wek_werror_scoped_targets)
        if(TARGET ${_t})
            target_compile_options(${_t} PRIVATE ${_wek_werror_scoped_flags})
        endif()
    endforeach()
    unset(_wek_werror_scoped_targets)
    unset(_wek_werror_scoped_flags)
endif()
