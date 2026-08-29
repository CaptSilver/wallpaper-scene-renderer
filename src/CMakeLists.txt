find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
find_package(Freetype REQUIRED)
pkg_check_modules(LZ4 REQUIRED liblz4)
pkg_check_modules(MPV REQUIRED mpv)
pkg_check_modules(EGL egl)
pkg_check_modules(GL gl)
pkg_check_modules(GBM gbm)
pkg_check_modules(LIBPULSE libpulse)

# HWVideoTextureDecoder.cpp includes EGL/egl.h, GL/gl.h and gbm.h unconditionally,
# so all three must be present or the translation unit will not compile.  Guarding
# on any single one of them breaks the configurations missing the others: EGL alone
# fails on a box with the proprietary NVIDIA stack and no mesa-libgbm-devel, GBM
# alone fails wherever gbm.pc is installed without egl.pc.  They are independent
# pkg-config probes, so one does not imply the others.  Decide once here and use
# the result for both the source guard and the feature macro, so the two can never
# disagree.
if(EGL_FOUND AND GL_FOUND AND GBM_FOUND)
    set(WEK_HAVE_EGL_HWDEC ON)
else()
    set(WEK_HAVE_EGL_HWDEC OFF)
endif()

option(ENABLE_RENDERDOC "Build with renderdoc api" OFF)

if(ENABLE_RENDERDOC)
  add_compile_definitions(ENABLE_RENDERDOC_API=1)
endif()

include(TestBigEndian)
test_big_endian(ENDIAN)
if(ENDIAN)
  add_compile_definitions(WP_BIG_ENDIAN)
endif()

# Renderer-lib warning set.  Richer than the shippable-target wek_warn_opts: it
# opts into the -Wconversion / -Wsign-conversion family that catches the
# recurring HLSL->GLSL int-promotion bug class.  WEK_WERROR_FLAGS (from
# cmake/WekWarnings.cmake, included at the submodule root before this dir) is the
# -Werror tail: empty unless WEK_WERROR=ON, and even then keeps the conversion
# family warning-only (-Wno-error=conversion/-Wno-error=sign-conversion).
set(warn_opts
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wno-unused-variable
    ${WEK_WERROR_FLAGS}
    CACHE INTERNAL "")

add_subdirectory(Fs)
add_subdirectory(Utils)
add_subdirectory(Timer)
add_subdirectory(Audio)
add_subdirectory(Scene)
add_subdirectory(Looper)
add_subdirectory(Vulkan)
add_subdirectory(Particle)
add_subdirectory(VulkanRender)
add_subdirectory(RenderGraph)

add_library(
  ${PROJECT_NAME} STATIC
  WPJson.cpp
  WPPkgFs.cpp
  Type.cpp
  wpscene/WPImageObject.cpp
  wpscene/WPParticleObject.cpp
  wpscene/WPMaterial.cpp
  wpscene/WPScene.cpp
  wpscene/WPLightObject.cpp
  wpscene/WPModelObject.cpp
  wpscene/WPTextObject.cpp
  WPTextRenderer.cpp
  WPParticleParser.cpp
  WPShaderParser.cpp
  WPSceneParser.cpp
  SystemFontFallback.cpp
  WPShaderValueUpdater.cpp
  WPTexImageParser.cpp
  WPSoundParser.cpp
  WPMdlParser.cpp
  WPPuppet.cpp
  VideoTextureDecoder.cpp
  $<$<BOOL:${WEK_HAVE_EGL_HWDEC}>:HWVideoTextureDecoder.cpp>
  SceneWallpaper.cpp)

set(InteralLib
    wpAudio
    wpLooper
    wpVulkanRender
    wpFs
    wpScene
    wpVulkan
    wpRGraph
    wpParticle
    wpTimer)

target_link_libraries(
  ${PROJECT_NAME}
  PUBLIC wpUtils
  PRIVATE ${InteralLib} nlohmann_json Eigen3::Eigen ${LZ4_LIBRARIES} ${MPV_LIBRARIES}
          $<$<BOOL:${EGL_FOUND}>:${EGL_LIBRARIES}>
          $<$<BOOL:${GL_FOUND}>:${GL_LIBRARIES}>
          $<$<BOOL:${GBM_FOUND}>:${GBM_LIBRARIES}>
          Freetype::Freetype ${FONTCONFIG_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT} ${CMAKE_DL_LIBS})

if(WEK_HAVE_EGL_HWDEC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_EGL_HWDEC=1)
    message(STATUS "HW video texture decoder: enabled (EGL + GL + GBM)")
else()
    message(STATUS "HW video texture decoder: disabled (EGL/GL/GBM not found)")
endif()

target_include_directories(${PROJECT_NAME} PUBLIC . Swapchain PRIVATE ${FONTCONFIG_INCLUDE_DIRS})
set_property(TARGET ${PROJECT_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
