# The desktop editor (ROADMAP.md §3). Needs Qt6 (Widgets) plus QScintilla
# built against that same Qt (see README.md "Prerequis GUI" for how to
# build QScintilla from source with qmake - there is no prebuilt package
# for it, unlike Qt itself).

set(CMAKE_AUTOMOC ON)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/Version.hpp
    @ONLY
)

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent OpenGLWidgets)

get_filename_component(SHADERFMT_QT_PREFIX "${Qt6_DIR}/../../.." ABSOLUTE)

find_path(QSCINTILLA_INCLUDE_DIR
    NAMES Qsci/qsciscintilla.h
    HINTS "${SHADERFMT_QT_PREFIX}/include"
)
find_library(QSCINTILLA_LIBRARY
    NAMES qscintilla2_qt6
    HINTS "${SHADERFMT_QT_PREFIX}/lib"
)

if(NOT QSCINTILLA_INCLUDE_DIR OR NOT QSCINTILLA_LIBRARY)
    message(FATAL_ERROR
        "QScintilla (qscintilla2_qt6) not found under ${SHADERFMT_QT_PREFIX}. "
        "Build it from source with qmake against this Qt install first - "
        "see README.md for the exact steps.")
endif()

# WIN32 (a no-op on non-Windows platforms): links as a GUI-subsystem
# executable instead of the CMake default of console-subsystem, so
# launching shaderfmt_app.exe never pops up a console/log window behind
# the actual editor window. --self-test/--smoke-test's std::cout/cerr
# output still works fine when the exe is invoked from a script with its
# output captured/redirected (the normal way automated tests run it) -
# GUI subsystem only changes whether a *new* console is allocated for an
# interactive, non-redirected launch.
add_executable(shaderfmt_app WIN32
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.hpp
    src/EditorTab.cpp
    src/EditorTab.hpp
    src/Theme.cpp
    src/Theme.hpp
    src/ShaderfmtLexer.cpp
    src/ShaderfmtLexer.hpp
    src/StylePanel.cpp
    src/StylePanel.hpp
    src/ShaderViewport.cpp
    src/ShaderViewport.hpp
)

target_include_directories(shaderfmt_app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${QSCINTILLA_INCLUDE_DIR}
)

target_link_libraries(shaderfmt_app PRIVATE
    shaderfmt
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::OpenGLWidgets
    ${QSCINTILLA_LIBRARY}
)

target_compile_options(shaderfmt_app PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)

# QScintilla itself needs no DLL-deployment step: it's built
# CONFIG+=staticlib (see README.md "Building from source"), so it's
# already inside shaderfmt_app.exe. Qt's own DLLs are not static, though -
# without windeployqt, shaderfmt_app.exe fails to start with a missing-DLL
# error the moment it's run from outside a shell that has Qt's bin/ on
# PATH (i.e. every normal double-click launch).
if(WIN32)
    find_program(SHADERFMT_WINDEPLOYQT
        NAMES windeployqt6 windeployqt
        HINTS "${SHADERFMT_QT_PREFIX}/bin"
    )
    if(SHADERFMT_WINDEPLOYQT)
        # Run right after linking, on the exe in the build tree itself -
        # not just at `cmake --install`/`cpack` time - so a plain
        # `cmake --build` already produces a directly double-clickable
        # shaderfmt_app.exe, matching what README.md's "Building from
        # source" instructions imply.
        add_custom_command(TARGET shaderfmt_app POST_BUILD
            COMMAND "${SHADERFMT_WINDEPLOYQT}" --release --no-translations
                    --no-system-d3d-compiler
                    "$<TARGET_FILE:shaderfmt_app>"
            COMMENT "Deploying Qt DLLs next to shaderfmt_app.exe (windeployqt)"
        )
    else()
        message(WARNING "windeployqt not found: shaderfmt_app.exe will be built without "
                         "its Qt DLLs bundled alongside it, and will fail to start outside "
                         "a shell that has Qt's bin/ on PATH.")
    endif()
endif()

# Packaging (roadmap §8): install the executable, then - on Windows - run
# windeployqt again against the installed copy, so `cpack`'s package is
# self-contained too (its own separate directory, needs its own copy).
install(TARGETS shaderfmt_app RUNTIME DESTINATION .)

if(WIN32 AND SHADERFMT_WINDEPLOYQT)
    install(CODE "
        message(STATUS \"Running windeployqt on installed shaderfmt_app.exe...\")
        execute_process(
            COMMAND \"${SHADERFMT_WINDEPLOYQT}\" --release --no-translations
                    --no-system-d3d-compiler
                    \"\${CMAKE_INSTALL_PREFIX}/shaderfmt_app.exe\"
            RESULT_VARIABLE _shaderfmt_windeployqt_result
        )
        if(NOT _shaderfmt_windeployqt_result EQUAL 0)
            message(FATAL_ERROR \"windeployqt failed (exit code \${_shaderfmt_windeployqt_result})\")
        endif()
    ")
endif()
