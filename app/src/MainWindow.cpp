#include "MainWindow.hpp"

#include "CommandPalette.hpp"
#include "Theme.hpp"
#include "Version.hpp"
#include "shaderfmt/GlslVersionLint.hpp"
#include "shaderfmt/LanguageDetector.hpp"

#include <algorithm>
#include <string>

#include <Qsci/qsciscintilla.h>
#include <Qsci/qsciscintillabase.h>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QStyle>
#include <QTextStream>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrent/QtConcurrentRun>

namespace {

// Keep this in sync with the QComboBox items populated in MainWindow's
// constructor: index 0 is "Auto", the rest map 1:1 to shaderfmt::Language.
shaderfmt::Language languageForComboIndex(int index) {
    switch (index) {
        case 1: return shaderfmt::Language::GLSL;
        case 2: return shaderfmt::Language::Shadertoy;
        case 3: return shaderfmt::Language::HLSL;
        case 4: return shaderfmt::Language::WGSL;
        case 5: return shaderfmt::Language::MSL;
        case 6: return shaderfmt::Language::ShaderLab;
        default: return shaderfmt::Language::GLSL;
    }
}

int comboIndexForLanguage(shaderfmt::Language lang) {
    switch (lang) {
        case shaderfmt::Language::GLSL: return 1;
        case shaderfmt::Language::Shadertoy: return 2;
        case shaderfmt::Language::HLSL: return 3;
        case shaderfmt::Language::WGSL: return 4;
        case shaderfmt::Language::MSL: return 5;
        case shaderfmt::Language::ShaderLab: return 6;
    }
    return 1;
}

const char* languageLabel(shaderfmt::Language lang) {
    switch (lang) {
        case shaderfmt::Language::GLSL: return "GLSL";
        case shaderfmt::Language::Shadertoy: return "Shadertoy";
        case shaderfmt::Language::HLSL: return "HLSL";
        case shaderfmt::Language::WGSL: return "WGSL";
        case shaderfmt::Language::MSL: return "MSL";
        case shaderfmt::Language::ShaderLab: return "ShaderLab";
    }
    return "GLSL";
}

// Maps a flat byte offset in `before` to the "closest equivalent" offset
// in `after`, exploiting the fact that shaderfmt::format() only ever
// inserts/removes/reflows *whitespace* between otherwise-unmodified,
// unreordered tokens (the engine's own "zero semantic modification"
// invariant - see lib/include/shaderfmt/Formatter.hpp). Counting how many
// non-whitespace characters precede the cursor in `before`, then walking
// `after` until that many non-whitespace characters have again been seen,
// lands right next to the same piece of real code even though blank-line
// collapsing/reindentation may have completely changed which (line,
// column) that corresponds to.
int mapCursorPositionAfterFormat(const std::string& before, int flatPosBefore, const std::string& after) {
    auto isWhitespace = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };

    size_t clampedBefore = std::min<size_t>(static_cast<size_t>(std::max(0, flatPosBefore)), before.size());
    size_t meaningfulCharsBeforeCursor = 0;
    for (size_t i = 0; i < clampedBefore; i++) {
        if (!isWhitespace(static_cast<unsigned char>(before[i]))) meaningfulCharsBeforeCursor++;
    }

    size_t seen = 0;
    size_t i = 0;
    for (; i < after.size() && seen < meaningfulCharsBeforeCursor; i++) {
        if (!isWhitespace(static_cast<unsigned char>(after[i]))) seen++;
    }
    return static_cast<int>(i);
}

