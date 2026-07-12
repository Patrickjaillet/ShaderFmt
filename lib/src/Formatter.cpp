#include "shaderfmt/Formatter.hpp"
#include "shaderfmt/Ast.hpp"
#include "shaderfmt/Lexer.hpp"
#include "shaderfmt/Parser.hpp"

#include <set>
#include <string>

namespace shaderfmt {

namespace {

// Keywords after which "(" gets a leading space (control-flow style),
// as opposed to a function call "foo(" which stays tight. This is a
// lexical heuristic, not a real grammar decision, but since it only
// changes whitespace it can never alter behaviour.
const std::set<std::string>& controlKeywords() {
    static const std::set<std::string> kw = {
        "if", "for", "while", "switch", "return", "else", "do", "case"};
    return kw;
}

const std::set<std::string>& noSpaceBefore() {
    static const std::set<std::string> s = {
        ";", ",", ")", "]", ".", "::", "++", "--"};
    return s;
}

const std::set<std::string>& noSpaceAfterToken() {
    static const std::set<std::string> s = {
        "(", "[", ".", "!", "~", "@", "::", "++", "--"};
    return s;
}

// Keywords that introduce a fresh expression rather than ending one, so a
// '+'/'-' right after them is a unary sign, not a binary operator (see
// isUnaryOperatorPosition()). Doesn't need to be exhaustive: anything not
// listed here that also isn't an identifier/number/string/")"/"]"/"++"/
// "--" already falls back to "treat as unary" by default in
// isUnaryOperatorPosition() - this set only has to cover keywords whose
// token *type* is Identifier (same as an ordinary variable name) and
// which could plausibly be followed by a signed value.
const std::set<std::string>& unaryContextKeywords() {
    static const std::set<std::string> kw = {"return", "case"};
    return kw;
}

// WGSL built-in type constructors that can be followed by a "<...>"
// generic argument list (vec3<f32>, array<f32, 4>, ptr<function, f32>,
// texture_2d<f32>, ...). Used to recognize generic angle brackets so they
// can be spaced tight ("vec3<f32>") instead of like a comparison operator
// ("vec3 < f32 >") - see Emitter::handle()'s '<'/'>' handling below.
// Scoped deliberately to *this* fixed set of reserved WGSL type keywords:
// none of them can legally be a user-defined variable name in valid WGSL,
// so "someIdentifier < b" (a real comparison) can never be misread as a
// generic open by this heuristic - see BUGS_ROADMAP.md's WGSL-generics
// item for the full reasoning (also applied to the matching struct-field
// comma-splitting fix in Parser.cpp).
const std::set<std::string>& wgslGenericTypeNames() {
    static const std::set<std::string> names = {
        "vec2", "vec3", "vec4", "mat2x2", "mat2x3", "mat2x4", "mat3x2", "mat3x3", "mat3x4",
        "mat4x2", "mat4x3", "mat4x4", "array", "ptr", "atomic",
        "texture_1d", "texture_2d", "texture_2d_array", "texture_3d", "texture_cube",
        "texture_cube_array", "texture_multisampled_2d", "texture_storage_1d",
        "texture_storage_2d", "texture_storage_2d_array", "texture_storage_3d", "binding_array"};
    return names;
}

// Same idea as wgslGenericTypeNames() above, for HLSL's built-in generic
// resource/buffer types ("RWStructuredBuffer<float4>", "Texture2D<float4>",
// ...) - see BUGS_ROADMAP.md's "HLSL generic types" item, and the matching
// allowlist in Parser.cpp (hlslGenericTypeNames()) that lets the parser
// recognize these as declarations in the first place. HLSL has no
// user-defined generics, so this fixed set can never misclassify a real
// "a < b" comparison as a generic open.
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

class Emitter {
public:
    Emitter(const FormatOptions& opts, Language lang) : opts_(opts), lang_(lang) {}

    std::string result() const { return out_; }

