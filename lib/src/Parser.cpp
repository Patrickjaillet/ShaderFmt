#include "shaderfmt/Parser.hpp"

#include <algorithm>
#include <initializer_list>
#include <set>

namespace shaderfmt {

namespace {

// Consumed greedily as an opaque prefix before a type in declarations/
// fields/signatures - never individually validated, mirroring the
// existing "qualifiers aren't semantically checked" limitation shared
// with the rest of this codebase.
const std::set<std::string>& clikeQualifierKeywords() {
    static const std::set<std::string> kw = {
        "in", "out", "inout", "uniform", "const", "flat", "smooth", "noperspective",
        "centroid", "sample", "patch", "highp", "mediump", "lowp", "precision",
        "readonly", "writeonly", "coherent", "volatile", "restrict", "buffer",
        "shared", "layout", "invariant", "precise", "attribute", "varying",
        // HLSL
        "static", "groupshared", "inline", "extern", "nointerpolation", "linear",
        "row_major", "column_major", "globallycoherent", "unorm", "snorm",
        // MSL: function-role qualifiers (before the return type) and
        // address-space qualifiers (before a parameter/variable type).
        "vertex", "fragment", "kernel", "constant", "device", "threadgroup",
        "thread", "constexpr",
    };
    return kw;
}

// What a chunk-scan stopped on: either it found one of the requested
// terminator texts (at bracket-depth 0, not consumed), or it hit a
// depth-0 '{'/'}' (a block boundary always ends a chunk scan - a chunk
// never spans into or out of a nested block in this grammar), or EOF.
enum class ScanStop { StopText, OpenBrace, CloseBrace, End };

// ShaderLab keywords that always introduce a "keyword [args] { ... }"
// block, at any nesting depth (Pass can contain Tags just as SubShader
// can, etc.) - everything else identifier-led is a bare line command
// (Cull Back, ZWrite On, LOD 100, ...).
bool isShaderLabBlockKeyword(const std::string& text) {
    static const std::set<std::string> kw = {"Properties", "SubShader", "Pass", "Tags", "Category"};
    return kw.count(text) > 0;
}

// HLSL built-in generic resource/buffer type names. Unlike WGSL, HLSL has
// no user-defined generics, so this is an intentionally narrow allowlist
// (mirrors wgslGenericTypeNamesForFieldSplitting() below) - a real "a < b"
// comparison can never start with one of these reserved type names, so
// treating '<' right after one as a generic-open is always safe.
const std::set<std::string>& hlslGenericTypeNames() {
    static const std::set<std::string> names = {
        "Buffer", "RWBuffer", "StructuredBuffer", "RWStructuredBuffer",
        "AppendStructuredBuffer", "ConsumeStructuredBuffer", "ByteAddressBuffer",
        "RWByteAddressBuffer", "ConstantBuffer",
        "Texture1D", "Texture1DArray", "Texture2D", "Texture2DArray", "Texture2DMS",
        "Texture2DMSArray", "Texture3D", "TextureCube", "TextureCubeArray",
        "RWTexture1D", "RWTexture1DArray", "RWTexture2D", "RWTexture2DArray", "RWTexture3D",
        "InputPatch", "OutputPatch"};
    return names;
}

} // namespace

class Parser {
public:
    Parser(const std::vector<Token>& tokens, Language lang) : tokens_(tokens), lang_(lang) {}

    ParseResult run() {
        // parseProgram()/parseStatement() are language-aware at the few
        // points where GLSL/HLSL/Shadertoy and WGSL grammars genuinely
        // differ (declaration shape, struct field separators); everything
        // else (control flow, blocks, tolerant recovery) is shared code,
        // used by both. ShaderLab is structurally different enough (no
        // C-like statement grammar at the outer level at all) to get its
        // own top-level entry point.
        NodePtr program = (lang_ == Language::ShaderLab) ? parseShaderLabProgram() : parseProgram();
        return ParseResult{std::move(program), std::move(errors_)};
    }

private:
    const std::vector<Token>& tokens_;
    Language lang_;
    size_t cursor_ = 0;
    std::vector<LexError> errors_;

    // --- low-level token access ---

    bool atEnd() const { return cursor_ >= tokens_.size() || tokens_[cursor_].type == TokenType::EndOfFile; }

    const Token& peek(size_t offset = 0) const {
        size_t i = cursor_ + offset;
        return i < tokens_.size() ? tokens_[i] : tokens_.back(); // tokens_.back() is always EndOfFile
    }

    Token advance() {
        Token t = peek();
        if (cursor_ < tokens_.size() && tokens_[cursor_].type != TokenType::EndOfFile) cursor_++;
        return t;
    }

    bool atKeyword(const char* kw) const {
        return !atEnd() && peek().type == TokenType::Identifier && peek().text == kw;
    }

    bool atPunct(const char* p) const {
        return !atEnd() && peek().type == TokenType::Punctuator && peek().text == p;
    }

    bool atType(TokenType type) const { return !atEnd() && peek().type == type; }

    static bool isComment(const Token& t) {
        return t.type == TokenType::LineComment || t.type == TokenType::BlockComment;
    }

