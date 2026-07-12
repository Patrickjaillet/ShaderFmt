#include "shaderfmt/Lexer.hpp"

#include <cctype>

namespace shaderfmt {

namespace {

class Scanner {
public:
    Scanner(const std::string& src, Language lang) : src_(src), lang_(lang) {}

    LexResult run() {
        while (true) {
            consumeWhitespaceAndNewlines();
            if (atEnd()) break;

            int startLine = line_;
            int startCol = col_;
            size_t startOffset = pos_;
            bool ownLine = atLineStart_;
            int blanks = pendingBlankLines_;
            pendingBlankLines_ = 0;
            atLineStart_ = false;

            char c = peek();

            Token tok;
            tok.onOwnLine = ownLine;
            tok.blankLinesBefore = blanks;
            tok.line = startLine;
            tok.column = startCol;
            tok.offset = startOffset;

            if (c == '#' && lang_ != Language::WGSL && ownLine) {
                tok.type = TokenType::Preprocessor;
                tok.text = scanPreprocessorLine();
            } else if (c == '/' && peekAt(1) == '/') {
                tok.type = TokenType::LineComment;
                tok.text = scanLineComment();
            } else if (c == '/' && peekAt(1) == '*') {
                tok.type = TokenType::BlockComment;
                tok.text = scanBlockComment();
            } else if (c == '"') {
                tok.type = TokenType::StringLiteral;
                tok.text = scanString();
            } else if (std::isdigit(static_cast<unsigned char>(c)) ||
                       (c == '.' && std::isdigit(static_cast<unsigned char>(peekAt(1))))) {
                tok.type = TokenType::Number;
                tok.text = scanNumber();
            } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                tok.type = TokenType::Identifier;
                tok.text = scanIdentifier();
            } else {
                tok.type = TokenType::Punctuator;
                tok.text = scanPunctuator();
                if (tok.text.empty()) {
                    // Unknown byte (e.g. stray control character). Record a
                    // diagnostic but keep going instead of aborting, per the
                    // "tolerant parser" principle.
                    result_.errors.push_back(
                        {std::string("caractere inattendu: '") + c + "'", startLine, startCol});
                    tok.text = std::string(1, c);
                    advance();
                }
            }

            result_.tokens.push_back(std::move(tok));
        }

        Token eof;
        eof.type = TokenType::EndOfFile;
        eof.onOwnLine = atLineStart_;
        eof.blankLinesBefore = pendingBlankLines_;
        eof.line = line_;
        eof.column = col_;
        eof.offset = pos_;
        result_.tokens.push_back(eof);
        return result_;
    }

private:
    const std::string& src_;
    Language lang_;
    size_t pos_ = 0;
    int line_ = 1;
    int col_ = 1;
    bool atLineStart_ = true;
    int pendingBlankLines_ = 0;
    LexResult result_;

    bool atEnd() const { return pos_ >= src_.size(); }
    char peek() const { return atEnd() ? '\0' : src_[pos_]; }
    char peekAt(size_t offset) const {
        size_t p = pos_ + offset;
        return p < src_.size() ? src_[p] : '\0';
    }

    char advance() {
        char c = src_[pos_++];
        if (c == '\n') {
            line_++;
            col_ = 1;
        } else {
            col_++;
        }
        return c;
    }

    // Consumes runs of spaces/tabs/newlines, tracking whether a fully blank
    // line (a newline followed only by whitespace then another newline) was
    // seen, so the formatter can later collapse-but-preserve blank lines.
    void consumeWhitespaceAndNewlines() {
        int newlinesSeenInARow = 0;
        while (!atEnd()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\r') {
                advance();
            } else if (c == '\n') {
                advance();
                newlinesSeenInARow++;
                atLineStart_ = true;
                if (newlinesSeenInARow >= 2) {
                    pendingBlankLines_++;
                }
            } else {
                break;
            }
        }
    }

    std::string scanLineComment() {
        std::string out;
        while (!atEnd() && peek() != '\n') out += advance();
        return out;
    }