QSettings makeAppSettings() {
    return QSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("ShaderFmt"),
                      QStringLiteral("ShaderFmt"));
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("ShaderFmt %1").arg(versionStringForTesting()));
    setAcceptDrops(true);
    resize(1000, 700); // default size; restoreWindowLayout() below overrides it if a prior session saved one

    // Theme (roadmap "UI pro" phase 0): persisted independently of the
    // style panel's own QSettings usage, read once here so the toolbar
    // icons below are drawn in the right color from the very first frame.
    {
        QSettings settings = makeAppSettings();
        isDarkTheme_ = settings.value(QStringLiteral("theme/dark"), true).toBool();
    }

    // --- Tabs (roadmap "UI pro" phase 1): each open file is an
    // independent EditorTab (own editor, lexer, async-format state) so
    // formatting/editing one never blocks or interferes with another.
    tabs_ = new QTabWidget(this);
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->setDocumentMode(true);
    connect(tabs_, &QTabWidget::currentChanged, this, &MainWindow::onCurrentTabChanged);
    connect(tabs_, &QTabWidget::tabCloseRequested, this, &MainWindow::onTabCloseRequested);
    setCentralWidget(tabs_);

    // --- Toolbar: language selector, format action, always-visible copy
    // button, format-on-the-fly toggle.
    QToolBar* toolbar = addToolBar(QStringLiteral("Main"));
    toolbar->setMovable(false);

    languageCombo_ = new QComboBox(toolbar);
    languageCombo_->addItem(QStringLiteral("Auto"));
    languageCombo_->addItem(QStringLiteral("GLSL"));
    languageCombo_->addItem(QStringLiteral("Shadertoy"));
    languageCombo_->addItem(QStringLiteral("HLSL"));
    languageCombo_->addItem(QStringLiteral("WGSL"));
    languageCombo_->addItem(QStringLiteral("MSL"));
    languageCombo_->addItem(QStringLiteral("ShaderLab"));
    toolbar->addWidget(languageCombo_);
    connect(languageCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::onLanguageComboChanged);

    QAction* formatAction =
        toolbar->addAction(shaderfmt_app::formatActionIcon(isDarkTheme_), QStringLiteral("Formater"));
    formatAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")));
    connect(formatAction, &QAction::triggered, this, &MainWindow::onFormatClicked);

    QAction* copyAction =
        toolbar->addAction(shaderfmt_app::copyActionIcon(isDarkTheme_), QStringLiteral("Copier"));
    connect(copyAction, &QAction::triggered, this, &MainWindow::onCopyClicked);

    formatOnFlyCheck_ = new QCheckBox(QStringLiteral("Formatage a la volee"), toolbar);
    toolbar->addWidget(formatOnFlyCheck_);
    connect(formatOnFlyCheck_, &QCheckBox::toggled, this, &MainWindow::onFormatOnFlyToggled);

    formatOnFlyTimer_ = new QTimer(this);
    formatOnFlyTimer_->setSingleShot(true);
    formatOnFlyTimer_->setInterval(500);
    connect(formatOnFlyTimer_, &QTimer::timeout, this, &MainWindow::onFormatClicked);

    // --- Menu: file (new/open/save/close tab).
    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("Fichier"));
    QAction* newAction = fileMenu->addAction(QStringLiteral("Nouveau"));
    newAction->setShortcut(QKeySequence::New);
    connect(newAction, &QAction::triggered, this, &MainWindow::onNewTabAction);

    QAction* openAction = fileMenu->addAction(
        style()->standardIcon(QStyle::SP_DialogOpenButton), QStringLiteral("Ouvrir..."));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::onOpenFile);

    QAction* saveAction = fileMenu->addAction(
        style()->standardIcon(QStyle::SP_DialogSaveButton), QStringLiteral("Enregistrer sous..."));
    saveAction->setShortcut(QKeySequence::SaveAs);
    connect(saveAction, &QAction::triggered, this, &MainWindow::onSaveAs);

    QAction* closeTabAction = fileMenu->addAction(QStringLiteral("Fermer l'onglet"));
    closeTabAction->setShortcut(QKeySequence::Close);
    connect(closeTabAction, &QAction::triggered, this, [this]() { onTabCloseRequested(tabs_->currentIndex()); });

    // --- Menu: theme toggle (roadmap "UI pro" phase 0).
    QMenu* viewMenu = menuBar()->addMenu(QStringLiteral("Affichage"));
    QAction* themeAction = viewMenu->addAction(QStringLiteral("Theme sombre"));
    themeAction->setCheckable(true);
    themeAction->setChecked(isDarkTheme_);
    connect(themeAction, &QAction::toggled, this, &MainWindow::onThemeToggled);

    // --- Menu: "A propos".
    QMenu* helpMenu = menuBar()->addMenu(QStringLiteral("Aide"));
    QAction* aboutAction = helpMenu->addAction(QStringLiteral("A propos de ShaderFmt"));
    connect(aboutAction, &QAction::triggered, this, &MainWindow::onAboutClicked);

    // --- Style panel (roadmap §4): side dock, options apply immediately
    // to the current tab, preferences persist between sessions on their
    // own (StylePanel owns its QSettings I/O).
    stylePanel_ = new StylePanel(this);
    QDockWidget* styleDock = new QDockWidget(QStringLiteral("Style"), this);
    styleDock->setObjectName(QStringLiteral("styleDock"));
    styleDock->setWidget(stylePanel_);
    styleDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::RightDockWidgetArea, styleDock);
    connect(stylePanel_, &StylePanel::optionsChanged, this, &MainWindow::onStyleOptionsChanged);
    connect(stylePanel_, &StylePanel::maxLineLengthChanged, this, &MainWindow::onMaxLineLengthChanged);

    formatOptions_ = stylePanel_->options();

    // --- Viewport: live GLSL/Shadertoy render preview of the current tab,
    // docked at the bottom so it doesn't compete with the style panel.
    viewportPanel_ = new ViewportPanel(this);
    QDockWidget* viewportDock = new QDockWidget(QStringLiteral("Apercu rendu"), this);
    viewportDock->setObjectName(QStringLiteral("viewportDock"));
    viewportDock->setWidget(viewportPanel_);
    viewportDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::BottomDockWidgetArea, viewportDock);

    viewportUpdateTimer_ = new QTimer(this);
    viewportUpdateTimer_->setSingleShot(true);
    viewportUpdateTimer_->setInterval(400);
    connect(viewportUpdateTimer_, &QTimer::timeout, this, &MainWindow::onViewportUpdateTimeout);

    newTab(); // first tab, now that formatOptions_/stylePanel_ exist for it to read

    languageCombo_->setCurrentIndex(0); // Auto
    restoreWindowLayout();
    statusBar()->showMessage(QStringLiteral("Pret."));
}

