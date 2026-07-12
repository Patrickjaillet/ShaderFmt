#include "shaderfmt/GlslVersionLint.hpp"
#include "shaderfmt/Lexer.hpp"
#include "shaderfmt/Token.hpp"

#include <cctype>

namespace shaderfmt {

namespace {

// Parses "#version 330 core" (or "#version 330", or malformed/partial
// directives typed mid-edit) into a version number. Returns 110 - GLSL's
// documented default when no #version directive is present - if nothing
// usable is found.
//
// Bug-audit fix: this used to locate the directive with text.find("version"),
// a plain substring search over the *whole* raw preprocessor line. That
// matched any "#define"/"#ifdef"/... line that merely happened to contain
// "version" somewhere in its name (e.g. "#define version2_check 100"),
// and if a digit immediately followed that substring, the digits were
// parsed as if they were the declared GLSL version - a false positive
// that could silently override the real "#version" directive elsewhere in
// the file (found via `#define version2_check 100` before a real
// `#version 330`, which produced a bogus "requires GLSL >= 140" warning).
// Fixed by requiring "version" to be the directive keyword itself: only
// after the '#' and optional whitespace, not merely present anywhere in
// the line.
int parseDeclaredVersion(const std::vector<Token>& tokens) {
    for (const Token& t : tokens) {
        if (t.type != TokenType::Preprocessor) continue;
        const std::string& text = t.text;
        if (text.empty() || text[0] != '#') continue;

        size_t i = 1;
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) i++;
        static const std::string kKeyword = "version";
        if (text.compare(i, kKeyword.size(), kKeyword) != 0) continue;
        i += kKeyword.size();
        // Reject "#versionXYZ" (part of a longer identifier, e.g. a
        // hypothetical "#versioned_foo" directive) - a real "#version"
        // directive is followed by whitespace or end of line.
        if (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) continue;

        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) i++;
        size_t numStart = i;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) i++;
        if (i == numStart) continue; // "#version" with no number yet (mid-typing)

        return std::stoi(text.substr(numStart, i - numStart));
    }
    return 110;
}

} // namespace

std::vector<GlslVersionWarning> lintGlslVersionQualifiers(const std::string& source) {
    std::vector<GlslVersionWarning> warnings;

    LexResult lexed = lex(source, Language::GLSL);
    int version = parseDeclaredVersion(lexed.tokens);

    for (const Token& t : lexed.tokens) {
        if (t.type != TokenType::Identifier) continue;

        if ((t.text == "attribute" || t.text == "varying") && version >= 130) {
            warnings.push_back(GlslVersionWarning{
                t.line, t.column, static_cast<int>(t.text.size()),
                "'" + t.text + "' is deprecated in GLSL " + std::to_string(version)
                    + " (>= 130) - use 'in'/'out' instead"});
        } else if (t.text == "layout" && version < 140) {
            warnings.push_back(GlslVersionWarning{
                t.line, t.column, static_cast<int>(t.text.size()),
                "'layout(...)' requires GLSL >= 140, but this file declares #version "
                    + std::to_string(version)});
        }
    }

    return warnings;
}

} // namespace shaderfmt
