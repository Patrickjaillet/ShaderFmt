add_library(shaderfmt STATIC
    src/Lexer.cpp
    src/Formatter.cpp
    src/LanguageDetector.cpp
    src/Parser.cpp
    src/GlslVersionLint.cpp
)

target_include_directories(shaderfmt PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(shaderfmt PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)
