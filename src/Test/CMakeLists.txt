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
    # Compile specific source files to avoid pulling in Vulkan
    ${SRC_DIR}/WPTexImageParser.cpp
    ${SRC_DIR}/Type.cpp
    ${SRC_DIR}/WPPuppet.cpp
)

target_include_directories(backend_scene_tests PRIVATE
    ${SRC_DIR}
    ${SRC_DIR}/Scene/include
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
    )
endif()

add_test(NAME backend_scene_tests COMMAND backend_scene_tests)
