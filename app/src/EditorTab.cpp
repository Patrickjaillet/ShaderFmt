#include "EditorTab.hpp"

#include <Qsci/qsciscintilla.h>
#include <QColor>
#include <QVBoxLayout>

EditorTab::EditorTab(QWidget* parent) : QWidget(parent) {
    editor_ = new QsciScintilla(this);
    editor_->setUtf8(true);
    editor_->setEolMode(QsciScintilla::EolUnix); // matches shaderfmt's '\n'-only assumption
    editor_->setIndentationsUseTabs(false);
    editor_->setTabWidth(4);
    editor_->setAutoIndent(true);
    editor_->setMarginType(0, QsciScintilla::NumberMargin);
    editor_->setMarginWidth(0, QStringLiteral("0000"));
    editor_->setWrapMode(QsciScintilla::WrapNone);
    editor_->setBraceMatching(QsciScintilla::SloppyBraceMatch);

    lexer_ = new ShaderfmtLexer(this);
    editor_->setLexer(lexer_);

    editor_->indicatorDefine(QsciScintilla::SquiggleIndicator, kErrorIndicator);
    editor_->setIndicatorForegroundColor(QColor(220, 50, 47), kErrorIndicator);
    editor_->setIndicatorDrawUnder(true, kErrorIndicator);

    // Advisory-only GLSL #version/qualifier warnings - a distinct color
    // from the red lex-error squiggle above, since these never block
    // anything (see GlslVersionLint.hpp).
    editor_->indicatorDefine(QsciScintilla::SquiggleIndicator, kGlslVersionWarningIndicator);
    editor_->setIndicatorForegroundColor(QColor(180, 140, 0), kGlslVersionWarningIndicator);
    editor_->setIndicatorDrawUnder(true, kGlslVersionWarningIndicator);

    connect(editor_, &QsciScintilla::modificationChanged, this, &EditorTab::setDirty);

    formatWatcher_ = new QFutureWatcher<shaderfmt::FormatResult>(this);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(editor_);
}

void EditorTab::setDirty(bool dirty) {
    if (dirty_ == dirty) return;
    dirty_ = dirty;
    emit dirtyChanged(dirty_);
}
