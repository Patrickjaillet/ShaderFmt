// Minimal, dependency-free test harness (no gtest / catch2) so the tests
// build offline with just the C++ standard library. Each TEST(name) block
// registers itself; run_all() executes them and reports a summary.

#include "shaderfmt/Ast.hpp"
#include "shaderfmt/Formatter.hpp"
#include "shaderfmt/GlslVersionLint.hpp"
#include "shaderfmt/LanguageDetector.hpp"
#include "shaderfmt/Lexer.hpp"
#include "shaderfmt/Parser.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(const std::string& name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

int g_failures = 0;
std::string g_currentTest;

#define TEST(name)                                                        \
    void test_##name();                                                   \
    Registrar registrar_##name(#name, test_##name);                       \
    void test_##name()

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::cerr << "  [FAIL] " << g_currentTest << ": " << #cond     \
                       << " at " << __FILE__ << ":" << __LINE__ << "\n";   \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        auto va = (a);                                                     \
        auto vb = (b);                                                     \
        if (!(va == vb)) {                                                 \
            std::cerr << "  [FAIL] " << g_currentTest << ": " << #a        \
                       << " != " << #b << " at " << __FILE__ << ":"        \
                       << __LINE__ << "\n    lhs=" << va << "\n    rhs="   \
                       << vb << "\n";                                      \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Used by both the GLSL/HLSL (glslangValidator) and WGSL (naga)
// reference-compiler tests below - kept unconditional (not inside either
// tool's #ifdef block) so either one can be present without the other.
std::string writeTempFile(const std::string& path, const std::string& content) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
    return path;
}

// Non-trivial = tokens that can affect program meaning: identifiers,
// numbers, strings, punctuators, preprocessor directives. Comments are
// intentionally excluded since their exact adjacency to code is a
// formatting choice, not semantics.
std::vector<std::string> semanticTokenTexts(const std::vector<shaderfmt::Token>& tokens) {
    std::vector<std::string> out;
    for (const auto& t : tokens) {
        using shaderfmt::TokenType;
        if (t.type == TokenType::LineComment || t.type == TokenType::BlockComment ||
            t.type == TokenType::EndOfFile) {
            continue;
        }
        out.push_back(t.text);
    }
    return out;
}

// Walks an AST (see Ast.hpp), reproducing the semantic (non-comment) token
// text sequence it represents. Synthesizes the fixed keywords that
// IfStmt/ForStmt/WhileStmt/DoWhileStmt/SwitchStmt deliberately don't store
// (see Ast.hpp's NodeKind comment) so this can be compared directly
// against semanticTokenTexts() on the original lex - the round-trip
// losslessness bar for the AST, mirroring how the existing formatter
// non-regression test compares semantic token sequences rather than raw
// bytes.
void collectAstSemanticTokenTexts(const shaderfmt::Node& node, std::vector<std::string>& out) {
    using shaderfmt::NodeKind;
    switch (node.kind) {
        case NodeKind::IfStmt:
            out.push_back("if");
            collectAstSemanticTokenTexts(*node.children[0], out);
            collectAstSemanticTokenTexts(*node.children[1], out);
            if (node.children.size() > 2) {
                out.push_back("else");
                collectAstSemanticTokenTexts(*node.children[2], out);
            }
            return;
        case NodeKind::ForStmt:
            out.push_back("for");
            collectAstSemanticTokenTexts(*node.children[0], out);
            collectAstSemanticTokenTexts(*node.children[1], out);
            return;
        case NodeKind::WhileStmt:
            out.push_back("while");
            collectAstSemanticTokenTexts(*node.children[0], out);
            collectAstSemanticTokenTexts(*node.children[1], out);
            return;
        case NodeKind::DoWhileStmt:
            out.push_back("do");
            collectAstSemanticTokenTexts(*node.children[0], out);
            out.push_back("while");
            collectAstSemanticTokenTexts(*node.children[1], out);
            out.push_back(";");
            return;
        case NodeKind::SwitchStmt:
            out.push_back("switch");
            collectAstSemanticTokenTexts(*node.children[0], out);
            collectAstSemanticTokenTexts(*node.children[1], out);
            return;
        case NodeKind::StructDecl: {
            // tokens = [header..., optional trailing ';'] (the ';' is
            // appended after children are already parsed - see
            // Parser.cpp's parseStructOrCbuffer - so it must be emitted
            // *after* children here despite living in the same vector).
            // The '{'/'}' around the field list, like Block's, aren't
            // stored (see Parser.cpp's parseStructOrCbuffer) - synthesized
            // here too.
            bool hasTrailingSemi = !node.tokens.empty() && node.tokens.back().type == shaderfmt::TokenType::Punctuator &&
                                    node.tokens.back().text == ";";
            size_t headerCount = node.tokens.size() - (hasTrailingSemi ? 1 : 0);
            for (size_t i = 0; i < headerCount; i++) out.push_back(node.tokens[i].text);
            if (node.hasBody) {
                out.push_back("{");
                for (const auto& c : node.children) collectAstSemanticTokenTexts(*c, out);
                if (node.hasClosingBrace) out.push_back("}"); // absent on truncated input, see Ast.hpp
            }
            if (hasTrailingSemi) out.push_back(";");
            // GLSL named interface block only (Parser.cpp's
            // parseInterfaceBlock()): the instance declarator/';' after
            // the body's closing '}', stored separately since it belongs
            // after the body, not before it - see Ast.hpp's
            // Node::trailingDeclarator. Comments excluded, same as the
            // generic `tokens` handling below.
            for (const auto& t : node.trailingDeclarator) {
                if (t.type == shaderfmt::TokenType::LineComment || t.type == shaderfmt::TokenType::BlockComment) continue;
                out.push_back(t.text);
            }
            return;
        }
        case NodeKind::Block:
            // '{'/'}' aren't stored on Block (see Ast.hpp) - synthesized.
            out.push_back("{");
            for (const auto& c : node.children) collectAstSemanticTokenTexts(*c, out);
            if (node.hasClosingBrace) out.push_back("}"); // absent on truncated input, see Ast.hpp
            return;
        case NodeKind::ShaderDecl:
        case NodeKind::ShaderLabBlock:
            // Same synthesized-brace shape as StructDecl, minus the
            // optional trailing ';' (ShaderLab blocks never have one).
            for (const auto& t : node.tokens) out.push_back(t.text);
            if (node.hasBody) {
                out.push_back("{");
                for (const auto& c : node.children) collectAstSemanticTokenTexts(*c, out);
                if (node.hasClosingBrace) out.push_back("}");
            }
            return;
        case NodeKind::CgProgramBlock:
            // tokens = [openMarker, closeMarker] but children[0] (the
            // embedded HLSL sub-program) belongs *between* them.
            out.push_back(node.tokens[0].text);
            if (!node.children.empty()) collectAstSemanticTokenTexts(*node.children[0], out);
            if (node.hasClosingBrace && node.tokens.size() > 1) out.push_back(node.tokens[1].text);
            return;
        default:
            break;
    }
    // Comment tokens can appear raw inside `tokens` for kinds that don't
    // do their own trivia extraction (e.g. ErrorNode, and the WGSL stub
    // path pending milestone 2) - excluded here too, matching
    // semanticTokenTexts()'s treatment of comments from the lexer side.
    for (const auto& t : node.tokens) {
        if (t.type == shaderfmt::TokenType::LineComment || t.type == shaderfmt::TokenType::BlockComment) continue;
        out.push_back(t.text);
    }
    for (const auto& c : node.children) collectAstSemanticTokenTexts(*c, out);
}

// Total comment tokens (LineComment + BlockComment) attached anywhere in
// the tree, as leading or trailing trivia - used to confirm comments
// aren't silently dropped during parsing (a lightweight but real
// complement to the semantic-token comparison above, which deliberately
// excludes comments).
size_t countAstTrivia(const shaderfmt::Node& node) {
    size_t count = node.leadingTrivia.size() + node.trailingTrivia.size();
    for (const auto& c : node.children) count += countAstTrivia(*c);
    return count;
}

