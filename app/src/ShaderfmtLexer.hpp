#pragma once

#include "shaderfmt/Lexer.hpp"
#include "shaderfmt/Token.hpp"

#include <Qsci/qscilexercustom.h>

// A single QsciLexerCustom driving syntax highlighting for all four
// dialects (roadmap §3: "Lexer custom par langage via QsciLexerCustom
// (GLSL/HLSL/WGSL n'ont pas de lexer QScintilla natif)"). One class is
// enough because the highlighting rule is the same for every dialect -
// map each shaderfmt::TokenType to a style - only the *tokenization*
// differs per Language, and that's already handled by shaderfmt::lex().
// Switching dialect is just switching which Language is passed to lex().
class ShaderfmtLexer : public QsciLexerCustom {
    Q_OBJECT
public:
    enum Style {
        Default = 0,
        Comment,
        Preprocessor,
        StringLiteral,
        Number,
        Punctuator,
        Identifier,
    };

    explicit ShaderfmtLexer(QObject* parent = nullptr);

    void setShaderLanguage(shaderfmt::Language lang);
    shaderfmt::Language shaderLanguage() const { return lang_; }

    // Recolors every style for the given theme (roadmap "UI pro" phase 0).
    // QsciLexerCustom styles are otherwise frozen at whatever defaultColor()/
    // defaultPaper() returned at construction time - switching the app-wide
    // QSS stylesheet alone does *not* touch QScintilla's own painting, so
    // this must be called explicitly whenever the theme changes.
    void applyTheme(bool dark);

    // Diagnostics from the most recent styleText() pass, so the editor can
    // surface them as non-blocking indicators (roadmap §3: "Indicateur
    // d'erreur de syntaxe non bloquant").
    const std::vector<shaderfmt::LexError>& lastErrors() const { return lastErrors_; }

    const char* language() const override;
    QString description(int style) const override;
    QColor defaultColor(int style) const override;
    QColor defaultPaper(int style) const override;
    QFont defaultFont(int style) const override;
    void styleText(int start, int end) override;

signals:
    void diagnosticsChanged();

private:
    shaderfmt::Language lang_ = shaderfmt::Language::GLSL;
    std::vector<shaderfmt::LexError> lastErrors_;
    bool darkTheme_ = true;
};
