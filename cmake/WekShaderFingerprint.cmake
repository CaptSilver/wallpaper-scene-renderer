# WekShaderFingerprint.cmake -- identifies the shader translator build.
#
# Compiled SPIR-V is cached on disk under a key derived from the preprocessed
# shader source, which is hashed BEFORE the HLSL->GLSL translation runs.  A
# translator fix therefore never reached anyone with a warm cache.  The cache
# key is salted with WEK_SHADER_TRANSLATOR_FINGERPRINT, a SHA-1 over every file
# whose content decides the translated / compiled output, so editing any of
# them invalidates old entries.
#
# Included from the submodule's CMakeLists.txt (which the parent project pulls
# in through add_subdirectory), so both build layouts share one definition.
# wek_apply_shader_fingerprint() stamps the define onto WPShaderParser.cpp for
# the targets of the CURRENT directory; call it in every directory that
# compiles that file.

set(_wek_fp_root "${CMAKE_CURRENT_LIST_DIR}/../src")
set(WEK_SHADER_TRANSLATOR_FILES
    # HLSL->GLSL rewrites
    "${_wek_fp_root}/WPShaderTransforms.h"
    # preprocessing, combo handling, the call into glslang
    "${_wek_fp_root}/WPShaderParser.cpp"
    # code prepended to every shader
    "${_wek_fp_root}/WPShaderPreamble.hpp"
    # source scans that decide what gets rewritten / guarded
    "${_wek_fp_root}/ShaderSourceScan.hpp"
    "${_wek_fp_root}/ShaderComboGuard.hpp"
    # glslang invocation: options, message flags, reflection
    "${_wek_fp_root}/Vulkan/Shader.cpp"
    "${_wek_fp_root}/Vulkan/include/Vulkan/ShaderComp.hpp")

set(_wek_fp_all "")
foreach(_f IN LISTS WEK_SHADER_TRANSLATOR_FILES)
    if(NOT EXISTS "${_f}")
        message(FATAL_ERROR "WekShaderFingerprint: missing translator source ${_f}")
    endif()
    file(SHA1 "${_f}" _h)
    string(APPEND _wek_fp_all "${_h}")
endforeach()
string(SHA1 WEK_SHADER_TRANSLATOR_FINGERPRINT "${_wek_fp_all}")
if(NOT WEK_SHADER_TRANSLATOR_FINGERPRINT)
    message(FATAL_ERROR "WekShaderFingerprint: computed an empty translator fingerprint")
endif()
# Editing a fingerprinted file must re-run configure so the value refreshes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${WEK_SHADER_TRANSLATOR_FILES})

function(wek_apply_shader_fingerprint)
    set_source_files_properties("${_wek_fp_root}/WPShaderParser.cpp"
        PROPERTIES COMPILE_DEFINITIONS
        "WEK_SHADER_TRANSLATOR_FINGERPRINT=\"${WEK_SHADER_TRANSLATOR_FINGERPRINT}\"")
endfunction()
