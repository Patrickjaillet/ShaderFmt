#pragma once

#include "shaderfmt/Token.hpp"
#include <string>
#include <vector>

namespace shaderfmt {

// Result of a lex pass. Lexing is tolerant: it never throws and never
// aborts on malformed input (e.g. an unterminated string typed mid-edit).
// Instead it records diagnostics in `errors` and best-effort tokenizes the
// rest of the buffer, so the editor can keep formatting/coloring partial
// code while the user is still typing.
struct LexResult {
    std::vector<Token> tokens; // always ends with a TokenType::EndOfFile token
    std::vector<LexError> errors;
    bool ok() const { return errors.empty(); }
};

// Tokenizes `source` as the given language dialect. Pure function: same
// input always produces the same token stream (required for idempotence
// of anything built on top, e.g. the formatter).
LexResult lex(const std::string& source, Language lang);

} // namespace shaderfmt
