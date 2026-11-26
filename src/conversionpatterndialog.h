#pragma once

#include <QDialog>
#include <QFutureWatcher>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include "logpattern.h"
#include "schemafit.h"
#include "schemastore.h"

class CardFrame;
class LogModel;
class PatternBlockCard;
class SeparatorNode;
class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
class QToolButton;
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
///
/// Format (F-4, docs/design/F-4.md): "Blocks" edits the regex chain;
/// "JSON" / "logfmt" edit a keyed schema instead — role keys (time, level,
/// message), extra columns (with "Add keys from sample"), epoch unit and
/// numeric level scale — on a card that replaces the chain. The cards stay
/// in the editor while the format is switched, so Blocks → JSON → Blocks
/// loses nothing until Apply (a keyed schema saves only the fallback
/// "Message" block). The preview runs the same LogPattern::matchLine and
/// colours the found values by column.
///
/// Coverage (F-17, step 1): the preview status line and, with a source tab
/// (setSource), a line under the editor report SchemaFit — the one measure of
/// "how many record lines the schema parses" — on the samples and on lines of
/// the tab's file (SchemaFitJob in the pool, restarted after edits). The
/// "▾" menu next to "Suggest schema" offers the catalogue schemas that fit
/// these lines best first, then the draft built from the sample lines.
class ConversionPatternDialog : public QDialog
{
    Q_OBJECT
public:
    /// Schemas come and go with their metadata (description, unknown keys)
    /// and their file name, so saving them back loses nothing.
    using PatternList = SchemaStore::SchemaList;
    /// A user preset: first = name, second = serialized schema.
    using PresetList  = QList<QPair<QString, QString>>;

    explicit ConversionPatternDialog(const PatternList& patterns,
                                     QWidget* parent = nullptr,
                                     const QStringList& sampleLines = QStringList(),
                                     const QString& activeSchema = QString());
    ~ConversionPatternDialog() override;

    /// Source tab of the lines: \a model's rows from \a currentRow (the top
    /// when negative) refill the preview, and the schemas are checked on the
    /// lines of its file. The model must outlive the dialog.
    void setSource(LogModel* model, int currentRow);

    /// Last SchemaFit of the edited schema on the preview lines (valid=false
    /// until the first preview) and on the source file (fileFitReady()).
    const SchemaFit::Result& sampleFit() const { return m_sampleFit; }
    const SchemaFit::Result& fileFit() const { return m_fileFit; }
    bool fileFitReady() const { return m_fileFitReady; }
    /// Catalogue rows ranked by SchemaFit, best first — on the file lines
    /// when a source is set and checked, else on the preview lines. Rows
    /// whose schema does not fit are left out.
    /// \a pending (may be nullptr) — the lines are still being checked
    /// (the result is then empty).
    QVector<SchemaFit::Ranked> catalogueSuggestions(bool* pending = nullptr) const;
    /// Time budget of the file check per schema (test seam: 0 — none).
    void setFitBudget(const SchemaFit::Budget& budget) { m_fitBudget = budget; }

    /// Returns the (possibly edited) list on dialog close.
    PatternList resultPatterns() const;

    /// Non-empty only when the user clicked "Use Selected".
    QString chosenPattern()    const { return m_chosenPattern; }
    /// Index of the chosen entry inside resultPatterns() (-1 if none chosen).
    int     chosenResultIndex() const { return m_chosenResultIndex; }

public slots:
    /// Esc / X / Close all behave identically: per-schema edits are handled
    /// by Apply/Revert, the (possibly reordered) schema list is committed.
    void reject() override;

private slots:
    void onAddSchema();
    void onRemoveSchema();
    void onImportSchema();
    void onExportSchema();
    void onOpenFolder();
    void onMoveSchemaUp();
    void onMoveSchemaDown();
    void onSchemaSelectionChanged(int currentRow, int currentColumn,
                                  int previousRow, int previousColumn);
    void onSchemaNameEdited(const QString& text);
    void onSchemaDescriptionEdited(const QString& text);
    void onAddBlock();
    void onFormatChanged();
    void onKeyedEdited();
    void onAddSampleKeys();
    void onBlocksEdited();
    void onApplyChanges();
    void onRevertChanges();
    void onAutoDetect();
    void onGrokImport();
    void onSavePreset();
    void onUseSelected();
    void onPreviewReady();
    void onFileFitReady();

private:
    struct PreviewJob {
        int generation = 0;
        QVector<LineMatchResult> lines;
        SchemaFit::Result fit;   ///< the same lines through SchemaFit (one measure)
    };

