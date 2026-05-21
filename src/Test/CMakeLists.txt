enable_testing()

# WPTextRenderer.cpp (pulled in for the WPSceneParser::Parse end-to-end test)
# rasterizes glyphs via FreeType.  find_package here so the Freetype::Freetype
# imported target is visible in this subdirectory scope (the one in
# src/CMakeLists.txt is a sibling and doesn't propagate the imported target).
find_package(Freetype REQUIRED)

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
    test_StringUtils.cpp
    test_Algorism.cpp
    test_WPUserProperties.cpp
    test_ParticleModify.cpp
    test_ParticleSystem.cpp
    test_ParticleTrail.cpp
    test_AudioRateMultiplier.cpp
    test_WPPuppet.cpp
    test_WPSceneAttachmentCompose.cpp
    test_WPTexImageHelpers.cpp
    test_WPTextLayer.cpp
    test_SceneCamera.cpp
    test_SceneNode.cpp
    test_SceneLight.cpp
    test_SceneMesh.cpp
    test_SceneShader.cpp
    test_Scene.cpp
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
    test_AudioAnalyzer.cpp
    test_SoundManager.cpp
    test_AudioCapture.cpp
    test_BinaryStream.cpp
    test_VFS.cpp
    test_SoundStreamSeek.cpp
    test_WPVolumeAnimation.cpp
    test_WPPropertyAnimation.cpp
    test_WPMdlParser.cpp
    test_WPMdlParserFormats.cpp
    test_SceneProfiler.cpp
    test_PassCacheAlias.cpp
    test_BlendModeFactors.cpp
    test_TexFormatVk.cpp
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
    test_WPParticleOperators.cpp
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
    test_WPSceneParse.cpp
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
    # End-to-end WPSceneParser::Parse() coverage (test_WPSceneParse.cpp) needs
    # the parser TU plus its transitive non-Vulkan deps.  WPShaderParser.cpp
    # transitively references glslang (host-CPU SPIR-V toolchain, NOT a Vulkan
    # device) via Vulkan/ShaderComp.hpp — satisfied by linking wpVulkan below,
    # mirroring fuzz_WPShaderParser.  No vkCreate*-class device symbol is
    # pulled in (the fixture scene loads no materials/shaders).
    ${SRC_DIR}/WPSceneParser.cpp
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
)

target_link_libraries(backend_scene_tests PRIVATE
    wpUtils
    wpFs
    wpAudio
    wpParticle
    # wpVulkan supplies the glslang/SPIRV-Reflect symbols WPShaderParser.cpp
    # references (CompileToSpv / Init/FinalGlslang).  It is the host-CPU shader
    # toolchain, not a Vulkan device — the binary stays headless-safe.
    wpVulkan
    # WPTextRenderer.cpp rasterizes via FreeType.
    Freetype::Freetype
    ${LZ4_LIBRARIES}
    Eigen3::Eigen
    nlohmann_json
)

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
)
target_include_directories(backend_scene_thread_tests PRIVATE
    ${SRC_DIR}
    ${SRC_DIR}/Scene/include
    ${SRC_DIR}/Scene/include/Scene
)
target_link_libraries(backend_scene_thread_tests PRIVATE
    wpScene wpUtils wpFs Eigen3::Eigen nlohmann_json Threads::Threads
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

    # Convenience: build all fuzz harnesses with one target.
    add_custom_target(fuzzers DEPENDS
        fuzz_WPMdlParser fuzz_WPTexImageParser fuzz_WPPkgFs
        fuzz_WPShaderParser fuzz_WPSceneParser fuzz_WPParticleParser
        fuzz_WPSoundParser)
endif()