size_t countCommentTokens(const std::vector<shaderfmt::Token>& tokens) {
    size_t count = 0;
    for (const auto& t : tokens) {
        if (t.type == shaderfmt::TokenType::LineComment || t.type == shaderfmt::TokenType::BlockComment) count++;
    }
    return count;
}

} // namespace

// ---------------------------------------------------------------------
// Lexer unit tests (isolated snippets)
// ---------------------------------------------------------------------

TEST(lexer_identifiers_numbers_punctuators) {
    auto r = shaderfmt::lex("vec3 x = a1 + 2.5e-3;", shaderfmt::Language::GLSL);
    CHECK(r.ok());
    std::vector<std::string> expected = {"vec3", "x", "=", "a1", "+", "2.5e-3", ";"};
    auto actual = semanticTokenTexts(r.tokens);
    CHECK_EQ(actual.size(), expected.size());
    for (size_t i = 0; i < expected.size() && i < actual.size(); i++) {
        CHECK_EQ(actual[i], expected[i]);
    }
}

TEST(lexer_preprocessor_is_single_token_and_not_split) {
    std::string src = "#define FOO(x) ((x) * 2)\nint y = FOO(3);";
    auto r = shaderfmt::lex(src, shaderfmt::Language::GLSL);
    CHECK(r.ok());
    CHECK(!r.tokens.empty());
    CHECK(r.tokens[0].type == shaderfmt::TokenType::Preprocessor);
    CHECK_EQ(r.tokens[0].text, std::string("#define FOO(x) ((x) * 2)"));
}

TEST(lexer_preprocessor_not_recognized_in_wgsl) {
    // WGSL has no preprocessor; a leading '#' must not be swallowed as a
    // directive - it should surface as an error/unknown-byte instead of
    // silently eating the rest of the line.
    auto r = shaderfmt::lex("#define X 1\n", shaderfmt::Language::WGSL);
    CHECK(!r.tokens.empty());
    CHECK(r.tokens[0].type != shaderfmt::TokenType::Preprocessor);
}

TEST(lexer_line_and_block_comments_preserved_verbatim) {
    std::string src = "int x; // trailing note\n/* block\n   comment */\nint y;";
    auto r = shaderfmt::lex(src, shaderfmt::Language::GLSL);
    bool sawLine = false, sawBlock = false;
    for (auto& t : r.tokens) {
        if (t.type == shaderfmt::TokenType::LineComment) {
            sawLine = true;
            CHECK_EQ(t.text, std::string("// trailing note"));
        }
        if (t.type == shaderfmt::TokenType::BlockComment) {
            sawBlock = true;
            CHECK_EQ(t.text, std::string("/* block\n   comment */"));
        }
    }
    CHECK(sawLine);
    CHECK(sawBlock);
}

TEST(lexer_wgsl_nested_block_comments) {
    auto r = shaderfmt::lex("/* outer /* inner */ still-outer */\nfn f() {}",
                             shaderfmt::Language::WGSL);
    CHECK(r.ok());
    CHECK(r.tokens[0].type == shaderfmt::TokenType::BlockComment);
    CHECK_EQ(r.tokens[0].text, std::string("/* outer /* inner */ still-outer */"));
}

TEST(lexer_tolerant_unterminated_string_does_not_crash) {
    auto r = shaderfmt::lex("int x = \"oops\nint y = 2;", shaderfmt::Language::GLSL);
    CHECK(!r.ok()); // an error is reported...
    CHECK(!r.tokens.empty()); // ...but lexing still produced a usable stream
}

TEST(lexer_tolerant_partial_code_mid_typing) {
    // Simulates the user mid-keystroke: an incomplete function body.
    auto r = shaderfmt::lex("void main() {\n    float x = ", shaderfmt::Language::GLSL);
    CHECK(r.ok()); // nothing here is actually malformed lexically
    CHECK(!r.tokens.empty());
}

struct CorpusFile {
    std::string filename;
    shaderfmt::Language lang;
};

std::vector<CorpusFile> corpusFiles() {
    return {
        {"basic.glsl", shaderfmt::Language::GLSL},
        {"glsl_version_qualifiers.glsl", shaderfmt::Language::GLSL},
        {"shadertoy.glsl", shaderfmt::Language::Shadertoy},
        {"shadertoy_extra.glsl", shaderfmt::Language::Shadertoy},
        {"basic.hlsl", shaderfmt::Language::HLSL},
        {"compute.hlsl", shaderfmt::Language::HLSL},
        {"basic.wgsl", shaderfmt::Language::WGSL},
        {"compute.wgsl", shaderfmt::Language::WGSL},
        {"basic.metal", shaderfmt::Language::MSL},
        {"compute.metal", shaderfmt::Language::MSL},
        {"basic.shader", shaderfmt::Language::ShaderLab},
        // Real open-source shaders with verified per-file (or repo-level)
        // licenses - see tests/corpus/licensed/SOURCES.md for exact
        // provenance and license per file.
        {"licensed/base.frag", shaderfmt::Language::GLSL},
        {"licensed/base.vert", shaderfmt::Language::GLSL},
        {"licensed/hello_triangle.hlsl", shaderfmt::Language::HLSL},
        {"licensed/hello_triangle.wgsl", shaderfmt::Language::WGSL},
        {"licensed/hello_workgroups.wgsl", shaderfmt::Language::WGSL},
        {"licensed/Metal_Blit.metal", shaderfmt::Language::MSL},
        {"licensed/Mobile-Diffuse.shader", shaderfmt::Language::ShaderLab},
    };
}

TEST(lexer_token_offset_recovers_exact_text) {
    for (const auto& cf : corpusFiles()) {
        std::string path = std::string(SHADERFMT_CORPUS_DIR) + "/" + cf.filename;
        std::string src = readFile(path);
        auto r = shaderfmt::lex(src, cf.lang);
        for (const auto& t : r.tokens) {
            if (t.type == shaderfmt::TokenType::EndOfFile) continue;
            if (t.offset + t.text.size() > src.size() || src.substr(t.offset, t.text.size()) != t.text) {
                std::cerr << "  [FAIL] " << g_currentTest << ": offset mismatch in " << cf.filename
                           << " for token '" << t.text << "' at offset " << t.offset << "\n";
                g_failures++;
            }
        }
    }
}

// ---------------------------------------------------------------------
// AST / Parser (ROADMAP §1): C-like grammar (GLSL/Shadertoy/HLSL) and WGSL
// grammar, both real. Not yet wired into format() (that's milestone 3+4):
// these tests exercise shaderfmt::parse() directly.
// ---------------------------------------------------------------------

TEST(ast_semantic_tokens_match_original_lex_on_corpus) {
    // All 8 corpus files, both grammars.
    for (const auto& cf : corpusFiles()) {
        std::string path = std::string(SHADERFMT_CORPUS_DIR) + "/" + cf.filename;
        std::string src = readFile(path);
        auto lexed = shaderfmt::lex(src, cf.lang);
        auto parsed = shaderfmt::parse(lexed.tokens, cf.lang);

        std::vector<std::string> fromLex = semanticTokenTexts(lexed.tokens);
        std::vector<std::string> fromAst;
        collectAstSemanticTokenTexts(*parsed.program, fromAst);

        if (fromLex != fromAst) {
            std::cerr << "  [FAIL] " << g_currentTest << ": AST semantic tokens diverge from lex for "
                       << cf.filename << " (lex=" << fromLex.size() << " ast=" << fromAst.size() << ")\n";
            g_failures++;
        }
    }
}

TEST(ast_comment_count_matches_original_lex_on_corpus) {
    for (const auto& cf : corpusFiles()) {
        std::string path = std::string(SHADERFMT_CORPUS_DIR) + "/" + cf.filename;
        std::string src = readFile(path);
        auto lexed = shaderfmt::lex(src, cf.lang);
        auto parsed = shaderfmt::parse(lexed.tokens, cf.lang);

        size_t lexComments = countCommentTokens(lexed.tokens);
        size_t astTrivia = countAstTrivia(*parsed.program);
        if (lexComments != astTrivia) {
            std::cerr << "  [FAIL] " << g_currentTest << ": " << cf.filename << " has " << lexComments
                       << " comment tokens but only " << astTrivia << " attached as AST trivia\n";
            g_failures++;
        }
    }
}

