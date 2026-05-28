enable_testing()

# WPTextRenderer.cpp (pulled in for the WPSceneParser::Parse end-to-end test)
# rasterizes glyphs via FreeType.  find_package here so the Freetype::Freetype
# imported target is visible in this subdirectory scope (the one in
# src/CMakeLists.txt is a sibling and doesn't propagate the imported target).
find_package(Freetype REQUIRED)

# libmpv for the VideoTextureDecoder lifecycle test.  The PkgConfig module is
# enabled at the submodule root, so pkg_check_modules works here too — same
# pattern as the sibling src/CMakeLists.txt.  The test binary stays runnable
# without a real video file: the open() branches are gated on WEKDE_HAS_VIDEO;
# the offline cases never touch libmpv at runtime (constructor + destructor +
# triple-buffer indices), they just need the headers/symbols at link time.
pkg_check_modules(MPV REQUIRED mpv)
# EGL + GBM for the HWVideoTextureDecoder test.  These are SIBLINGS of src/
# (which detects them in src/CMakeLists.txt) — that scope isn't visible here,
# so re-probe.  When absent we skip the HW test sources; the SW test still
# builds (it's the bulk of test_VideoTextureDecoder.cpp coverage).  Mirrors
# the gate in src/CMakeLists.txt:91.
pkg_check_modules(EGL egl)
pkg_check_modules(GBM gbm)
pkg_check_modules(GL gl)

set(DOCTEST_INCLUDE_DIR
    "${CMAKE_SOURCE_DIR}/third_party/nlohmann/tests/thirdparty/doctest")
set(SRC_DIR "${CMAKE_SOURCE_DIR}/src")

