find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
find_package(Freetype REQUIRED)
pkg_check_modules(LZ4 REQUIRED liblz4)
pkg_check_modules(MPV REQUIRED mpv)
pkg_check_modules(EGL egl)
pkg_check_modules(GL gl)
pkg_check_modules(GBM gbm)

option(ENABLE_RENDERDOC "Build with renderdoc api" OFF)

if(ENABLE_RENDERDOC)
  add_compile_definitions(ENABLE_RENDERDOC_API=1)
endif()

include(TestBigEndian)
test_big_endian(ENDIAN)
if(ENDIAN)
  add_compile_definitions(WP_BIG_ENDIAN)
endif()

set(warn_opts
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wno-unused-variable
    CACHE INTERNAL "")

include_directories(.)

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
  WPShaderValueUpdater.cpp
  WPTexImageParser.cpp
  WPSoundParser.cpp
  WPMdlParser.cpp
  WPPuppet.cpp
  VideoTextureDecoder.cpp
  $<$<BOOL:${EGL_FOUND}>:HWVideoTextureDecoder.cpp>
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
  PRIVATE ${InteralLib} nlohmann_json ${LZ4_LIBRARIES} ${MPV_LIBRARIES}
          $<$<BOOL:${EGL_FOUND}>:${EGL_LIBRARIES}>
          $<$<BOOL:${GL_FOUND}>:${GL_LIBRARIES}>
          $<$<BOOL:${GBM_FOUND}>:${GBM_LIBRARIES}>
          Freetype::Freetype ${CMAKE_THREAD_LIBS_INIT} ${CMAKE_DL_LIBS})

if(EGL_FOUND AND GL_FOUND AND GBM_FOUND)
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_EGL_HWDEC=1)
    message(STATUS "HW video texture decoder: enabled (EGL + GL)")
else()
    message(STATUS "HW video texture decoder: disabled (EGL/GL/GBM not found)")
endif()

target_include_directories(${PROJECT_NAME} PUBLIC . Swapchain)
set_property(TARGET ${PROJECT_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