TEST(ast_parser_never_throws_on_fuzz_inputs) {
    std::mt19937 rng(0xC0FFEE);
    std::uniform_int_distribution<int> lenDist(0, 400);
    static const char kAlphabet[] = "abcxyz01239_{}()[];,.\"'/*#@<>=+- \t\n";
    std::uniform_int_distribution<int> charDist(0, static_cast<int>(sizeof(kAlphabet)) - 2);
    const shaderfmt::Language langs[] = {shaderfmt::Language::GLSL, shaderfmt::Language::Shadertoy,
                                          shaderfmt::Language::HLSL, shaderfmt::Language::WGSL};

    for (int iter = 0; iter < 500; iter++) {
        int len = lenDist(rng);
        std::string src;
        src.reserve(static_cast<size_t>(len));
        for (int i = 0; i < len; i++) src += kAlphabet[charDist(rng)];
        shaderfmt::Language lang = langs[iter % 4];
        try {
            auto lexed = shaderfmt::lex(src, lang);
            shaderfmt::parse(lexed.tokens, lang);
        } catch (const std::exception& e) {
            std::cerr << "  [FAIL] " << g_currentTest << ": exception parsing random input (iter " << iter
                       << ", len " << len << "): " << e.what() << "\n";
            g_failures++;
        }
    }
}

TEST(ast_parser_never_throws_on_truncated_corpus) {
    for (const auto& cf : corpusFiles()) {
        std::string path = std::string(SHADERFMT_CORPUS_DIR) + "/" + cf.filename;
        std::string src = readFile(path);
        for (size_t cut = 0; cut <= src.size(); cut += std::max<size_t>(1, src.size() / 50)) {
            std::string truncated = src.substr(0, cut);
            try {
                auto lexed = shaderfmt::lex(truncated, cf.lang);
                shaderfmt::parse(lexed.tokens, cf.lang);
            } catch (const std::exception& e) {
                std::cerr << "  [FAIL] " << g_currentTest << ": exception truncating " << cf.filename
                           << " at byte " << cut << ": " << e.what() << "\n";
                g_failures++;
            }
        }
    }
}

TEST(ast_function_decl_shape_and_if_else_children) {
    std::string src = "void f() {\n"
                       "    if (x) {\n"
                       "        y;\n"
                       "    } else {\n"
                       "        z;\n"
                       "    }\n"
                       "}\n";
    auto lexed = shaderfmt::lex(src, shaderfmt::Language::GLSL);
    auto parsed = shaderfmt::parse(lexed.tokens, shaderfmt::Language::GLSL);

    using shaderfmt::NodeKind;
    CHECK_EQ(parsed.program->children.size(), static_cast<size_t>(1));
    CHECK(parsed.program->children[0]->kind == NodeKind::FunctionDecl);
    auto& fn = *parsed.program->children[0];
    CHECK_EQ(fn.children.size(), static_cast<size_t>(1)); // body block
    auto& body = *fn.children[0];
    CHECK(body.kind == NodeKind::Block);
    CHECK_EQ(body.children.size(), static_cast<size_t>(1)); // the if-statement
    auto& ifStmt = *body.children[0];
    CHECK(ifStmt.kind == NodeKind::IfStmt);
    CHECK_EQ(ifStmt.children.size(), static_cast<size_t>(3)); // cond, then, else
    CHECK(ifStmt.children[0]->kind == NodeKind::Chunk);
    CHECK(ifStmt.children[1]->kind == NodeKind::Block);
    CHECK(ifStmt.children[2]->kind == NodeKind::Block);
}

TEST(ast_function_forward_declaration_has_no_body) {
    std::string src = "void f();\n";
    auto lexed = shaderfmt::lex(src, shaderfmt::Language::GLSL);
    auto parsed = shaderfmt::parse(lexed.tokens, shaderfmt::Language::GLSL);
    CHECK_EQ(parsed.program->children.size(), static_cast<size_t>(1));
    CHECK(parsed.program->children[0]->kind == shaderfmt::NodeKind::FunctionDecl);
    CHECK(parsed.program->children[0]->children.empty());
}

TEST(ast_leading_comment_attaches_to_the_following_statement) {
    std::string src = "void f() {\n"
                       "    // about x\n"
                       "    int x;\n"
                       "    int y;\n"
                       "}\n";
    auto lexed = shaderfmt::lex(src, shaderfmt::Language::GLSL);
    auto parsed = shaderfmt::parse(lexed.tokens, shaderfmt::Language::GLSL);
    auto& body = *parsed.program->children[0]->children[0];
    CHECK_EQ(body.children.size(), static_cast<size_t>(2));
    CHECK_EQ(body.children[0]->leadingTrivia.size(), static_cast<size_t>(1));
    CHECK_EQ(body.children[0]->leadingTrivia[0].token.text, std::string("// about x"));
    CHECK(body.children[1]->leadingTrivia.empty());
}

TEST(ast_trailing_comment_attaches_to_the_same_line_statement) {
    std::string src = "int x; // note\nint y;\n";
    auto lexed = shaderfmt::lex(src, shaderfmt::Language::GLSL);
    auto parsed = shaderfmt::parse(lexed.tokens, shaderfmt::Language::GLSL);
    CHECK_EQ(parsed.program->children.size(), static_cast<size_t>(2));
    CHECK_EQ(parsed.program->children[0]->trailingTrivia.size(), static_cast<size_t>(1));
    CHECK_EQ(parsed.program->children[0]->trailingTrivia[0].token.text, std::string("// note"));
    CHECK(parsed.program->children[1]->trailingTrivia.empty());
}

TEST(ast_struct_fields_are_children) {
    std::string src = "struct VSOutput {\n"
                       "    float4 position : SV_POSITION;\n"
                       "    float3 normal : NORMAL;\n"
                       "};\n";
    auto lexed = shaderfmt::lex(src, shaderfmt::Language::HLSL);
    auto parsed = shaderfmt::parse(lexed.tokens, shaderfmt::Language::HLSL);
    CHECK_EQ(parsed.program->children.size(), static_cast<size_t>(1));
    auto& st = *parsed.program->children[0];
    CHECK(st.kind == shaderfmt::NodeKind::StructDecl);
    CHECK_EQ(st.children.size(), static_cast<size_t>(2));
    CHECK(st.children[0]->kind == shaderfmt::NodeKind::StructField);
    CHECK(st.children[1]->kind == shaderfmt::NodeKind::StructField);
    // trailing ';' after the closing '}' must be captured (see
    // collectAstSemanticTokenTexts' StructDecl special case).
    CHECK(!st.tokens.empty());
    CHECK_EQ(st.tokens.back().text, std::string(";"));
}

TEST(ast_hlsl_cbuffer_reuses_struct_decl_shape) {
    std::string src = "cbuffer PerFrame : register(b0) {\n"
                       "    float4x4 mvp;\n"
                       "};\n";
    auto lexed = shaderfmt::lex(src, shaderfmt::Language::HLSL);
    auto parsed = shaderfmt::parse(lexed.tokens, shaderfmt::Language::HLSL);
    CHECK_EQ(parsed.program->children.size(), static_cast<size_t>(1));
    auto& cb = *parsed.program->children[0];
    CHECK(cb.kind == shaderfmt::NodeKind::StructDecl);
    CHECK_EQ(cb.children.size(), static_cast<size_t>(1));
    bool sawRegister = false;
    for (auto& t : cb.tokens) {
        if (t.text == "register") sawRegister = true;
    }
    CHECK(sawRegister);
}

