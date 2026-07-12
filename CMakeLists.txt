cmake_minimum_required(VERSION 3.16)
# Single source of truth for the project version (roadmap §8: "numero de
# version affiche dans l'appli... pas code en dur"): read from CMake's own
# project(VERSION) by app/CMakeLists.txt via a configure_file, rather than
# a second hardcoded literal anywhere else.
project(shaderfmt VERSION 0.2.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

option(SHADERFMT_BUILD_TESTS "Build shaderfmt unit tests" ON)
option(SHADERFMT_BUILD_APP "Build the Qt/QScintilla desktop app (roadmap §3)" ON)

add_subdirectory(lib)

if(SHADERFMT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# The GUI app (roadmap §3) needs Qt6 + a QScintilla built against it, which
# aren't available on every machine this CMake project is configured on
# (see README.md "Prerequis GUI"). Probe for Qt6 first so a missing Qt
# install degrades to "app skipped" instead of failing the whole configure -
# `libshaderfmt` (roadmap §5: engine decoupled from any UI) must remain
# buildable on its own.
set(SHADERFMT_APP_BUILT OFF)
if(SHADERFMT_BUILD_APP)
    find_package(Qt6 QUIET COMPONENTS Widgets)
    if(Qt6_FOUND)
        add_subdirectory(app)
        set(SHADERFMT_APP_BUILT ON)
    else()
        message(STATUS "Qt6 not found: skipping app/ (roadmap §3). "
                        "Set CMAKE_PREFIX_PATH to your Qt install to enable it.")
    endif()
endif()

# Packaging (roadmap §8): `cpack` (after `cmake --install`) produces a real
# ZIP of the desktop app on every OS (no extra tooling needed), plus - on
# Windows, when `makensis` is found - a real NSIS installer .exe. A signed
# macOS .app/DMG and a Linux AppImage/.deb are NOT set up here: they need
# a macOS machine (to build/sign a .app bundle) and a Linux machine/fpm
# respectively, neither available in this environment - left as explicit
# TODOs in ROADMAP.md rather than half-configured. Only registered when
# app/ was actually built - packaging `lib/` alone as an end-user download
# isn't a real use case for this project (roadmap §8 talks about
# installers for the *editor*, not the library).
if(SHADERFMT_APP_BUILT)
    set(CPACK_PACKAGE_NAME "ShaderFmt")
    set(CPACK_PACKAGE_VENDOR "Patrick JAILLET")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Editeur et formateur de code shader (GLSL/Shadertoy/HLSL/WGSL/MSL/ShaderLab)")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "ShaderFmt")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
    set(CPACK_GENERATOR "ZIP")

    if(WIN32)
        find_program(SHADERFMT_MAKENSIS NAMES makensis makensis.exe)
        if(SHADERFMT_MAKENSIS)
            message(STATUS "makensis found (${SHADERFMT_MAKENSIS}): NSIS installer enabled")
            list(APPEND CPACK_GENERATOR "NSIS")
            set(CPACK_PACKAGE_EXECUTABLES "shaderfmt_app" "ShaderFmt")
            set(CPACK_NSIS_PACKAGE_NAME "ShaderFmt")
            set(CPACK_NSIS_DISPLAY_NAME "ShaderFmt ${PROJECT_VERSION}")
            set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
            set(CPACK_NSIS_MODIFY_PATH OFF)
        else()
            message(STATUS "makensis not found: NSIS installer skipped, ZIP package still produced")
        endif()
    endif()

    include(CPack)
endif()
