set(LIB_NAME wpUtils)

add_library(${LIB_NAME}
STATIC
Logging.cpp
FpsCounter.cpp
Algorism.cpp
Sha.cpp
DynamicLibrary.cpp
SceneProfiler.cpp
)

target_link_libraries(${LIB_NAME}
PUBLIC
    Eigen3::Eigen
)
target_include_directories(${LIB_NAME}
    PUBLIC include "${CMAKE_CURRENT_SOURCE_DIR}/.."
    PRIVATE include/Utils)
target_include_directories(${LIB_NAME} SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party")
target_compile_options(${LIB_NAME} PRIVATE ${warn_opts})
set_property(TARGET ${LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