    // Captures the current token's onOwnLine/blankLinesBefore before any
    // caller consumes it - see Ast.hpp's comment on Node::onOwnLine for
    // why this matters (structural kinds don't otherwise store their own
    // leading keyword token). Call this before consuming the first token
    // of whatever construct is about to be parsed.
    NodePtr makeNode(NodeKind kind) {
        auto n = std::make_unique<Node>();
        n->kind = kind;
        if (!atEnd()) {
            n->onOwnLine = peek().onOwnLine;
            n->blankLinesBefore = peek().blankLinesBefore;
        }
        return n;
    }

    // --- trivia (comments) ---

    std::vector<Trivia> collectLeadingTrivia() {
        std::vector<Trivia> out;
        while (!atEnd() && isComment(peek()) && peek().onOwnLine) {
            out.push_back(Trivia{advance(), false});
        }
        return out;
    }

    void collectTrailingTrivia(Node& node) {
        if (!atEnd() && isComment(peek()) && !peek().onOwnLine) {
            node.trailingTrivia.push_back(Trivia{advance(), true});
        }
    }

    // --- generic chunk scanning ---

    // Scans tokens into `out`, tracking ()/[] nesting depth, stopping
    // (without consuming the triggering token) at a depth-0 '{'/'}' or a
    // depth-0 occurrence of any text in `stopTexts`. Always makes forward
    // progress if the current token isn't itself a stop condition -
    // callers that need a stronger guarantee (e.g. top-level recovery)
    // use recoverAsErrorNode() instead.
    ScanStop scanChunkTokens(std::vector<Token>& out, std::initializer_list<const char*> stopTexts) {
        int depth = 0;
        while (!atEnd()) {
            const Token& t = peek();
            if (t.type == TokenType::Punctuator && depth == 0) {
                if (t.text == "{") return ScanStop::OpenBrace;
                if (t.text == "}") return ScanStop::CloseBrace;
                for (const char* s : stopTexts) {
                    if (t.text == s) return ScanStop::StopText;
                }
            }
            if (t.type == TokenType::Punctuator) {
                if (t.text == "(" || t.text == "[") depth++;
                else if (t.text == ")" || t.text == "]") depth = std::max(0, depth - 1);
            }
            out.push_back(advance());
        }
        return ScanStop::End;
    }

    // Consumes a balanced "(...)" run (parens included), tolerant of a
    // missing/unbalanced opening paren (returns whatever was found).
    NodePtr parseParenChunk() {
        auto chunk = makeNode(NodeKind::Chunk);
        if (!atPunct("(")) return chunk;
        int depth = 0;
        do {
            bool isOpen = atPunct("(");
            bool isClose = atPunct(")");
            chunk->tokens.push_back(advance());
            if (isOpen) depth++;
            if (isClose) depth--;
        } while (depth > 0 && !atEnd());
        return chunk;
    }

    // --- tolerant-parse fallback ---

    // Wraps whatever the parser couldn't classify into an ErrorNode,
    // resyncing at the next top-level ';' (consumed) or the next
    // unconsumed '}' (left for the caller's own block-closing check) or
    // EOF. Always consumes at least one token when !atEnd() at entry,
    // guaranteeing forward progress for every caller's loop.
    NodePtr recoverAsErrorNode() {
        auto node = makeNode(NodeKind::ErrorNode);
        if (atEnd()) return node;
        int depth = 0;
        bool first = true;
        int startLine = peek().line, startCol = peek().column;
        while (!atEnd()) {
            const Token& t = peek();
            if (!first && t.type == TokenType::Punctuator && depth == 0 && t.text == "}") break;
            first = false;
            if (t.type == TokenType::Punctuator) {
                if (t.text == "(" || t.text == "[") depth++;
                else if (t.text == ")" || t.text == "]") depth = std::max(0, depth - 1);
            }
            bool isTopLevelSemicolon = (t.type == TokenType::Punctuator && t.text == ";" && depth == 0);
            node->tokens.push_back(advance());
            if (isTopLevelSemicolon) break;
        }
        errors_.push_back(LexError{"construct not recognized by the grammar, preserved verbatim", startLine, startCol});
        return node;
    }

    // --- lookahead classification ---

    size_t skipQualifiersForLookahead(size_t i) const {
        while (i < tokens_.size()) {
            const Token& t = tokens_[i];
            if (t.type == TokenType::Identifier && clikeQualifierKeywords().count(t.text)) {
                bool isLayout = (t.text == "layout");
                i++;
                if (isLayout && i < tokens_.size() && tokens_[i].type == TokenType::Punctuator &&
                    tokens_[i].text == "(") {
                    int depth = 0;
                    do {
                        if (tokens_[i].text == "(") depth++;
                        else if (tokens_[i].text == ")") depth--;
                        i++;
                    } while (depth > 0 && i < tokens_.size());
                }
                continue;
            }
            if (isComment(t)) {
                i++;
                continue;
            }
            if (t.type == TokenType::Punctuator && t.text == "[") {
                // HLSL attribute ("[numthreads(64, 1, 1)]" before a
                // function, "[unroll]"/"[branch]" before a statement) -
                // skip the whole balanced "[...]" run so the checks below
                // see the actual return type/statement start next.
                int depth = 0;
                do {
                    if (tokens_[i].type == TokenType::Punctuator && tokens_[i].text == "[") depth++;
                    else if (tokens_[i].type == TokenType::Punctuator && tokens_[i].text == "]") depth--;
                    i++;
                } while (depth > 0 && i < tokens_.size());
                continue;
            }
            break;
        }
        return i;
    }

