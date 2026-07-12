#include "shaderfmt/LanguageDetector.hpp"
#include "shaderfmt/Lexer.hpp"

namespace shaderfmt {

namespace {

bool contains(const std::string& src, const char* needle) {
    return src.find(needle) != std::string::npos;
}

// Bug-audit fix: every looksLikeXxx() below used to run contains() over
// the *raw* source text, so a signal keyword sitting inside a comment or
// string literal - not actual code - could trigger a false-positive
// detection (e.g. a plain GLSL shader with a porting comment like
// "// port of a cbuffer-based effect, see SV_Target notes" was detected
// as HLSL). looksLikeMsl() already had to work around exactly this
// failure mode for bare "vertex"/"fragment"/"kernel" words in comments
// (see its own comment below) - "a lesson from testing against actual
// shader source" - but that lesson wasn't applied to the other dialects'
// signal lists, which were just as exposed. Fixed once, for every
// dialect at once: blank out comment/string-literal spans (by character
// offset, via the real lexer, so everything else keeps its exact
// original text and adjacency - e.g. "@vertex" stays adjacent) before any
// signal is looked up. Detection runs before the language is known, so
// this uses GLSL's lexing rules as a generic-enough stand-in (shared with
// HLSL/MSL/ShaderLab; WGSL source has no '#'/comment construct this would
// mis-scan for the common case). Residual limitation, strictly better
// than before rather than a full fix: GLSL mode doesn't nest block
// comments, so a WGSL source using a *nested* block comment
// ("/* outer /* inner */ still outer */") would stop stripping at the
// first "*/" and could still false-positive on a signal inside what WGSL
// considers the (still-open) outer comment - a narrower version of the
// original bug, not a new one.
std::string stripCommentsAndStrings(const std::string& src) {
    LexResult lexed = lex(src, Language::GLSL);
    std::string cleaned = src;
    for (const Token& t : lexed.tokens) {
        if (t.type != TokenType::LineComment && t.type != TokenType::BlockComment &&
            t.type != TokenType::StringLiteral) {
            continue;
        }
        for (size_t i = t.offset; i < t.offset + t.text.size() && i < cleaned.size(); i++) {
            cleaned[i] = ' ';
        }
    }
    return cleaned;
}

// '@vertex'/'@fragment'/'@compute' attributes and the 'fn' keyword don't
// exist in GLSL/HLSL/Shadertoy at all, so they're an unambiguous WGSL
// signal on their own.
bool looksLikeWgsl(const std::string& src) {
    if (contains(src, "@vertex") || contains(src, "@fragment") || contains(src, "@compute")) {
        return true;
    }
    return contains(src, "fn ") && (contains(src, "->") || contains(src, "var<"));
}

// HLSL semantics (': SV_Target' etc.), 'cbuffer', and the compute/resource
// keywords below have no equivalent spelling in GLSL/Shadertoy.
bool looksLikeHlsl(const std::string& src) {
    return contains(src, "cbuffer") || contains(src, "SV_Target") || contains(src, "SV_POSITION") ||
           contains(src, "SV_Position") || contains(src, ": register(") || contains(src, "RWStructuredBuffer") ||
           contains(src, "RWTexture") || contains(src, "numthreads") || contains(src, "Texture2D") ||
           contains(src, "SamplerState");
}

// Shadertoy is plain GLSL with an implicit environment: the 'mainImage'
// entry point and the 'iXxx' implicit uniforms are Shadertoy-specific by
// convention (not part of the GLSL language), so their presence is a
// reliable signal without needing a real symbol table.
bool looksLikeShadertoy(const std::string& src) {
    return contains(src, "mainImage") || contains(src, "iResolution") || contains(src, "iTime") ||
           contains(src, "iChannel") || contains(src, "iMouse") || contains(src, "iFrame");
}

// MSL is the trickiest to disambiguate from HLSL/GLSL since it's also a
// C-like language with a preprocessor. '#include <metal_stdlib>', 'using
// namespace metal', and the 'metal::' namespace are reliable signals.
// The '[[attribute]]' double-bracket forms are unique to MSL among these
// four dialects (GLSL uses 'layout(...)', HLSL ': SEMANTIC', WGSL
// '@attr' - none use '[[...]]'), so they're reliable too. Deliberately
// NOT checking bare 'vertex'/'fragment'/'kernel' as substrings: those are
// common English words that show up in GLSL/HLSL comments describing
// what a shader does (e.g. "// Simple lighting fragment shader"), which
// caused real false positives against the corpus - a lesson from testing
// against actual shader source, not just hand-picked snippets.
bool looksLikeMsl(const std::string& src) {
    return contains(src, "metal_stdlib") || contains(src, "using namespace metal") || contains(src, "metal::") ||
           contains(src, "[[stage_in]]") || contains(src, "[[buffer(") || contains(src, "[[texture(") ||
           contains(src, "[[position]]") || contains(src, "[[vertex_id]]") ||
           contains(src, "[[thread_position_in_grid]]");
}

// Unity ShaderLab wraps Cg/HLSL in its own block syntax - "SubShader",
// "CGPROGRAM"/"ENDCG", "HLSLPROGRAM"/"ENDHLSL" don't appear in any other
// dialect here. Checked before HLSL: a ShaderLab file's embedded
// CGPROGRAM block commonly contains genuine HLSL signals too (SV_Target,
// cbuffer...), which would otherwise misclassify the whole file as plain
// HLSL instead of ShaderLab.
bool looksLikeShaderLab(const std::string& src) {
    return contains(src, "SubShader") || contains(src, "CGPROGRAM") || contains(src, "ENDCG") ||
           contains(src, "HLSLPROGRAM") || contains(src, "ENDHLSL");
}

} // namespace

Language detectLanguage(const std::string& source) {
    // Order matters: WGSL/HLSL/MSL/ShaderLab each have tokens that are
    // illegal in GLSL/Shadertoy, so they're checked first and
    // independently. ShaderLab is checked before HLSL since a ShaderLab
    // file's embedded CGPROGRAM block commonly contains real HLSL
    // signals too (see looksLikeShaderLab). MSL is checked after HLSL
    // since HLSL's signals (cbuffer, SV_Target, ...) are more specific/
    // rare and should win if somehow both matched. Shadertoy is a GLSL
    // dialect (every Shadertoy token is also legal GLSL), so it must be
    // checked after WGSL/HLSL/MSL/ShaderLab but before falling back to
    // plain GLSL.
    //
    // All signals are matched against `cleaned`, not `source` directly -
    // see stripCommentsAndStrings() above (bug-audit fix: comments/string
    // literals used to be able to trigger false-positive detection).
    std::string cleaned = stripCommentsAndStrings(source);
    if (looksLikeWgsl(cleaned)) return Language::WGSL;
    if (looksLikeShaderLab(cleaned)) return Language::ShaderLab;
    if (looksLikeHlsl(cleaned)) return Language::HLSL;
    if (looksLikeMsl(cleaned)) return Language::MSL;
    if (looksLikeShadertoy(cleaned)) return Language::Shadertoy;
    return Language::GLSL;
}

} // namespace shaderfmt