add_executable(backend_scene_tests
    test_main.cpp
    test_WPTexImageParser.cpp
    test_SpecTexs.cpp
    test_SpriteAnimation.cpp
    test_helpers.cpp
    test_WPShaderTransforms.cpp
    test_WPShaderInclude.cpp
    test_WPShaderPreamble_DepthGather.cpp
    test_StringUtils.cpp
    test_Algorism.cpp
    test_WPUserProperties.cpp
    test_ParticleModify.cpp
    test_ParticleSystem.cpp
    test_ParticleSubSystem_BurstDoneQueue.cpp
    test_ParticleTrail.cpp
    test_SceneClockClamp.cpp
    test_AudioRateMultiplier.cpp
    test_WPPuppet.cpp
    test_WPSceneAttachmentCompose.cpp
    test_WPTexImageHelpers.cpp
    test_WPTextLayer.cpp
    test_WPTextRenderer.cpp
    test_WPTextRendererGlyphCache.cpp
    test_SceneCamera.cpp
    test_SceneNode.cpp
    test_UniformDirtyGate.cpp
    test_FramebufferCache.cpp
    test_StagingReuse.cpp
    test_StagingBufferIncrease.cpp
    test_TransformEpoch.cpp
    test_SceneLight.cpp
    test_SceneMesh.cpp
    test_SceneShader.cpp
    test_Scene.cpp
    test_Scene_VolumetricsTick.cpp
    test_SceneToRenderGraph_Volumetrics.cpp
    test_WPSceneInitJsonHierarchy.cpp
    test_WPSceneGroupParse.cpp
    test_SceneIndexArray.cpp
    test_SceneVertexArray.cpp
    test_WPJson.cpp
    test_WPCommon_OrthoCameraSize.cpp
    test_WPSoundHelpers.cpp
    test_WPSoundParser.cpp
    test_SceneTickHelpers.cpp
    test_FpsCounter.cpp
    test_Logging.cpp
    test_AudioAnalyzer.cpp
    test_SoundManager.cpp
    test_AudioCapture.cpp
    test_AudioCapture_Rebind.cpp
    test_BinaryStream.cpp
    test_VFS.cpp
    test_WPPkgFs.cpp
    ${SRC_DIR}/WPPkgFs.cpp
    test_SoundStreamSeek.cpp
    test_WPVolumeAnimation.cpp
    test_WPPropertyAnimation.cpp
    test_WPMdlParser.cpp
    test_WPMdlParserFormats.cpp
    test_SceneProfiler.cpp
    test_PassCacheAlias.cpp
    test_BlendModeFactors.cpp
    test_CustomShaderPassDesc.cpp
    test_CustomShaderPass_DepthBinding.cpp
    test_CustomShaderPass_ForceClearOutput.cpp
    test_CustomShaderPass_ScratchReuse.cpp
    test_DepthToColorResolvePass.cpp
    test_SceneDepthFallbackGate.cpp
    test_DeviceCaps.cpp
    test_DeviceMemoryBudget.cpp
    test_DepthSamplerConfig.cpp
    test_DepthUsageFlags.cpp
    test_TextureCache_SamplerDedup.cpp
    test_TextureCache_LoadStaging.cpp
    test_TextureCacheQueryLRU.cpp
    test_TexFormatVk.cpp
    test_SwapchainPolicy.cpp
    test_PipelineCacheIO.cpp
    test_Shader.cpp
    test_FenceWaitRetry.cpp
    ${SRC_DIR}/VulkanRender/FenceWaitRetry.cpp
    test_VulkanDebugCallbackGate.cpp
    test_DeviceQueueSelection.cpp
    test_SystemFontFallback.cpp
    ${SRC_DIR}/SystemFontFallback.cpp
    test_SceneImageEffectLayer.cpp
    test_WPCreateLayerScan.cpp
    ${SRC_DIR}/Scene/SceneImageEffectLayer.cpp
    test_WPPropertyScriptExtract.cpp
    test_WPParticleInstanceoverride.cpp
    test_WPSceneFileResolver.cpp
    test_WPMapSequenceParse.cpp
    test_WPRopeCombos.cpp
    test_RopeMedian.cpp
    test_RopeScratchThreadLocal.cpp
    test_WPParticleOperators.cpp
    test_WPRemapValueOps.cpp
    test_WPScenePostprocessing.cpp
    test_WPImageObject.cpp
    test_ParticleEmitter.cpp
    test_Determinism.cpp
    test_WPSoundObject.cpp
    test_WPScene.cpp
    test_WPMaterial.cpp
    test_WPParticleObject.cpp
    test_WPImageObjectExpanded.cpp
    test_WPParticleParserMore.cpp
    test_WPParticleParserGaps.cpp
    test_WPParticleParserDeepKill.cpp
    test_WPParticleParserResidual.cpp
    test_WPParticleParserResidual2.cpp
    test_WPParticleParser_SS8.cpp
    test_WPSceneParse.cpp
    test_WPLightObject.cpp
    test_WPLightKindParse.cpp
    test_WPShaderValueUpdater_Volumetric.cpp
    test_WPShaderValueUpdater_Uniforms.cpp
    test_WPShaderValueUpdater_RtResolution.cpp
    test_UniformLookupHash.cpp
    test_DependencyGraph.cpp
    test_RenderGraph.cpp
    test_WorldCacheGate.cpp
    test_SpriteSnapshotGate.cpp
    test_ImagePayload.cpp
    test_AudioConsumerGate.cpp
    test_TextStyleMerge.cpp
    test_WarmCacheTexSlots.cpp
    test_VideoTextureDecoder.cpp
    # VideoTextureDecoder.cpp: SW video texture decoder (libmpv triple-buffer).
    # Pulled in directly (not via wescene-renderer) because the latter drags
    # the entire Vulkan device chain. The TU's only non-stdlib dep is libmpv,
    # added via MPV_LIBRARIES below.
    ${SRC_DIR}/VideoTextureDecoder.cpp
    # HWVideoTextureDecoder: only included when EGL+GBM+GL are available
    # (same gate as src/CMakeLists.txt for the production lib).  Otherwise
    # we skip the cpp + test_HWVideoTextureDecoder.cpp entirely on systems
    # without EGL/GBM/GL dev libs.
    $<$<AND:$<BOOL:${EGL_FOUND}>,$<BOOL:${GBM_FOUND}>,$<BOOL:${GL_FOUND}>>:test_HWVideoTextureDecoder.cpp>
    $<$<AND:$<BOOL:${EGL_FOUND}>,$<BOOL:${GBM_FOUND}>,$<BOOL:${GL_FOUND}>>:${SRC_DIR}/HWVideoTextureDecoder.cpp>
    # Compile specific source files to avoid pulling in Vulkan
    ${SRC_DIR}/wpscene/WPScene.cpp
    ${SRC_DIR}/wpscene/WPImageObject.cpp
    ${SRC_DIR}/WPTexImageParser.cpp
    ${SRC_DIR}/Type.cpp
    ${SRC_DIR}/WPPuppet.cpp
    ${SRC_DIR}/WPMdlParser.cpp
    ${SRC_DIR}/WPJson.cpp
    ${SRC_DIR}/WPParticleParser.cpp
    ${SRC_DIR}/WPSoundParser.cpp
    ${SRC_DIR}/wpscene/WPParticleObject.cpp
    ${SRC_DIR}/wpscene/WPMaterial.cpp
    ${SRC_DIR}/Scene/SceneCamera.cpp
    ${SRC_DIR}/Scene/SceneNode.cpp
    ${SRC_DIR}/Scene/SceneIndexArray.cpp
    ${SRC_DIR}/Scene/SceneVertexArray.cpp
    ${SRC_DIR}/Scene/SceneShader.cpp
    ${SRC_DIR}/Scene/SceneLight.cpp
    # End-to-end WPSceneParser::Parse() coverage (test_WPSceneParse.cpp) needs
    # the parser TU plus its transitive non-Vulkan deps.  WPShaderParser.cpp
    # transitively references glslang (host-CPU SPIR-V toolchain, NOT a Vulkan
    # device) via Vulkan/ShaderComp.hpp — satisfied by linking wpVulkan below,
    # mirroring fuzz_WPShaderParser.  No vkCreate*-class device symbol is
    # pulled in (the fixture scene loads no materials/shaders).
    ${SRC_DIR}/WPSceneParser.cpp
    ${SRC_DIR}/VulkanRender/VolumetricChain.cpp
    # CustomShaderPass: ctor/dtor + Desc copy live here; required so the test
    # target can instantiate CustomShaderPass through the RenderGraph::addPass
    # template (the volumetric chain emitters do).  The Vulkan device-side code
    # (prepare/execute/destory) is never called from tests, so no real GPU
    # interaction occurs — the heavy headers are compile-only.
    ${SRC_DIR}/VulkanRender/CustomShaderPass.cpp
    # Tiny stubs for the per-pass RT dump globals (live in VulkanRender.cpp,
    # which is too heavy to pull in to the headless test binary).  They are
    # never read — CustomShaderPass::execute() is the sole consumer and tests
    # never call it.
    test_CustomShaderPass_stubs.cpp
    ${SRC_DIR}/WPShaderParser.cpp
    ${SRC_DIR}/WPShaderValueUpdater.cpp
    ${SRC_DIR}/WPTextRenderer.cpp
    ${SRC_DIR}/wpscene/WPLightObject.cpp
    ${SRC_DIR}/wpscene/WPModelObject.cpp
    ${SRC_DIR}/wpscene/WPTextObject.cpp
)

