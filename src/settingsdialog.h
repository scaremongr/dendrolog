#pragma once

#include <QColor>
#include <QDialog>
#include <QHash>
#include <QVector>
#include <QString>
#include <QStringList>

// Forward declarations
class QLabel;
class QPushButton;
class QKeySequenceEdit;
class QSpinBox;
class QComboBox;

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

    // Хранилище памяти файлов окна: «Clear Memory» чистит через него
    // (дожидается записей, сбрасывает перечень). Без него — каталог напрямую.
    void setFileMemory(class FileMemoryStore* store) { m_fileMemory = store; }
    // Открыть на вкладке AI (окно «Find a Schema» → «Set Up AI…»).
    void showAiPage();

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

    // ---- Shortcut editor row ----------------------------------------------
    struct ShortcutRow {
        QString           id;            // ShortcutManager command id
        QKeySequenceEdit* edit = nullptr;
        // Что показано при открытии: сохраняются только изменённые строки.
        // Иначе пустое поле команды, чьё сочетание по умолчанию уступлено
        // другой команде (ShortcutManager::sequence), стало бы пустым
        // переопределением навсегда, едва та команда освободит клавишу.
        QKeySequence initial;
    };

    // ---- Helpers -----------------------------------------------------------
    void buildColorsTab();
    void updateColorButton(ColorEntry& entry);
    void buildShortcutsTab();
    // Highlights shortcuts bound to more than one command; returns the
    // clashes ("keys — command, command"), empty when there are none.
    QStringList updateShortcutConflicts();
    void buildPerformanceControls();
    // Память файлов (R-4): «помнить состояние файлов» и «Clear Memory».
    void buildFileMemoryControls();
    void resetShortcutsToDefaults();
    // Нейро-подсказчик (F-17, шаг 5): включение, модель, ключ (SecretStore).
    void buildAiTab();
    void updateAiKeyStatus();

    // ---- Init / finalise ---------------------------------------------------
    void loadFromSettings();
    void applyToSettings();

    Ui::SettingsDialog* ui;
    QVector<ColorEntry>  m_colorEntries;
    QVector<ShortcutRow> m_shortcutRows;
    QHash<QString, QString> m_shortcutLabels;     // command id → display label
    QLabel* m_shortcutConflictLabel = nullptr;

    // ---- Performance (большие файлы; создаются в коде, не в .ui) ----------
    QSpinBox* m_indexedThresholdSpinBox = nullptr;
    QSpinBox* m_textCacheSpinBox = nullptr;
    // Запасная кодировка автоопределения (F-1); данные пункта — int
    // TextEncoding, -1 — по локали системы.
    QComboBox* m_fallbackEncodingCombo = nullptr;
    class QCheckBox* m_rememberFileStateCheckBox = nullptr;
    class QCheckBox* m_restoreSessionCheckBox = nullptr;
    class FileMemoryStore* m_fileMemory = nullptr;

    // ---- AI (F-17, шаг 5; создаются в коде) --------------------------------
    QWidget* m_aiTab = nullptr;
    class QCheckBox* m_aiEnabledCheckBox = nullptr;
    QComboBox* m_aiModelCombo = nullptr;
    class QLineEdit* m_aiKeyEdit = nullptr;
    QLabel* m_aiKeyStatus = nullptr;
    QPushButton* m_aiRemoveKeyButton = nullptr;
};