    // `forceNoSpaceBeforeThis` overrides the normal spacing heuristic for
    // *this* token only, unconditionally suppressing any leading space -
    // used by emitNode() for the ':' that terminates a "case"/"default"
    // label (never the ternary ':', which keeps its spaces on both
    // sides - see the CaseLabel case in emitNode()).
    void handle(const Token& t, bool forceNoSpaceBeforeThis = false) {
        switch (t.type) {
            case TokenType::Preprocessor: return emitPreprocessor(t);
            case TokenType::LineComment: return emitLineComment(t);
            case TokenType::BlockComment: return emitBlockComment(t);
            case TokenType::EndOfFile: return;
            default: break;
        }

        if (t.type == TokenType::Punctuator) {
            if (t.text == "{") return emitOpenBrace(t);
            if (t.text == "}") return emitCloseBrace(t);
            if (t.text == "(" || t.text == "[") parenDepth_++;
            if (t.text == ")" || t.text == "]") parenDepth_ = parenDepth_ > 0 ? parenDepth_ - 1 : 0;
            if (t.text == ";") return emitSemicolon(t);

            // WGSL generic angle brackets ("vec3<f32>", "array<f32, 4>"):
            // spaced tight, unlike a real '<'/'>' comparison. See
            // wgslGenericTypeNames() above for why this can never
            // misclassify a genuine comparison expression.
            bool atGenericOpen =
                (lang_ == Language::WGSL && wgslGenericTypeNames().count(prevText_) > 0) ||
                (lang_ == Language::HLSL && hlslGenericTypeNames().count(prevText_) > 0);
            if (t.text == "<" && prevType_ == TokenType::Identifier && atGenericOpen) {
                writeToken(t.text, t.blankLinesBefore, t.onOwnLine, /*forceNoSpaceBeforeThis=*/true);
                prevType_ = t.type;
                genericDepth_++;
                forceNoSpaceBeforeNext_ = true; // no space before whatever follows '<' either
                return;
            }
            if (t.text == ">" && genericDepth_ > 0) {
                genericDepth_--;
                writeToken(t.text, t.blankLinesBefore, t.onOwnLine, /*forceNoSpaceBeforeThis=*/true);
                prevType_ = t.type;
                return;
            }

            // Unary '+'/'-': a sign attached to the value that follows it,
            // not a binary operator - see isUnaryOperatorPosition(). Only
            // '+'/'-' are ambiguous this way; '!'/'~' are always unary in
            // these languages and already handled by noSpaceAfterToken().
            if (t.text == "-" || t.text == "+") {
                bool unary = isUnaryOperatorPosition();
                writeToken(t.text, t.blankLinesBefore, t.onOwnLine, forceNoSpaceBeforeThis);
                prevType_ = t.type;
                if (unary) forceNoSpaceBeforeNext_ = true;
                return;
            }
        }

        writeToken(t.text, t.blankLinesBefore, t.onOwnLine, forceNoSpaceBeforeThis);
        prevType_ = t.type;
    }

    // Emits a token as plain text, bypassing handle()'s special-cased
    // '{'/'}'/';' handling. Needed for the rare case where a language
    // embeds a brace pair as ordinary (non-block) syntax - e.g. Unity
    // ShaderLab's `_MainTex (...) = "white" {}` empty-texture marker
    // (see ShaderLabCommand in Formatter.cpp's emitNode()) - where
    // treating '{'/'}' as a real block would incorrectly perturb
    // indentation.
    void handleAsPlainToken(const Token& t) {
        writeToken(t.text, t.blankLinesBefore, t.onOwnLine);
        prevType_ = t.type;
    }

    // Call once all tokens are processed to make sure the output ends with
    // exactly one trailing newline (common convention, also makes the
    // output stable/idempotent regardless of trailing whitespace quirks in
    // the input).
    void finish() {
        while (!out_.empty() && (out_.back() == '\n')) out_.pop_back();
        out_ += '\n';
    }

private:
    // A statement/brace can finish "mid-line" (e.g. right after the ';' or
    // '}') because a trailing same-line comment might still need to attach
    // to that same line. The actual line break is therefore deferred until
    // we know what the *next* token is - that's what `pending_` tracks.
    enum class Pending { None, AfterStatement, AfterCloseBrace };

    FormatOptions opts_;
    Language lang_;
    std::string out_;
    int indent_ = 0;
    int parenDepth_ = 0;
    int genericDepth_ = 0; // WGSL/HLSL only: nesting depth of recognized "<...>" generics
    bool atLineStart_ = true;
    std::string prevText_;
    TokenType prevType_ = TokenType::EndOfFile; // EndOfFile doubles as "nothing emitted yet"
    // Set by a token that must never have a space before whatever comes
    // right after it (a unary '+'/'-', or a WGSL generic-opening '<') and
    // consumed by the very next writeToken() call.
    bool forceNoSpaceBeforeNext_ = false;
    Pending pending_ = Pending::None;

