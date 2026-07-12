#include "StylePanel.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QFontDatabase>
#include <QGroupBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {

const char* kPreviewSample =
    "void mainImage(out vec4 fragColor, in vec2 fragCoord) {\n"
    "vec2 uv = fragCoord / iResolution.xy;\n"
    "if (uv.x > 0.5) {\n"
    "fragColor = vec4(1.0, 0.0, 0.0, 1.0);\n"
    "} else {\n"
    "fragColor = vec4(0.0, 0.0, 1.0, 1.0);\n"
    "}\n"
    "}\n";

struct PresetValues {
    shaderfmt::FormatOptions options;
    int maxLineLength;
};

// One-click presets (ROADMAP.md §4). These are reasonable approximations
// of each ecosystem's *general* code style (there is no official style
// guide specifically for shader source in any of them), not a verified
// spec: Unreal's public C++ coding standard uses tabs and Allman braces;
// Unity/C# code commonly uses Allman braces with spaces; Shadertoy's
// community shaders are GLSL with no fixed convention, so it's kept
// close to Default but with a looser line-length guide.
PresetValues presetValuesFor(int presetIndex) {
    switch (presetIndex) {
        case 1: // Default
            return {shaderfmt::FormatOptions{4, false, shaderfmt::BraceStyle::SameLine, true}, 100};
        case 2: // Shadertoy
            return {shaderfmt::FormatOptions{4, false, shaderfmt::BraceStyle::SameLine, true}, 120};
        case 3: // Unreal
            return {shaderfmt::FormatOptions{4, true, shaderfmt::BraceStyle::NextLine, true}, 120};
        case 4: // Unity
            return {shaderfmt::FormatOptions{4, false, shaderfmt::BraceStyle::NextLine, true}, 100};
        default:
            return {shaderfmt::FormatOptions{4, false, shaderfmt::BraceStyle::SameLine, true}, 100};
    }
}

} // namespace

StylePanel::StylePanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);

    auto* group = new QGroupBox(QStringLiteral("Style"), this);
    auto* form = new QFormLayout(group);

    presetCombo_ = new QComboBox(group);
    presetCombo_->addItem(QStringLiteral("Personnalise"));
    presetCombo_->addItem(QStringLiteral("Default"));
    presetCombo_->addItem(QStringLiteral("Shadertoy"));
    presetCombo_->addItem(QStringLiteral("Unreal"));
    presetCombo_->addItem(QStringLiteral("Unity"));
    form->addRow(QStringLiteral("Preset"), presetCombo_);
    connect(presetCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &StylePanel::onPresetSelected);

    indentWidthSpin_ = new QSpinBox(group);
    indentWidthSpin_->setRange(1, 8);
    form->addRow(QStringLiteral("Indentation (espaces)"), indentWidthSpin_);
    connect(indentWidthSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this,
            &StylePanel::onIndentWidthChanged);

    useTabsCheck_ = new QCheckBox(QStringLiteral("Utiliser des tabulations"), group);
    form->addRow(QString(), useTabsCheck_);
    connect(useTabsCheck_, &QCheckBox::toggled, this, &StylePanel::onUseTabsToggled);

    braceStyleCombo_ = new QComboBox(group);
    braceStyleCombo_->addItem(QStringLiteral("Meme ligne (K&R)"));
    braceStyleCombo_->addItem(QStringLiteral("Ligne suivante (Allman)"));
    form->addRow(QStringLiteral("Accolades"), braceStyleCombo_);
    connect(braceStyleCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &StylePanel::onBraceStyleChanged);

    spaceAfterControlKeywordCheck_ =
        new QCheckBox(QStringLiteral("Espace avant parenthese (if/for/while...)"), group);
    form->addRow(QString(), spaceAfterControlKeywordCheck_);
    connect(spaceAfterControlKeywordCheck_, &QCheckBox::toggled, this,
            &StylePanel::onSpaceAfterControlKeywordToggled);

    maxLineLengthSpin_ = new QSpinBox(group);
    maxLineLengthSpin_->setRange(40, 300);
    form->addRow(QStringLiteral("Longueur de ligne (guide)"), maxLineLengthSpin_);
    connect(maxLineLengthSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this,
            &StylePanel::onMaxLineLengthChanged);

    layout->addWidget(group);

    layout->addWidget(new QLabel(QStringLiteral("Apercu"), this));
    preview_ = new QPlainTextEdit(this);
    preview_->setReadOnly(true);
    preview_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    preview_->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(preview_, 1);

    loadFromSettings();
    applyOptionsToControls();
    refreshPreview();
}

void StylePanel::onIndentWidthChanged(int value) {
    if (updatingControls_) return;
    options_.indentWidth = value;
    markCustomPreset();
    emitOptionsChangedAndPersist();
}

void StylePanel::onUseTabsToggled(bool checked) {
    if (updatingControls_) return;
    options_.useTabs = checked;
    markCustomPreset();
    emitOptionsChangedAndPersist();
}

