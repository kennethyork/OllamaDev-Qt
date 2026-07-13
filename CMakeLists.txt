cmake_minimum_required(VERSION 3.21)
project(ollamadev-qt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Network Concurrent Widgets)
qt_standard_project_setup()

# The CLI must build without a GUI toolchain — headless boxes and CI want it alone.
option(ODV_BUILD_ADE "Build the Qt desktop app" ON)

add_subdirectory(core)
add_subdirectory(cli)
if(ODV_BUILD_ADE)
    add_subdirectory(ade)
endif()

enable_testing()
add_subdirectory(tests)
