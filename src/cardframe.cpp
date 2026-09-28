#include "cardframe.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QScrollBar>
#include <QTimer>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>
#include <QToolButton>
#include <QVBoxLayout>

// Рендерит SVG и заливает непрозрачные пиксели цветом `color`. Через
// QSvgRenderer (Qt Svg слинкован), а не QPixmap — чтобы не зависеть от
// разворачивания плагина формата SVG.
QIcon CardFrame::tintedIcon(const QString& resourcePath, const QColor& color)
{
    QSvgRenderer renderer(resourcePath);
    if (!renderer.isValid())
        return QIcon(resourcePath);

    QSize size = renderer.defaultSize();
    if (!size.isValid() || size.isEmpty())
        size = QSize(64, 64);
    size.scale(64, 64, Qt::KeepAspectRatio);

    QPixmap result(size);
    result.fill(Qt::transparent);
    QPainter p(&result);
    renderer.render(&p);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(result.rect(), color);
    p.end();
    return QIcon(result);
}

CardFrame::CardFrame(QWidget* parent)
    : QFrame(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 2, 4, 2);
    outer->setSpacing(4);

    m_stripe = new QFrame(this);
    m_stripe->setFixedWidth(4);
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

QColor CardFrame::mixedColor(const QColor& a, const QColor& b, qreal t)
{
    t = qBound<qreal>(0.0, t, 1.0);
    return QColor(int(a.red()   * (1.0 - t) + b.red()   * t),
                  int(a.green() * (1.0 - t) + b.green() * t),
                  int(a.blue()  * (1.0 - t) + b.blue()  * t));
}

QColor CardFrame::mutedBorderColor(const QPalette& palette)
{
    return mixedColor(palette.color(QPalette::WindowText),
                      palette.color(QPalette::Window), 0.65);
}

QColor CardFrame::mutedTextColor(const QPalette& palette)
{
    return mixedColor(palette.color(QPalette::WindowText),
                      palette.color(QPalette::Window), 0.35);
}

void CardFrame::changeEvent(QEvent* event)
{
    QFrame::changeEvent(event);
    if (event->type() == QEvent::PaletteChange)
        applyFrameStyle();
}

void CardFrame::applyFrameStyle()
{
    // Селектор по имени типа действует и на наследников (FilterRuleCard и
    // т.п.), при этом не задевает дочерние виджеты карточки.
    const QString neutral = mutedBorderColor(palette()).name();
    const QString border = m_accentBorder && m_accent.isValid()
        ? QStringLiteral("2px solid %1").arg(m_accent.name())
        : QStringLiteral("1px solid %1").arg(neutral);
    setStyleSheet(QStringLiteral(
        "CardFrame { border: %1; border-radius: 4px; }").arg(border));

    if (m_stripe) {
        m_stripe->setStyleSheet(QStringLiteral(
            "background-color: %1; border: none; border-radius: 2px;")
                .arg(m_accent.isValid() ? m_accent.name() : neutral));
    }
}

// ============================================================
// CardListArea
// ============================================================

CardListArea::CardListArea(QWidget* parent)
    : QScrollArea(parent)
{
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* host = new QWidget(this);
    auto* hostLayout = new QVBoxLayout(host);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    hostLayout->setSpacing(4);

    m_cards = new QVBoxLayout();
    m_cards->setContentsMargins(0, 0, 0, 0);
    m_cards->setSpacing(4);
    hostLayout->addLayout(m_cards);

    m_emptyLabel = new QLabel(host);
    m_emptyLabel->setWordWrap(true);
    m_emptyLabel->setContentsMargins(2, 4, 2, 4);
    m_emptyLabel->setVisible(false);
    hostLayout->addWidget(m_emptyLabel);
    hostLayout->addStretch(1);

    setWidget(host);
    changeEvent(nullptr);
}

void CardListArea::setEmptyText(const QString& text)
{
    m_emptyLabel->setText(text);
    cardsChanged();
}

void CardListArea::cardsChanged()
{
    m_emptyLabel->setVisible(m_cards->count() == 0 && !m_emptyLabel->text().isEmpty());
    updateGeometry();
}

void CardListArea::revealLater(QWidget* card)
{
    QPointer<QWidget> guard(card);
    QTimer::singleShot(0, this, [this, guard]() {
        if (guard)
            ensureWidgetVisible(guard, 0, 4);
    });
}

QSize CardListArea::minimumSizeHint() const
{
    QSize hint = QScrollArea::minimumSizeHint();
    if (const QWidget* content = widget()) {
        const int scrollBar = verticalScrollBar() ? verticalScrollBar()->sizeHint().width() : 0;
        hint.setWidth(qMax(hint.width(),
                           content->minimumSizeHint().width() + scrollBar + 2 * frameWidth()));
    }
    return hint;
}

void CardListArea::changeEvent(QEvent* event)
{
    if (event)
        QScrollArea::changeEvent(event);
    if (!event || event->type() == QEvent::PaletteChange)
        m_emptyLabel->setStyleSheet(QStringLiteral("color: %1;")
            .arg(CardFrame::mutedTextColor(palette()).name()));
}
