#pragma once

#include "shaderfmt/Token.hpp"
#include <string>

namespace shaderfmt {

// How the '{' that opens a block is placed, relative to the statement
// that introduces it. Both styles only move whitespace/newlines around
// the same brace token, so - like every other FormatOptions knob - they
// can never change program behaviour (roadmap principle: zero semantic
// modification).
enum class BraceStyle {
    SameLine,  // K&R-style: "if (x) {" - the current default.
    NextLine,  // Allman-style: '{' alone on the line after the statement,
               // at the same indent depth as that statement.
};

struct FormatOptions {
    int indentWidth = 4;
    bool useTabs = false;
    BraceStyle braceStyle = BraceStyle::SameLine;
    // Espace entre un mot-clé de contrôle (if/for/while/switch/return/
    // else/do/case) et la parenthèse ouvrante qui suit : "if (x)" (true,
    // par défaut) vs "if(x)" (false). Seul espacement individuellement
    // configurable pour l'instant (roadmap §4) - les autres espacements
    // restent les heuristiques fixes de l'Emitter (roadmap §1), qui ne
    // varient jamais d'un token à l'autre donc n'ont pas d'ambiguïté de
    // style à trancher (contrairement à celui-ci, où les deux conventions
    // sont courantes dans des guides de style réels).
    bool spaceAfterControlKeyword = true;
};

struct FormatResult {
    std::string formatted;
    std::vector<LexError> errors; // propagated from the lex pass, if any
};

// Formats `source` written in `lang`. This is a pure, deterministic
// function of (source, lang, options): formatting never depends on any
// external/global state, and formatting already-formatted output is a
// no-op (idempotence), which the unit tests enforce on the corpus.
//
// Implementation note: lexes, parses into a lossless CST (see Ast.hpp/
// Parser.hpp - a tolerant parser, comments attached to the syntax node
// they precede rather than positioned by proximity), then walks that tree
// to re-emit it. Expressions stay opaque token runs within the tree
// rather than being parsed with operator precedence - GLSL/HLSL/WGSL are
// whitespace-insensitive, so only inserting/removing whitespace between
// unmodified, unreordered tokens can never change program behaviour, and
// that's all the emitter ever does. This satisfies "zero semantic
// modification" without needing a full expression grammar; see
// docs/lexer-parser-study.md for the scope rationale.
FormatResult format(const std::string& source, Language lang,
                     const FormatOptions& options = {});

} // namespace shaderfmt
