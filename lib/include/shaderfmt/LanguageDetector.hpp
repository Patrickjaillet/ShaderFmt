#pragma once

#include "shaderfmt/Token.hpp"
#include <string>

namespace shaderfmt {

// Best-effort language detection from pasted source text alone: the editor
// has no file name/extension to go on (see ROADMAP.md §2), only the text
// the user just pasted or typed. This is a heuristic, not a parser - it
// looks for a handful of tokens that are effectively unique to one dialect
// and returns the first match in priority order (most distinctive dialect
// first). Falls back to GLSL, the most common paste target, when nothing
// distinctive is found. Pure function, safe to call on partial/invalid
// source (never throws).
Language detectLanguage(const std::string& source);

} // namespace shaderfmt