    // Returns the index just past a "<...>" generic argument list if `i`
    // points at an identifier in hlslGenericTypeNames() immediately
    // followed by '<' (nesting-aware, though these built-in types never
    // actually nest in practice - kept consistent with the WGSL handling
    // elsewhere in this file). Returns `i` unchanged when there's no
    // recognized generic there (including an unbalanced '<...>', which is
    // treated as "not a generic" rather than guessed at).
    size_t skipHlslGenericTypeSuffix(size_t i) const {
        if (i >= tokens_.size() || tokens_[i].type != TokenType::Identifier ||
            !hlslGenericTypeNames().count(tokens_[i].text)) {
            return i;
        }
        size_t j = i + 1;
        if (j >= tokens_.size() || tokens_[j].type != TokenType::Punctuator || tokens_[j].text != "<") return i;
        int depth = 0;
        while (j < tokens_.size()) {
            const Token& t = tokens_[j];
            if (t.type == TokenType::Punctuator && t.text == "<") {
                depth++;
            } else if (t.type == TokenType::Punctuator && t.text == ">") {
                depth--;
                j++;
                if (depth == 0) return j;
                continue;
            }
            j++;
        }
        return i; // unbalanced - bail out, treat as no generic recognized
    }

    bool looksLikeDeclStart() const {
        if (!atType(TokenType::Identifier)) return false;
        if (clikeQualifierKeywords().count(peek().text)) return true;
        size_t afterType = skipHlslGenericTypeSuffix(cursor_);
        if (afterType != cursor_) {
            // Recognized "GenericType<Args>" - a decl iff a name follows,
            // e.g. "RWStructuredBuffer<float4> gParticles : register(u0);".
            return afterType < tokens_.size() && tokens_[afterType].type == TokenType::Identifier;
        }
        // "Type name" - two bare identifiers back to back is reliable in
        // valid C-like code: an expression can never start that way (a
        // function call/assignment/increment always has a non-identifier
        // token right after the first identifier).
        return peek(1).type == TokenType::Identifier;
    }

    // Detects a GLSL named interface block header: zero or more qualifiers
    // (typically "layout(...)" and/or "uniform"/"buffer"/"in"/"out") then
    // a single Identifier (the block's type name) immediately followed by
    // '{' - e.g. "layout(set=0, binding=1) uniform GlobalUniform {". This
    // never fires for an ordinary declaration ("uniform vec3 lightPos;":
    // the second identifier is followed by ';', not '{') nor for
    // "struct"/"cbuffer" (those keywords are routed to parseStructOrCbuffer()
    // by parseTopLevelItem() before this is ever checked).
    bool looksLikeInterfaceBlockStart() const {
        size_t i = skipQualifiersForLookahead(cursor_);
        if (i >= tokens_.size() || tokens_[i].type != TokenType::Identifier) return false;
        i++;
        return i < tokens_.size() && tokens_[i].type == TokenType::Punctuator && tokens_[i].text == "{";
    }

    bool looksLikeFunctionDeclStart() const {
        size_t i = skipQualifiersForLookahead(cursor_);
        if (i >= tokens_.size() || tokens_[i].type != TokenType::Identifier) return false;
        i++; // return type
        if (i >= tokens_.size() || tokens_[i].type != TokenType::Identifier) return false;
        i++; // function name
        return i < tokens_.size() && tokens_[i].type == TokenType::Punctuator && tokens_[i].text == "(";
    }

    // --- productions ---

    NodePtr parseProgram() {
        auto program = makeNode(NodeKind::Program);
        for (;;) {
            auto leading = collectLeadingTrivia();
            if (atEnd()) {
                program->trailingTrivia.insert(program->trailingTrivia.end(), leading.begin(), leading.end());
                break;
            }
            size_t before = cursor_;
            auto item = parseTopLevelItem();
            if (cursor_ == before) { // safety net: should be unreachable, kept defensively
                item = makeNode(NodeKind::ErrorNode);
                item->tokens.push_back(advance());
            }
            item->leadingTrivia = std::move(leading);
            collectTrailingTrivia(*item);
            program->children.push_back(std::move(item));
        }
        return program;
    }

    NodePtr parseTopLevelItem() {
        if (atType(TokenType::Preprocessor)) return parsePreprocessorLine();
        if (lang_ == Language::WGSL) {
            if (atKeyword("struct")) return parseWgslStruct();
            return parseWgslAttributedDecl();
        }
        if (atKeyword("struct") || (lang_ == Language::HLSL && atKeyword("cbuffer"))) return parseStructOrCbuffer();
        if (looksLikeInterfaceBlockStart()) return parseInterfaceBlock();
        if (looksLikeFunctionDeclStart()) return parseFunctionDecl();
        if (looksLikeDeclStart()) return parseSimpleStatement();
        return recoverAsErrorNode();
    }

    NodePtr parsePreprocessorLine() {
        auto node = makeNode(NodeKind::PreprocessorLine);
        node->tokens.push_back(advance());
        return node;
    }

