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

# ---------------------------------------------------------------------------
# Install + packaging. Everything below only ADDS rules — no target or option
# above is touched.
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS ollamadev RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The desktop entry and icon are meaningless without the GUI binary, so they are
# installed only when it is actually built (ODV_BUILD_ADE=OFF is the headless/CI
# configuration).
if(ODV_BUILD_ADE)
    install(TARGETS ollamadev-ade RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES packaging/ollamadev-ade.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES packaging/ollamadev-ade.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()

set(CPACK_PACKAGE_NAME "ollamadev")
set(CPACK_PACKAGE_VENDOR "OllamaDev")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Local AI coding agent — Ollama and every major coding CLI, in parallel")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/kennethyork/OllamaDev")
set(CPACK_PACKAGE_FILE_NAME "ollamadev-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "ollamadev")
set(CPACK_STRIP_FILES ON)

if(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "TGZ;DEB")

    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "OllamaDev <noreply@ollamadev.local>")
    set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    # Let dpkg-shlibdeps read the built ELF files and write the Qt/libc deps
    # itself. A hand-written Depends: list goes stale the first time Qt bumps a
    # soname — the package would then install and fail to start.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    # Ollama is talked to over HTTP and is not a Debian package, so it is a
    # runtime suggestion rather than a dependency. The recorders are what
    # `ollamadev voice` shells out to; any one of them is enough.
    set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "alsa-utils | ffmpeg | pulseaudio-utils")
endif()

include(CPack)