    std::string indentString() const {
        if (opts_.useTabs) return std::string(indent_, '\t');
        return std::string(static_cast<size_t>(indent_) * static_cast<size_t>(opts_.indentWidth), ' ');
    }

    void newline() {
        out_ += '\n';
        atLineStart_ = true;
    }

    void ensureNewline() {
        if (!atLineStart_) newline();
    }

    void emitBlankLinesIfAtLineStart(const Token& t) {
        if (atLineStart_ && t.blankLinesBefore > 0 && !out_.empty()) {
            out_ += '\n';
        }
    }

    // Resolves any deferred line break now that we know the text of the
    // next token to be emitted (pass "" for tokens like '{'/'}'/preprocessor
    // lines that always force a break regardless of what follows).
    void resolvePending(const std::string& nextText) {
        if (pending_ == Pending::None) return;
        Pending p = pending_;
        pending_ = Pending::None;

        if (p == Pending::AfterStatement) {
            ensureNewline();
            return;
        }
        // AfterCloseBrace: a few tokens attach directly to a '}' on the
        // same line (e.g. "};", "} ,", "})"), 'else'/'while' cuddle onto
        // the same line with a space, everything else starts a new line.
        static const std::set<std::string> attach = {";", ",", ")", "]"};
        if (attach.count(nextText)) return;
        if (nextText == "else" || nextText == "while") {
            if (!out_.empty() && out_.back() != '\n') out_ += ' ';
            return;
        }
        ensureNewline();
    }

    bool needSpaceBefore(const std::string& cur) const {
        if (prevText_.empty()) return false;
        if (cur == "(") return opts_.spaceAfterControlKeyword && controlKeywords().count(prevText_) > 0;
        if (cur == "[") {
            // The second '[' of an MSL "[[attribute(0)]]" marker always
            // sits tight against the first (same as noSpaceAfterToken()'s
            // ordinary "[" rule below, checked explicitly here since this
            // branch returns before reaching that check).
            if (prevText_ == "[") return false;
            // A '[' right after whatever can end an operand (identifier,
            // number, string, or a ')'/']' closing a call/previous
            // subscript) is a postfix subscript or array-size bracket -
            // "arr[i]", "particles[id].position", "float lights[48];" -
            // and sits tight against it, same postfix positions as
            // isUnaryOperatorPosition() below. Anything else (start of a
            // statement, right after another punctuator/keyword) is an
            // attribute marker instead (HLSL "[numthreads(...)]", MSL's
            // first "[[attribute(0)]]"), which - not being a subscript -
            // keeps the default leading space.
            switch (prevType_) {
                case TokenType::Identifier:
                case TokenType::Number:
                case TokenType::StringLiteral:
                    return false;
                case TokenType::Punctuator:
                    return prevText_ != ")" && prevText_ != "]";
                default:
                    return true;
            }
        }
        if (noSpaceBefore().count(cur)) return false;
        if (noSpaceAfterToken().count(prevText_)) return false;
        return true;
    }

    // '+'/'-' act as a unary sign (not a binary operator) unless the
    // token right before them could actually end an operand: an
    // identifier/number/string, a ')'/']' (end of a call/subscript), or a
    // postfix '++'/'--'. Everything else - start of input, right after
    // most punctuators/operators, or right after a handful of
    // expression-starting keywords (return/case) - means '+'/'-' is a
    // sign attached to what follows. A lexical heuristic like the rest of
    // this file, not a real precedence parser, but it only affects
    // whitespace and is scoped narrowly enough (see unaryContextKeywords())
    // to never misjudge a real binary '+'/'-' between two operands.
    bool isUnaryOperatorPosition() const {
        if (prevText_.empty()) return true;
        if (forceNoSpaceBeforeNext_) return true; // e.g. second '-' in "- -b"
        switch (prevType_) {
            case TokenType::Identifier:
                return unaryContextKeywords().count(prevText_) > 0;
            case TokenType::Number:
            case TokenType::StringLiteral:
                return false;
            case TokenType::Punctuator: {
                static const std::set<std::string> operandEnders = {")", "]", "++", "--"};
                return operandEnders.count(prevText_) == 0;
            }
            default:
                return true; // comments, preprocessor lines, etc.: safe default
        }
    }

