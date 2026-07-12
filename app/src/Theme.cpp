#include "Theme.hpp"

#include <QApplication>
#include <QPainter>
#include <QPixmap>

namespace shaderfmt_app {

namespace {

// Dark palette modeled on the "editor-first" tools users already know
// (VS Code Dark+ / JetBrains Darcula tonal range), so ShaderFmt reads as
// a professional dev tool rather than a stock Qt Widgets app. Values are
// hex literals rather than QPalette roles because QSS gives per-widget-
// class control (dock title bars, the editor's own margin, disabled
// states) that QPalette alone can't express.
constexpr const char* kDarkSheet = R"(
QWidget {
    background-color: #1e1e1e;
    color: #d4d4d4;
    selection-background-color: #264f78;
    selection-color: #ffffff;
}

QMainWindow, QDialog { background-color: #1e1e1e; }

QMenuBar { background-color: #252526; border-bottom: 1px solid #3c3c3c; }
QMenuBar::item { background: transparent; padding: 4px 10px; }
QMenuBar::item:selected { background: #3c3c3c; }
QMenu { background-color: #252526; border: 1px solid #3c3c3c; }
QMenu::item { padding: 4px 24px; }
QMenu::item:selected { background-color: #094771; }

QToolBar {
    background-color: #252526;
    border-bottom: 1px solid #3c3c3c;
    spacing: 6px;
    padding: 3px;
}
QToolButton {
    background: transparent;
    border: 1px solid transparent;
    border-radius: 4px;
    padding: 4px 8px;
    color: #d4d4d4;
}
QToolButton:hover { background-color: #3c3c3c; border-color: #4c4c4c; }
QToolButton:pressed { background-color: #094771; }

QStatusBar { background-color: #007acc; color: #ffffff; }
QStatusBar::item { border: none; }

QDockWidget {
    color: #d4d4d4;
    titlebar-close-icon: none;
    titlebar-normal-icon: none;
}
QDockWidget::title {
    background-color: #2d2d2d;
    padding: 5px 6px;
    border-bottom: 1px solid #3c3c3c;
}

QComboBox, QSpinBox, QLineEdit, QPlainTextEdit {
    background-color: #3c3c3c;
    border: 1px solid #4c4c4c;
    border-radius: 3px;
    padding: 3px 6px;
    color: #d4d4d4;
}
QComboBox:hover, QSpinBox:hover, QLineEdit:hover { border-color: #007acc; }
QComboBox QAbstractItemView {
    background-color: #3c3c3c;
    border: 1px solid #4c4c4c;
    selection-background-color: #094771;
}

QCheckBox { spacing: 6px; }
QCheckBox::indicator {
    width: 14px;
    height: 14px;
    border: 1px solid #6c6c6c;
    border-radius: 2px;
    background: #3c3c3c;
}
QCheckBox::indicator:checked { background-color: #007acc; border-color: #007acc; }

QPushButton {
    background-color: #3c3c3c;
    border: 1px solid #4c4c4c;
    border-radius: 3px;
    padding: 5px 12px;
}
QPushButton:hover { background-color: #4c4c4c; border-color: #007acc; }
QPushButton:pressed { background-color: #094771; }

QScrollBar:vertical, QScrollBar:horizontal {
    background: #1e1e1e;
    width: 12px;
    height: 12px;
    margin: 0;
}
QScrollBar::handle { background: #4c4c4c; border-radius: 5px; min-height: 20px; min-width: 20px; }
QScrollBar::handle:hover { background: #6c6c6c; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }

QLabel { background: transparent; }
QGroupBox {
    border: 1px solid #3c3c3c;
    border-radius: 4px;
    margin-top: 8px;
    padding-top: 6px;
}
QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }
)";

// Light counterpart of the same class list above, kept structurally
// parallel (same selectors, same order) so the two are easy to diff
// against each other when one gets a new rule.
constexpr const char* kLightSheet = R"(
QWidget {
    background-color: #ffffff;
    color: #1e1e1e;
    selection-background-color: #add6ff;
    selection-color: #1e1e1e;
}

QMainWindow, QDialog { background-color: #ffffff; }

QMenuBar { background-color: #f3f3f3; border-bottom: 1px solid #d0d0d0; }
QMenuBar::item { background: transparent; padding: 4px 10px; }
QMenuBar::item:selected { background: #d0d0d0; }
QMenu { background-color: #ffffff; border: 1px solid #d0d0d0; }
QMenu::item { padding: 4px 24px; }
QMenu::item:selected { background-color: #add6ff; }

QToolBar {
    background-color: #f3f3f3;
    border-bottom: 1px solid #d0d0d0;
    spacing: 6px;
    padding: 3px;
}
QToolButton {
    background: transparent;
    border: 1px solid transparent;
    border-radius: 4px;
    padding: 4px 8px;
    color: #1e1e1e;
}
QToolButton:hover { background-color: #e0e0e0; border-color: #c0c0c0; }
QToolButton:pressed { background-color: #add6ff; }

QStatusBar { background-color: #005fb8; color: #ffffff; }
QStatusBar::item { border: none; }

QDockWidget {
    color: #1e1e1e;
    titlebar-close-icon: none;
    titlebar-normal-icon: none;
}
QDockWidget::title {
    background-color: #ececec;
    padding: 5px 6px;
    border-bottom: 1px solid #d0d0d0;
}

QComboBox, QSpinBox, QLineEdit, QPlainTextEdit {
    background-color: #ffffff;
    border: 1px solid #c0c0c0;
    border-radius: 3px;
    padding: 3px 6px;
    color: #1e1e1e;
}
QComboBox:hover, QSpinBox:hover, QLineEdit:hover { border-color: #005fb8; }
QComboBox QAbstractItemView {
    background-color: #ffffff;
    border: 1px solid #c0c0c0;
    selection-background-color: #add6ff;
}

QCheckBox { spacing: 6px; }
QCheckBox::indicator {
    width: 14px;
    height: 14px;
    border: 1px solid #a0a0a0;
    border-radius: 2px;
    background: #ffffff;
}
QCheckBox::indicator:checked { background-color: #005fb8; border-color: #005fb8; }

QPushButton {
    background-color: #f3f3f3;
    border: 1px solid #c0c0c0;
    border-radius: 3px;
    padding: 5px 12px;
}
QPushButton:hover { background-color: #e0e0e0; border-color: #005fb8; }
QPushButton:pressed { background-color: #add6ff; }

QScrollBar:vertical, QScrollBar:horizontal {
    background: #ffffff;
    width: 12px;
    height: 12px;
    margin: 0;
}
QScrollBar::handle { background: #c0c0c0; border-radius: 5px; min-height: 20px; min-width: 20px; }
QScrollBar::handle:hover { background: #a0a0a0; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }

QLabel { background: transparent; }
QGroupBox {
    border: 1px solid #d0d0d0;
    border-radius: 4px;
    margin-top: 8px;
    padding-top: 6px;
}
QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }
)";

} // namespace

QString styleSheetFor(ThemeMode mode) {
    return QString::fromUtf8(mode == ThemeMode::Dark ? kDarkSheet : kLightSheet);
}

void applyTheme(QApplication& app, ThemeMode mode) {
    app.setStyleSheet(styleSheetFor(mode));
}

QIcon formatActionIcon(bool darkTheme) {
    constexpr int kSize = 24;
    QPixmap pixmap(kSize, kSize);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QColor fg = darkTheme ? QColor(0xd4, 0xd4, 0xd4) : QColor(0x1e, 0x1e, 0x1e);
    QPen pen(fg, 1.6);
    painter.setPen(pen);

    // Three left-aligned horizontal bars of decreasing width - a stylized
    // "reformat/align text" glyph, distinct from a generic refresh arrow.
    painter.drawLine(5, 7, 19, 7);
    painter.drawLine(5, 12, 15, 12);
    painter.drawLine(5, 17, 19, 17);
    return QIcon(pixmap);
}

QIcon copyActionIcon(bool darkTheme) {
    constexpr int kSize = 24;
    QPixmap pixmap(kSize, kSize);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QColor fg = darkTheme ? QColor(0xd4, 0xd4, 0xd4) : QColor(0x1e, 0x1e, 0x1e);
    QPen pen(fg, 1.4);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    // Two overlapping rounded rectangles - the conventional "copy" glyph.
    painter.drawRoundedRect(4, 4, 12, 14, 2, 2);
    painter.drawRoundedRect(8, 8, 12, 14, 2, 2);
    return QIcon(pixmap);
}

} // namespace shaderfmt_app
