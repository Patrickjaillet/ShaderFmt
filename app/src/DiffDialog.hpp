#pragma once

#include <QDialog>
#include <QString>

// Read-only unified-diff viewer (roadmap "UI pro" phase 2): shows what
// the last Format pass actually changed in the current tab, so the
// engine's "never touches your code's logic, only whitespace" promise
// can be visually double-checked rather than taken on faith.
class DiffDialog : public QDialog {
    Q_OBJECT
public:
    explicit DiffDialog(const QString& unifiedDiffText, QWidget* parent = nullptr);
};

// Builds a unified, line-based diff ("  " unchanged / "- " removed /
// "+ " added prefixes) via the classic LCS backtrace. Returns a single
// placeholder line instead of the real diff for inputs large enough that
// the O(lines(before) * lines(after)) DP table would be a real cost.
QString buildUnifiedDiff(const QString& before, const QString& after);
