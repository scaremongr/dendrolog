#include "separatornode.h"

#include "cardframe.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QStackedWidget>
#include <QToolButton>

namespace {
constexpr int kNodeHeight = 26;
}

SeparatorNode::SeparatorNode(QWidget* parent)
    : QWidget(parent)
{
    setFixedHeight(kNodeHeight);

    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_stack = new QStackedWidget(this);
    m_stack->setFixedHeight(kNodeHeight);

    // --- Page 0: auto glue (default) --------------------------------- //
    {
        auto* page = new QWidget(m_stack);
        auto* layout = new QHBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);

        auto* tie = new QLabel(QStringLiteral("─"), page);
        m_ties.append(tie);

        m_autoBtn = new QToolButton(page);
        m_autoBtn->setText(QStringLiteral("· · ·"));
        m_autoBtn->setAutoRaise(true);
        m_autoBtn->setCursor(Qt::PointingHandCursor);
        m_autoBtn->setToolTip(tr("Auto separator: any spaces or tabs before the next block.\n"
                                 "Click to set an explicit separator (literal text or regex)."));
        layout->addWidget(tie);
        layout->addWidget(m_autoBtn);
        layout->addStretch(1);
        m_stack->addWidget(page);
    }

    // --- Page 1: explicit glue --------------------------------------- //
    {
        auto* page = new QWidget(m_stack);
        auto* layout = new QHBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);

        auto* tie = new QLabel(QStringLiteral("─"), page);
        m_ties.append(tie);

        // The chip frame visually groups the separator controls, so its
        // own ✕ cannot be confused with the card's remove button.
        auto* chip = new QFrame(page);
        chip->setObjectName(QStringLiteral("glueChip"));
        chip->setToolTip(tr("Separator before the next block."));
        m_chip = chip;
        auto* chipLayout = new QHBoxLayout(chip);
        chipLayout->setContentsMargins(6, 1, 4, 1);
        chipLayout->setSpacing(2);

        m_edit = new QLineEdit(chip);
        m_edit->setFixedWidth(100);
        m_edit->setFrame(false);
        m_edit->setPlaceholderText(tr("separator"));
        m_edit->setToolTip(tr("Exact text expected before the next block, e.g. \" - \".\n"
                              "Surrounding spaces are matched automatically."));

        m_rxBtn = new QToolButton(chip);
        m_rxBtn->setText(QStringLiteral(".*"));
        m_rxBtn->setCheckable(true);
        m_rxBtn->setAutoRaise(true);
        m_rxBtn->setToolTip(tr("Treat the separator as a regular expression."));

        m_resetBtn = new QToolButton(chip);
        m_resetBtn->setText(QStringLiteral("✕"));
        m_resetBtn->setAutoRaise(true);
        m_resetBtn->setToolTip(tr("Remove this separator (back to auto whitespace).\n"
                                  "Does not affect the block itself."));

        chipLayout->addWidget(m_edit);
        chipLayout->addWidget(m_rxBtn);
        chipLayout->addWidget(m_resetBtn);

        layout->addWidget(tie);
        layout->addWidget(chip);
        layout->addStretch(1);
        m_stack->addWidget(page);
    }

    outer->addWidget(m_stack);

    connect(m_autoBtn, &QToolButton::clicked, this, [this]() { showEditState(true); });
    connect(m_edit, &QLineEdit::textEdited, this, &SeparatorNode::edited);
    connect(m_rxBtn, &QToolButton::toggled, this, [this](bool) { emit edited(); });
    connect(m_resetBtn, &QToolButton::clicked, this, [this]() {
        m_edit->clear();
        m_rxBtn->setChecked(false);
        showAutoState();
        emit edited();
    });
    // Collapse back to the auto icon when the user leaves the edit empty.
    connect(m_edit, &QLineEdit::editingFinished, this, [this]() {
        if (m_edit->text().isEmpty())
            showAutoState();
    });

    applyMutedStyles();
    showAutoState();
}

void SeparatorNode::applyMutedStyles()
{
    // Blended from the palette in code: QSS palette(mid) is nearly
    // invisible on dark palettes ("─" ties and the "· · ·" auto button
    // used to disappear there).
    const QString glyph = CardFrame::mutedTextColor(palette()).name();
    const QString border = CardFrame::mutedBorderColor(palette()).name();

    for (QLabel* tie : m_ties)
        tie->setStyleSheet(QStringLiteral("color: %1; border: none;").arg(glyph));
    m_autoBtn->setStyleSheet(
        QStringLiteral("QToolButton { color: %1; border: none; }").arg(glyph));
    m_chip->setStyleSheet(QStringLiteral(
        "QFrame#glueChip { border: 1px solid %1; border-radius: 9px; }").arg(border));
}

void SeparatorNode::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange)
        applyMutedStyles();
}

void SeparatorNode::setSeparator(const QString& text, bool isRegex)
{
    m_edit->setText(text);
    m_rxBtn->setChecked(isRegex);
    if (text.isEmpty())
        showAutoState();
    else
        showEditState(false);
}

QString SeparatorNode::separatorText() const
{
    return m_edit->text();
}

bool SeparatorNode::isRegex() const
{
    return m_rxBtn->isChecked() && !m_edit->text().isEmpty();
}

void SeparatorNode::showAutoState()
{
    m_stack->setCurrentIndex(0);
}

void SeparatorNode::showEditState(bool focus)
{
    m_stack->setCurrentIndex(1);
    if (focus) {
        m_edit->setFocus();
        m_edit->selectAll();
    }
}