    NodePtr parseSimpleStatement() {
        bool isDecl = looksLikeDeclStart();
        auto node = makeNode(isDecl ? NodeKind::DeclStmt : NodeKind::ExprStmt);
        ScanStop stop = scanChunkTokens(node->tokens, {";"});
        if (stop == ScanStop::StopText) node->tokens.push_back(advance());
        // OpenBrace/CloseBrace/End: tolerant - statement ends without a
        // ';' (truncated/malformed input); nothing more to do here.
        return node;
    }

    NodePtr parseFunctionDecl() {
        auto node = makeNode(NodeKind::FunctionDecl);
        ScanStop stop = scanChunkTokens(node->tokens, {";"});
        if (stop == ScanStop::StopText) {
            node->tokens.push_back(advance()); // forward declaration
        } else if (stop == ScanStop::OpenBrace) {
            node->children.push_back(parseBlock());
        }
        // CloseBrace/End: malformed/truncated - return signature-only node.
        return node;
    }

    NodePtr parseStructOrCbuffer() {
        auto node = makeNode(NodeKind::StructDecl);
        ScanStop stop = scanChunkTokens(node->tokens, {";"});
        if (stop != ScanStop::OpenBrace) {
            node->hasBody = false;
            return node;
        }
        advance(); // '{'
        for (;;) {
            auto leading = collectLeadingTrivia();
            if (atEnd() || atPunct("}")) {
                node->trailingTrivia.insert(node->trailingTrivia.end(), leading.begin(), leading.end());
                break;
            }
            size_t before = cursor_;
            NodePtr field = atType(TokenType::Preprocessor) ? parsePreprocessorLine() : parseStructField();
            if (cursor_ == before) {
                field = makeNode(NodeKind::ErrorNode);
                field->tokens.push_back(advance());
            }
            field->leadingTrivia = std::move(leading);
            collectTrailingTrivia(*field);
            node->children.push_back(std::move(field));
        }
        if (atPunct("}")) {
            advance();
        } else {
            node->hasClosingBrace = false; // truncated: never found the matching '}'
        }
        if (atPunct(";")) node->tokens.push_back(advance()); // struct has one, cbuffer doesn't
        return node;
    }

    // GLSL named interface block: "[layout(...)] (uniform|buffer|in|out)
    // Name { field... } [instanceName[dims]] ;" - see
    // looksLikeInterfaceBlockStart(). Same header/body shape as
    // parseStructOrCbuffer() (reused verbatim: scanChunkTokens's paren-
    // depth tracking already handles "layout(...)" correctly), but GLSL
    // requires a ';' after the body - either immediately (anonymous block)
    // or after an instance declarator - so the optional declarator run is
    // captured into its own field (Node::trailingDeclarator) rather than
    // reusing `tokens`, since it belongs after the body, not before it.
    NodePtr parseInterfaceBlock() {
        auto node = makeNode(NodeKind::StructDecl);
        ScanStop stop = scanChunkTokens(node->tokens, {";"});
        if (stop != ScanStop::OpenBrace) {
            node->hasBody = false;
            return node;
        }
        advance(); // '{'
        for (;;) {
            auto leading = collectLeadingTrivia();
            if (atEnd() || atPunct("}")) {
                node->trailingTrivia.insert(node->trailingTrivia.end(), leading.begin(), leading.end());
                break;
            }
            size_t before = cursor_;
            NodePtr field = atType(TokenType::Preprocessor) ? parsePreprocessorLine() : parseStructField();
            if (cursor_ == before) {
                field = makeNode(NodeKind::ErrorNode);
                field->tokens.push_back(advance());
            }
            field->leadingTrivia = std::move(leading);
            collectTrailingTrivia(*field);
            node->children.push_back(std::move(field));
        }
        if (atPunct("}")) {
            advance();
        } else {
            node->hasClosingBrace = false;
        }
        // Bounded the same way as every other chunk scan here (stops at
        // the next top-level ';' or a further depth-0 '{') - a genuinely
        // malformed block missing its ';' entirely could still swallow a
        // following header's tokens up to *its* '{', the same residual
        // limitation already accepted elsewhere in this tolerant parser
        // for other malformed-input edge cases.
        if (node->hasClosingBrace) {
            ScanStop trailStop = scanChunkTokens(node->trailingDeclarator, {";"});
            if (trailStop == ScanStop::StopText) node->trailingDeclarator.push_back(advance());
        }
        return node;
    }

    NodePtr parseStructField() {
        auto node = makeNode(NodeKind::StructField);
        ScanStop stop = scanChunkTokens(node->tokens, {";"});
        if (stop == ScanStop::StopText) node->tokens.push_back(advance());
        return node;
    }

    NodePtr parseBlock() {
        // assumes current token is '{'; capture its own layout metadata
        // before consuming it, since makeNode() (called after) would
        // otherwise capture the first statement's metadata instead - see
        // Ast.hpp's comment on Node::onOwnLine.
        bool braceOnOwnLine = !atEnd() && peek().onOwnLine;
        int braceBlankLines = !atEnd() ? peek().blankLinesBefore : 0;
        advance();
        auto block = makeNode(NodeKind::Block);
        block->onOwnLine = braceOnOwnLine;
        block->blankLinesBefore = braceBlankLines;
        for (;;) {
            auto leading = collectLeadingTrivia();
            if (atEnd() || atPunct("}")) {
                block->trailingTrivia.insert(block->trailingTrivia.end(), leading.begin(), leading.end());
                break;
            }
            size_t before = cursor_;
            auto stmt = parseStatement();
            if (cursor_ == before) {
                stmt = makeNode(NodeKind::ErrorNode);
                stmt->tokens.push_back(advance());
            }
            stmt->leadingTrivia = std::move(leading);
            collectTrailingTrivia(*stmt);
            block->children.push_back(std::move(stmt));
        }
        if (atPunct("}")) {
            advance();
        } else {
            block->hasClosingBrace = false; // truncated: never found the matching '}'
        }
        return block;
    }