TEST(ast_msl_function_and_struct_with_attributes) {
    std::string src = "#include <metal_stdlib>\n"
                       "using namespace metal;\n"
                       "\n"
                       "struct VertexOut {\n"
                       "    float4 position [[position]];\n"
                       "};\n"
                       "\n"
                       "vertex VertexOut vertex_main(uint vid [[vertex_id]]) {\n"
                       "    VertexOut out;\n"
                       "    return out;\n"
                       "}\n";
    auto lexed = shaderfmt::lex(src, shaderfmt::Language::MSL);
    auto parsed = shaderfmt::parse(lexed.tokens, shaderfmt::Language::MSL);
    // 2 PreprocessorLine/DeclStmt-ish top items ("#include", "using
    // namespace metal;") + StructDecl + FunctionDecl.
    CHECK_EQ(parsed.program->children.size(), static_cast<size_t>(4));
    CHECK(parsed.program->children[0]->kind == shaderfmt::NodeKind::PreprocessorLine);
    auto& st = *parsed.program->children[2];
    CHECK(st.kind == shaderfmt::NodeKind::StructDecl);
    CHECK_EQ(st.children.size(), static_cast<size_t>(1));
    auto& fn = *parsed.program->children[3];
    CHECK(fn.kind == shaderfmt::NodeKind::FunctionDecl);
    CHECK_EQ(fn.children.size(), static_cast<size_t>(1)); // body block
    bool sawVertexId = false;
    for (auto& t : fn.tokens) {
        if (t.text == "vertex_id") sawVertexId = true;
    }
    CHECK(sawVertexId);
}

TEST(ast_shaderlab_shape_and_embedded_hlsl_program) {
    std::string src = "Shader \"Custom/X\" {\n"
                       "    Properties {\n"
                       "        _MainTex (\"Texture\", 2D) = \"white\" {}\n"
                       "    }\n"
                       "    SubShader {\n"
                       "        Pass {\n"
                       "            CGPROGRAM\n"
                       "            float4 frag() : SV_Target { return float4(1,1,1,1); }\n"
                       "            ENDCG\n"
                       "        }\n"
                       "    }\n"
                       "    Fallback \"Diffuse\"\n"
                       "}\n";
    auto lexed = shaderfmt::lex(src, shaderfmt::Language::ShaderLab);
    auto parsed = shaderfmt::parse(lexed.tokens, shaderfmt::Language::ShaderLab);

    using shaderfmt::NodeKind;
    CHECK_EQ(parsed.program->children.size(), static_cast<size_t>(1));
    auto& shaderDecl = *parsed.program->children[0];
    CHECK(shaderDecl.kind == NodeKind::ShaderDecl);
    CHECK_EQ(shaderDecl.children.size(), static_cast<size_t>(3)); // Properties, SubShader, Fallback

    auto& properties = *shaderDecl.children[0];
    CHECK(properties.kind == NodeKind::ShaderLabBlock);
    CHECK_EQ(properties.children.size(), static_cast<size_t>(1));
    CHECK(properties.children[0]->kind == NodeKind::ShaderLabCommand);

    auto& subShader = *shaderDecl.children[1];
    CHECK(subShader.kind == NodeKind::ShaderLabBlock);
    auto& pass = *subShader.children[0];
    CHECK(pass.kind == NodeKind::ShaderLabBlock);
    auto& cg = *pass.children[0];
    CHECK(cg.kind == NodeKind::CgProgramBlock);

    // The embedded HLSL got parsed for real, not kept opaque: a genuine
    // FunctionDecl with an HLSL semantic in its signature.
    CHECK_EQ(cg.children.size(), static_cast<size_t>(1));
    auto& hlslProgram = *cg.children[0];
    CHECK(hlslProgram.kind == NodeKind::Program);
    CHECK_EQ(hlslProgram.children.size(), static_cast<size_t>(1));
    CHECK(hlslProgram.children[0]->kind == NodeKind::FunctionDecl);
    bool sawSvTarget = false;
    for (auto& t : hlslProgram.children[0]->tokens) {
        if (t.text == "SV_Target") sawSvTarget = true;
    }
    CHECK(sawSvTarget);

    auto& fallback = *shaderDecl.children[2];
    CHECK(fallback.kind == NodeKind::ShaderLabCommand);
}

TEST(ast_switch_case_labels_are_siblings_not_nested) {
    std::string src = "void f() {\n"
                       "    switch (x) {\n"
                       "    case 1:\n"
                       "        y;\n"
                       "        break;\n"
                       "    default:\n"
                       "        z;\n"
                       "    }\n"
                       "}\n";
    auto lexed = shaderfmt::lex(src, shaderfmt::Language::GLSL);
    auto parsed = shaderfmt::parse(lexed.tokens, shaderfmt::Language::GLSL);
    auto& sw = *parsed.program->children[0]->children[0]->children[0];
    CHECK(sw.kind == shaderfmt::NodeKind::SwitchStmt);
    auto& swBody = *sw.children[1];
    // case 1: , y; , break; , default: , z;  -> 5 flat siblings
    CHECK_EQ(swBody.children.size(), static_cast<size_t>(5));
    CHECK(swBody.children[0]->kind == shaderfmt::NodeKind::CaseLabel);
    CHECK(swBody.children[3]->kind == shaderfmt::NodeKind::CaseLabel);
}

TEST(ast_unrecognized_construct_becomes_error_node_and_still_progresses) {
    // A stray ')' at top level matches no production - must not hang, must
    // resync at the next top-level ';', and must still correctly parse a
    // well-formed declaration that follows it.
    std::string src = ") int x;\nint y;\n";
    auto lexed = shaderfmt::lex(src, shaderfmt::Language::GLSL);
    auto parsed = shaderfmt::parse(lexed.tokens, shaderfmt::Language::GLSL);
    CHECK_EQ(parsed.program->children.size(), static_cast<size_t>(2));
    CHECK(parsed.program->children[0]->kind == shaderfmt::NodeKind::ErrorNode);
    CHECK(parsed.program->children[1]->kind == shaderfmt::NodeKind::DeclStmt);
    CHECK(!parsed.errors.empty());
}

TEST(ast_wgsl_fn_with_attributes_and_body) {
    std::string src = "@vertex\n"
                       "fn vs_main(@location(0) position: vec3<f32>) -> @builtin(position) vec4<f32> {\n"
                       "    return vec4<f32>(position, 1.0);\n"
                       "}\n";
    auto lexed = shaderfmt::lex(src, shaderfmt::Language::WGSL);
    auto parsed = shaderfmt::parse(lexed.tokens, shaderfmt::Language::WGSL);
    CHECK_EQ(parsed.program->children.size(), static_cast<size_t>(1));
    auto& fn = *parsed.program->children[0];
    CHECK(fn.kind == shaderfmt::NodeKind::WgslFnDecl);
    CHECK_EQ(fn.children.size(), static_cast<size_t>(1)); // body block
    CHECK(fn.children[0]->kind == shaderfmt::NodeKind::Block);
    // the leading "@vertex" attribute must be preserved somewhere in the
    // opaque signature run (see Ast.hpp: attributes fold into `tokens`).
    bool sawVertexAttr = false;
    for (auto& t : fn.tokens) {
        if (t.text == "vertex") sawVertexAttr = true;
    }
    CHECK(sawVertexAttr);
}

TEST(ast_wgsl_struct_fields_are_children_comma_separated) {
    std::string src = "struct Uniforms {\n"
                       "    view_proj: mat4x4<f32>,\n"
                       "    light_dir: vec3<f32>,\n"
                       "};\n";
    auto lexed = shaderfmt::lex(src, shaderfmt::Language::WGSL);
    auto parsed = shaderfmt::parse(lexed.tokens, shaderfmt::Language::WGSL);
    CHECK_EQ(parsed.program->children.size(), static_cast<size_t>(1));
    auto& st = *parsed.program->children[0];
    CHECK(st.kind == shaderfmt::NodeKind::StructDecl);
    CHECK_EQ(st.children.size(), static_cast<size_t>(2));
    CHECK(st.children[0]->kind == shaderfmt::NodeKind::StructField);
    CHECK(!st.tokens.empty());
    CHECK_EQ(st.tokens.back().text, std::string(";"));
}