EditorTab* MainWindow::newTab(const QString& initialText) {
    auto* tab = new EditorTab(tabs_);
    applyStyleSettingsTo(tab);
    applyEditorTheme(tab);

    connect(tab->editor(), &QsciScintilla::textChanged, this, [this, tab]() { onEditorTextChangedFor(tab); });
    connect(tab->lexer(), &ShaderfmtLexer::diagnosticsChanged, this, [this, tab]() { onDiagnosticsChangedFor(tab); });
    connect(tab->formatWatcher(), &QFutureWatcher<shaderfmt::FormatResult>::finished, this,
            [this, tab]() { onFormatFinishedFor(tab); });
    connect(tab, &EditorTab::dirtyChanged, this, [this, tab](bool) { updateTabTitle(tab); });

    untitledCounter_++;
    tab->displayName = QStringLiteral("Sans titre %1").arg(untitledCounter_);
    int index = tabs_->addTab(tab, tab->displayName);
    tabs_->setCurrentIndex(index);

    if (!initialText.isEmpty()) {
        tab->applyingProgrammaticChange = true;
        tab->editor()->setText(initialText);
        tab->applyingProgrammaticChange = false;
        tab->editor()->setModified(false);
    }
    return tab;
}

void MainWindow::closeTab(int index) {
    if (index < 0 || index >= tabs_->count()) return;
    QWidget* w = tabs_->widget(index);
    tabs_->removeTab(index);
    w->deleteLater();
    if (tabs_->count() == 0) {
        newTab(); // always keep at least one tab open
    }
}

