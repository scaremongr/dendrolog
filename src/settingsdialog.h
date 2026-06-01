#pragma once

#include <QColor>
#include <QDialog>
#include <QVector>

// Forward declarations
class QPushButton;

QT_BEGIN_NAMESPACE
namespace Ui { class SettingsDialog; }
QT_END_NAMESPACE

// ---------------------------------------------------------------------------
// SettingsDialog — edits all AppSettings + AppTheme preferences.
//
// Architecture notes:
//   • The dialog works on local copies of every preference (no live editing).
//   • On accept(), values are written back via AppSettings setters and
//     AppTheme field assignment, followed by AppSettings::save().
//   • Color pickers are built dynamically from the m_colorEntries table so
//     that adding a new theme color requires only one table row change.
//   • Fonts are filtered to monospaced-only in the .ui file via the
//     fontFilters property of QFontComboBox.
// ---------------------------------------------------------------------------

class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget* parent = nullptr);
    ~SettingsDialog() override;

protected:
    void accept() override;

private slots:
    void updateFontPreview();

private:
    // ---- Colour table entry ------------------------------------------------
    // One row per editable AppTheme colour. The dialog keeps working copies
    // (value) so that clicking Cancel discards all changes.
    struct ColorEntry {
        QString      label;             // Displayed in the Colors tab
        QColor       value;             // Working copy (mutated by picker)
        QColor*      target = nullptr;  // Pointer to the real AppTheme field
        QPushButton* button = nullptr;  // Created in buildColorsTab()
    };

    // ---- Helpers -----------------------------------------------------------
    void buildColorsTab();
    void updateColorButton(ColorEntry& entry);

    // ---- Init / finalise ---------------------------------------------------
    void loadFromSettings();
    void applyToSettings();

    Ui::SettingsDialog* ui;
    QVector<ColorEntry> m_colorEntries;
};
