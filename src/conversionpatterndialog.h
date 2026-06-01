#pragma once

#include <QDialog>
#include <QList>
#include <QPair>
#include <QString>

#include "logpattern.h"

class QCloseEvent;
class QTableWidget;
class QLineEdit;
class QPushButton;
class QComboBox;

/// Dialog for managing named dynamic field schemas.
/// A schema consists of an ordered list of blocks.
/// Each block has a name, a match rule and, when applicable, separator/details.
class ConversionPatternDialog : public QDialog
{
    Q_OBJECT
public:
    /// A single schema entry: first = display name, second = serialized schema.
    using PatternEntry = QPair<QString, QString>;
    using PatternList  = QList<PatternEntry>;

    explicit ConversionPatternDialog(const PatternList& patterns,
                                     QWidget* parent = nullptr);

    /// Returns the (possibly edited) list on dialog close.
    PatternList resultPatterns() const;

    /// Non-empty only when the user clicked "Use Selected".
    QString chosenPattern()    const { return m_chosenPattern; }
    /// Index of the chosen entry inside resultPatterns() (-1 if none chosen).
    int     chosenResultIndex() const { return m_chosenResultIndex; }

private slots:
    void onAddSchema();
    void onRemoveSchema();
    void onMoveSchemaUp();
    void onMoveSchemaDown();
    void onSchemaSelectionChanged(int currentRow, int currentColumn,
                                  int previousRow, int previousColumn);
    void onSchemaNameEdited(const QString& text);
    void onAddBlock();
    void onRemoveBlock();
    void onMoveBlockUp();
    void onMoveBlockDown();
    void onBlockItemChanged();
    void onUseSelected();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void populateSchemas(const PatternList& list);
    void setSchemaRow(int row, const QString& name, const QString& serializedSchema);
    void updateSchemaSummary(int row);
    void loadSchemaRow(int row);
    void saveSchemaRow(int row);
    void clearBlockTable();
    void addBlockRow(const PatternBlock& block);
    PatternDefinition currentDefinitionFromEditor() const;
    void updateButtonStates();
    // Called when Rule combo changes: refreshes Mode-combo enable state and Details visibility.
    void updateRowMode(int row);
    // Called when Mode combo changes: refreshes Details widget visibility.
    void updateRowValue(int row);

    QTableWidget* m_schemaTable      = nullptr;
    QLineEdit*    m_nameEdit         = nullptr;
    QTableWidget* m_blockTable       = nullptr;
    QPushButton*  m_addSchemaBtn     = nullptr;
    QPushButton*  m_removeSchemaBtn  = nullptr;
    QPushButton*  m_moveSchemaUpBtn  = nullptr;
    QPushButton*  m_moveSchemaDownBtn = nullptr;
    QPushButton*  m_addBlockBtn      = nullptr;
    QPushButton*  m_removeBlockBtn   = nullptr;
    QPushButton*  m_moveBlockUpBtn   = nullptr;
    QPushButton*  m_moveBlockDownBtn = nullptr;
    QPushButton*  m_useBtn           = nullptr;
    QString       m_chosenPattern;
    int           m_chosenResultIndex = -1;
    bool          m_syncing = false;
};
