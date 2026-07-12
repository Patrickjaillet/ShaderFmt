#pragma once

#include <string>
#include <vector>

namespace shaderfmt {

// Language dialect the lexer should tokenize. This changes a small set of
// rules (preprocessor directives, nested block comments, '@' attributes)
// but reuses the same C-like tokenizer core for GLSL/HLSL/Shadertoy/WGSL,
// since all of them share the same lexical family (braces, operators,
// identifiers). Shadertoy is kept as its own value (rather than folded
// into GLSL) because it is the entry point most users will paste from,
// and because it is the natural place to hang Shadertoy-specific
// formatting/detection rules later without disturbing plain GLSL.
enum class Language {
    GLSL,       // OpenGL/Vulkan GLSL
    Shadertoy,  // GLSL dialect: mainImage(), implicit iTime/iResolution/...
    HLSL,       // DirectX HLSL
    WGSL,       // WebGPU Shading Language
    MSL,        // Apple Metal Shading Language (C++14-based; close enough
                // to the C-like family - preprocessor, non-nested block
                // comments, "[[attribute]]" syntax already falls out of
                // the existing bracket-depth tracking - that it shares
                // the GenericC-family lexer/parser path, see Lexer.cpp
                // and Parser.cpp)
    ShaderLab   // Unity ShaderLab (Cg/HLSL wrapped in Shader/Properties/
                // SubShader/Pass blocks). Lexically C-like (same lexer
                // path), but the outer block syntax has its own grammar -
                // see Parser.cpp's parseShaderLabProgram(). CGPROGRAM/
                // HLSLPROGRAM ... ENDCG/ENDHLSL regions embed real HLSL,
                // parsed by recursively invoking the HLSL grammar on that
                // token sub-range.
};

enum class TokenType {
    Identifier,
    Number,
    StringLiteral,
    Punctuator,
    Preprocessor,   // whole directive line, e.g. "#define FOO 1"
    LineComment,    // "// ..."
    BlockComment,   // "/* ... */"
    EndOfFile
};

struct Token {
    TokenType type = TokenType::EndOfFile;
    std::string text;

    // Formatting-relevant lexical context, captured at scan time so the
    // formatter never has to re-inspect raw source:
    bool onOwnLine = false;      // only whitespace precedes this token since the last '\n'
    int blankLinesBefore = 0;    // number of fully-blank lines immediately before this token
    int line = 0;                // 1-based source line, for diagnostics
    int column = 0;              // 1-based source column, for diagnostics
    size_t offset = 0;           // 0-based byte offset of this token's first character in
                                  // the source string; source.substr(offset, text.size())
                                  // recovers `text` exactly. Used by consumers that need to
                                  // map tokens back onto raw source (e.g. a syntax-highlighting
                                  // lexer driving QScintilla's styleText, roadmap §3).
};

struct LexError {
    std::string message;
    int line = 0;
    int column = 0;
};

} // namespace shaderfmt
