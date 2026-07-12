#pragma once

#include <QIcon>
#include <QString>

class QApplication;

namespace shaderfmt_app {

enum class ThemeMode { Dark, Light };

// Whole-app QSS stylesheet for the given mode. Exposed separately from
// applyTheme() so tests can inspect the stylesheet text without needing a
// live QApplication to apply it to.
QString styleSheetFor(ThemeMode mode);

// Applies the stylesheet to the whole application (every widget, not just
// MainWindow), so dialogs (QFileDialog, QMessageBox, the About box) match
// too instead of standing out as unstyled native widgets.
void applyTheme(QApplication& app, ThemeMode mode);

// Small painter-drawn (not a bundled asset) icons for actions Qt's own
// QStyle::standardIcon() set has no good match for. Drawn against a
// transparent background at a fixed logical size so QToolBar scales them
// consistently with the platform style's real standard icons alongside
// them on the same toolbar.
QIcon formatActionIcon(bool darkTheme);
QIcon copyActionIcon(bool darkTheme);

} // namespace shaderfmt_app