target_include_directories(backend_scene_tests PRIVATE
    ${SRC_DIR}
    ${SRC_DIR}/Scene/include
    ${SRC_DIR}/Scene/include/Scene
    ${SRC_DIR}/Particle/include
    ${SRC_DIR}/VulkanRender/include
    ${SRC_DIR}/Vulkan/include
    ${DOCTEST_INCLUDE_DIR}
    # libmpv headers (mpv/client.h, mpv/render.h) — VideoTextureDecoder.cpp.
    ${MPV_INCLUDE_DIRS}
)
target_include_directories(backend_scene_tests SYSTEM PRIVATE
    "${CMAKE_SOURCE_DIR}/third_party")

target_link_libraries(backend_scene_tests PRIVATE
    wpUtils
    wpFs
    wpAudio
    wpParticle
    # RenderGraph (DependencyGraph cycle detection / topo sort). Vulkan-free
    # static lib — its .cpp files include only their own headers + Logging.
    wpRGraph
    # wpVulkan supplies the glslang/SPIRV-Reflect symbols WPShaderParser.cpp
    # references (CompileToSpv / Init/FinalGlslang).  It is the host-CPU shader
    # toolchain, not a Vulkan device — the binary stays headless-safe.
    wpVulkan
    # WPTextRenderer.cpp rasterizes via FreeType.
    Freetype::Freetype
    ${LZ4_LIBRARIES}
    Eigen3::Eigen
    nlohmann_json
    # libmpv — VideoTextureDecoder.cpp (test_VideoTextureDecoder.cpp).
    ${MPV_LIBRARIES}
    # EGL + GBM + GL — HWVideoTextureDecoder.cpp (test_HWVideoTextureDecoder.cpp).
    # The test only exercises initEGL (return-false path under WEK_HW_DECODE_DRI_PATH),
    # but HWVideoTextureDecoder.cpp references glGenTextures/glReadPixels/etc
    # so the linker needs the GL lib at link time even if those code paths
    # never execute.
    $<$<BOOL:${EGL_FOUND}>:${EGL_LIBRARIES}>
    $<$<BOOL:${GBM_FOUND}>:${GBM_LIBRARIES}>
    $<$<BOOL:${GL_FOUND}>:${GL_LIBRARIES}>
)