void MainWindow::onTabCloseRequested(int index) {
    if (index < 0 || index >= tabs_->count()) return;
    auto* tab = qobject_cast<EditorTab*>(tabs_->widget(index));
    if (tab && tab->isDirty()) {
        auto reply = QMessageBox::question(this, QStringLiteral("Fermer l'onglet"),
            QStringLiteral("\"%1\" contient des modifications non enregistrees. Fermer quand meme ?")
                .arg(tab->displayName),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (reply != QMessageBox::Yes) return;
    }
    closeTab(index);
}

void MainWindow::updateTabTitle(EditorTab* tab) {
    int index = tabs_->indexOf(tab);
    if (index < 0) return;
    QString title = tab->isDirty() ? QStringLiteral("● ") + tab->displayName : tab->displayName;
    tabs_->setTabText(index, title);
}

void MainWindow::applyStyleSettingsTo(EditorTab* tab) {
    tab->editor()->setIndentationsUseTabs(formatOptions_.useTabs);
    tab->editor()->setTabWidth(formatOptions_.indentWidth);
    tab->editor()->setIndentationWidth(formatOptions_.useTabs ? 0 : formatOptions_.indentWidth);
    tab->editor()->setEdgeMode(QsciScintilla::EdgeLine);
    tab->editor()->setEdgeColumn(stylePanel_->maxLineLength());
}

void MainWindow::onStyleOptionsChanged(shaderfmt::FormatOptions options) {
    formatOptions_ = options;
    for (int i = 0; i < tabs_->count(); i++) {
        applyStyleSettingsTo(qobject_cast<EditorTab*>(tabs_->widget(i)));
    }
    // "Application immediate des changements de style sur le texte deja
    // present dans l'editeur": re-run Format right away on the current
    // tab rather than waiting for the next click/keystroke.
    applyFormat();
}

void MainWindow::onMaxLineLengthChanged(int columns) {
    Q_UNUSED(columns);
    // Line length is a visual guide, not enforced wrapping.
    for (int i = 0; i < tabs_->count(); i++) {
        applyStyleSettingsTo(qobject_cast<EditorTab*>(tabs_->widget(i)));
    }
}

void MainWindow::setLanguageForTesting(shaderfmt::Language lang) {
    languageCombo_->setCurrentIndex(comboIndexForLanguage(lang));
}

shaderfmt::Language MainWindow::currentLanguageForTesting() const {
    return currentTab()->lexer()->shaderLanguage();
}

void MainWindow::setFormatOnFlyForTesting(bool enabled) {
    formatOnFlyCheck_->setChecked(enabled);
}

void MainWindow::onNewTabAction() {
    newTab();
}

void MainWindow::onLanguageComboChanged(int index) {
    EditorTab* tab = currentTab();
    if (!tab) return; // combo population during construction, before the first tab exists

    if (index == 0) {
        tab->setLanguageManuallySet(false);
        redetectLanguageFor(tab);
    } else {
        tab->setLanguageManuallySet(true);
        tab->lexer()->setShaderLanguage(languageForComboIndex(index));
        tab->editor()->recolor();
    }
    onViewportUpdateTimeout();
}

void MainWindow::onViewportUpdateTimeout() {
    EditorTab* tab = currentTab();
    if (!tab) return;
    viewportPanel_->setShader(tab->editor()->text(), tab->lexer()->shaderLanguage());
}

void MainWindow::onCurrentTabChanged(int index) {
    Q_UNUSED(index);
    EditorTab* tab = currentTab();
    if (!tab) return;

    // A pending debounce from the tab just switched away from must not
    // fire against the newly-current one (see onEditorTextChangedFor()).
    formatOnFlyTimer_->stop();
    viewportUpdateTimer_->stop();

    const QSignalBlocker blocker(languageCombo_);
    languageCombo_->setCurrentIndex(tab->languageManuallySet() ? comboIndexForLanguage(tab->lexer()->shaderLanguage())
                                                                : 0);

    applyIndicatorsFor(tab);
    onViewportUpdateTimeout();
}

void MainWindow::onEditorTextChangedFor(EditorTab* tab) {
    if (tab->applyingProgrammaticChange) return;
    if (tab != currentTab()) return; // background tabs never drive toolbar/debounce state

    if (!tab->languageManuallySet()) {
        redetectLanguageFor(tab);
    }

    // Re-run styleText()/lex() on every edit, not just on a language
    // switch: QScintilla's own SCN_STYLENEEDED notification is tied to
    // repainting, which isn't reliable enough to depend on here, and the
    // error indicator must reflect the *current* text.
    tab->editor()->recolor();

    if (formatOnFlyCheck_->isChecked()) {
        // Debounced: restart the timer on every keystroke so formatting
        // only runs once typing pauses, never mid-keystroke.
        formatOnFlyTimer_->start();
    }

    viewportUpdateTimer_->start();
}

void MainWindow::redetectLanguageFor(EditorTab* tab) {
    std::string source = tab->editor()->text().toUtf8().toStdString();
    shaderfmt::Language detected = shaderfmt::detectLanguage(source);
    if (detected != tab->lexer()->shaderLanguage()) {
        tab->lexer()->setShaderLanguage(detected);
        tab->editor()->recolor();
    }
    if (tab == currentTab()) {
        // Reflect detection in the combo without re-triggering
        // onLanguageComboChanged (which would mark the language as
        // manually set and defeat auto-detection on the next paste).
        const QSignalBlocker blocker(languageCombo_);
        languageCombo_->setCurrentIndex(0);
        statusBar()->showMessage(QStringLiteral("Langage detecte : %1").arg(languageLabel(detected)));
    }
}

void MainWindow::onFormatOnFlyToggled(bool checked) {
    if (checked) {
        onFormatClicked(); // apply once immediately so toggling on isn't a no-op
    }
}

// Runs shaderfmt::format() on a QtConcurrent thread-pool thread (roadmap
// §7) rather than blocking the GUI thread. Never touches the editor
// directly here - only onFormatFinishedFor(), on the GUI thread, is
// allowed to do that.
void MainWindow::applyFormat() {
    applyFormatFor(currentTab());
}

void MainWindow::applyFormatFor(EditorTab* tab) {
    if (!tab) return;
    if (tab->formatWatcher()->isRunning()) {
        // A format is already in flight for this tab (e.g. the user hit
        // the button twice, or a style change landed mid-format): don't
        // start a second overlapping computation, just remember to run
        // once more on the latest text once this one settles.
        tab->formatAgainRequested = true;
        return;
    }

    shaderfmt::Language lang = tab->lexer()->shaderLanguage();
    tab->formatRequestedSource = tab->editor()->text().toUtf8().toStdString();
    shaderfmt::FormatOptions opts = formatOptions_;
    std::string src = tab->formatRequestedSource; // captured by value for the worker thread

    QFuture<shaderfmt::FormatResult> future =
        QtConcurrent::run([src, lang, opts]() { return shaderfmt::format(src, lang, opts); });
    tab->formatWatcher()->setFuture(future);
}

void MainWindow::onFormatFinishedFor(EditorTab* tab) {
    shaderfmt::FormatResult result = tab->formatWatcher()->result();
    std::string currentSource = tab->editor()->text().toUtf8().toStdString();

    if (currentSource == tab->formatRequestedSource) {
        // Nothing changed in this tab while formatting ran off-thread:
        // safe to apply. If the user kept typing meanwhile, currentSource
        // won't match and this result is silently discarded instead of
        // clobbering their newer edits.
        if (result.formatted != currentSource) {
            int line = 0, index = 0;
            tab->editor()->getCursorPosition(&line, &index);
            long flatPosBefore = tab->editor()->positionFromLineIndex(line, index);

            tab->applyingProgrammaticChange = true;
            tab->editor()->beginUndoAction();
            tab->editor()->selectAll();
            tab->editor()->replaceSelectedText(QString::fromStdString(result.formatted));
            tab->editor()->endUndoAction();
            tab->applyingProgrammaticChange = false;

            int flatPosAfter = mapCursorPositionAfterFormat(currentSource, static_cast<int>(flatPosBefore),
                                                              result.formatted);
            int newLine = 0, newIndex = 0;
            tab->editor()->lineIndexFromPosition(flatPosAfter, &newLine, &newIndex);
            tab->editor()->setCursorPosition(newLine, newIndex);
        }
        tab->editor()->recolor();
        if (tab == currentTab()) {
            statusBar()->showMessage(
                QStringLiteral("Formate (%1).").arg(languageLabel(tab->lexer()->shaderLanguage())), 2000);
        }
    }

    if (tab->formatAgainRequested) {
        tab->formatAgainRequested = false;
        applyFormatFor(tab);
    } else {
        emit formatSettledForTesting();
    }
}

void MainWindow::onFormatClicked() {
    applyFormat();
}

void MainWindow::onCopyClicked() {
    EditorTab* tab = currentTab();
    if (!tab) return;
    QApplication::clipboard()->setText(tab->editor()->text());
    statusBar()->showMessage(QStringLiteral("Copie dans le presse-papiers."), 2000);
}

void MainWindow::onDiagnosticsChangedFor(EditorTab* tab) {
    applyIndicatorsFor(tab);
}

void MainWindow::applyIndicatorsFor(EditorTab* tab) {
    tab->editor()->clearIndicatorRange(0, 0, tab->editor()->lines(), 0, EditorTab::kErrorIndicator);
    for (const auto& err : tab->lexer()->lastErrors()) {
        int line = std::max(0, err.line - 1);
        int col = std::max(0, err.column - 1);
        tab->editor()->fillIndicatorRange(line, col, line, col + 1, EditorTab::kErrorIndicator);
    }

    tab->editor()->clearIndicatorRange(0, 0, tab->editor()->lines(), 0, EditorTab::kGlslVersionWarningIndicator);
    if (tab->lexer()->shaderLanguage() == shaderfmt::Language::GLSL) {
        for (const auto& w : shaderfmt::lintGlslVersionQualifiers(tab->editor()->text().toStdString())) {
            int line = std::max(0, w.line - 1);
            int col = std::max(0, w.column - 1);
            tab->editor()->fillIndicatorRange(line, col, line, col + w.length, EditorTab::kGlslVersionWarningIndicator);
        }
    }
}

int MainWindow::glslVersionWarningIndicatorCountForTesting() const {
    EditorTab* tab = currentTab();
    if (tab->lexer()->shaderLanguage() != shaderfmt::Language::GLSL) return 0;
    return static_cast<int>(shaderfmt::lintGlslVersionQualifiers(tab->editor()->text().toStdString()).size());
}

void MainWindow::onOpenFile() {
    QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Ouvrir un shader"));
    if (!path.isEmpty()) loadFile(path);
}

void MainWindow::onSaveAs() {
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Enregistrer sous"));
    if (!path.isEmpty()) saveToFile(path);
}

QString MainWindow::versionStringForTesting() const {
    return QString::fromUtf8(shaderfmt_app::kVersion);
}

void MainWindow::onAboutClicked() {
    QMessageBox::about(this, QStringLiteral("A propos de ShaderFmt"),
        QStringLiteral("ShaderFmt %1\n\n"
                        "Editeur et formateur de code shader (GLSL, Shadertoy, HLSL, "
                        "WGSL, MSL, Unity ShaderLab).\n\n"
                        "Copyright (c) 2026 Patrick JAILLET. "
                        "Distribue sous licence GPL v3.")
            .arg(versionStringForTesting()));
}

void MainWindow::saveToFile(const QString& path) {
    EditorTab* tab = currentTab();
    if (!tab) return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("ShaderFmt"), QStringLiteral("Impossible d'ecrire le fichier."));
        return;
    }
    QTextStream out(&file);
    out << tab->editor()->text();

    tab->setFilePath(path);
    tab->displayName = QFileInfo(path).fileName();
    tab->editor()->setModified(false);
    updateTabTitle(tab);
    statusBar()->showMessage(QStringLiteral("Enregistre : %1").arg(path), 2000);
}