    // Parses a control-flow body (the statement or block right after
    // "if (...)"/"for (...)"/"while (...)"): either a real Block, or - for
    // a brace-less body - a single statement node wearing the trivia that
    // preceded it.
    NodePtr parseControlBody() {
        auto leading = collectLeadingTrivia();
        NodePtr body;
        if (atPunct("{")) {
            body = parseBlock();
        } else {
            size_t before = cursor_;
            body = parseStatement();
            if (cursor_ == before) {
                body = makeNode(NodeKind::ErrorNode);
                if (!atEnd()) body->tokens.push_back(advance());
            }
        }
        body->leadingTrivia.insert(body->leadingTrivia.begin(), leading.begin(), leading.end());
        collectTrailingTrivia(*body);
        return body;
    }

    // Shared by both grammars: control flow (if/for/while/switch/return/
    // break-continue-discard) is structurally the same in WGSL and the
    // C-like family, so only the "what's a plain declaration/expression
    // statement" branch differs (WGSL: var/let/const; C-like: the
    // "Type name" heuristic). do-while doesn't exist in WGSL.
    NodePtr parseStatement() {
        if (atPunct("{")) return parseBlock();
        if (atKeyword("if")) return parseIf();
        if (atKeyword("for")) return parseFor();
        if (atKeyword("while")) return parseWhile();
        if (lang_ != Language::WGSL && atKeyword("do")) return parseDoWhile();
        if (atKeyword("switch")) return parseSwitch();
        if (atKeyword("return")) return parseReturn();
        if (atKeyword("break") || atKeyword("continue") || atKeyword("discard")) return parseBreakContinue();
        if (atType(TokenType::Preprocessor)) return parsePreprocessorLine();
        if (lang_ == Language::WGSL) {
            if (atKeyword("var")) return parseWgslVarDecl();
            if (atKeyword("let") || atKeyword("const")) return parseWgslLetConstDecl();
            auto node = makeNode(NodeKind::ExprStmt);
            ScanStop stop = scanChunkTokens(node->tokens, {";"});
            if (stop == ScanStop::StopText) node->tokens.push_back(advance());
            return node;
        }
        return parseSimpleStatement();
    }

    // C-like conditions are always "(...)"; WGSL's if/while conditions may
    // omit the parens entirely (valid WGSL grammar has no parens around
    // if/while conditions - only around for-loop headers, which always
    // use parseParenChunk() directly, not this). Handle both uniformly:
    // if a '(' is actually present, it's a real (WGSL: optional, C-like:
    // mandatory) paren-wrapped chunk; otherwise scan up to the body's
    // opening '{'.
    NodePtr parseCondition() {
        if (atPunct("(")) return parseParenChunk();
        auto chunk = makeNode(NodeKind::Chunk);
        scanChunkTokens(chunk->tokens, {});
        return chunk;
    }

    NodePtr parseIf() {
        auto node = makeNode(NodeKind::IfStmt);
        advance(); // 'if'
        node->children.push_back(parseCondition());
        node->children.push_back(parseControlBody());

        auto leadingForElse = collectLeadingTrivia();
        if (atKeyword("else")) {
            advance();
            NodePtr elseBranch = atKeyword("if") ? parseIf() : parseControlBody();
            elseBranch->leadingTrivia.insert(elseBranch->leadingTrivia.begin(), leadingForElse.begin(),
                                              leadingForElse.end());
            node->children.push_back(std::move(elseBranch));
        } else if (!leadingForElse.empty()) {
            // No 'else' followed after all: these comments don't belong to
            // anything downstream we've identified yet, so keep them
            // attached (losslessly, if not perfectly ideally placed) as
            // this if-statement's own trailing trivia.
            node->trailingTrivia.insert(node->trailingTrivia.end(), leadingForElse.begin(), leadingForElse.end());
        }
        return node;
    }

    NodePtr parseFor() {
        auto node = makeNode(NodeKind::ForStmt);
        advance(); // 'for'
        node->children.push_back(parseParenChunk());
        node->children.push_back(parseControlBody());
        return node;
    }

    NodePtr parseWhile() {
        auto node = makeNode(NodeKind::WhileStmt);
        advance(); // 'while'
        node->children.push_back(parseCondition());
        node->children.push_back(parseControlBody());
        return node;
    }

    NodePtr parseDoWhile() {
        auto node = makeNode(NodeKind::DoWhileStmt);
        advance(); // 'do'
        node->children.push_back(parseControlBody());
        auto leading = collectLeadingTrivia();
        NodePtr cond;
        if (atKeyword("while")) {
            advance();
            cond = parseParenChunk();
            if (atPunct(";")) advance(); // synthesized on emission, not stored
        } else {
            cond = makeNode(NodeKind::Chunk); // malformed: missing "while (...)"
        }
        cond->leadingTrivia = std::move(leading);
        node->children.push_back(std::move(cond));
        return node;
    }