    void populateSchemas(const PatternList& list, const QString& activeSchema = QString());
    void setSchemaRow(int row, const SchemaStore::SchemaEntry& entry);
    SchemaStore::SchemaEntry schemaRow(int row) const;
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

    // ---- Ключевой режим (F-4) ----
    void buildKeyedEditor();
    /// Формат и поля ключевой карточки; nullopt — режим Blocks (поля —
    /// умолчания KeyedDefinition, а с keepFields — как были).
    void setKeyedEditor(const std::optional<KeyedDefinition>& keyed, bool keepFields = false);
    /// Ключевая часть из полей карточки; режим Blocks — nullopt.
    std::optional<KeyedDefinition> keyedFromEditor() const;
    bool isKeyedMode() const;
    void updateFormatMode();
    void updateKeyedLegend();

    void rebuildPresetMenu();
    void applyPreset(const PatternDefinition& definition);
    PresetList userPresets() const;
    void saveUserPresets(const PresetList& presets);

    void schedulePreviewUpdate();
    void startFileFit();
    void updateFileFitLabel();
    void rebuildSuggestMenu();
    /// Catalogue rows as patterns, in table order (invalid rows — invalid patterns).
    QVector<LogPattern> cataloguePatterns() const;
    QStringList catalogueSchemas() const;
    /// Ranks the list on the preview lines in a worker (the "▾" menu
    /// without a source tab); no-op when done or running for the same.
    void startCatalogueFit(const QStringList& lines);
    void startPreviewJob();
    void updateValidationLabel();
    void updateButtonStates();

    QStringList previewLines() const;

    QStringList   m_fileSampleLines; ///< Log lines at the current view position, for the "reload sample" button.
    QToolButton*  m_reloadSampleBtn = nullptr;

    // ---- Coverage (F-17, step 1) ----
    SchemaFit::Result m_sampleFit;
    SchemaFit::Result m_fileFit;
    bool          m_fileFitReady = false;
    SchemaFitJob* m_fitJob = nullptr;      ///< lines of the source tab's file
    QTimer*       m_fitTimer = nullptr;    ///< restart 400 ms after an edit
    QStringList   m_fitCatalogue;          ///< catalogue (serialized) the last file fit ranked
    QVector<SchemaFit::Result> m_catalogueFileFits; ///< by catalogue row, from the file fit
    SchemaFit::Budget m_fitBudget;
    struct CatalogueFit {
        QStringList catalogue;
        QStringList lines;
        QVector<SchemaFit::Result> results;
        bool cancelled = false;
    };
    QFutureWatcher<CatalogueFit>* m_catalogueWatcher = nullptr;
    QPair<QStringList, QStringList> m_catalogueFitKey;   ///< catalogue, preview lines
    QVector<SchemaFit::Result> m_previewCatalogueFits;  ///< by row, on the preview lines
    std::shared_ptr<std::atomic_bool> m_catalogueCancel;
    QLabel*       m_fileFitLabel = nullptr;
    QToolButton*  m_suggestMenuBtn = nullptr;

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

    QComboBox*    m_formatCombo = nullptr;      ///< Blocks / JSON / logfmt.
    CardFrame*    m_keyedCard = nullptr;
    QWidget*      m_keyedHost = nullptr;        ///< Card + stretch, in place of the chain.
    QLineEdit*    m_timeKeysEdit = nullptr;
    QLineEdit*    m_levelKeysEdit = nullptr;
    QLineEdit*    m_messageKeysEdit = nullptr;
    QLineEdit*    m_columnsEdit = nullptr;
    QComboBox*    m_epochUnitCombo = nullptr;
    QComboBox*    m_numericLevelsCombo = nullptr;
    QPushButton*  m_addSampleKeysBtn = nullptr;
    QLabel*       m_keyedLegend = nullptr;

    QAction*      m_importAction = nullptr;     ///< Items of the "⋯" schema-list menu.
    QAction*      m_exportAction = nullptr;
    QAction*      m_openFolderAction = nullptr;

    Ui::ConversionPatternDialog* ui = nullptr;
};