int MainWindow::indicatorValueForTesting(int line, int index) const {
    EditorTab* tab = currentTab();
    long pos = tab->editor()->positionFromLineIndex(line, index);
    return static_cast<int>(
        tab->editor()->SendScintilla(QsciScintillaBase::SCI_INDICATORVALUEAT, EditorTab::kErrorIndicator, pos));
}

void MainWindow::loadFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("ShaderFmt"), QStringLiteral("Impossible de lire le fichier."));
        return;
    }
    QTextStream in(&file);
    QString content = in.readAll();

    EditorTab* tab = newTab();
    tab->applyingProgrammaticChange = true;
    tab->editor()->setText(content);
    tab->applyingProgrammaticChange = false;
    tab->editor()->setModified(false);

    tab->setFilePath(path);
    tab->displayName = QFileInfo(path).fileName();
    tab->setLanguageManuallySet(false);
    redetectLanguageFor(tab);
    updateTabTitle(tab);

    statusBar()->showMessage(QStringLiteral("Charge : %1").arg(path), 2000);
}

bool MainWindow::isDarkThemeForTesting() const {
    return isDarkTheme_;
}

void MainWindow::onThemeToggled(bool dark) {
    isDarkTheme_ = dark;
    QSettings settings = makeAppSettings();
    settings.setValue(QStringLiteral("theme/dark"), dark);

    auto* app = qobject_cast<QApplication*>(QApplication::instance());
    if (app) {
        shaderfmt_app::applyTheme(*app, dark ? shaderfmt_app::ThemeMode::Dark : shaderfmt_app::ThemeMode::Light);
    }
    for (int i = 0; i < tabs_->count(); i++) {
        applyEditorTheme(qobject_cast<EditorTab*>(tabs_->widget(i)));
    }
    statusBar()->showMessage(
        dark ? QStringLiteral("Theme sombre active.") : QStringLiteral("Theme clair active."), 2000);
}

