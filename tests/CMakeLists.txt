add_executable(shaderfmt_tests test_main.cpp)
target_link_libraries(shaderfmt_tests PRIVATE shaderfmt)
target_compile_definitions(shaderfmt_tests PRIVATE
    SHADERFMT_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus"
    SHADERFMT_TEST_TMPDIR="${CMAKE_CURRENT_BINARY_DIR}/compile_check_tmp"
)

# Reference-compiler non-regression test (roadmap §6): compiles each
# GLSL/HLSL corpus file before and after formatting with the real Khronos
# glslang front-end and checks formatting never turns a valid shader
# invalid. Optional: not every machine building this repo will have
# glslangValidator installed, so the test degrades to a documented SKIP
# rather than failing the whole suite when it's absent (see README.md
# "Prerequis" for how to obtain it - prebuilt Khronos releases exist for
# Linux/Windows/macOS, no build-from-source required).
find_program(SHADERFMT_GLSLANG_VALIDATOR
    NAMES glslangValidator glslangValidator.exe
    HINTS "C:/Qt/tools/glslang/bin" # local dev convenience only; harmless if absent
)
if(SHADERFMT_GLSLANG_VALIDATOR)
    message(STATUS "glslangValidator found (${SHADERFMT_GLSLANG_VALIDATOR}): reference-compiler test enabled")
    target_compile_definitions(shaderfmt_tests PRIVATE
        SHADERFMT_GLSLANG_VALIDATOR="${SHADERFMT_GLSLANG_VALIDATOR}"
    )
else()
    message(STATUS "glslangValidator not found: reference-compiler test will report SKIP")
endif()

# Same idea, for WGSL (roadmap §6's documented gap: no reference compiler
# was wired up for WGSL originally, since Tint/naga weren't set up in this
# environment - naga is now used, via its `naga-cli` binary, obtained with
# `cargo install naga-cli`. Optional in exactly the same way as
# glslangValidator above.
find_program(SHADERFMT_NAGA
    NAMES naga naga.exe
    HINTS "$ENV{USERPROFILE}/.cargo/bin" # local dev convenience only; harmless if absent
)
if(SHADERFMT_NAGA)
    message(STATUS "naga found (${SHADERFMT_NAGA}): WGSL reference-compiler test enabled")
    target_compile_definitions(shaderfmt_tests PRIVATE
        SHADERFMT_NAGA="${SHADERFMT_NAGA}"
    )
else()
    message(STATUS "naga not found: WGSL reference-compiler test will report SKIP")
endif()

add_test(NAME shaderfmt_tests COMMAND shaderfmt_tests)
