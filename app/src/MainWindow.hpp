#pragma once

#include "DiffDialog.hpp"
#include "EditorTab.hpp"
#include "ShaderViewport.hpp"
#include "ShaderfmtLexer.hpp"
#include "StylePanel.hpp"
#include "shaderfmt/Formatter.hpp"
#include "shaderfmt/Token.hpp"

#include <Qsci/qsciscintilla.h>
#include <QCheckBox>
#include <QComboBox>
#include <QList>
#include <QMainWindow>
#include <QMenu>
#include <QString>
#include <QStringList>
#include <QTabWidget>
#include <QTimer>

class QAction;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    // Test/automation hooks (used by app --self-test, see main.cpp): these
    // drive the same code paths a user's mouse/keyboard would, so
    // "does Format actually work" can be verified by a real run rather
    // than assumed (ROADMAP principle #14). All of these act on the
    // *current* tab, mirroring what a user looking at that tab would
    // trigger - use the tab-management hooks below to test multi-tab
    // behavior specifically.
    QsciScintilla* editorForTesting() { return currentTab()->editor(); }
    void triggerFormatForTesting() { onFormatClicked(); }
    void setLanguageForTesting(shaderfmt::Language lang);
    shaderfmt::Language currentLanguageForTesting() const;
    void setFormatOnFlyForTesting(bool enabled);
    void loadFileForTesting(const QString& path) { loadFile(path); }
    void saveToFile(const QString& path); // also used by the real "Enregistrer sous..." action
    int indicatorValueForTesting(int line, int index) const;
    StylePanel* stylePanelForTesting() { return stylePanel_; }
    bool isFormattingForTesting() const { return currentTab()->formatWatcher()->isRunning(); }
    QString versionStringForTesting() const;
    int glslVersionWarningIndicatorCountForTesting() const;
    ViewportPanel* viewportPanelForTesting() { return viewportPanel_; }
    bool isDarkThemeForTesting() const;

    // Tab-management hooks (roadmap "UI pro" phase 1: onglets multi-
    // fichiers), exercising the same actions the "Nouveau"/"Fermer
    // l'onglet" menu entries and the tab bar's own UI (close button, tab
    // click) would.
    int tabCountForTesting() const { return tabs_->count(); }
    int currentTabIndexForTesting() const { return tabs_->currentIndex(); }
    QString tabTitleForTesting(int index) const { return tabs_->tabText(index); }
    void newTabForTesting() { newTab(); }
    void switchToTabForTesting(int index) { tabs_->setCurrentIndex(index); }
    void closeTabForTesting(int index) { closeTab(index); }

    // Cancels any pending debounced update before firing an immediate one -
    // otherwise a stale timer restarted by an *earlier*, unrelated
    // setText() call (every text change restarts it, see
    // onEditorTextChanged()) can fire mid-test and race with a test's own
    // explicit trigger, since self-test drives Qt via repeated
    // processEvents() calls rather than a real continuous event loop.
    void triggerViewportUpdateForTesting() {
        viewportUpdateTimer_->stop();
        onViewportUpdateTimeout();
    }

    // Phase 2 hooks (fichiers recents, palette de commandes, diff).
    QStringList recentFilesForTesting() const { return recentFiles_; }
    QList<QAction*> paletteActionsForTesting() const { return collectPaletteActions(); }
    bool hasLastFormatDiffForTesting() const { return currentTab()->hasLastFormatDiff; }
    QString lastFormatDiffForTesting() const {
        return buildUnifiedDiff(currentTab()->lastFormatBefore, currentTab()->lastFormatAfter);
    }

signals:
    // Emitted once an async format request (and any coalesced re-request
    // made while it was running) has fully settled - the signal a test
    // waits on instead of assuming triggerFormatForTesting() completed
    // synchronously (roadmap §7: formatting runs off the UI thread now).
    void formatSettledForTesting();

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onFormatClicked();
    void onCopyClicked();
    void onOpenFile();
    void onSaveAs();
    void onNewTabAction();
    void onLanguageComboChanged(int index);
    void onFormatOnFlyToggled(bool checked);
    void onStyleOptionsChanged(shaderfmt::FormatOptions options);
    void onMaxLineLengthChanged(int columns);
    void onAboutClicked();
    void onViewportUpdateTimeout();
    void onThemeToggled(bool dark);
    void onCurrentTabChanged(int index);
    void onTabCloseRequested(int index);
    void onOpenCommandPalette();
    void onShowDiffClicked();
    void onOpenRecentFile();
    void onClearRecentFiles();

private:
    EditorTab* currentTab() const { return qobject_cast<EditorTab*>(tabs_->currentWidget()); }
    EditorTab* newTab(const QString& initialText = QString());
    void closeTab(int index);
    void updateTabTitle(EditorTab* tab);

    void loadFile(const QString& path);
    void applyFormat();
    void applyFormatFor(EditorTab* tab);
    void onFormatFinishedFor(EditorTab* tab);
    void onEditorTextChangedFor(EditorTab* tab);
    void onDiagnosticsChangedFor(EditorTab* tab);
    void redetectLanguageFor(EditorTab* tab);
    void applyIndicatorsFor(EditorTab* tab);
    void applyEditorTheme(EditorTab* tab);
    void applyStyleSettingsTo(EditorTab* tab);
    void saveWindowLayout();
    void restoreWindowLayout();

    QList<QAction*> collectPaletteActions() const;
    void addRecentFile(const QString& path);
    void rebuildRecentFilesMenu();
    void loadRecentFiles();
    void saveRecentFiles() const;

    QTabWidget* tabs_ = nullptr;
    int untitledCounter_ = 0;

    QComboBox* languageCombo_ = nullptr;
    QCheckBox* formatOnFlyCheck_ = nullptr;
    QTimer* formatOnFlyTimer_ = nullptr;
    StylePanel* stylePanel_ = nullptr;
    ViewportPanel* viewportPanel_ = nullptr;
    QTimer* viewportUpdateTimer_ = nullptr;
    QMenu* recentFilesMenu_ = nullptr;

    shaderfmt::FormatOptions formatOptions_;
    bool isDarkTheme_ = true;
    QStringList recentFiles_;
    static constexpr int kMaxRecentFiles = 10;
};