    void writeToken(const std::string& text, int blankLinesBefore = 0, bool onOwnLine = false,
                     bool forceNoSpaceBeforeThis = false) {
        resolvePending(text);
        // A token that genuinely started a new source line (e.g. right
        // after a same-line trailing comment, which doesn't itself force a
        // break) must still start a new output line - otherwise unrelated
        // statements could get glued together on one line.
        if (onOwnLine && !atLineStart_) {
            ensureNewline();
        }
        if (atLineStart_ && blankLinesBefore > 0 && !out_.empty()) {
            out_ += '\n';
        }
        if (atLineStart_) {
            out_ += indentString();
            atLineStart_ = false;
        } else if (!forceNoSpaceBeforeThis && !forceNoSpaceBeforeNext_ && needSpaceBefore(text)) {
            out_ += ' ';
        }
        out_ += text;
        prevText_ = text;
        forceNoSpaceBeforeNext_ = false; // consumed, regardless of which branch above used it
    }

    void emitOpenBrace(const Token& t) {
        resolvePending("{");
        emitBlankLinesIfAtLineStart(t);
        if (opts_.braceStyle == BraceStyle::NextLine) {
            // Allman: force the brace onto its own line at the *current*
            // indent (i.e. before indent_++ below), regardless of whether
            // the source had it on the same line as the statement.
            ensureNewline();
            out_ += indentString();
        } else if (atLineStart_) {
            out_ += indentString();
        } else {
            out_ += ' ';
        }
        out_ += '{';
        atLineStart_ = false;
        prevText_ = "{";
        prevType_ = TokenType::Punctuator;
        forceNoSpaceBeforeNext_ = false;
        indent_++;
        newline();
    }

    void emitCloseBrace(const Token& /*t*/) {
        resolvePending("}");
        indent_ = indent_ > 0 ? indent_ - 1 : 0;
        ensureNewline();
        // Deliberately not honouring blankLinesBefore here: a blank line
        // immediately before a closing brace is suppressed for readability,
        // matching common style-guide conventions.
        out_ += indentString();
        out_ += '}';
        atLineStart_ = false;
        prevText_ = "}";
        prevType_ = TokenType::Punctuator;
        forceNoSpaceBeforeNext_ = false;
        pending_ = Pending::AfterCloseBrace;
    }

    void emitSemicolon(const Token& t) {
        (void)t;
        resolvePending(";");
        out_ += ';'; // never preceded by a space, see noSpaceBefore()
        prevText_ = ";";
        prevType_ = TokenType::Punctuator;
        forceNoSpaceBeforeNext_ = false;
        if (parenDepth_ == 0) {
            pending_ = Pending::AfterStatement;
        }
    }

    void emitPreprocessor(const Token& t) {
        resolvePending("#");
        ensureNewline();
        emitBlankLinesIfAtLineStart(t);
        // Preprocessor directives are flush-left by convention and are
        // copied verbatim (including any backslash-newline continuations)
        // since they are never interpreted.
        out_ += t.text;
        newline();
        prevText_.clear();
        prevType_ = TokenType::EndOfFile;
        forceNoSpaceBeforeNext_ = false;
    }

    void emitLineComment(const Token& t) {
        if (t.onOwnLine || out_.empty()) {
            resolvePending("//");
            ensureNewline();
            emitBlankLinesIfAtLineStart(t);
            out_ += indentString();
            out_ += t.text;
            atLineStart_ = false;
        } else {
            // Trailing comment: stays glued to whatever line it followed in
            // the source, so any deferred break is dropped, not resolved.
            pending_ = Pending::None;
            out_ += "  ";
            out_ += t.text;
        }
        newline();
        prevText_.clear();
        prevType_ = TokenType::EndOfFile;
        forceNoSpaceBeforeNext_ = false;
    }