    NodePtr parseSwitch() {
        auto node = makeNode(NodeKind::SwitchStmt);
        advance(); // 'switch'
        node->children.push_back(parseParenChunk());

        auto body = makeNode(NodeKind::Block);
        if (atPunct("{")) {
            advance();
            for (;;) {
                auto leading = collectLeadingTrivia();
                if (atEnd() || atPunct("}")) {
                    body->trailingTrivia.insert(body->trailingTrivia.end(), leading.begin(), leading.end());
                    break;
                }
                size_t before = cursor_;
                NodePtr item;
                if (atKeyword("case") || atKeyword("default")) {
                    item = parseCaseLabel();
                } else if (atType(TokenType::Preprocessor)) {
                    item = parsePreprocessorLine();
                } else {
                    item = parseStatement();
                }
                if (cursor_ == before) {
                    item = makeNode(NodeKind::ErrorNode);
                    item->tokens.push_back(advance());
                }
                item->leadingTrivia = std::move(leading);
                collectTrailingTrivia(*item);
                body->children.push_back(std::move(item));
            }
            if (atPunct("}")) {
                advance();
            } else {
                body->hasClosingBrace = false; // truncated: never found the matching '}'
            }
        }
        node->children.push_back(std::move(body));
        return node;
    }

    NodePtr parseCaseLabel() {
        auto node = makeNode(NodeKind::CaseLabel);
        node->tokens.push_back(advance()); // 'case' or 'default'
        ScanStop stop = scanChunkTokens(node->tokens, {":"});
        if (stop == ScanStop::StopText) node->tokens.push_back(advance());
        return node;
    }

    NodePtr parseReturn() {
        auto node = makeNode(NodeKind::ReturnStmt);
        node->tokens.push_back(advance()); // 'return'
        ScanStop stop = scanChunkTokens(node->tokens, {";"});
        if (stop == ScanStop::StopText) node->tokens.push_back(advance());
        return node;
    }

    NodePtr parseBreakContinue() {
        auto node = makeNode(NodeKind::BreakContinueStmt);
        node->tokens.push_back(advance()); // 'break' / 'continue' / 'discard'
        if (atPunct(";")) node->tokens.push_back(advance());
        return node;
    }

    // --- WGSL-specific productions ---
    // "@attr(...)" decorators are deliberately left in place (not
    // consumed separately) before calling into parseWgsl{Fn,VarDecl,
    // LetConstDecl}: those already scan everything up to their
    // terminator as one opaque run (see Ast.hpp - "keeping them in the
    // same opaque run loses no round-trip fidelity"), so the only reason
    // to look past them here is to classify *which* production applies.

    size_t skipWgslAttributesForLookahead(size_t i) const {
        while (i < tokens_.size()) {
            const Token& t = tokens_[i];
            if (t.type == TokenType::Punctuator && t.text == "@") {
                i++;
                if (i < tokens_.size() && tokens_[i].type == TokenType::Identifier) i++; // attribute name
                if (i < tokens_.size() && tokens_[i].type == TokenType::Punctuator && tokens_[i].text == "(") {
                    int depth = 0;
                    do {
                        if (tokens_[i].text == "(") depth++;
                        else if (tokens_[i].text == ")") depth--;
                        i++;
                    } while (depth > 0 && i < tokens_.size());
                }
                continue;
            }
            if (isComment(t)) {
                i++;
                continue;
            }
            break;
        }
        return i;
    }

    NodePtr parseWgslAttributedDecl() {
        size_t i = skipWgslAttributesForLookahead(cursor_);
        if (i < tokens_.size() && tokens_[i].type == TokenType::Identifier) {
            if (tokens_[i].text == "fn") return parseWgslFn();
            if (tokens_[i].text == "var") return parseWgslVarDecl();
            if (tokens_[i].text == "let" || tokens_[i].text == "const") return parseWgslLetConstDecl();
        }
        return recoverAsErrorNode();
    }

    NodePtr parseWgslFn() {
        auto node = makeNode(NodeKind::WgslFnDecl);
        ScanStop stop = scanChunkTokens(node->tokens, {";"});
        if (stop == ScanStop::OpenBrace) {
            node->children.push_back(parseBlock());
        } else if (stop == ScanStop::StopText) {
            node->tokens.push_back(advance()); // forward declaration (rare in WGSL, tolerated)
        }
        return node;
    }

    NodePtr parseWgslVarDecl() {
        auto node = makeNode(NodeKind::WgslVarDecl);
        ScanStop stop = scanChunkTokens(node->tokens, {";"});
        if (stop == ScanStop::StopText) node->tokens.push_back(advance());
        return node;
    }

    NodePtr parseWgslLetConstDecl() {
        auto node = makeNode(NodeKind::WgslLetConstDecl);
        ScanStop stop = scanChunkTokens(node->tokens, {";"});
        if (stop == ScanStop::StopText) node->tokens.push_back(advance());
        return node;
    }

