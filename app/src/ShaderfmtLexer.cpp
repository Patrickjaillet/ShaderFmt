#include "ShaderfmtLexer.hpp"

#include <Qsci/qsciscintilla.h>
#include <QColor>
#include <QFont>

ShaderfmtLexer::ShaderfmtLexer(QObject* parent) : QsciLexerCustom(parent) {}

void ShaderfmtLexer::setShaderLanguage(shaderfmt::Language lang) {
    lang_ = lang;
}

const char* ShaderfmtLexer::language() const {
    switch (lang_) {
        case shaderfmt::Language::GLSL: return "GLSL";
        case shaderfmt::Language::Shadertoy: return "Shadertoy";
        case shaderfmt::Language::HLSL: return "HLSL";
        case shaderfmt::Language::WGSL: return "WGSL";
        case shaderfmt::Language::MSL: return "MSL";
        case shaderfmt::Language::ShaderLab: return "ShaderLab";
    }
    return "GLSL";
}

QString ShaderfmtLexer::description(int style) const {
    switch (style) {
        case Default: return QStringLiteral("Default");
        case Comment: return QStringLiteral("Comment");
        case Preprocessor: return QStringLiteral("Preprocessor");
        case StringLiteral: return QStringLiteral("String");
        case Number: return QStringLiteral("Number");
        case Punctuator: return QStringLiteral("Punctuator");
        case Identifier: return QStringLiteral("Identifier");
        default: return QString();
    }
}

namespace {

// VS Code Dark+ -like foreground palette (the one this lexer originally
// shipped with) alongside a parallel light palette, so applyTheme() can
// switch between them without changing the class of colors used (still
// muted-green comments, muted strings, etc.) - only their tone.
QColor foregroundFor(int style, bool dark) {
    if (dark) {
        switch (style) {
            case ShaderfmtLexer::Comment: return QColor(0x6a, 0x99, 0x55);
            case ShaderfmtLexer::Preprocessor: return QColor(0x9b, 0x9b, 0x9b);
            case ShaderfmtLexer::StringLiteral: return QColor(0xce, 0x91, 0x78);
            case ShaderfmtLexer::Number: return QColor(0xb5, 0xce, 0xa8);
            case ShaderfmtLexer::Punctuator: return QColor(0xd4, 0xd4, 0xd4);
            case ShaderfmtLexer::Identifier: return QColor(0x9c, 0xdc, 0xfe);
            case ShaderfmtLexer::Default:
            default: return QColor(0xd4, 0xd4, 0xd4);
        }
    }
    switch (style) {
        case ShaderfmtLexer::Comment: return QColor(0x00, 0x80, 0x00);
        case ShaderfmtLexer::Preprocessor: return QColor(0x80, 0x80, 0x80);
        case ShaderfmtLexer::StringLiteral: return QColor(0xa3, 0x15, 0x15);
        case ShaderfmtLexer::Number: return QColor(0x09, 0x86, 0x58);
        case ShaderfmtLexer::Punctuator: return QColor(0x1e, 0x1e, 0x1e);
        case ShaderfmtLexer::Identifier: return QColor(0x00, 0x10, 0xa0);
        case ShaderfmtLexer::Default:
        default: return QColor(0x1e, 0x1e, 0x1e);
    }
}

QColor paperFor(bool dark) {
    return dark ? QColor(0x1e, 0x1e, 0x1e) : QColor(0xff, 0xff, 0xff);
}

} // namespace

QColor ShaderfmtLexer::defaultColor(int style) const {
    return foregroundFor(style, darkTheme_);
}

QColor ShaderfmtLexer::defaultPaper(int style) const {
    Q_UNUSED(style);
    return paperFor(darkTheme_);
}

QFont ShaderfmtLexer::defaultFont(int style) const {
    Q_UNUSED(style);
    return QFont(QStringLiteral("Consolas"), 10);
}

void ShaderfmtLexer::applyTheme(bool dark) {
    darkTheme_ = dark;
    QColor paper = paperFor(dark);
    for (int style : {Default, Comment, Preprocessor, StringLiteral, Number, Punctuator, Identifier}) {
        setColor(foregroundFor(style, dark), style);
        setPaper(paper, style);
    }
}

// Re-lexes the whole document on every call rather than restyling only
// [start, end): shaderfmt::lex() is a cheap, non-incremental pure function
// of the full source (see docs/lexer-parser-study.md), and re-running it
// keeps this in lockstep with what format() will actually see. Revisit if
// this becomes a perceptible input lag on large files (roadmap §7).
void ShaderfmtLexer::styleText(int start, int end) {
    Q_UNUSED(start);
    Q_UNUSED(end);
    if (!editor()) return;

    QByteArray bytes = editor()->text().toUtf8();
    std::string source(bytes.constData(), static_cast<size_t>(bytes.size()));

    auto lexResult = shaderfmt::lex(source, lang_);
    lastErrors_ = lexResult.errors;

    startStyling(0);
    size_t pos = 0;
    for (const auto& tok : lexResult.tokens) {
        if (tok.type == shaderfmt::TokenType::EndOfFile) break;
        if (tok.offset > pos) {
            setStyling(static_cast<int>(tok.offset - pos), Default);
        }

        int styleId = Default;
        switch (tok.type) {
            case shaderfmt::TokenType::LineComment:
            case shaderfmt::TokenType::BlockComment:
                styleId = Comment;
                break;
            case shaderfmt::TokenType::Preprocessor:
                styleId = Preprocessor;
                break;
            case shaderfmt::TokenType::StringLiteral:
                styleId = StringLiteral;
                break;
            case shaderfmt::TokenType::Number:
                styleId = Number;
                break;
            case shaderfmt::TokenType::Punctuator:
                styleId = Punctuator;
                break;
            case shaderfmt::TokenType::Identifier:
                styleId = Identifier;
                break;
            default:
                styleId = Default;
                break;
        }
        setStyling(static_cast<int>(tok.text.size()), styleId);
        pos = tok.offset + tok.text.size();
    }
    if (pos < source.size()) {
        setStyling(static_cast<int>(source.size() - pos), Default);
    }

    emit diagnosticsChanged();
}