TEST(ast_wgsl_var_decl_with_attributes_and_address_space) {
    std::string src = "@group(0) @binding(0) var<storage, read_write> particles: array<Particle>;\n";
    auto lexed = shaderfmt::lex(src, shaderfmt::Language::WGSL);
    auto parsed = shaderfmt::parse(lexed.tokens, shaderfmt::Language::WGSL);
    CHECK_EQ(parsed.program->children.size(), static_cast<size_t>(1));
    CHECK(parsed.program->children[0]->kind == shaderfmt::NodeKind::WgslVarDecl);
}

TEST(ast_wgsl_local_let_and_if_inside_fn_body) {
    std::string src = "fn f() {\n"
                       "    let x = 1;\n"
                       "    if (x > 0) {\n"
                       "        return;\n"
                       "    }\n"
                       "}\n";
    auto lexed = shaderfmt::lex(src, shaderfmt::Language::WGSL);
    auto parsed = shaderfmt::parse(lexed.tokens, shaderfmt::Language::WGSL);
    auto& body = *parsed.program->children[0]->children[0];
    CHECK_EQ(body.children.size(), static_cast<size_t>(2));
    CHECK(body.children[0]->kind == shaderfmt::NodeKind::WgslLetConstDecl);
    CHECK(body.children[1]->kind == shaderfmt::NodeKind::IfStmt);
}

TEST(ast_wgsl_if_and_while_conditions_without_parens) {
    // Unlike C-like if/while, WGSL's grammar has no parens around
    // if/while conditions (only for-loop headers keep them) - real WGSL
    // code commonly omits them, e.g. "if lid.x == 0u { ... }".
    std::string src = "fn f() {\n"
                       "    if lid.x == 0u {\n"
                       "        a += 1;\n"
                       "    } else if lid.x == 1u {\n"
                       "        b += 1;\n"
                       "    }\n"
                       "    while i < 10u {\n"
                       "        i += 1u;\n"
                       "    }\n"
                       "}\n";
    auto lexed = shaderfmt::lex(src, shaderfmt::Language::WGSL);
    auto parsed = shaderfmt::parse(lexed.tokens, shaderfmt::Language::WGSL);
    auto& body = *parsed.program->children[0]->children[0];
    CHECK_EQ(body.children.size(), static_cast<size_t>(2)); // if-chain, while
    auto& ifStmt = *body.children[0];
    CHECK(ifStmt.kind == shaderfmt::NodeKind::IfStmt);
    CHECK_EQ(ifStmt.children.size(), static_cast<size_t>(3)); // cond, then, else-if
    CHECK(ifStmt.children[1]->kind == shaderfmt::NodeKind::Block); // real then-block, not a stray sibling
    auto& whileStmt = *body.children[1];
    CHECK(whileStmt.kind == shaderfmt::NodeKind::WhileStmt);
    CHECK(whileStmt.children[1]->kind == shaderfmt::NodeKind::Block);
}

// ---------------------------------------------------------------------
// Language auto-detection (ROADMAP §2: "Détection automatique du langage")
// ---------------------------------------------------------------------

TEST(detect_wgsl_from_attributes) {
    std::string src = "@vertex\nfn vs_main() -> @builtin(position) vec4<f32> { return vec4<f32>(); }";
    CHECK(shaderfmt::detectLanguage(src) == shaderfmt::Language::WGSL);
}

TEST(detect_hlsl_from_cbuffer_and_semantics) {
    std::string src = "cbuffer PerFrame : register(b0) { float4x4 mvp; };\n"
                       "float4 PSMain() : SV_Target { return float4(1,1,1,1); }";
    CHECK(shaderfmt::detectLanguage(src) == shaderfmt::Language::HLSL);
}

TEST(detect_shadertoy_from_mainimage_and_implicit_uniforms) {
    std::string src = "void mainImage(out vec4 fragColor, in vec2 fragCoord) {\n"
                       "    fragColor = vec4(fragCoord / iResolution.xy, 0.0, iTime);\n}";
    CHECK(shaderfmt::detectLanguage(src) == shaderfmt::Language::Shadertoy);
}

TEST(detect_glsl_as_fallback) {
    std::string src = "#version 450 core\nlayout(location = 0) out vec4 outColor;\n"
                       "void main() { outColor = vec4(1.0); }";
    CHECK(shaderfmt::detectLanguage(src) == shaderfmt::Language::GLSL);
}

TEST(detect_msl_from_metal_stdlib_and_attributes) {
    std::string src = "#include <metal_stdlib>\nusing namespace metal;\n"
                       "fragment float4 fs_main(float4 in [[stage_in]]) { return in; }";
    CHECK(shaderfmt::detectLanguage(src) == shaderfmt::Language::MSL);
}

TEST(detect_msl_does_not_false_positive_on_glsl_comment_mentioning_fragment) {
    // Regression: "vertex"/"fragment"/"kernel" are common English words
    // that show up in GLSL/HLSL comments describing the shader - must not
    // be used as bare-substring MSL signals (see LanguageDetector.cpp).
    std::string src = "#version 450 core\n// Simple lighting fragment shader\n"
                       "void main() { gl_FragColor = vec4(1.0); }";
    CHECK(shaderfmt::detectLanguage(src) == shaderfmt::Language::GLSL);
}

TEST(detect_shaderlab_from_subshader_and_cgprogram) {
    std::string src = "Shader \"Custom/X\" {\n"
                       "    SubShader {\n"
                       "        Pass {\n"
                       "            CGPROGRAM\n"
                       "            #pragma vertex vert\n"
                       "            ENDCG\n"
                       "        }\n"
                       "    }\n"
                       "}\n";
    CHECK(shaderfmt::detectLanguage(src) == shaderfmt::Language::ShaderLab);
}

TEST(detect_shaderlab_wins_over_hlsl_when_cgprogram_contains_hlsl_signals) {
    // A ShaderLab file's embedded CGPROGRAM commonly contains genuine
    // HLSL signals (SV_Target, cbuffer...) - ShaderLab must still win,
    // not get misclassified as plain HLSL (see LanguageDetector.cpp).
    std::string src = "Shader \"Custom/X\" { SubShader { Pass { CGPROGRAM\n"
                       "float4 frag() : SV_Target { return float4(1,1,1,1); }\n"
                       "ENDCG } } }\n";
    CHECK(shaderfmt::detectLanguage(src) == shaderfmt::Language::ShaderLab);
}

TEST(detect_matches_language_of_every_corpus_file) {
    for (const auto& cf : corpusFiles()) {
        std::string path = std::string(SHADERFMT_CORPUS_DIR) + "/" + cf.filename;
        std::string src = readFile(path);
        auto detected = shaderfmt::detectLanguage(src);
        if (detected != cf.lang) {
            std::cerr << "  [FAIL] " << g_currentTest << ": detectLanguage mismatch on "
                       << cf.filename << "\n";
            g_failures++;
        }
    }
}

// ---------------------------------------------------------------------
// Formatter: idempotence + semantic preservation over the corpus
// ---------------------------------------------------------------------

TEST(formatter_idempotent_on_corpus) {
    for (const auto& cf : corpusFiles()) {
        std::string path = std::string(SHADERFMT_CORPUS_DIR) + "/" + cf.filename;
        std::string src = readFile(path);

        auto pass1 = shaderfmt::format(src, cf.lang);
        auto pass2 = shaderfmt::format(pass1.formatted, cf.lang);

        if (pass1.formatted != pass2.formatted) {
            std::cerr << "  [FAIL] " << g_currentTest << ": not idempotent on " << cf.filename
                       << "\n";
            g_failures++;
        }
    }
}

