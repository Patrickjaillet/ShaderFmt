#pragma once

#include "shaderfmt/Token.hpp"

#include <memory>
#include <vector>

namespace shaderfmt {

// A lossless concrete syntax tree (CST): every token from the lexer -
// including every comment and the preprocessor - is recoverable by
// walking the tree, in original order. This is deliberately NOT a
// compiler-grade AST: expressions are kept as opaque token runs (`Chunk`)
// rather than parsed with operator precedence, since the formatter only
// ever re-spaces/re-indents tokens and never needs to reason about what
// an expression *means* (see docs/lexer-parser-study.md). The tree exists
// to solve two problems a flat token stream can't: attaching a comment to
// the syntactic unit it actually precedes (not just "the previous line"),
// and recovering tolerantly from a construct the grammar doesn't
// recognize without losing or reordering any token.
//
// children[] layout is fixed per NodeKind and documented one line above
// each entry below. A single flat struct with a `kind` tag (rather than a
// virtual class hierarchy) matches this codebase's existing style
// (Lexer.cpp's `Scanner`, Formatter.cpp's `Emitter` are both one flat
// class each) and keeps the tree trivially walkable.
//
// Known limitation, stated plainly: a preprocessor directive that opens
// *mid*-construct (e.g. a "#ifdef" splitting a single parameter list or
// struct body rather than sitting between two statements/declarations) is
// not specially spliced in - it makes that one enclosing construct fail
// to match its production, so the whole thing (still including the
// directive, still in original order) falls back to ErrorNode. Nothing is
// ever lost or reordered, but that span isn't reformatted/reindented, only
// preserved. Preprocessor directives sitting between statements or
// declarations (by far the common case - #version, #define, #ifdef
// wrapping a whole function) parse as ordinary PreprocessorLine siblings
// and format normally.
// Convention: IfStmt/ForStmt/WhileStmt/DoWhileStmt/SwitchStmt store none of
// their own tokens (`tokens` stays empty) - "if"/"for"/"while"/"do"/
// "switch"/"else"/the separating ";" in "while (...);" are fixed,
// deterministic spellings with zero data content, synthesized from
// NodeKind (and, for "else", from children.size()) rather than stored.
// Every other NodeKind stores real, variable content in `tokens` and
// nothing is synthesized. This keeps the tree honestly lossless for
// everything that actually varies (identifiers, literals, punctuation,
// comments) without redundantly duplicating keyword spellings that can
// never differ from what NodeKind already says.
enum class NodeKind {
    // children = top-level items, in source order: FunctionDecl,
    // StructDecl, DeclStmt (top-level uniform/var), PreprocessorLine, or
    // ErrorNode for anything unrecognized. WGSL: WgslFnDecl, WgslVarDecl,
    // WgslLetConstDecl, StructDecl.
    Program,

    // children = statements in source order (If/For/While/DoWhile/Switch/
    // Return/BreakContinue/ExprStmt/DeclStmt/Block/PreprocessorLine/
    // ErrorNode). The '{'/'}' braces themselves are NOT stored here (the
    // emitter synthesizes them from indent depth, same as today), keeping
    // Block purely a container of statements.
    Block,

    // children[0] = condition (Chunk, the parenthesized "(...)" run
    // including the parens), children[1] = then-branch (Block or a single
    // statement node for a brace-less if), children[2] = else-branch
    // (optional: Block, another IfStmt for "else if", or a single
    // statement node).
    IfStmt,

    // children[0] = header (Chunk: "(init; cond; step)" as one opaque
    // run, since splitting it buys nothing - the existing parenDepth_
    // guard already keeps its semicolons from being treated as statement
    // ends), children[1] = body (Block or single statement).
    ForStmt,

    // children[0] = condition (Chunk), children[1] = body (Block or
    // single statement).
    WhileStmt,

    // children[0] = body (Block or single statement), children[1] =
    // condition (Chunk, "(...)").
    DoWhileStmt,

    // children[0] = subject (Chunk, "(...)"), children[1] = body (Block
    // containing CaseLabel/statement children in source order).
    SwitchStmt,

    // tokens = "case <chunk>:" or "default:" run up to and including the
    // ':'. Statements after a label are NOT nested under it - like real
    // C-like switch/case, they're subsequent siblings in the enclosing
    // SwitchStmt body Block (fall-through is just "the next sibling").
    CaseLabel,