    // WGSL struct fields are comma-separated ("name: type,"), not
    // semicolon-terminated like the C-like family, and the last field may
    // omit its trailing comma - reuses the same StructDecl/StructField
    // NodeKinds (the shape - header, field list, optional trailing ';'
    // after the closing '}' - is otherwise identical), just with its own
    // field-list loop for the different separator.
    NodePtr parseWgslStruct() {
        auto node = makeNode(NodeKind::StructDecl);
        ScanStop stop = scanChunkTokens(node->tokens, {";"});
        if (stop != ScanStop::OpenBrace) {
            node->hasBody = false;
            return node;
        }
        advance(); // '{'
        for (;;) {
            auto leading = collectLeadingTrivia();
            if (atEnd() || atPunct("}")) {
                node->trailingTrivia.insert(node->trailingTrivia.end(), leading.begin(), leading.end());
                break;
            }
            size_t before = cursor_;
            NodePtr field = atType(TokenType::Preprocessor) ? parsePreprocessorLine() : parseWgslStructField();
            if (cursor_ == before) {
                field = makeNode(NodeKind::ErrorNode);
                field->tokens.push_back(advance());
            }
            field->leadingTrivia = std::move(leading);
            collectTrailingTrivia(*field);
            node->children.push_back(std::move(field));
        }
        if (atPunct("}")) {
            advance();
        } else {
            node->hasClosingBrace = false; // truncated: never found the matching '}'
        }
        if (atPunct(";")) node->tokens.push_back(advance());
        return node;
    }

    NodePtr parseWgslStructField() {
        auto node = makeNode(NodeKind::StructField);
        ScanStop stop = scanWgslStructFieldTokens(node->tokens);
        if (stop == ScanStop::StopText) node->tokens.push_back(advance());
        // CloseBrace: last field with no trailing comma - fine, tolerant.
        return node;
    }

    // Bug-audit fix: a field type with a comma-separated multi-arg generic
    // (e.g. "x: array<f32, 10>,") used to be mis-split at that *inner*
    // comma, since plain scanChunkTokens(..., {","}) doesn't track '<'/'>'
    // nesting at all - "array<f32" became one (malformed) field and
    // " 10>" another. Fixed the same way as Formatter.cpp's WGSL-generics
    // spacing fix (see wgslGenericTypeNames() there): only treat '<' as
    // opening a nesting level when it immediately follows one of WGSL's
    // reserved type-constructor keywords, which - being reserved words -
    // can never be a real variable name a genuine "a < b" comparison
    // could start with. Struct fields never contain arbitrary comparison
    // expressions anyway (only "name: type[,]"), so this is even safer
    // here than in the formatter, but kept as the same narrow check for
    // consistency and in case that ever changes.
    static const std::set<std::string>& wgslGenericTypeNamesForFieldSplitting() {
        static const std::set<std::string> names = {
            "vec2", "vec3", "vec4", "mat2x2", "mat2x3", "mat2x4", "mat3x2", "mat3x3", "mat3x4",
            "mat4x2", "mat4x3", "mat4x4", "array", "ptr", "atomic",
            "texture_1d", "texture_2d", "texture_2d_array", "texture_3d", "texture_cube",
            "texture_cube_array", "texture_multisampled_2d", "texture_storage_1d",
            "texture_storage_2d", "texture_storage_2d_array", "texture_storage_3d", "binding_array"};
        return names;
    }

    ScanStop scanWgslStructFieldTokens(std::vector<Token>& out) {
        int depth = 0;         // '('/'[' nesting, same as scanChunkTokens
        int genericDepth = 0;  // recognized WGSL '<...>' generic nesting
        while (!atEnd()) {
            const Token& t = peek();
            if (t.type == TokenType::Punctuator && depth == 0 && genericDepth == 0) {
                if (t.text == "{") return ScanStop::OpenBrace;
                if (t.text == "}") return ScanStop::CloseBrace;
                if (t.text == ",") return ScanStop::StopText;
            }
            if (t.type == TokenType::Punctuator) {
                if (t.text == "(" || t.text == "[") {
                    depth++;
                } else if (t.text == ")" || t.text == "]") {
                    depth = std::max(0, depth - 1);
                } else if (t.text == "<" && !out.empty() && out.back().type == TokenType::Identifier &&
                           wgslGenericTypeNamesForFieldSplitting().count(out.back().text) > 0) {
                    genericDepth++;
                } else if (t.text == ">" && genericDepth > 0) {
                    genericDepth--;
                }
            }
            out.push_back(advance());
        }
        return ScanStop::End;
    }

    // --- ShaderLab-specific productions (Language::ShaderLab) ---

    NodePtr parseShaderLabProgram() {
        auto program = makeNode(NodeKind::Program);
        for (;;) {
            auto leading = collectLeadingTrivia();
            if (atEnd()) {
                program->trailingTrivia.insert(program->trailingTrivia.end(), leading.begin(), leading.end());
                break;
            }
            size_t before = cursor_;
            NodePtr item = atKeyword("Shader") ? parseShaderDecl() : parseShaderLabItem();
            if (cursor_ == before) {
                item = makeNode(NodeKind::ErrorNode);
                item->tokens.push_back(advance());
            }
            item->leadingTrivia = std::move(leading);
            collectTrailingTrivia(*item);
            program->children.push_back(std::move(item));
        }
        return program;
    }