# HAVE_EGL_HWDEC matches the production gate (src/CMakeLists.txt:92); without
# it the HWVideoTextureDecoder.hpp/.cpp pieces are referenced but no callers
# in tests depend on the symbol — this define is informational here, but kept
# parallel with production so a future test-side HW path activation is one-step.
if(EGL_FOUND AND GBM_FOUND AND GL_FOUND)
    target_compile_definitions(backend_scene_tests PRIVATE HAVE_EGL_HWDEC=1)
endif()

target_compile_options(backend_scene_tests PRIVATE ${warn_opts})

# Opt-in sanitizers (WEK_SANITIZE=address,undefined | thread). No-op when empty.
wek_apply_sanitizers(backend_scene_tests)

if(MUTATION_TESTING)
    target_compile_options(backend_scene_tests PRIVATE
        -fpass-plugin=${MULL_PLUGIN_PATH}
        -g -grecord-command-line
        -DWP_SUPPRESS_DEBUG_LOGGING
    )
endif()

add_test(NAME backend_scene_tests COMMAND backend_scene_tests)
# Backstop against a wedged run (e.g. a device-enumeration block that slips
# the WEKDE_HAS_AUDIO_DEVICE env gate).  The non-blocking suite finishes in
# seconds; 120s leaves headroom for a real audio-enabled local run.  NOTE:
# this only protects ctest-driven runs — preflight.sh invokes the binary
# BARE, so the env gate in the audio test cases is the load-bearing fix.
set_tests_properties(backend_scene_tests PROPERTIES TIMEOUT 120)

# Expose wp-pkg binary path to tests when BUILD_TOOLS is on.  The wp-pkg pack
# format suite shells out to the real binary; when the tool isn't built (the
# default, including CI) the suite skips at runtime instead of hardcoding a
# user-specific absolute path.
if(BUILD_TOOLS AND TARGET wp-pkg)
    target_compile_definitions(backend_scene_tests PRIVATE
        WP_PKG_TOOL_PATH="$<TARGET_FILE:wp-pkg>")
    add_dependencies(backend_scene_tests wp-pkg)
endif()

