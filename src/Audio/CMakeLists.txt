set(LIB_NAME wpAudio)

add_library(${LIB_NAME}
    STATIC
    SoundManager.cpp
    AudioAnalyzer.cpp
    AudioCapture.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/kissfft/kiss_fft.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/kissfft/kiss_fftr.c
)

target_link_libraries(${LIB_NAME}
PUBLIC
    wpUtils
PRIVATE
    wpFs
)
target_include_directories(${LIB_NAME}
    PUBLIC include
    PRIVATE include/Audio
            ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/kissfft
)
target_compile_options(${LIB_NAME} PRIVATE ${warn_opts})
# Suppress warnings from KissFFT C sources
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/kissfft/kiss_fft.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/kissfft/kiss_fftr.c
    PROPERTIES COMPILE_OPTIONS "-w"
)
set_property(TARGET ${LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