    NodePtr parseShaderDecl() {
        auto node = makeNode(NodeKind::ShaderDecl);
        ScanStop stop = scanChunkTokens(node->tokens, {}); // "Shader "Name"" up to '{'
        if (stop != ScanStop::OpenBrace) {
            node->hasBody = false; // malformed/truncated: no body found
            return node;
        }
        advance(); // '{'
        parseShaderLabBlockBody(*node);
        return node;
    }

    // Dispatches one item inside any ShaderLab block body (Shader/
    // Properties/SubShader/Pass/Category are all "keyword { ... }"
    // containers whose children are the same three possible shapes).
    NodePtr parseShaderLabItem() {
        if (atType(TokenType::Preprocessor)) return parsePreprocessorLine();
        if (atKeyword("CGPROGRAM") || atKeyword("HLSLPROGRAM")) return parseCgProgramBlock();
        if (atType(TokenType::Identifier) && isShaderLabBlockKeyword(peek().text)) return parseShaderLabBlock();
        return parseShaderLabCommand();
    }

    NodePtr parseShaderLabBlock() {
        auto node = makeNode(NodeKind::ShaderLabBlock);
        ScanStop stop = scanChunkTokens(node->tokens, {}); // "Keyword [args]" up to '{'
        if (stop != ScanStop::OpenBrace) {
            node->hasBody = false; // malformed/truncated: no body found
            return node;
        }
        advance(); // '{'
        parseShaderLabBlockBody(*node);
        return node;
    }

    // Shared body-parsing loop for ShaderDecl/ShaderLabBlock: assumes the
    // opening '{' was already consumed, fills `node.children` with
    // ShaderLabItem results up to the matching '}' (or EOF, tolerantly).
    void parseShaderLabBlockBody(Node& node) {
        for (;;) {
            auto leading = collectLeadingTrivia();
            if (atEnd() || atPunct("}")) {
                node.trailingTrivia.insert(node.trailingTrivia.end(), leading.begin(), leading.end());
                break;
            }
            size_t before = cursor_;
            auto item = parseShaderLabItem();
            if (cursor_ == before) {
                item = makeNode(NodeKind::ErrorNode);
                item->tokens.push_back(advance());
            }
            item->leadingTrivia = std::move(leading);
            collectTrailingTrivia(*item);
            node.children.push_back(std::move(item));
        }
        if (atPunct("}")) {
            advance();
        } else {
            node.hasClosingBrace = false; // truncated: never found the matching '}'
        }
    }

    // A bare ShaderLab command line: "Cull Back", "ZWrite On", "LOD 100",
    // `_MainTex ("Texture", 2D) = "white" {}` (Unity property syntax,
    // note the trailing empty-brace pair some texture properties use).
    // Terminated by the next token that starts a new source line (or a
    // comment, left for the caller's trailing-trivia collection, or a
    // real '{'/'}' block boundary).
    NodePtr parseShaderLabCommand() {
        auto node = makeNode(NodeKind::ShaderLabCommand);
        node->tokens.push_back(advance()); // always consume at least one token
        while (!atEnd() && !peek().onOwnLine && !isComment(peek())) {
            if (atPunct("{")) {
                // Only fold through a brace pair if it's the common Unity
                // "no default texture" empty-brace suffix on a property
                // line - a real block body starting here belongs to a
                // different (ShaderLabBlock) item, not this command.
                if (peek(1).type == TokenType::Punctuator && peek(1).text == "}") {
                    node->tokens.push_back(advance()); // '{'
                    node->tokens.push_back(advance()); // '}'
                    continue;
                }
                break;
            }
            if (atPunct("}")) break;
            node->tokens.push_back(advance());
        }
        return node;
    }

    // "CGPROGRAM ... ENDCG" or "HLSLPROGRAM ... ENDHLSL": the actual
    // Cg/HLSL source inside is extracted and parsed for real with the
    // existing HLSL grammar (a fresh, nested Parser instance), rather
    // than being kept as an opaque blob like ShaderLabCommand - this is
    // the whole point of ShaderLab support, not just recognizing its
    // outer syntax.
    NodePtr parseCgProgramBlock() {
        auto node = makeNode(NodeKind::CgProgramBlock);
        node->tokens.push_back(advance()); // 'CGPROGRAM' or 'HLSLPROGRAM'
        bool isHlslMarker = node->tokens[0].text == "HLSLPROGRAM";
        const char* closeKeyword = isHlslMarker ? "ENDHLSL" : "ENDCG";

        std::vector<Token> innerTokens;
        while (!atEnd() && !(peek().type == TokenType::Identifier && peek().text == closeKeyword)) {
            innerTokens.push_back(advance());
        }
        if (!atEnd()) {
            node->tokens.push_back(advance()); // consume ENDCG/ENDHLSL
        } else {
            node->hasClosingBrace = false; // truncated: never found it
        }

        Token eof;
        eof.type = TokenType::EndOfFile;
        innerTokens.push_back(eof);

        Parser subParser(innerTokens, Language::HLSL);
        ParseResult inner = subParser.run();
        node->children.push_back(std::move(inner.program));
        errors_.insert(errors_.end(), inner.errors.begin(), inner.errors.end());
        return node;
    }
};

ParseResult parse(const std::vector<Token>& tokens, Language lang) {
    Parser parser(tokens, lang);
    return parser.run();
}

} // namespace shaderfmt