    void emitBlockComment(const Token& t) {
        if (t.onOwnLine || out_.empty()) {
            resolvePending("/*");
            ensureNewline();
            emitBlankLinesIfAtLineStart(t);
            out_ += indentString();
            out_ += t.text;
            atLineStart_ = false;
            newline();
            prevText_.clear();
            prevType_ = TokenType::EndOfFile;
            forceNoSpaceBeforeNext_ = false;
        } else {
            pending_ = Pending::None;
            if (!forceNoSpaceBeforeNext_ && needSpaceBefore("/*")) out_ += ' ';
            out_ += t.text;
            prevText_ = t.text;
            prevType_ = TokenType::BlockComment;
            forceNoSpaceBeforeNext_ = false;
        }
    }
};

// Builds a Token for a fixed keyword/brace/separator that a structural
// AST node doesn't store itself (see Ast.hpp's synthesized-keyword
// convention: "if"/"for"/"while"/"do"/"switch"/"else", and Block/
// StructDecl's '{'/'}'). Reuses the exact same Emitter as format() - the
// point of synthesizing Tokens rather than writing separate emission
// logic is that Emitter::handle() already has every spacing/indentation/
// pending-newline rule; walking the AST only needs to feed it tokens in
// the right order.
Token syntheticToken(TokenType type, const std::string& text, bool onOwnLine = false, int blankLinesBefore = 0) {
    Token t;
    t.type = type;
    t.text = text;
    t.onOwnLine = onOwnLine;
    t.blankLinesBefore = blankLinesBefore;
    return t;
}

void emitNode(Emitter& emitter, const Node& node) {
    for (const auto& tv : node.leadingTrivia) emitter.handle(tv.token);

    switch (node.kind) {
        case NodeKind::IfStmt:
            emitter.handle(syntheticToken(TokenType::Identifier, "if", node.onOwnLine, node.blankLinesBefore));
            emitNode(emitter, *node.children[0]);
            emitNode(emitter, *node.children[1]);
            if (node.children.size() > 2) {
                emitter.handle(syntheticToken(TokenType::Identifier, "else"));
                emitNode(emitter, *node.children[2]);
            }
            break;
        case NodeKind::ForStmt:
            emitter.handle(syntheticToken(TokenType::Identifier, "for", node.onOwnLine, node.blankLinesBefore));
            emitNode(emitter, *node.children[0]);
            emitNode(emitter, *node.children[1]);
            break;
        case NodeKind::WhileStmt:
            emitter.handle(syntheticToken(TokenType::Identifier, "while", node.onOwnLine, node.blankLinesBefore));
            emitNode(emitter, *node.children[0]);
            emitNode(emitter, *node.children[1]);
            break;
        case NodeKind::DoWhileStmt:
            emitter.handle(syntheticToken(TokenType::Identifier, "do", node.onOwnLine, node.blankLinesBefore));
            emitNode(emitter, *node.children[0]);
            emitter.handle(syntheticToken(TokenType::Identifier, "while"));
            emitNode(emitter, *node.children[1]);
            emitter.handle(syntheticToken(TokenType::Punctuator, ";"));
            break;
        case NodeKind::SwitchStmt:
            emitter.handle(syntheticToken(TokenType::Identifier, "switch", node.onOwnLine, node.blankLinesBefore));
            emitNode(emitter, *node.children[0]);
            emitNode(emitter, *node.children[1]);
            break;
        case NodeKind::Block:
            emitter.handle(syntheticToken(TokenType::Punctuator, "{", node.onOwnLine, node.blankLinesBefore));
            for (const auto& c : node.children) emitNode(emitter, *c);
            // Never synthesize a closing brace the parser never actually
            // found (truncated/mid-typing input) - the token-stream
            // emitter simply has no such token to emit either, so
            // "completing" it here would diverge from that behaviour.
            if (node.hasClosingBrace) emitter.handle(syntheticToken(TokenType::Punctuator, "}"));
            break;
        case NodeKind::StructDecl: {
            // tokens = [header..., optional trailing ';'] - the ';' comes
            // after the field list despite living in the same vector
            // (appended once fields are already parsed - see
            // Parser.cpp's parseStructOrCbuffer/parseWgslStruct).
            bool hasTrailingSemi = !node.tokens.empty() && node.tokens.back().type == TokenType::Punctuator &&
                                    node.tokens.back().text == ";";
            size_t headerCount = node.tokens.size() - (hasTrailingSemi ? 1 : 0);
            for (size_t i = 0; i < headerCount; i++) emitter.handle(node.tokens[i]);
            if (node.hasBody) {
                emitter.handle(syntheticToken(TokenType::Punctuator, "{"));
                for (const auto& c : node.children) emitNode(emitter, *c);
                if (node.hasClosingBrace) emitter.handle(syntheticToken(TokenType::Punctuator, "}"));
            }
            if (hasTrailingSemi) emitter.handle(node.tokens.back());
            // GLSL named interface block only (Parser.cpp's
            // parseInterfaceBlock()) - the instance declarator and/or ';'
            // that follows the body's closing '}', e.g. "} global_uniform;".
            for (const auto& t : node.trailingDeclarator) emitter.handle(t);
            break;
        }
        case NodeKind::ShaderDecl:
        case NodeKind::ShaderLabBlock:
            // Same shape as StructDecl minus the optional trailing ';'
            // (ShaderLab blocks never have one): header tokens, then a
            // synthesized brace pair around the children.
            for (const auto& t : node.tokens) emitter.handle(t);
            if (node.hasBody) {
                emitter.handle(syntheticToken(TokenType::Punctuator, "{"));
                for (const auto& c : node.children) emitNode(emitter, *c);
                if (node.hasClosingBrace) emitter.handle(syntheticToken(TokenType::Punctuator, "}"));
            }
            break;
        case NodeKind::CgProgramBlock:
            // tokens = [openMarker, closeMarker] but the embedded HLSL
            // program (children[0]) belongs *between* them, not after
            // both - unlike the generic "tokens then children" default,
            // this needs its own order.
            emitter.handle(node.tokens[0]); // 'CGPROGRAM' / 'HLSLPROGRAM'
            if (!node.children.empty()) emitNode(emitter, *node.children[0]);
            if (node.hasClosingBrace && node.tokens.size() > 1) emitter.handle(node.tokens[1]); // 'ENDCG' / 'ENDHLSL'
            break;
        case NodeKind::ShaderLabCommand:
            // Unlike every other opaque/leaf kind, a ShaderLabCommand's
            // tokens can include a literal "{}" pair (Unity's empty-
            // texture property suffix, e.g. `_MainTex (...) = "white"
            // {}`) that is NOT a real block - feeding '{'/'}' through
            // handle()'s normal brace-tracking would incorrectly perturb
            // indentation (see Parser.cpp's parseShaderLabCommand()), so
            // those two tokens go through the plain-text path instead.
            for (const auto& t : node.tokens) {
                if (t.type == TokenType::Punctuator && (t.text == "{" || t.text == "}")) {
                    emitter.handleAsPlainToken(t);
                } else {
                    emitter.handle(t);
                }
            }
            break;
        case NodeKind::CaseLabel: {
            // tokens = ["case"/"default", <expr tokens...>, ':'] (see
            // Parser.cpp::parseCaseLabel()). That trailing ':' is a label
            // terminator, not a ternary ':' - it must never get a leading
            // space ("case 1:", not "case 1 :"), so it's fed through
            // handle()'s forceNoSpaceBeforeThis override instead of the
            // ordinary spacing heuristic every other token here uses.
            for (size_t i = 0; i < node.tokens.size(); i++) {
                bool isTrailingColon = i + 1 == node.tokens.size() &&
                                        node.tokens[i].type == TokenType::Punctuator &&
                                        node.tokens[i].text == ":";
                emitter.handle(node.tokens[i], isTrailingColon);
            }
            break;
        }
        default:
            // Leaf/opaque kinds (Chunk, ErrorNode, PreprocessorLine,
            // DeclStmt, ExprStmt, ReturnStmt, BreakContinueStmt,
            // FunctionDecl, StructField, WgslVarDecl, WgslLetConstDecl,
            // WgslFnDecl, ShaderLabCommand) and Program: real stored
            // tokens (with their own real onOwnLine/blankLinesBefore)
            // followed by structural children, if any (FunctionDecl/
            // WgslFnDecl's body, Program's items).
            for (const auto& t : node.tokens) emitter.handle(t);
            for (const auto& c : node.children) emitNode(emitter, *c);
            break;
    }

    for (const auto& tv : node.trailingTrivia) emitter.handle(tv.token);
}

} // namespace

FormatResult format(const std::string& source, Language lang, const FormatOptions& options) {
    LexResult lexed = lex(source, lang);
    ParseResult parsed = parse(lexed.tokens, lang);

    Emitter emitter(options, lang);
    emitNode(emitter, *parsed.program);
    emitter.finish();

    std::vector<LexError> errors = lexed.errors;
    errors.insert(errors.end(), parsed.errors.begin(), parsed.errors.end());
    return FormatResult{emitter.result(), std::move(errors)};
}

} // namespace shaderfmt
