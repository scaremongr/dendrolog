#ifndef PATTERNBLOCKCARD_H
#define PATTERNBLOCKCARD_H

#include "cardframe.h"
#include "logpattern.h"

#include <QColor>

class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QToolButton;
class QWidget;

// ============================================================
// PatternBlockCard
//
// One data-block node of the schema chain. The main row is IDENTICAL
// for every block type:
//
//   ▌ Type ▾ | [field name........] ☐ hide ⚙ ↑ ↓ ✕ │ ─(glue)─
//   ▌   Text/Regex [......]  Enclosed by [ [ ] and [ ] ]   (⚙ row)
//
// Everything type-specific lives in the collapsible ⚙ Advanced row:
// the literal/regex of Constant text / Custom regex blocks (the row
// auto-expands for those types) and the wrappers around the value.
//
// The separator ("glue") to the NEXT block is owned by the dialog as a
// SeparatorNode; the dialog embeds it at the right end of the main row
// via attachGlueWidget(), so the whole chain stays compact vertically.
//
// Self-delimiting types (Timestamp, Level, Integer, Hex, IP) — tokens
// whose shape ends the match by itself, needing no separator — are
// marked with a border in the block's accent colour.
// ============================================================

class PatternBlockCard : public CardFrame
{
    Q_OBJECT
public:
    explicit PatternBlockCard(QWidget* parent = nullptr);

    /// Fills the card. The block's separator is intentionally NOT shown
    /// here — the dialog renders it as a SeparatorNode link.
    void         setBlock(const PatternBlock& block);
    /// Returns the card state; \c separator is always left empty.
    PatternBlock block() const;

    /// Прячет базовый метод: дополнительно обновляет тонировку ⚙ и рамку.
    void setAccentColor(const QColor& color);

    /// Embeds the glue widget (SeparatorNode) at the right end of the
    /// main row. The card takes ownership via the layout.
    void attachGlueWidget(QWidget* widget);

    /// Enables/disables the move arrows according to the card position.
    void setPosition(int index, int count);

signals:
    void blockEdited();
    void moveUpRequested();
    void moveDownRequested();
    void removeRequested();

private slots:
    void onKindChanged();

private:
    void refreshFieldStates();
    void updateGearHighlight();
    void applyFrameStyle();

    QComboBox*   m_kindCombo    = nullptr;
    QLineEdit*   m_nameEdit     = nullptr;
    QCheckBox*   m_ignoreCheck  = nullptr;
    QToolButton* m_wrapBtn      = nullptr;  ///< ⚙ toggle for the advanced wrapper row.
    QToolButton* m_upBtn        = nullptr;
    QToolButton* m_downBtn      = nullptr;
    QToolButton* m_removeBtn    = nullptr;
    QHBoxLayout* m_mainRow      = nullptr;

    QWidget*     m_wrapRow      = nullptr;  ///< Advanced (⚙) row.
    QLabel*      m_patternLabel = nullptr;
    QLineEdit*   m_patternEdit  = nullptr;  ///< Literal (Constant) or regex (Custom regex).
    QLabel*      m_wrapLabel    = nullptr;
    QLineEdit*   m_openEdit     = nullptr;
    QLabel*      m_andLabel     = nullptr;
    QLineEdit*   m_closeEdit    = nullptr;
};

#endif // PATTERNBLOCKCARD_H
