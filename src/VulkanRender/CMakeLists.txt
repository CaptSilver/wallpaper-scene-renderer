set(LIB_NAME wpVulkanRender)

add_library(${LIB_NAME}
STATIC
CopyPass.cpp
CustomShaderPass.cpp
DepthToColorResolvePass.cpp
FenceWaitRetry.cpp
FinPass.cpp
PrePass.cpp
SceneToRenderGraph.cpp
VolumetricChain.cpp
VulkanRender.cpp
)

target_link_libraries(${LIB_NAME}
PRIVATE
	wpVulkan
    wpScene
    wpRGraph
    # VolumetricChain.cpp's build*Material helpers return wpscene::WPMaterial,
    # whose header transitively includes nlohmann/json — needed on this
    # target's include path even though VolumetricChain itself never invokes
    # FromJson directly.
    nlohmann_json
)
target_include_directories(${LIB_NAME} PUBLIC include PRIVATE include/VulkanRender)
target_compile_options(${LIB_NAME} PRIVATE ${warn_opts} -Wno-missing-field-initializers)
set_property(TARGET ${LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