    std::string scanBlockComment() {
        std::string out;
        out += advance(); // '/'
        out += advance(); // '*'
        int depth = 1;
        // WGSL allows nested block comments; GLSL/HLSL/Shadertoy do not, but
        // treating them uniformly as non-nesting for those and nesting for
        // WGSL keeps behaviour correct for both without misparsing WGSL.
        while (!atEnd() && depth > 0) {
            if (lang_ == Language::WGSL && peek() == '/' && peekAt(1) == '*') {
                out += advance();
                out += advance();
                depth++;
            } else if (peek() == '*' && peekAt(1) == '/') {
                out += advance();
                out += advance();
                depth--;
            } else {
                out += advance();
            }
        }
        if (depth > 0) {
            result_.errors.push_back({"commentaire bloc non termine", line_, col_});
        }
        return out;
    }

    std::string scanString() {
        std::string out;
        out += advance(); // opening quote
        while (!atEnd() && peek() != '"' && peek() != '\n') {
            if (peek() == '\\' && peekAt(1) != '\0') {
                out += advance();
                out += advance();
            } else {
                out += advance();
            }
        }
        if (!atEnd() && peek() == '"') {
            out += advance();
        } else {
            result_.errors.push_back({"chaine non terminee", line_, col_});
        }
        return out;
    }

    std::string scanNumber() {
        std::string out;
        if (peek() == '0' && (peekAt(1) == 'x' || peekAt(1) == 'X')) {
            out += advance();
            out += advance();
            while (std::isxdigit(static_cast<unsigned char>(peek()))) out += advance();
        } else {
            while (std::isdigit(static_cast<unsigned char>(peek()))) out += advance();
            if (peek() == '.') {
                out += advance();
                while (std::isdigit(static_cast<unsigned char>(peek()))) out += advance();
            }
            if (peek() == 'e' || peek() == 'E') {
                out += advance();
                if (peek() == '+' || peek() == '-') out += advance();
                while (std::isdigit(static_cast<unsigned char>(peek()))) out += advance();
            }
        }
        // numeric suffixes: f, F, u, U, lf, LF, h, H (half), etc.
        while (std::isalpha(static_cast<unsigned char>(peek()))) out += advance();
        return out;
    }

    std::string scanIdentifier() {
        std::string out;
        while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_') {
            out += advance();
        }
        return out;
    }

    // Preprocessor directives are captured as a single raw token (including
    // backslash-newline continuations) and never interpreted/executed -
    // formatting must not depend on macro expansion.
    std::string scanPreprocessorLine() {
        std::string out;
        while (!atEnd()) {
            char c = peek();
            if (c == '\\' && peekAt(1) == '\n') {
                out += advance(); // backslash
                out += advance(); // newline
                continue;
            }
            if (c == '\n') break;
            out += advance();
        }
        return out;
    }

    std::string scanPunctuator() {
        static const char* multi3[] = {"<<=", ">>=", "...", nullptr};
        static const char* multi2[] = {
            "==", "!=", "<=", ">=", "&&", "||", "++", "--", "+=", "-=",
            "*=", "/=", "%=", "&=", "|=", "^=", "->", "::", "<<", ">>", nullptr};

        for (int i = 0; multi3[i]; i++) {
            if (matches(multi3[i])) return consumeLiteral(multi3[i]);
        }
        for (int i = 0; multi2[i]; i++) {
            if (matches(multi2[i])) return consumeLiteral(multi2[i]);
        }

        static const std::string singles = "{}()[];,.:?+-*/%=<>!&|^~@";
        if (singles.find(peek()) != std::string::npos) {
            return std::string(1, advance());
        }
        return {};
    }

    bool matches(const char* lit) const {
        size_t n = std::string(lit).size();
        for (size_t i = 0; i < n; i++) {
            if (peekAt(i) != lit[i]) return false;
        }
        return true;
    }

    std::string consumeLiteral(const char* lit) {
        std::string out;
        size_t n = std::string(lit).size();
        for (size_t i = 0; i < n; i++) out += advance();
        return out;
    }
};

} // namespace

LexResult lex(const std::string& source, Language lang) {
    Scanner scanner(source, lang);
    return scanner.run();
}

} // namespace shaderfmt