# Qt-dependent tests (SceneScript timers, device detection)
find_package(Qt6 COMPONENTS Core Qml QUIET)
if(Qt6_FOUND)
    add_executable(scenescript_tests
        test_main_qt.cpp
        test_SceneScript.cpp
        ${CMAKE_SOURCE_DIR}/qml_helper/SceneTimerBridge.h
    )
    set_target_properties(scenescript_tests PROPERTIES AUTOMOC ON)
    target_include_directories(scenescript_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/qml_helper
        ${DOCTEST_INCLUDE_DIR}
    )
    target_link_libraries(scenescript_tests PRIVATE Qt6::Core Qt6::Qml)
    wek_apply_sanitizers(scenescript_tests)
    add_test(NAME scenescript_tests COMMAND scenescript_tests)
    set_tests_properties(scenescript_tests PROPERTIES TIMEOUT 120)
endif()

# ── Thread-safety repro (run under -DWEK_SANITIZE=thread) ─────────────────────
# Focused multithread exerciser for the B5 races.  Links wpScene (Scene +
# SceneNode, NO Vulkan) so it drives the REAL parent-tree fix (B5b), plus a
# generic atomic<shared_ptr> stand-in for the B5a pattern (SceneWallpaper.cpp
# can't link without Vulkan).  Builds in any config; only meaningful under TSAN.
# WEK_THREAD_REPRO_PROVE_RACE=ON swaps repro 1 to a deliberately racy plain
# shared_ptr to demonstrate the harness bites (TSAN should then report a race).
add_executable(backend_scene_thread_tests
    test_thread_repro.cpp
    # WPShaderParser.cpp + WPJson.cpp compiled directly (the glslang concurrency
    # repro needs CompileToSpv/InitGlslang).  Mirrors the fuzz_WPShaderCompile
    # link set instead of dragging the whole wescene-renderer lib in — wpVulkan
    # supplies the glslang/SPIRV-Reflect symbols (host-CPU toolchain, no device).
    ${SRC_DIR}/WPShaderParser.cpp
    ${SRC_DIR}/WPJson.cpp
)
target_include_directories(backend_scene_thread_tests PRIVATE
    ${SRC_DIR}
    ${SRC_DIR}/Scene/include
    ${SRC_DIR}/Scene/include/Scene
)
target_link_libraries(backend_scene_thread_tests PRIVATE
    wpScene wpUtils wpFs wpVulkan Eigen3::Eigen nlohmann_json Threads::Threads
)
option(WEK_THREAD_REPRO_PROVE_RACE
       "Compile the thread repro with a deliberately racy plain shared_ptr \
(demonstrates TSAN bites; leave OFF for the clean regression run)" OFF)
if(WEK_THREAD_REPRO_PROVE_RACE)
    target_compile_definitions(backend_scene_thread_tests PRIVATE
        WEK_THREAD_REPRO_PROVE_RACE)
endif()
wek_apply_sanitizers(backend_scene_thread_tests)
add_test(NAME backend_scene_thread_tests COMMAND backend_scene_thread_tests)
set_tests_properties(backend_scene_thread_tests PROPERTIES TIMEOUT 300)

