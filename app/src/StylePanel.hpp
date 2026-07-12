#pragma once

#include "shaderfmt/Formatter.hpp"

#include <QWidget>

class QComboBox;
class QSpinBox;
class QCheckBox;
class QPlainTextEdit;

// Side panel (ROADMAP.md §4 "Panneau de style"): style options applied
// immediately, one-click presets, a live before/after preview on a fixed
// sample snippet, and preferences auto-persisted between sessions via
// QSettings (an .ini file this widget alone writes - never meant to be
// hand-edited, per the roadmap's "jamais édité à la main").
class StylePanel : public QWidget {
    Q_OBJECT
public:
    explicit StylePanel(QWidget* parent = nullptr);

    shaderfmt::FormatOptions options() const { return options_; }
    int maxLineLength() const { return maxLineLength_; }

    // Test/automation hooks, mirroring MainWindow's *ForTesting() pattern.
    void selectPresetForTesting(int index);
    void setIndentWidthForTesting(int value);
    void setUseTabsForTesting(bool checked);
    void setBraceStyleForTesting(shaderfmt::BraceStyle style);
    void setSpaceAfterControlKeywordForTesting(bool checked);
    QString previewTextForTesting() const;

signals:
    void optionsChanged(shaderfmt::FormatOptions options);
    void maxLineLengthChanged(int columns);

private slots:
    void onIndentWidthChanged(int value);
    void onUseTabsToggled(bool checked);
    void onBraceStyleChanged(int index);
    void onSpaceAfterControlKeywordToggled(bool checked);
    void onMaxLineLengthChanged(int value);
    void onPresetSelected(int index);

private:
    void applyOptionsToControls();
    void refreshPreview();
    void emitOptionsChangedAndPersist();
    void loadFromSettings();
    void saveToSettings() const;
    void markCustomPreset();

    shaderfmt::FormatOptions options_;
    int maxLineLength_ = 100;

    QSpinBox* indentWidthSpin_ = nullptr;
    QCheckBox* useTabsCheck_ = nullptr;
    QComboBox* braceStyleCombo_ = nullptr;
    QCheckBox* spaceAfterControlKeywordCheck_ = nullptr;
    QSpinBox* maxLineLengthSpin_ = nullptr;
    QComboBox* presetCombo_ = nullptr;
    QPlainTextEdit* preview_ = nullptr;

    // Guards re-entrancy while a preset (or settings load) programmatically
    // updates several controls at once, so each individual control's
    // changed-signal doesn't fire emitOptionsChangedAndPersist() N times or
    // reset the preset combo back to "Personnalise".
    bool updatingControls_ = false;
};