TEST(formatter_never_changes_semantic_token_sequence_on_corpus) {
    for (const auto& cf : corpusFiles()) {
        std::string path = std::string(SHADERFMT_CORPUS_DIR) + "/" + cf.filename;
        std::string src = readFile(path);

        auto before = shaderfmt::lex(src, cf.lang);
        auto formatted = shaderfmt::format(src, cf.lang);
        auto after = shaderfmt::lex(formatted.formatted, cf.lang);

        auto beforeTokens = semanticTokenTexts(before.tokens);
        auto afterTokens = semanticTokenTexts(after.tokens);

        if (beforeTokens != afterTokens) {
            std::cerr << "  [FAIL] " << g_currentTest << ": token sequence changed for "
                       << cf.filename << " (before=" << beforeTokens.size()
                       << " after=" << afterTokens.size() << ")\n";
            g_failures++;
        }
    }
}

// ---------------------------------------------------------------------
// Reference-compiler non-regression (roadmap §6: "compilation avant/apres
// avec un compilateur de reference"). Optional: only runs when
// glslangValidator was found at configure time (see tests/CMakeLists.txt);
// otherwise this reports a SKIP, not a failure, so the rest of the suite
// stays runnable on machines without it. Covers GLSL/Shadertoy/HLSL, since
// glslang can compile all three (its HLSL front end is deprecated upstream
// but still present and usable here); WGSL has no reference compiler
// wired up in this iteration - see README "Ce qui est simplifie".
#ifdef SHADERFMT_GLSLANG_VALIDATOR

namespace {

struct CompileCheck {
    std::string filename;
    shaderfmt::Language lang;
    std::vector<std::string> stageArgs;
    bool isShadertoy; // needs the implicit-uniforms wrapper below
};

std::vector<CompileCheck> compileChecks() {
    return {
        {"basic.glsl", shaderfmt::Language::GLSL, {"-S", "frag"}, false},
        {"glsl_version_qualifiers.glsl", shaderfmt::Language::GLSL, {"-S", "vert"}, false},
        {"shadertoy.glsl", shaderfmt::Language::Shadertoy, {"-S", "frag"}, true},
        {"shadertoy_extra.glsl", shaderfmt::Language::Shadertoy, {"-S", "frag"}, true},
        {"basic.hlsl", shaderfmt::Language::HLSL, {"-D", "-V", "-S", "frag", "-e", "PSMain"}, false},
        {"compute.hlsl", shaderfmt::Language::HLSL, {"-D", "-V", "-S", "comp", "-e", "CSMain"}, false},
        // Real shaders (tests/corpus/licensed/SOURCES.md). licensed/base.frag
        // is deliberately excluded: it references a local "lighting.h" via
        // #include that isn't vendored, and glslangValidator actually tries
        // to open #include targets (unlike this project's own lexer, which
        // never resolves them) - see SOURCES.md.
        {"licensed/base.vert", shaderfmt::Language::GLSL, {"-S", "vert"}, false},
        {"licensed/hello_triangle.hlsl", shaderfmt::Language::HLSL, {"-D", "-V", "-S", "vert", "-e", "VSMain"}, false},
        {"licensed/hello_triangle.hlsl", shaderfmt::Language::HLSL, {"-D", "-V", "-S", "frag", "-e", "PSMain"}, false},
    };
}

// Shadertoy sources assume a hosting environment (mainImage entry point,
// implicit iTime/iResolution/... uniforms) that isn't part of the GLSL
// language itself - a real compiler needs those declared and needs a
// real main(). This is the same shim Shadertoy's own runtime effectively
// provides.
std::string wrapShadertoy(const std::string& src) {
    std::string out =
        "#version 450 core\n"
        "uniform vec3 iResolution;\n"
        "uniform float iTime;\n"
        "uniform vec4 iMouse;\n"
        "uniform int iFrame;\n"
        "uniform sampler2D iChannel0;\n"
        "out vec4 shaderfmt_fragColor;\n";
    out += src;
    out += "\nvoid main() { mainImage(shaderfmt_fragColor, gl_FragCoord.xy); }\n";
    return out;
}

bool runGlslang(const std::vector<std::string>& stageArgs, const std::string& filePath) {
    std::string cmd = std::string("\"") + SHADERFMT_GLSLANG_VALIDATOR + "\"";
    for (const auto& a : stageArgs) cmd += " " + a;
    // HLSL entries pass -V (SPIR-V codegen is mandatory for glslang's HLSL
    // front end), which by default writes a SPIR-V file ("comp.spv" etc.)
    // into the *current working directory* - wherever ctest/the test
    // binary happens to be invoked from, not a scratch dir. Redirect it to
    // the null device so running the tests never leaves stray .spv files
    // lying around the repo - found comp.spv/frag.spv sitting in the
    // project root after a normal test run, hence this. -o only makes
    // sense (glslang errors otherwise: "no binary generation requested")
    // when -V is actually present, so it's conditional, unlike the output
    // redirection below.
    bool hasCodegenFlag = std::find(stageArgs.begin(), stageArgs.end(), std::string("-V")) != stageArgs.end();
#ifdef _WIN32
    if (hasCodegenFlag) cmd += " -o NUL";
    cmd += " \"" + filePath + "\"";
    cmd += " > NUL 2>&1";
#else
    if (hasCodegenFlag) cmd += " -o /dev/null";
    cmd += " \"" + filePath + "\"";
    cmd += " > /dev/null 2>&1";
#endif
#ifdef _WIN32
    // MSVCRT's system() hands the string to "cmd /c <string>" as a single
    // argument; when <string> itself starts with a quoted token (our exe
    // path) cmd's own quote-stripping gets confused unless the whole
    // thing is wrapped in one more pair of quotes.
    cmd = "\"" + cmd + "\"";
#endif
    int rc = std::system(cmd.c_str());
    return rc == 0;
}

} // namespace

TEST(formatter_reference_compiler_accepts_output_before_and_after) {
    std::string tmpDir = std::string(SHADERFMT_TEST_TMPDIR);
    for (const auto& cc : compileChecks()) {
        std::string path = std::string(SHADERFMT_CORPUS_DIR) + "/" + cc.filename;
        std::string src = readFile(path);
        std::string formatted = shaderfmt::format(src, cc.lang).formatted;

        std::string beforeSrc = cc.isShadertoy ? wrapShadertoy(src) : src;
        std::string afterSrc = cc.isShadertoy ? wrapShadertoy(formatted) : formatted;
        std::string ext = (cc.lang == shaderfmt::Language::HLSL) ? ".hlsl" : ".glsl";

        std::string beforePath = writeTempFile(tmpDir + "/" + cc.filename + ".before" + ext, beforeSrc);
        std::string afterPath = writeTempFile(tmpDir + "/" + cc.filename + ".after" + ext, afterSrc);

        bool beforeOk = runGlslang(cc.stageArgs, beforePath);
        bool afterOk = runGlslang(cc.stageArgs, afterPath);

        if (!beforeOk) {
            std::cerr << "  [FAIL] " << g_currentTest << ": reference compiler rejects the ORIGINAL "
                       << cc.filename << " - the test corpus itself doesn't compile\n";
            g_failures++;
        } else if (!afterOk) {
            std::cerr << "  [FAIL] " << g_currentTest << ": " << cc.filename
                       << " compiles before formatting but not after - formatting changed validity\n";
            g_failures++;
        }
    }
}

#else

TEST(formatter_reference_compiler_accepts_output_before_and_after) {
    std::cout << "  [SKIP] " << g_currentTest << ": glslangValidator not found at configure time\n";
}

#endif

// WGSL reference-compiler non-regression test (roadmap §6's previously
// documented gap - "WGSL n'a pas de compilateur de reference branche dans
// cette iteration" - now closed via naga, https://github.com/gfx-rs/naga,
// installed as `naga-cli` (`cargo install naga-cli`)). Same idea as the
// glslang test above: compile the corpus before and after formatting,
// fail only if formatting turns a valid module invalid. naga's CLI just
// validates a single file with no separate stage-selection flags needed
// (unlike glslang's GLSL/HLSL front ends), so this is considerably
// simpler than runGlslang() above. Optional in the same way: reports a
// documented SKIP, not a failure, when naga wasn't found at configure
// time (see tests/CMakeLists.txt).
#ifdef SHADERFMT_NAGA