# ── Coverage report target ────────────────────────────────────────────────────
if(COVERAGE)
    find_program(LLVM_PROFDATA llvm-profdata REQUIRED)
    find_program(LLVM_COV      llvm-cov      REQUIRED)

    set(_cov_dir  ${CMAKE_BINARY_DIR}/coverage)
    set(_cov_prof ${_cov_dir}/merged.profdata)
    set(_cov_objs $<TARGET_FILE:backend_scene_tests>)
    set(_cov_tests backend_scene_tests)
    if(TARGET scenescript_tests)
        list(APPEND _cov_objs -object=$<TARGET_FILE:scenescript_tests>)
        list(APPEND _cov_tests scenescript_tests)
    endif()

    # Only report on first-party backend_scene source — strip third_party,
    # Qt/system headers, the tests themselves, and generated moc files.
    set(_cov_ignore
        -ignore-filename-regex=\(^/usr/\|/Qt6/\|/third_party/\|/src/Test/\|/moc_\|_autogen/\)
    )

    add_custom_target(coverage
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${_cov_dir}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_cov_dir}
        COMMAND ${CMAKE_COMMAND} -E env
                LLVM_PROFILE_FILE=${_cov_dir}/%p.profraw
                ${CMAKE_CTEST_COMMAND} --output-on-failure
        COMMAND ${CMAKE_COMMAND} -E env bash -c
                "${LLVM_PROFDATA} merge -sparse ${_cov_dir}/*.profraw -o ${_cov_prof}"
        COMMAND ${LLVM_COV} report ${_cov_objs}
                -instr-profile=${_cov_prof} ${_cov_ignore}
        DEPENDS ${_cov_tests}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running backend_scene tests with coverage and generating report"
        VERBATIM
        USES_TERMINAL
    )

    add_custom_target(coverage-html
        COMMAND ${LLVM_COV} show ${_cov_objs}
                -instr-profile=${_cov_prof} ${_cov_ignore}
                -format=html -output-dir=${_cov_dir}/html
                -show-line-counts-or-regions
        DEPENDS coverage
        COMMENT "Generating HTML coverage report in ${_cov_dir}/html"
        VERBATIM
    )
endif()

