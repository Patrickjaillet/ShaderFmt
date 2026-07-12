#include "DiffDialog.hpp"

#include <QFont>
#include <QPlainTextEdit>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

namespace {

// Colors "+ " / "- " prefixed lines green/red; unchanged ("  "-prefixed)
// lines are left in the view's default color.
class DiffHighlighter : public QSyntaxHighlighter {
public:
    explicit DiffHighlighter(QTextDocument* doc) : QSyntaxHighlighter(doc) {}

protected:
    void highlightBlock(const QString& text) override {
        if (text.startsWith(QStringLiteral("+"))) {
            QTextCharFormat fmt;
            fmt.setForeground(QColor(0x6a, 0xcf, 0x6a));
            setFormat(0, text.size(), fmt);
        } else if (text.startsWith(QStringLiteral("-"))) {
            QTextCharFormat fmt;
            fmt.setForeground(QColor(0xe0, 0x6c, 0x6c));
            setFormat(0, text.size(), fmt);
        }
    }
};

} // namespace

QString buildUnifiedDiff(const QString& before, const QString& after) {
    QStringList a = before.split(QLatin1Char('\n'));
    QStringList b = after.split(QLatin1Char('\n'));
    size_t n = static_cast<size_t>(a.size());
    size_t m = static_cast<size_t>(b.size());

    // A full LCS table is O(n*m) cells; cap it so a pathologically large
    // pair of files can't turn "show me the diff" into a multi-second
    // freeze (see the 3000-function formatting benchmark in
    // tests/test_main.cpp for the scale this app is expected to handle
    // instantly - this cap sits well above that).
    if (n * m > 4000000) {
        return QStringLiteral("(fichier trop volumineux pour afficher un diff detaille)");
    }

    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
    for (int i = static_cast<int>(n) - 1; i >= 0; i--) {
        for (int j = static_cast<int>(m) - 1; j >= 0; j--) {
            dp[i][j] = (a[i] == b[j]) ? dp[i + 1][j + 1] + 1 : std::max(dp[i + 1][j], dp[i][j + 1]);
        }
    }

    QStringList out;
    int i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == b[j]) {
            out << QStringLiteral("  ") + a[i];
            i++;
            j++;
        } else if (dp[i + 1][j] >= dp[i][j + 1]) {
            out << QStringLiteral("- ") + a[i];
            i++;
        } else {
            out << QStringLiteral("+ ") + b[j];
            j++;
        }
    }
    while (i < a.size()) {
        out << QStringLiteral("- ") + a[i];
        i++;
    }
    while (j < b.size()) {
        out << QStringLiteral("+ ") + b[j];
        j++;
    }

    return out.join(QLatin1Char('\n'));
}

DiffDialog::DiffDialog(const QString& unifiedDiffText, QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Differences du dernier formatage"));
    resize(700, 500);

    auto* view = new QPlainTextEdit(this);
    view->setReadOnly(true);
    view->setFont(QFont(QStringLiteral("Consolas"), 10));
    view->setPlainText(unifiedDiffText);
    new DiffHighlighter(view->document());

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(view);
}
