cmake_minimum_required(VERSION 3.14)
project(wescene-renderer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)


include_directories(SYSTEM third_party)

link_libraries(Eigen3::Eigen)

add_subdirectory(third_party)
add_subdirectory(src)

option(BUILD_TESTS "Build backend_scene unit tests" OFF)
option(MUTATION_TESTING "Enable Mull mutation testing instrumentation (requires Clang)" OFF)

if(MUTATION_TESTING AND NOT BUILD_TESTS)
    message(FATAL_ERROR "MUTATION_TESTING requires BUILD_TESTS=ON")
endif()

if(MUTATION_TESTING)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "MUTATION_TESTING requires Clang (found ${CMAKE_CXX_COMPILER_ID})")
    endif()

    if(NOT MULL_PLUGIN_PATH)
        file(GLOB _mull_candidates /usr/lib/mull-ir-frontend-* /usr/lib64/mull-ir-frontend-*)
        if(_mull_candidates)
            list(GET _mull_candidates 0 MULL_PLUGIN_PATH)
        endif()
    endif()

    if(NOT MULL_PLUGIN_PATH)
        message(FATAL_ERROR "Cannot find Mull IR frontend plugin. Set -DMULL_PLUGIN_PATH=...")
    endif()

    message(STATUS "Mull plugin: ${MULL_PLUGIN_PATH}")
endif()

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(src/Test)
endif()

if(BUILD_QML)
    find_package(Qt6 COMPONENTS Gui Quick Qml REQUIRED)

    set(CMAKE_AUTOMOC ON) 
    set(CMAKE_AUTORCC ON) 
    set(CMAKE_AUTOUIC ON)

    add_library(${PROJECT_NAME}-qml
    STATIC
        qml_helper/SceneBackend.cpp
        qml_helper/glExtra.cpp
    )
    target_link_libraries(${PROJECT_NAME}-qml
    PUBLIC
        Qt::Quick
        Qt::Gui
        Qt::Qml
        glad
        ${PROJECT_NAME}
    )
    target_include_directories(${PROJECT_NAME}-qml PUBLIC qml_helper third_party
        PRIVATE src/Audio/include)

    set(CMAKE_AUTOMOC OFF) 
    set(CMAKE_AUTORCC OFF) 
    set(CMAKE_AUTOUIC OFF) 
endif()