// Colors everything QScintilla paints itself and the app-wide QSS
// stylesheet therefore can't reach: the lexer's per-style paper, the
// line-number/fold margins, the caret, and the selection.
void MainWindow::applyEditorTheme(EditorTab* tab) {
    tab->lexer()->applyTheme(isDarkTheme_);

    QColor paper = isDarkTheme_ ? QColor(0x1e, 0x1e, 0x1e) : QColor(0xff, 0xff, 0xff);
    QColor marginPaper = isDarkTheme_ ? QColor(0x25, 0x25, 0x26) : QColor(0xf3, 0xf3, 0xf3);
    QColor marginText = isDarkTheme_ ? QColor(0x85, 0x85, 0x85) : QColor(0x80, 0x80, 0x80);
    QColor caret = isDarkTheme_ ? QColor(0xd4, 0xd4, 0xd4) : QColor(0x1e, 0x1e, 0x1e);
    QColor selection = isDarkTheme_ ? QColor(0x26, 0x4f, 0x78) : QColor(0xad, 0xd6, 0xff);

    tab->editor()->setPaper(paper);
    tab->editor()->setMarginsBackgroundColor(marginPaper);
    tab->editor()->setMarginsForegroundColor(marginText);
    tab->editor()->setCaretForegroundColor(caret);
    tab->editor()->setSelectionBackgroundColor(selection);
    tab->editor()->setFoldMarginColors(marginPaper, marginPaper);
}

void MainWindow::saveWindowLayout() {
    QSettings settings = makeAppSettings();
    settings.beginGroup(QStringLiteral("window"));
    settings.setValue(QStringLiteral("geometry"), saveGeometry());
    settings.setValue(QStringLiteral("state"), saveState());
    settings.endGroup();
}

void MainWindow::restoreWindowLayout() {
    QSettings settings = makeAppSettings();
    settings.beginGroup(QStringLiteral("window"));
    QByteArray geometry = settings.value(QStringLiteral("geometry")).toByteArray();
    QByteArray state = settings.value(QStringLiteral("state")).toByteArray();
    settings.endGroup();

    if (!geometry.isEmpty()) restoreGeometry(geometry);
    if (!state.isEmpty()) restoreState(state);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveWindowLayout();
    QMainWindow::closeEvent(event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event) {
    const QList<QUrl> urls = event->mimeData()->urls();
    bool loadedAny = false;
    for (const QUrl& url : urls) {
        if (url.isLocalFile()) {
            loadFile(url.toLocalFile());
            loadedAny = true;
        }
    }
    if (loadedAny) event->acceptProposedAction();
}
