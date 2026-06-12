#ifndef SEPARATORNODE_H
#define SEPARATORNODE_H

#include <QWidget>

class QLineEdit;
class QStackedWidget;
class QToolButton;

// ============================================================
// SeparatorNode
//
// The "link" between two block cards in the schema chain:
//
//   [ Block ] ─ ( separator ) ─ [ Block ]
//
// Embedded by the dialog at the right end of a card's main row (via
// PatternBlockCard::attachGlueWidget), so the chain stays compact.
//
// Two states:
//   • auto ("invisible glue", separator empty) — rendered as a tiny
//     dotted link icon. The parser implicitly accepts any run of
//     spaces/tabs here, so the user normally never thinks about it.
//     Clicking the icon opens the editor for an explicit separator.
//   • explicit — a small edit with the literal text (or, with the
//     ".*" toggle, a regular expression) that must appear between
//     the two blocks. The ✕ button resets the link back to auto.
//
// Both states have the same fixed height, so toggling never makes the
// chain jump.
// ============================================================

class SeparatorNode : public QWidget
{
    Q_OBJECT
public:
    explicit SeparatorNode(QWidget* parent = nullptr);

    void    setSeparator(const QString& text, bool isRegex);
    QString separatorText() const;
    bool    isRegex() const;

signals:
    void edited();

private:
    void showAutoState();
    void showEditState(bool focus);

    QStackedWidget* m_stack    = nullptr;
    QToolButton*    m_autoBtn  = nullptr;
    QLineEdit*      m_edit     = nullptr;
    QToolButton*    m_rxBtn    = nullptr;
    QToolButton*    m_resetBtn = nullptr;
};

#endif // SEPARATORNODE_H
