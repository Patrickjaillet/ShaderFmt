#pragma once

#include "shaderfmt/Ast.hpp"
#include "shaderfmt/Token.hpp"

#include <vector>

namespace shaderfmt {

struct ParseResult {
    NodePtr program;               // never null: worst case, one ErrorNode child spanning everything
    std::vector<LexError> errors;  // best-effort diagnostics; same shape as the lexer's, never fatal
};

// Builds a lossless concrete syntax tree from an already-lexed token
// stream (composes after lex() exactly like the formatter's Emitter does
// today - see Formatter.cpp). Pure function, never throws: on input it
// can't structurally recognize (including token streams built from
// truncated/malformed source), it falls back to ErrorNode spans rather
// than aborting, exactly mirroring the lexer's own tolerance guarantee.
// Walking the returned tree and concatenating every token (structural
// tokens, all trivia, all ErrorNode/interleaved tokens) in order
// reproduces the exact input token sequence - see the round-trip test in
// tests/test_main.cpp.
ParseResult parse(const std::vector<Token>& tokens, Language lang);

} // namespace shaderfmt