void StylePanel::onBraceStyleChanged(int index) {
    if (updatingControls_) return;
    options_.braceStyle = (index == 1) ? shaderfmt::BraceStyle::NextLine : shaderfmt::BraceStyle::SameLine;
    markCustomPreset();
    emitOptionsChangedAndPersist();
}

void StylePanel::onSpaceAfterControlKeywordToggled(bool checked) {
    if (updatingControls_) return;
    options_.spaceAfterControlKeyword = checked;
    markCustomPreset();
    emitOptionsChangedAndPersist();
}

void StylePanel::onMaxLineLengthChanged(int value) {
    if (updatingControls_) return;
    maxLineLength_ = value;
    markCustomPreset();
    saveToSettings();
    emit maxLineLengthChanged(maxLineLength_);
    // Line length is a visual guide only (no line-wrapping engine exists -
    // see README "Ce qui est simplifie"), so it doesn't affect the
    // formatted preview text, only the editor's ruler in MainWindow.
}

void StylePanel::onPresetSelected(int index) {
    if (updatingControls_ || index == 0) return;
    PresetValues values = presetValuesFor(index);
    options_ = values.options;
    maxLineLength_ = values.maxLineLength;
    applyOptionsToControls();
    // applyOptionsToControls() blocks control signals, so the preset combo
    // itself must be left showing this preset (not reset to "Personnalise").
    const QSignalBlocker blocker(presetCombo_);
    presetCombo_->setCurrentIndex(index);
    refreshPreview();
    emitOptionsChangedAndPersist();
    emit maxLineLengthChanged(maxLineLength_);
}

void StylePanel::markCustomPreset() {
    const QSignalBlocker blocker(presetCombo_);
    presetCombo_->setCurrentIndex(0);
}

void StylePanel::applyOptionsToControls() {
    updatingControls_ = true;
    indentWidthSpin_->setValue(options_.indentWidth);
    useTabsCheck_->setChecked(options_.useTabs);
    braceStyleCombo_->setCurrentIndex(options_.braceStyle == shaderfmt::BraceStyle::NextLine ? 1 : 0);
    spaceAfterControlKeywordCheck_->setChecked(options_.spaceAfterControlKeyword);
    maxLineLengthSpin_->setValue(maxLineLength_);
    updatingControls_ = false;
}

void StylePanel::refreshPreview() {
    auto result = shaderfmt::format(kPreviewSample, shaderfmt::Language::GLSL, options_);
    preview_->setPlainText(QString::fromStdString(result.formatted));
}

void StylePanel::emitOptionsChangedAndPersist() {
    refreshPreview();
    saveToSettings();
    emit optionsChanged(options_);
}

void StylePanel::loadFromSettings() {
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("ShaderFmt"),
                        QStringLiteral("ShaderFmt"));
    settings.beginGroup(QStringLiteral("style"));
    options_.indentWidth = settings.value(QStringLiteral("indentWidth"), options_.indentWidth).toInt();
    options_.useTabs = settings.value(QStringLiteral("useTabs"), options_.useTabs).toBool();
    options_.braceStyle = settings.value(QStringLiteral("braceStyleNextLine"), false).toBool()
                               ? shaderfmt::BraceStyle::NextLine
                               : shaderfmt::BraceStyle::SameLine;
    options_.spaceAfterControlKeyword =
        settings.value(QStringLiteral("spaceAfterControlKeyword"), options_.spaceAfterControlKeyword).toBool();
    maxLineLength_ = settings.value(QStringLiteral("maxLineLength"), maxLineLength_).toInt();
    settings.endGroup();
}

void StylePanel::saveToSettings() const {
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("ShaderFmt"),
                        QStringLiteral("ShaderFmt"));
    settings.beginGroup(QStringLiteral("style"));
    settings.setValue(QStringLiteral("indentWidth"), options_.indentWidth);
    settings.setValue(QStringLiteral("useTabs"), options_.useTabs);
    settings.setValue(QStringLiteral("braceStyleNextLine"), options_.braceStyle == shaderfmt::BraceStyle::NextLine);
    settings.setValue(QStringLiteral("spaceAfterControlKeyword"), options_.spaceAfterControlKeyword);
    settings.setValue(QStringLiteral("maxLineLength"), maxLineLength_);
    settings.endGroup();
}

void StylePanel::selectPresetForTesting(int index) {
    presetCombo_->setCurrentIndex(index);
}

void StylePanel::setIndentWidthForTesting(int value) {
    indentWidthSpin_->setValue(value);
}

void StylePanel::setUseTabsForTesting(bool checked) {
    useTabsCheck_->setChecked(checked);
}

void StylePanel::setBraceStyleForTesting(shaderfmt::BraceStyle style) {
    braceStyleCombo_->setCurrentIndex(style == shaderfmt::BraceStyle::NextLine ? 1 : 0);
}

void StylePanel::setSpaceAfterControlKeywordForTesting(bool checked) {
    spaceAfterControlKeywordCheck_->setChecked(checked);
}

QString StylePanel::previewTextForTesting() const {
    return preview_->toPlainText();
}