namespace {

std::vector<std::string> wgslCompileChecks() {
    return {"basic.wgsl", "compute.wgsl", "licensed/hello_triangle.wgsl", "licensed/hello_workgroups.wgsl"};
}

bool runNaga(const std::string& filePath) {
    std::string cmd = std::string("\"") + SHADERFMT_NAGA + "\" \"" + filePath + "\"";
#ifdef _WIN32
    cmd += " > NUL 2>&1";
    cmd = "\"" + cmd + "\"";
#else
    cmd += " > /dev/null 2>&1";
#endif
    int rc = std::system(cmd.c_str());
    return rc == 0;
}

} // namespace

TEST(formatter_wgsl_reference_compiler_accepts_output_before_and_after) {
    std::string tmpDir = std::string(SHADERFMT_TEST_TMPDIR);
    for (const auto& filename : wgslCompileChecks()) {
        std::string path = std::string(SHADERFMT_CORPUS_DIR) + "/" + filename;
        std::string src = readFile(path);
        std::string formatted = shaderfmt::format(src, shaderfmt::Language::WGSL).formatted;

        std::string beforePath = writeTempFile(tmpDir + "/" + filename + ".before.wgsl", src);
        std::string afterPath = writeTempFile(tmpDir + "/" + filename + ".after.wgsl", formatted);

        bool beforeOk = runNaga(beforePath);
        bool afterOk = runNaga(afterPath);

        if (!beforeOk) {
            std::cerr << "  [FAIL] " << g_currentTest << ": naga rejects the ORIGINAL "
                       << filename << " - the test corpus itself doesn't validate\n";
            g_failures++;
        } else if (!afterOk) {
            std::cerr << "  [FAIL] " << g_currentTest << ": " << filename
                       << " validates before formatting but not after - formatting changed validity\n";
            g_failures++;
        }
    }
}

#else

TEST(formatter_wgsl_reference_compiler_accepts_output_before_and_after) {
    std::cout << "  [SKIP] " << g_currentTest << ": naga not found at configure time\n";
}

#endif

// ---------------------------------------------------------------------
// Fuzzing (roadmap §6: "entrees aleatoires/malformees, aucun crash").
// This is a seeded pseudo-random *stress test*, not a coverage-guided
// fuzzer (libFuzzer/AFL) - MSVC's lack of first-class libFuzzer support
// made that a much bigger lift than this project's scope justifies. What
// it verifies is exactly the roadmap's wording: random and malformed
// byte sequences, including truncated/incomplete source, never crash or
// throw through lex()/format(), across every Language.
// ---------------------------------------------------------------------

TEST(fuzz_lex_and_format_never_crash_on_random_bytes) {
    std::mt19937 rng(0xC0FFEE);
    std::uniform_int_distribution<int> lenDist(0, 400);
    // Bias toward printable/shader-ish characters so a meaningful
    // fraction of inputs resemble almost-valid code (braces, quotes,
    // slashes) rather than being pure noise the lexer trivially rejects
    // token-by-token.
    static const char kAlphabet[] = "abcxyz01239_{}()[];,.\"'/*#@<>=+- \t\n";
    std::uniform_int_distribution<int> charDist(0, static_cast<int>(sizeof(kAlphabet)) - 2);

    const shaderfmt::Language langs[] = {shaderfmt::Language::GLSL, shaderfmt::Language::Shadertoy,
                                          shaderfmt::Language::HLSL, shaderfmt::Language::WGSL};

    for (int iter = 0; iter < 500; iter++) {
        int len = lenDist(rng);
        std::string src;
        src.reserve(static_cast<size_t>(len));
        for (int i = 0; i < len; i++) src += kAlphabet[charDist(rng)];

        shaderfmt::Language lang = langs[iter % 4];
        try {
            auto lexed = shaderfmt::lex(src, lang);
            auto formatted = shaderfmt::format(src, lang);
            // format() must be safe to re-run on its own (possibly odd)
            // output too, since the editor does this on every keystroke.
            shaderfmt::format(formatted.formatted, lang);
        } catch (const std::exception& e) {
            std::cerr << "  [FAIL] " << g_currentTest << ": exception on random input (seed iter "
                       << iter << ", len " << len << "): " << e.what() << "\n";
            g_failures++;
        }
    }
}

TEST(fuzz_lex_and_format_never_crash_on_truncated_corpus) {
    // Mutation-based: truncate every corpus file at every possible byte
    // offset - the exact "code collé incomplet" scenario the roadmap
    // calls out (mid-keystroke paste), applied exhaustively rather than
    // randomly since it's cheap enough to just do all of them.
    for (const auto& cf : corpusFiles()) {
        std::string path = std::string(SHADERFMT_CORPUS_DIR) + "/" + cf.filename;
        std::string src = readFile(path);
        for (size_t cut = 0; cut <= src.size(); cut += std::max<size_t>(1, src.size() / 50)) {
            std::string truncated = src.substr(0, cut);
            try {
                shaderfmt::lex(truncated, cf.lang);
                shaderfmt::format(truncated, cf.lang);
            } catch (const std::exception& e) {
                std::cerr << "  [FAIL] " << g_currentTest << ": exception truncating " << cf.filename
                           << " at byte " << cut << ": " << e.what() << "\n";
                g_failures++;
            }
        }
    }
}

// ---------------------------------------------------------------------
// Performance (roadmap §7: "Budget cible : formater 1000 lignes en moins
// de X ms percu comme instantane"). The roadmap leaves X unspecified;
// this test picks and enforces a concrete number - 50ms - chosen with
// ~15x headroom over what was actually measured on the dev machine for
// this exact synthetic input (~3ms release-build lex+format for 1000
// lines), comfortably under the classic "feels instant" UX threshold of
// ~100ms (Nielsen), while leaving margin for slower/virtualized CI
// runners. This is a real, permanent regression budget, not an assumed
// one - a future change that makes formatting meaningfully slower will
// fail this test.
// ---------------------------------------------------------------------

namespace {

std::string generateSyntheticGlsl(int targetLines) {
    std::ostringstream ss;
    ss << "#version 450 core\nuniform vec3 uLightDir;\nuniform sampler2D uAlbedo;\n\n";
    int linesEmitted = 0;
    int i = 0;
    while (linesEmitted < targetLines) {
        ss << "vec3 fn" << i << "(vec3 n, float t) {\n"
           << "    vec3 c = n * t + vec3(0.1, 0.2, 0.3);\n"
           << "    if (t > 0.5) {\n"
           << "        c = normalize(c);\n"
           << "        for (int k = 0; k < 4; k++) {\n"
           << "            c += vec3(0.01) * float(k);\n"
           << "        }\n"
           << "    } else {\n"
           << "        c = -c;\n"
           << "    }\n"
           << "    return c; // trailing comment\n"
           << "}\n\n";
        linesEmitted += 13;
        i++;
    }
    return ss.str();
}

} // namespace

TEST(perf_formats_1000_lines_within_budget) {
    std::string src = generateSyntheticGlsl(1000);

    auto t0 = std::chrono::steady_clock::now();
    auto result = shaderfmt::format(src, shaderfmt::Language::GLSL);
    auto t1 = std::chrono::steady_clock::now();

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double kBudgetMs = 50.0;

    std::cerr << "  [INFO] " << g_currentTest << ": " << ms << " ms for "
               << std::count(src.begin(), src.end(), '\n') << " lines (budget " << kBudgetMs << " ms)\n";

    if (ms >= kBudgetMs) {
        std::cerr << "  [FAIL] " << g_currentTest << ": " << ms << " ms exceeds the " << kBudgetMs
                   << " ms budget for 1000 lines\n";
        g_failures++;
    }
    CHECK(!result.formatted.empty());
}

TEST(formatter_collapses_multiple_blank_lines_to_one) {
    std::string src = "int a;\n\n\n\n\nint b;\n";
    auto out = shaderfmt::format(src, shaderfmt::Language::GLSL).formatted;
    CHECK(out.find("\n\n\n") == std::string::npos); // never more than one blank line
    CHECK(out.find("\n\n") != std::string::npos);    // but the single blank line survives
}

