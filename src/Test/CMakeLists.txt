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
    test_WPPuppet.cpp
    test_WPTexImageHelpers.cpp
    test_WPTextLayer.cpp
    test_SceneCamera.cpp
    test_WPSoundHelpers.cpp
    # Compile specific source files to avoid pulling in Vulkan
    ${SRC_DIR}/WPTexImageParser.cpp
    ${SRC_DIR}/Type.cpp
    ${SRC_DIR}/WPPuppet.cpp
    ${SRC_DIR}/Scene/SceneCamera.cpp
    ${SRC_DIR}/Scene/SceneNode.cpp
)

target_include_directories(backend_scene_tests PRIVATE
    ${SRC_DIR}
    ${SRC_DIR}/Scene/include
    ${SRC_DIR}/Scene/include/Scene
    ${SRC_DIR}/Particle/include
    ${DOCTEST_INCLUDE_DIR}
)

target_link_libraries(backend_scene_tests PRIVATE
    wpUtils
    wpFs
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