    // tokens = "return <chunk> ;" (chunk may be empty for a bare "return;").
    ReturnStmt,

    // tokens = "break ;" / "continue ;" / "discard ;" verbatim.
    BreakContinueStmt,

    // tokens = the whole expression-statement including the trailing ';'
    // (kept opaque - assignments, function calls, increment/decrement).
    ExprStmt,

    // tokens = the whole declaration including the trailing ';'
    // (qualifiers + type + declarator(s) + optional initializer + optional
    // trailing ": SEMANTIC"/": register(...)" suffix). Kept as one opaque
    // run: the point of DeclStmt as its own NodeKind is so a leading
    // comment attaches to "this declaration" rather than to a sibling,
    // not to expose sub-structure the Emitter doesn't need.
    DeclStmt,

    // tokens = qualifiers + return type + name + "(" ... ")" signature,
    // INCLUDING an optional trailing ": SEMANTIC" (HLSL) - kept opaque for
    // the same reason as DeclStmt. children[0] = body (Block), or absent
    // for a forward declaration ("void f();").
    FunctionDecl,

    // tokens = "struct Name" (or "cbuffer Name : register(b0)" for HLSL -
    // same shape, reuses this NodeKind rather than adding a parallel one).
    // children = StructField/PreprocessorLine/ErrorNode, in source order.
    // tokens also holds the trailing ';' if the struct declaration ends
    // with one (GLSL/HLSL struct decls do; cbuffer doesn't).
    StructDecl,

    // tokens = one field: qualifiers + type + name + optional array +
    // optional ": SEMANTIC" + ';'.
    StructField,

    // tokens = the single raw Preprocessor token (see Token.hpp - already
    // captured whole by the lexer, never split further here).
    PreprocessorLine,

    // tokens = an opaque run of tokens: an expression, a qualifier list, a
    // macro-like argument list - anything the grammar deliberately doesn't
    // structure further (see file header comment).
    Chunk,

    // tokens = whatever span the parser couldn't fit into any production.
    // Tolerant-parse fallback: never dropped, never reordered, just not
    // understood structurally. See Parser.cpp's recoverAsErrorNode().
    ErrorNode,

    // --- WGSL-specific (WGSL's grammar differs enough from the C-like
    // family - `fn`/`let`/`var`/`const`, `@attribute` decorators, `->`
    // return type, `vec3<f32>` generics - that it gets its own node kinds
    // rather than being forced into the C-like shapes above). Leading
    // "@attr(...)" decorators are kept as a token prefix inside `tokens`
    // itself (not a separate sub-node): they're always immediately
    // followed by exactly the declaration they modify, so keeping them in
    // the same opaque run loses no round-trip fidelity and needs no extra
    // machinery - the same "opaque unless structure is actually needed"
    // philosophy as Chunk. ---

    // tokens = optional "@attr..." prefix + "var<space>? name : type
    // = init ;" (or without the optional "<address space>"/initializer).
    WgslVarDecl,

    // tokens = optional "@attr..." prefix + "let|const name : type = init ;".
    WgslLetConstDecl,

    // tokens = optional "@attr..." prefix (e.g. "@vertex", "@compute
    // @workgroup_size(64)") + "fn name (" ... ")" signature, including
    // per-parameter "@builtin(...)"/"@location(...)" attributes and the
    // "-> <return type>" clause if present - all kept opaque, same
    // rationale as FunctionDecl. children[0] = body (Block).
    WgslFnDecl,

    // --- Unity ShaderLab-specific (Language::ShaderLab). ShaderLab wraps
    // Cg/HLSL in its own brace-delimited block syntax (Shader/Properties/
    // SubShader/Pass/Tags...), which isn't C-like at all - a "statement"
    // has no ';' terminator, it's just whatever's on one source line. ---

    // tokens = `Shader "Name"`. children = top-level ShaderLabBlock/
    // ShaderLabCommand items in source order. The '{'/'}' wrapping them
    // aren't stored, same synthesized-brace convention as StructDecl.
    ShaderDecl,

    // A generic "Keyword [args] { ... }" block: Properties, SubShader,
    // Pass, Tags, Category, or any other ShaderLab block-shaped
    // construct. tokens = the header (keyword + any args/quoted name
    // before the '{'). children = nested ShaderLabBlock/ShaderLabCommand/
    // CgProgramBlock items, in source order (braces synthesized by the
    // emitter, same convention as Block/StructDecl).
    ShaderLabBlock,

