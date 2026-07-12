#pragma once

#include <string>
#include <vector>

namespace shaderfmt {

// Advisory-only: never changes formatting behaviour (roadmap §0's first
// principle), never blocks anything. Purely a diagnostic the editor can
// show via its existing non-blocking error indicator (roadmap §3).
struct GlslVersionWarning {
    int line = 0;    // 1-based, matches Token::line
    int column = 0;  // 1-based, matches Token::column
    int length = 0;  // length in characters of the flagged token
    std::string message;
};

// Bounded GLSL #version/qualifier consistency check - deliberately not a
// full semantic validator (see ROADMAP.md §2's GLSL entry for the scope
// this project has chosen not to cover: it doesn't understand extensions,
// doesn't know every qualifier's exact version cutoff, and profile
// (core/compatibility/es) is read but not used to further narrow rules).
// Covers the two most common real-world version mistakes:
//   - `attribute`/`varying` (removed from core profile GLSL >= 150,
//     deprecated already at >= 130) used instead of `in`/`out`.
//   - `layout(...)` (introduced in GLSL 1.40) used in a file declared
//     for an earlier version.
// If no `#version` directive is present, GLSL defaults to 1.10 (per the
// spec) - so both checks are meaningful even without one, since 1.10 is
// the most restrictive case (originates the "attribute/varying" era and
// predates `layout`).
std::vector<GlslVersionWarning> lintGlslVersionQualifiers(const std::string& source);

} // namespace shaderfmt
