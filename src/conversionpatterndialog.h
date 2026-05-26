#pragma once

#include <QDialog>
#include <QList>
#include <QPair>
#include <QString>

class QTableWidget;
class QTextBrowser;
class QLineEdit;
class QToolButton;
class QPushButton;

/// Dialog for managing named Log4j/Log4cxx conversion patterns.
/// Displays a table of (name, pattern) entries; users can add, remove,
/// reorder and edit them.  Clicking "Use Selected" stores the chosen
/// pattern and accepts the dialog.
class ConversionPatternDialog : public QDialog
{
    Q_OBJECT
public:
    /// A single pattern entry: first = display name, second = pattern string.
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
    void onAdd();
    void onRemove();
    void onMoveUp();
    void onMoveDown();
    void onSelectionChanged();
    void onNameEdited(const QString& text);
    void onPatternEdited(const QString& text);
    void onUseSelected();

private:
    void populateTable(const PatternList& list);
    void syncFromRow(int row);
    void syncToRow(int row);
    void updateButtonStates();

    QTableWidget* m_table       = nullptr;
    QLineEdit*    m_nameEdit    = nullptr;
    QLineEdit*    m_patternEdit = nullptr;
    QToolButton*  m_insertBtn   = nullptr;
    QPushButton*  m_addBtn      = nullptr;
    QPushButton*  m_removeBtn   = nullptr;
    QPushButton*  m_moveUpBtn   = nullptr;
    QPushButton*  m_moveDownBtn = nullptr;
    QPushButton*  m_useBtn      = nullptr;
    QString       m_chosenPattern;
    int           m_chosenResultIndex = -1;
    bool          m_syncing = false;
};
