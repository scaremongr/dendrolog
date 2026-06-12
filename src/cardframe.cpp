#include "cardframe.h"

#include <QHBoxLayout>
#include <QToolButton>
#include <QVBoxLayout>

CardFrame::CardFrame(QWidget* parent)
    : QFrame(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 3, 6, 3);
    outer->setSpacing(5);

    m_stripe = new QFrame(this);
    m_stripe->setFixedWidth(6);
    outer->addWidget(m_stripe);

    m_rows = new QVBoxLayout();
    m_rows->setContentsMargins(0, 0, 0, 0);
    m_rows->setSpacing(2);
    outer->addLayout(m_rows, 1);

    applyFrameStyle();
}

void CardFrame::setAccentColor(const QColor& color)
{
    m_accent = color;
    applyFrameStyle();
}

void CardFrame::setAccentBorder(bool on)
{
    if (m_accentBorder == on)
        return;
    m_accentBorder = on;
    applyFrameStyle();
}

QToolButton* CardFrame::makeToolButton(const QString& text, const QString& toolTip)
{
    auto* btn = new QToolButton(this);
    btn->setText(text);
    btn->setToolTip(toolTip);
    btn->setAutoRaise(true);
    return btn;
}

void CardFrame::tintToolButton(QToolButton* button, bool hasContent) const
{
    if (!button)
        return;
    if (hasContent && m_accent.isValid()) {
        QColor tint = m_accent;
        tint.setAlpha(70);
        button->setStyleSheet(QStringLiteral(
            "QToolButton { background-color: rgba(%1,%2,%3,%4); border-radius: 3px; }")
                .arg(tint.red()).arg(tint.green()).arg(tint.blue()).arg(tint.alpha()));
    } else {
        button->setStyleSheet(QString());
    }
}

void CardFrame::applyFrameStyle()
{
    // Селектор по имени типа действует и на наследников (FilterRuleCard и
    // т.п.), при этом не задевает дочерние виджеты карточки.
    const QString border = m_accentBorder && m_accent.isValid()
        ? QStringLiteral("2px solid %1").arg(m_accent.name())
        : QStringLiteral("1px solid palette(mid)");
    setStyleSheet(QStringLiteral(
        "CardFrame { border: %1; border-radius: 4px; }").arg(border));

    if (m_stripe) {
        m_stripe->setStyleSheet(QStringLiteral(
            "background-color: %1; border: none; border-radius: 2px;")
                .arg(m_accent.isValid() ? m_accent.name() : QStringLiteral("palette(mid)")));
    }
}