    // A bare ShaderLab command with no block body - "Cull Back", "ZWrite
    // On", "LOD 100", "Fallback "Diffuse"", a `#pragma`-less line, etc.
    // tokens = the whole line, opaque (ShaderLab has no reserved-word
    // grammar for these beyond "starts a new line and isn't a block
    // keyword" - see Parser.cpp's parseShaderLabItem()).
    ShaderLabCommand,

    // A "CGPROGRAM ... ENDCG" or "HLSLPROGRAM ... ENDHLSL" region.
    // tokens = [openMarker, closeMarker] (both stored verbatim since,
    // unlike if/for/while's fixed spelling, ShaderLab has two different
    // legal marker pairs - not deterministic from NodeKind alone).
    // children[0] = the embedded HLSL code's own Program node, produced
    // by recursively invoking parse() on the token span between the
    // markers with Language::HLSL - real Cg/HLSL formatting for the part
    // that's actually a C-like language, not just an opaque blob.
    CgProgramBlock,
};

// A comment attached to a node rather than left floating in a flat token
// stream - this is what makes comment placement a parsing decision
// (attached during parse()) instead of an Emitter-side proximity guess.
struct Trivia {
    Token token;            // LineComment or BlockComment
    bool trailing = false;  // true: glued to the end of the previous line (token.onOwnLine == false)
};

struct Node {
    NodeKind kind = NodeKind::ErrorNode;

    // Captured from the first token of this node's span at parse time
    // (before it's consumed), same meaning as Token::onOwnLine/
    // blankLinesBefore. Needed because structural kinds (IfStmt and
    // friends, Block) don't store their own leading keyword/brace token
    // (see the synthesized-keyword convention above) - without this,
    // that token's layout metadata would simply be lost, and e.g. a
    // blank line right before an "if" wouldn't survive formatting. For
    // leaf/opaque kinds this duplicates tokens[0]'s own fields, which is
    // harmless and lets callers read layout metadata uniformly without
    // checking `kind` first.
    bool onOwnLine = false;
    int blankLinesBefore = 0;

    // Comments immediately before this node that belong to it (leading,
    // each on its own line) and, at most one, immediately after it on the
    // same line (trailing).
    std::vector<Trivia> leadingTrivia;
    std::vector<Trivia> trailingTrivia;

    // Opaque/verbatim token run for leaf-shaped kinds (Chunk, ErrorNode,
    // PreprocessorLine, DeclStmt, ExprStmt, FunctionDecl signature, etc. -
    // see the per-kind comments on NodeKind above for exactly what each
    // kind stores here).
    std::vector<Token> tokens;

    // Structural children, meaning fixed per NodeKind (see comments above).
    std::vector<std::unique_ptr<Node>> children;

    // Block/StructDecl/ShaderDecl/ShaderLabBlock/CgProgramBlock only: true
    // unless the parser hit EOF before finding the matching '}' (or, for
    // CgProgramBlock, the matching ENDCG/ENDHLSL) - truncated/mid-typing
    // input, see the fuzz/truncation tests. The emitter must NOT
    // synthesize a terminator that was never actually there, or it would
    // "complete" malformed input differently than the token-stream
    // emitter does (which simply has no such token to emit) - see the
    // parity tests in tests/test_main.cpp that caught exactly this.
    bool hasClosingBrace = true;

    // StructDecl/ShaderDecl/ShaderLabBlock only (default false there too,
    // see Parser.cpp): true iff an opening '{' was actually found for
    // this header (truncated input, e.g. just "cbuffer" or "SubShader"
    // with nothing after it, never gets a body at all - not even an
    // empty one). Block doesn't need this: parseBlock() is only ever
    // called once '{' has already been confirmed present, so it always
    // has a body.
    bool hasBody = true;

    // StructDecl only, populated solely by Parser.cpp's
    // parseInterfaceBlock(): the optional declarator run after a GLSL
    // named interface block's closing '}' - "instance;", "instance[4];"
    // ("uniform Block { ... } instance;") - including its terminating
    // ';'. Kept separate from `tokens` (the header, before the body)
    // rather than appended there, since it belongs *after* the body, not
    // before it - unlike `tokens`' own trailing ';' for a plain struct/
    // cbuffer with no instance name. Empty for every other StructDecl.
    std::vector<Token> trailingDeclarator;
};

using NodePtr = std::unique_ptr<Node>;

} // namespace shaderfmt
