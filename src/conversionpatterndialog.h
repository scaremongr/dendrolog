#pragma once

#include <QDialog>
#include <QFutureWatcher>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include "logpattern.h"

class PatternBlockCard;
class SeparatorNode;
class QCloseEvent;
class QTimer;
class QVBoxLayout;

QT_BEGIN_NAMESPACE
namespace Ui { class ConversionPatternDialog; }
QT_END_NAMESPACE

/// Dialog for managing named dynamic field schemas.
///
/// A schema is an ordered list of visual block cards
/// ( [prefix] (name : type) [suffix] ). The bottom pane is a live
/// preview: paste real log lines and every block match is highlighted
/// with the colour of its card while you edit. Extras: auto-detect
/// ("Suggest schema"), ready-made and user presets (stored in the INI
/// file) and Grok import.
///
/// Preview matching runs in a worker thread, so a pathological custom
/// regex can never freeze the UI; schemas are validated before they
/// can be applied ("Use Selected").
class ConversionPatternDialog : public QDialog
{
    Q_OBJECT
public:
    /// A single schema entry: first = display name, second = serialized schema.
    using PatternEntry = QPair<QString, QString>;
    using PatternList  = QList<PatternEntry>;

    explicit ConversionPatternDialog(const PatternList& patterns,
                                     QWidget* parent = nullptr,
                                     const QStringList& sampleLines = QStringList(),
                                     const QString& activeSchema = QString());
    ~ConversionPatternDialog() override;

    /// Returns the (possibly edited) list on dialog close.
    PatternList resultPatterns() const;

    /// Non-empty only when the user clicked "Use Selected".
    QString chosenPattern()    const { return m_chosenPattern; }
    /// Index of the chosen entry inside resultPatterns() (-1 if none chosen).
    int     chosenResultIndex() const { return m_chosenResultIndex; }

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onAddSchema();
    void onRemoveSchema();
    void onMoveSchemaUp();
    void onMoveSchemaDown();
    void onSchemaSelectionChanged(int currentRow, int currentColumn,
                                  int previousRow, int previousColumn);
    void onSchemaNameEdited(const QString& text);
    void onAddBlock();
    void onBlocksEdited();
    void onApplyChanges();
    void onRevertChanges();
    void onAutoDetect();
    void onGrokImport();
    void onSavePreset();
    void onUseSelected();
    void onPreviewReady();

private:
    struct PreviewJob {
        int generation = 0;
        QVector<LineMatchResult> lines;
    };

    void populateSchemas(const PatternList& list, const QString& activeSchema = QString());
    void setSchemaRow(int row, const QString& name, const QString& serializedSchema);
    void updateSchemaSummary(int row);
    void loadSchemaRow(int row);
    void saveSchemaRow(int row);

    /// Marks the editor as having unapplied changes.
    void setDirty();
    /// If the editor holds unapplied changes for \a row, asks the user
    /// whether to apply or discard them. Clears the dirty flag.
    void resolveDirtyState(int row);

    void rebuildCards(const PatternDefinition& definition);
    void refreshCardChrome();
    PatternDefinition currentDefinitionFromEditor() const;
    void replaceCurrentBlocks(const PatternDefinition& definition);

    void rebuildPresetMenu();
    void applyPreset(const PatternDefinition& definition);
    PatternList userPresets() const;
    void saveUserPresets(const PatternList& presets);

    void schedulePreviewUpdate();
    void startPreviewJob();
    void updateValidationLabel();
    void updateButtonStates();

    QStringList previewLines() const;

    QStringList   m_fileSampleLines; ///< First lines of the active log, for the "reload sample" button.

    QVector<PatternBlockCard*> m_cards;
    QVector<SeparatorNode*>    m_separators; ///< Links between cards: m_separators[i] sits after m_cards[i].
    QVBoxLayout*  m_cardsLayout = nullptr;
    QTimer*       m_previewTimer = nullptr;
    QTimer*       m_previewWatchdog = nullptr;
    QFutureWatcher<PreviewJob>* m_previewWatcher = nullptr;
    int           m_previewGeneration = 0;
    bool          m_previewSlow = false;

    QString       m_chosenPattern;
    int           m_chosenResultIndex = -1;
    bool          m_syncing = false;
    bool          m_dirty = false;   ///< Editor changes not yet applied to the schema row.

    Ui::ConversionPatternDialog* ui = nullptr;
};
