cmake_minimum_required(VERSION 3.13)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project("sceneviewer")

set(DEFAULT_BUILD_TYPE "Release")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to '${DEFAULT_BUILD_TYPE}' as none was specified.")
    set(CMAKE_BUILD_TYPE "${DEFAULT_BUILD_TYPE}" CACHE STRING "Choose the type of build." FORCE)
endif()

option(BUILD_QML "Build qml scene viewer" ${BUILD_QML})

find_package(PkgConfig REQUIRED)

add_compile_options(
    $<$<CONFIG:Debug>:-ggdb>
    $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
    $<$<CONFIG:Debug>:-fsanitize=address>
)

add_link_options(
    $<$<CONFIG:Debug>:-fsanitize=address>
)

include_directories(../third_party)
add_subdirectory(.. scenebackend)

pkg_check_modules(GLFW REQUIRED glfw3)
add_executable(${PROJECT_NAME}
    glfwviewer.cpp
)
target_link_libraries(${PROJECT_NAME}
    PUBLIC 
        ${GLFW_LIBRARIES}
        wescene-renderer
)

if(BUILD_QML)
    if(NOT QT_MAJOR_VERSION)
        set(QT_MAJOR_VERSION 6)
    endif()
    find_package(Qt${QT_MAJOR_VERSION} COMPONENTS Qml Quick Gui REQUIRED)

    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    set(CMAKE_AUTOUIC ON)

    # sceneviewer-script: same renderer, but links the QJSEngine-powered
    # SceneBackend so SceneScript (property/color/init/update scripts), media
    # integration, audio analyzer and the full JS API are live.  Pick this
    # variant when you want to iterate on scripted wallpapers.
    add_executable(${PROJECT_NAME}-script
        qmlviewer.cpp
        main.qml
        pkg.qrc
    )
    target_link_libraries(${PROJECT_NAME}-script
        PUBLIC
        wescene-renderer-qml
    )

    # Back-compat alias target: old docs referred to sceneviewer-qml.
    add_custom_target(${PROJECT_NAME}-qml DEPENDS ${PROJECT_NAME}-script)

    set(CMAKE_AUTOMOC OFF)
    set(CMAKE_AUTORCC OFF)
    set(CMAKE_AUTOUIC OFF)
endif()
