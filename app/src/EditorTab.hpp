#pragma once

#include "ShaderfmtLexer.hpp"
#include "shaderfmt/Formatter.hpp"

#include <QFutureWatcher>
#include <QString>
#include <QWidget>

#include <string>

class QsciScintilla;

// One open file/buffer (roadmap "UI pro" phase 1: onglets multi-fichiers).
// Bundles everything that used to be a single set of MainWindow members
// (editor_, lexer_, languageManuallySet_, the async-format bookkeeping) so
// each tab can be edited, formatted, and lexed completely independently of
// every other open tab. MainWindow now operates on whichever EditorTab is
// current, via the small *For(EditorTab*) methods that replaced the old
// parameterless ones.
class EditorTab : public QWidget {
    Q_OBJECT
public:
    explicit EditorTab(QWidget* parent = nullptr);

    QsciScintilla* editor() const { return editor_; }
    ShaderfmtLexer* lexer() const { return lexer_; }
    QFutureWatcher<shaderfmt::FormatResult>* formatWatcher() const { return formatWatcher_; }

    // Empty for a never-saved "Sans titre" tab.
    QString filePath() const { return filePath_; }
    void setFilePath(const QString& path) { filePath_ = path; }

    // The tab bar label sans any dirty marker - "Sans titre N" until a
    // real file path is set, then that file's name. Kept separately from
    // QTabWidget::tabText() itself since the displayed text also carries a
    // "*" prefix once dirty, which would otherwise have to be stripped
    // back out every time the underlying name is needed.
    QString displayName;

    // Whether the language combo's "Auto" mode should keep re-detecting on
    // every edit (false once the user picks a dialect manually).
    bool languageManuallySet() const { return languageManuallySet_; }
    void setLanguageManuallySet(bool manual) { languageManuallySet_ = manual; }

    bool isDirty() const { return dirty_; }
    void setDirty(bool dirty);

    // Async-format bookkeeping (roadmap §7), now per tab so formatting one
    // tab can never race with or block formatting another - see
    // MainWindow::applyFormatFor()/onFormatFinishedFor().
    std::string formatRequestedSource;
    bool formatAgainRequested = false;
    // Guards re-entrancy while a format result (or a file load) replaces
    // this tab's text programmatically, so textChanged doesn't re-trigger
    // detection/formatting on text nobody typed.
    bool applyingProgrammaticChange = false;

    // Snapshot of the last Format pass that actually changed this tab's
    // text (roadmap "UI pro" phase 2: "vue diff avant/apres formatage"),
    // so MainWindow::onShowDiffClicked() can build a diff on demand
    // without needing to keep the async format machinery's transient
    // strings around any longer than the format call itself needed them.
    QString lastFormatBefore;
    QString lastFormatAfter;
    bool hasLastFormatDiff = false;

    static constexpr int kErrorIndicator = 0;
    static constexpr int kGlslVersionWarningIndicator = 1;

signals:
    void dirtyChanged(bool dirty);

private:
    QsciScintilla* editor_ = nullptr;
    ShaderfmtLexer* lexer_ = nullptr;
    QFutureWatcher<shaderfmt::FormatResult>* formatWatcher_ = nullptr;
    QString filePath_;
    bool languageManuallySet_ = false;
    bool dirty_ = false;
};
