enable_testing()

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
    test_WPPuppet.cpp
    test_WPTexImageHelpers.cpp
    test_WPTextLayer.cpp
    test_SceneCamera.cpp
    test_SceneNode.cpp
    test_SceneLight.cpp
    test_SceneMesh.cpp
    test_SceneShader.cpp
    test_Scene.cpp
    test_SceneIndexArray.cpp
    test_SceneVertexArray.cpp
    test_WPJson.cpp
    test_WPSoundHelpers.cpp
    test_SceneTickHelpers.cpp
    test_AudioAnalyzer.cpp
    test_SoundManager.cpp
    test_BinaryStream.cpp
    test_VFS.cpp
    test_SoundStreamSeek.cpp
    test_WPVolumeAnimation.cpp
    test_WPPropertyAnimation.cpp
    test_WPMdlParser.cpp
    test_WPMdlParserFormats.cpp
    test_SceneProfiler.cpp
    test_PassCacheAlias.cpp
    # Compile specific source files to avoid pulling in Vulkan
    ${SRC_DIR}/WPTexImageParser.cpp
    ${SRC_DIR}/Type.cpp
    ${SRC_DIR}/WPPuppet.cpp
    ${SRC_DIR}/WPMdlParser.cpp
    ${SRC_DIR}/WPJson.cpp
    ${SRC_DIR}/Scene/SceneCamera.cpp
    ${SRC_DIR}/Scene/SceneNode.cpp
    ${SRC_DIR}/Scene/SceneIndexArray.cpp
    ${SRC_DIR}/Scene/SceneVertexArray.cpp
    ${SRC_DIR}/Scene/SceneShader.cpp
)

target_include_directories(backend_scene_tests PRIVATE
    ${SRC_DIR}
    ${SRC_DIR}/Scene/include
    ${SRC_DIR}/Scene/include/Scene
    ${SRC_DIR}/Particle/include
    ${SRC_DIR}/VulkanRender/include
    ${DOCTEST_INCLUDE_DIR}
)

target_link_libraries(backend_scene_tests PRIVATE
    wpUtils
    wpFs
    wpAudio
    wpParticle
    ${LZ4_LIBRARIES}
    Eigen3::Eigen
    nlohmann_json
)

target_compile_options(backend_scene_tests PRIVATE ${warn_opts})

if(MUTATION_TESTING)
    target_compile_options(backend_scene_tests PRIVATE
        -fpass-plugin=${MULL_PLUGIN_PATH}
        -g -grecord-command-line
        -DWP_SUPPRESS_DEBUG_LOGGING
    )
endif()

add_test(NAME backend_scene_tests COMMAND backend_scene_tests)

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
    add_test(NAME scenescript_tests COMMAND scenescript_tests)
endif()

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
