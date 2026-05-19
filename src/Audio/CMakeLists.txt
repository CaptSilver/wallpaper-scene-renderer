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

# Live-rebind for system-audio capture: when libpulse headers are available we
# subscribe to PA server/sink events and re-open miniaudio against the new
# default sink's monitor.  Without libpulse the capture is bound once at init
# and the user has to restart plasmashell if that sink disappears.
if(LIBPULSE_FOUND)
    target_compile_definitions(${LIB_NAME} PRIVATE WEK_HAVE_LIBPULSE=1)
    target_include_directories(${LIB_NAME} PRIVATE ${LIBPULSE_INCLUDE_DIRS})
    target_link_libraries(${LIB_NAME} PRIVATE ${LIBPULSE_LIBRARIES})
endif()

# Suppress warnings from KissFFT C sources
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/kissfft/kiss_fft.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/kissfft/kiss_fftr.c
    PROPERTIES COMPILE_OPTIONS "-w"
)
set_property(TARGET ${LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