# ── Fuzzers (libFuzzer) ──────────────────────────────────────────────────────
# Coverage-guided fuzz harnesses around binary-format parsers. Off by default;
# enable with -DBUILD_FUZZERS=ON (requires Clang). Each harness is a separate
# executable so libFuzzer's main isn't linked into the regular test binary.
if(BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "BUILD_FUZZERS requires Clang (found ${CMAKE_CXX_COMPILER_ID})")
    endif()

    set(_fuzz_san_opts -fsanitize=fuzzer,address,undefined
                       -fno-omit-frame-pointer -g)

    # EXCLUDE_FROM_ALL: fuzz binaries are slow to link (sanitizer runtimes) and
    # most builds don't need them. Build via `cmake --build ... --target fuzzers`
    # or by individual target name.
    add_executable(fuzz_WPMdlParser EXCLUDE_FROM_ALL
        fuzz/fuzz_WPMdlParser.cpp
        ${SRC_DIR}/WPMdlParser.cpp
        ${SRC_DIR}/WPPuppet.cpp
        ${SRC_DIR}/Type.cpp
        ${SRC_DIR}/WPJson.cpp
        ${SRC_DIR}/wpscene/WPMaterial.cpp
        ${SRC_DIR}/Scene/SceneShader.cpp
        ${SRC_DIR}/Scene/SceneVertexArray.cpp
        ${SRC_DIR}/Scene/SceneIndexArray.cpp
    )
    target_include_directories(fuzz_WPMdlParser PRIVATE
        ${SRC_DIR}
        ${SRC_DIR}/Scene/include
        ${SRC_DIR}/Scene/include/Scene
    )
    target_link_libraries(fuzz_WPMdlParser PRIVATE
        wpUtils wpFs Eigen3::Eigen nlohmann_json
    )
    target_compile_options(fuzz_WPMdlParser PRIVATE ${_fuzz_san_opts})
    target_link_options(fuzz_WPMdlParser PRIVATE ${_fuzz_san_opts})

    add_executable(fuzz_WPTexImageParser EXCLUDE_FROM_ALL
        fuzz/fuzz_WPTexImageParser.cpp
        ${SRC_DIR}/WPTexImageParser.cpp
        ${SRC_DIR}/Type.cpp
    )
    target_include_directories(fuzz_WPTexImageParser PRIVATE
        ${SRC_DIR}
        ${SRC_DIR}/Scene/include
    )
    # WPTexImageParser.cpp includes <stb_image.h>. The directory-scope
    # include_directories(SYSTEM third_party) that used to satisfy this was
    # removed in 3c55dea (target-scoped refactor); restore the path on this
    # specific target. Mirrors the pattern backend_scene_tests uses.
    target_include_directories(fuzz_WPTexImageParser SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/third_party")
    target_link_libraries(fuzz_WPTexImageParser PRIVATE
        wpUtils wpFs ${LZ4_LIBRARIES} nlohmann_json
    )
    target_compile_options(fuzz_WPTexImageParser PRIVATE ${_fuzz_san_opts})
    target_link_options(fuzz_WPTexImageParser PRIVATE ${_fuzz_san_opts})

    add_executable(fuzz_WPPkgFs EXCLUDE_FROM_ALL
        fuzz/fuzz_WPPkgFs.cpp
        ${SRC_DIR}/WPPkgFs.cpp
    )
    target_include_directories(fuzz_WPPkgFs PRIVATE ${SRC_DIR})
    target_link_libraries(fuzz_WPPkgFs PRIVATE wpUtils wpFs)
    target_compile_options(fuzz_WPPkgFs PRIVATE ${_fuzz_san_opts})
    target_link_options(fuzz_WPPkgFs PRIVATE ${_fuzz_san_opts})

    # WPShaderParser.cpp transitively references glslang via Vulkan/ShaderComp.hpp,
    # even though only CompileToSpv actually uses it. Link wpVulkan to satisfy
    # those symbols (heavier than the other harnesses but unavoidable without
    # splitting the .cpp).
    add_executable(fuzz_WPShaderParser EXCLUDE_FROM_ALL
        fuzz/fuzz_WPShaderParser.cpp
        ${SRC_DIR}/WPShaderParser.cpp
        ${SRC_DIR}/WPJson.cpp
    )
    target_include_directories(fuzz_WPShaderParser PRIVATE
        ${SRC_DIR}
        ${SRC_DIR}/Scene/include
    )
    target_link_libraries(fuzz_WPShaderParser PRIVATE
        wpUtils wpFs wpVulkan nlohmann_json
    )
    target_compile_options(fuzz_WPShaderParser PRIVATE ${_fuzz_san_opts})
    target_link_options(fuzz_WPShaderParser PRIVATE ${_fuzz_san_opts})

    # Drives the glslang CompileToSpv path (no cache dir -> synchronous compile),
    # which fuzz_WPShaderParser stops short of.  Same sources + link set;
    # wpVulkan satisfies the glslang/SPIRV-Reflect symbols (host-CPU toolchain,
    # no Vulkan device).
    add_executable(fuzz_WPShaderCompile EXCLUDE_FROM_ALL
        fuzz/fuzz_WPShaderCompile.cpp
        ${SRC_DIR}/WPShaderParser.cpp
        ${SRC_DIR}/WPJson.cpp
    )
    target_include_directories(fuzz_WPShaderCompile PRIVATE
        ${SRC_DIR}
        ${SRC_DIR}/Scene/include
    )
    target_link_libraries(fuzz_WPShaderCompile PRIVATE
        wpUtils wpFs wpVulkan nlohmann_json
    )
    target_compile_options(fuzz_WPShaderCompile PRIVATE ${_fuzz_san_opts})
    target_link_options(fuzz_WPShaderCompile PRIVATE ${_fuzz_san_opts})

    # Fuzzes the JSON-driven object-construction root of the scene parser
    # (wpscene::WPScene::FromJson). Skips VFS / SoundManager / sub-parsers —
    # those are fuzzed separately. Targets the JSON-count-driven resize sites.
    add_executable(fuzz_WPSceneParser EXCLUDE_FROM_ALL
        fuzz/fuzz_WPSceneParser.cpp
        ${SRC_DIR}/wpscene/WPScene.cpp
        ${SRC_DIR}/wpscene/WPImageObject.cpp
        ${SRC_DIR}/wpscene/WPParticleObject.cpp
        ${SRC_DIR}/wpscene/WPMaterial.cpp
        ${SRC_DIR}/wpscene/WPLightObject.cpp
        ${SRC_DIR}/wpscene/WPModelObject.cpp
        ${SRC_DIR}/wpscene/WPTextObject.cpp
        ${SRC_DIR}/Type.cpp
        ${SRC_DIR}/WPJson.cpp
    )
    target_include_directories(fuzz_WPSceneParser PRIVATE
        ${SRC_DIR}
        ${SRC_DIR}/Scene/include
    )
    target_link_libraries(fuzz_WPSceneParser PRIVATE
        wpUtils wpFs nlohmann_json
    )
    target_compile_options(fuzz_WPSceneParser PRIVATE ${_fuzz_san_opts})
    target_link_options(fuzz_WPSceneParser PRIVATE ${_fuzz_san_opts})

    add_executable(fuzz_WPParticleParser EXCLUDE_FROM_ALL
        fuzz/fuzz_WPParticleParser.cpp
        ${SRC_DIR}/WPParticleParser.cpp
        ${SRC_DIR}/wpscene/WPParticleObject.cpp
        ${SRC_DIR}/wpscene/WPMaterial.cpp
        ${SRC_DIR}/Type.cpp
        ${SRC_DIR}/WPJson.cpp
    )
    target_include_directories(fuzz_WPParticleParser PRIVATE
        ${SRC_DIR}
        ${SRC_DIR}/Scene/include
        ${SRC_DIR}/Particle/include
    )
    # WPParticleParser.cpp transitively pulls Core/Random.hpp, which includes
    # <effolkronium/random.hpp> in third_party. wpUtils marks third_party as
    # SYSTEM PRIVATE so the path does not propagate to consumers; restore it on
    # this target. Same pattern as fuzz_WPTexImageParser above.
    target_include_directories(fuzz_WPParticleParser SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/third_party")
    target_link_libraries(fuzz_WPParticleParser PRIVATE
        wpUtils wpFs wpParticle nlohmann_json
    )
    target_compile_options(fuzz_WPParticleParser PRIVATE ${_fuzz_san_opts})
    target_link_options(fuzz_WPParticleParser PRIVATE ${_fuzz_san_opts})

    # WPSoundObject is header-only; no parser .cpp to compile in.
    # Just need WPJson for the GET_JSON_* helpers used by FromJson.
    add_executable(fuzz_WPSoundParser EXCLUDE_FROM_ALL
        fuzz/fuzz_WPSoundParser.cpp
        ${SRC_DIR}/WPJson.cpp
    )
    target_include_directories(fuzz_WPSoundParser PRIVATE ${SRC_DIR})
    target_link_libraries(fuzz_WPSoundParser PRIVATE
        wpUtils wpFs nlohmann_json
    )
    target_compile_options(fuzz_WPSoundParser PRIVATE ${_fuzz_san_opts})
    target_link_options(fuzz_WPSoundParser PRIVATE ${_fuzz_san_opts})

    # Drives the SAX-driven ParseJson gate (depth / element / byte caps +
    # lenient cleanup pipeline).  Small TU set: only WPJson.cpp itself.
    add_executable(fuzz_WPJsonParse EXCLUDE_FROM_ALL
        fuzz/fuzz_WPJsonParse.cpp
        ${SRC_DIR}/WPJson.cpp
    )
    target_include_directories(fuzz_WPJsonParse PRIVATE ${SRC_DIR})
    target_link_libraries(fuzz_WPJsonParse PRIVATE
        wpUtils wpFs nlohmann_json
    )
    target_compile_options(fuzz_WPJsonParse PRIVATE ${_fuzz_san_opts})
    target_link_options(fuzz_WPJsonParse PRIVATE ${_fuzz_san_opts})

    # Convenience: build all fuzz harnesses with one target.
    add_custom_target(fuzzers DEPENDS
        fuzz_WPMdlParser fuzz_WPTexImageParser fuzz_WPPkgFs
        fuzz_WPShaderParser fuzz_WPShaderCompile fuzz_WPSceneParser
        fuzz_WPParticleParser fuzz_WPSoundParser fuzz_WPJsonParse)
endif()