TEST(formatter_reindents_nested_braces) {
    std::string src = "void f(){if(true){int x;}}";
    auto out = shaderfmt::format(src, shaderfmt::Language::GLSL).formatted;
    CHECK(out.find("    if") != std::string::npos);
    CHECK(out.find("        int x;") != std::string::npos);
}

TEST(formatter_does_not_break_for_loop_semicolons) {
    std::string src = "for(int i=0;i<4;i++){x();}";
    auto out = shaderfmt::format(src, shaderfmt::Language::GLSL).formatted;
    // the two ';' inside the for-header must stay on the same source line
    auto headerEnd = out.find(')');
    std::string header = out.substr(0, headerEnd);
    CHECK(header.find('\n') == std::string::npos);
}

TEST(formatter_preserves_trailing_line_comment_on_same_line) {
    std::string src = "int x; // note\nint y;\n";
    auto out = shaderfmt::format(src, shaderfmt::Language::GLSL).formatted;
    auto pos = out.find("int x;");
    auto nl = out.find('\n', pos);
    CHECK(out.substr(pos, nl - pos).find("// note") != std::string::npos);
}

TEST(formatter_preprocessor_lines_flush_left_and_untouched) {
    std::string src = "void f() {\n#ifdef FOO\n    int x;\n#endif\n}\n";
    auto out = shaderfmt::format(src, shaderfmt::Language::GLSL).formatted;
    CHECK(out.find("\n#ifdef FOO\n") != std::string::npos);
    CHECK(out.find("\n#endif\n") != std::string::npos);
}

TEST(formatter_brace_style_next_line_allman) {
    std::string src = "void f(){if(true){int x;}}";
    shaderfmt::FormatOptions opts;
    opts.braceStyle = shaderfmt::BraceStyle::NextLine;
    auto out = shaderfmt::format(src, shaderfmt::Language::GLSL, opts).formatted;
    CHECK(out.find("void f()\n{") != std::string::npos);
    CHECK(out.find("if (true)\n") != std::string::npos);
    // "int x;" sits two brace levels deep (f's body, then if's body), so
    // it's indented 8 spaces, not 4 - the inner '{' is on its own line
    // at the *first* level's indent (4 spaces), matching Allman style.
    CHECK(out.find("    {\n        int x;") != std::string::npos);
}

TEST(formatter_brace_style_next_line_is_idempotent) {
    std::string src = "void f(){if(true){int x;}}";
    shaderfmt::FormatOptions opts;
    opts.braceStyle = shaderfmt::BraceStyle::NextLine;
    auto pass1 = shaderfmt::format(src, shaderfmt::Language::GLSL, opts).formatted;
    auto pass2 = shaderfmt::format(pass1, shaderfmt::Language::GLSL, opts).formatted;
    CHECK_EQ(pass1, pass2);
}

TEST(formatter_space_after_control_keyword_can_be_disabled) {
    std::string src = "void f(){if(true){int x;}for(int i=0;i<1;i++){}}";
    shaderfmt::FormatOptions opts;
    opts.spaceAfterControlKeyword = false;
    auto out = shaderfmt::format(src, shaderfmt::Language::GLSL, opts).formatted;
    CHECK(out.find("if(true)") != std::string::npos);
    CHECK(out.find("for(int") != std::string::npos);
    CHECK(out.find("if (") == std::string::npos);
    CHECK(out.find("for (") == std::string::npos);
    // A regular function call must stay tight regardless of this option -
    // only the fixed control-flow keyword list is affected.
    CHECK(out.find("void f()") != std::string::npos);
}

TEST(formatter_space_after_control_keyword_default_is_enabled) {
    std::string src = "if(true){}";
    auto out = shaderfmt::format(src, shaderfmt::Language::GLSL).formatted;
    CHECK(out.find("if (true)") != std::string::npos);
}

TEST(glsl_version_lint_flags_attribute_varying_on_modern_version) {
    std::string src = "#version 330 core\nattribute vec3 aPos;\nvarying vec3 vColor;\nvoid main(){}\n";
    auto warnings = shaderfmt::lintGlslVersionQualifiers(src);
    CHECK_EQ(warnings.size(), size_t(2));
    CHECK(warnings[0].message.find("attribute") != std::string::npos);
    CHECK_EQ(warnings[0].line, 2);
    CHECK_EQ(warnings[0].column, 1);
    CHECK_EQ(warnings[0].length, 9); // "attribute"
    CHECK(warnings[1].message.find("varying") != std::string::npos);
    CHECK_EQ(warnings[1].line, 3);
}

TEST(glsl_version_lint_allows_attribute_varying_on_legacy_version) {
    std::string src = "#version 120\nattribute vec3 aPos;\nvarying vec3 vColor;\nvoid main(){}\n";
    auto warnings = shaderfmt::lintGlslVersionQualifiers(src);
    CHECK(warnings.empty());
}

TEST(glsl_version_lint_flags_layout_before_140) {
    std::string src = "#version 120\nlayout(location = 0) in vec3 aPos;\nvoid main(){}\n";
    auto warnings = shaderfmt::lintGlslVersionQualifiers(src);
    CHECK_EQ(warnings.size(), size_t(1));
    CHECK(warnings[0].message.find("layout") != std::string::npos);
    CHECK_EQ(warnings[0].line, 2);
}

TEST(glsl_version_lint_allows_layout_on_140_plus) {
    std::string src = "#version 330 core\nlayout(location = 0) in vec3 aPos;\nvoid main(){}\n";
    auto warnings = shaderfmt::lintGlslVersionQualifiers(src);
    CHECK(warnings.empty());
}

TEST(glsl_version_lint_defaults_to_110_without_a_version_directive) {
    // No #version directive -> GLSL defaults to 1.10 per spec, the most
    // restrictive case: attribute/varying are fine, layout is not.
    std::string src = "attribute vec3 aPos;\nlayout(location = 0) in vec3 bPos;\nvoid main(){}\n";
    auto warnings = shaderfmt::lintGlslVersionQualifiers(src);
    CHECK_EQ(warnings.size(), size_t(1));
    CHECK(warnings[0].message.find("layout") != std::string::npos);
}

TEST(glsl_version_lint_is_purely_advisory_never_changes_formatting) {
    // The linter must never influence format() - it's a separate,
    // advisory-only pass (roadmap §0's "zero semantic modification").
    std::string src = "#version 330 core\nattribute vec3 aPos;\nvoid main(){}\n";
    auto withoutLint = shaderfmt::format(src, shaderfmt::Language::GLSL).formatted;
    (void)shaderfmt::lintGlslVersionQualifiers(src);
    auto afterLintCalled = shaderfmt::format(src, shaderfmt::Language::GLSL).formatted;
    CHECK_EQ(withoutLint, afterLintCalled);
}

TEST(formatter_brace_style_same_line_is_default) {
    std::string src = "void f(){int x;}";
    auto out = shaderfmt::format(src, shaderfmt::Language::GLSL).formatted;
    CHECK(out.find("void f() {") != std::string::npos);
}

TEST(formatter_tolerant_input_still_produces_output) {
    // Malformed / mid-typing input must never crash and should still
    // produce *some* formatted text (per "parser tolerant aux erreurs").
    std::string src = "void main() {\n    float x = 1.0 +\n";
    auto res = shaderfmt::format(src, shaderfmt::Language::GLSL);
    CHECK(!res.formatted.empty());
}

int main() {
    int total = static_cast<int>(registry().size());
    int ran = 0;
    for (auto& tc : registry()) {
        g_currentTest = tc.name;
        try {
            tc.fn();
        } catch (const std::exception& e) {
            std::cerr << "  [FAIL] " << tc.name << ": exception: " << e.what() << "\n";
            g_failures++;
        }
        ran++;
    }
    std::cout << ran << "/" << total << " tests run, " << g_failures << " failure(s).\n";
    return g_failures == 0 ? 0 : 1;
}
