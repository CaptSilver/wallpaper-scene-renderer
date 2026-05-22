set(LIB_NAME wpParticle)

add_library(${LIB_NAME}
STATIC
ParticleModify.cpp
ParticleSystem.cpp
ParticleEmitter.cpp
WPParticleRawGener.cpp
)

target_link_libraries(${LIB_NAME} PUBLIC wpUtils Eigen3::Eigen PRIVATE wpScene)
target_include_directories(${LIB_NAME} PUBLIC include PRIVATE include/Particle)
target_include_directories(${LIB_NAME} SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party")
target_compile_options(${LIB_NAME} PRIVATE ${warn_opts})
set_property(TARGET ${LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
