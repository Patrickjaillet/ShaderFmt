#pragma once

#include <QDialog>
#include <QList>

class QAction;
class QLineEdit;
class QListWidget;

// Ctrl+Shift+P command palette (roadmap "UI pro" phase 2): a single
// searchable list of every enabled action in the app's menus, so a user
// who knows *what* they want ("theme sombre", "enregistrer") doesn't need
// to know *where* it lives in the menu bar to trigger it.
class CommandPalette : public QDialog {
    Q_OBJECT
public:
    // `actions` should already be filtered to the ones worth exposing
    // (see MainWindow::collectPaletteActions()) - this class doesn't
    // second-guess which actions it's handed, only how to search/trigger
    // them.
    explicit CommandPalette(const QList<QAction*>& actions, QWidget* parent = nullptr);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onFilterChanged(const QString& text);
    void onActivateCurrentRow();

private:
    void rebuildList(const QString& filter);

    QList<QAction*> actions_;
    QLineEdit* filterEdit_ = nullptr;
    QListWidget* listWidget_ = nullptr;
};
