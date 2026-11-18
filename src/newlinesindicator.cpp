#include "newlinesindicator.h"

#include <QApplication>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>

NewLinesIndicator::NewLinesIndicator(QWidget* parent)
    : QAbstractButton(parent)
{
    setObjectName(QStringLiteral("newLinesIndicator"));
    setCursor(Qt::PointingHandCursor);
    // Фокус не забирает: щелчок не должен уводить клавиатуру из лога.
    setFocusPolicy(Qt::NoFocus);
    setFont(QApplication::font());
    setAttribute(Qt::WA_Hover);
    hide();
}

// Числа — как в остальном (английском) интерфейсе: «12,345», независимо от
// локали системы. У QLocale::c() разделитель разрядов выключен — включаем.
static QLocale numberLocale()
{
    QLocale c = QLocale::c();
    c.setNumberOptions(QLocale::DefaultNumberOptions);
    return c;
}

QString NewLinesIndicator::label(qint64 count)
{
    const QLocale c = numberLocale();
    QString number;
    if (count < 100000) {
        number = c.toString(count);
    } else if (count < 1000000) {
        number = c.toString(count / 1000) + QLatin1Char('K');
    } else if (count < 10000000) {
        // Одна цифра после точки, без округления вверх: 9 999 999 — «9.9M».
        const qint64 tenths = count / 100000;
        number = c.toString(tenths / 10) + QLatin1Char('.') + c.toString(tenths % 10)
               + QLatin1Char('M');
    } else {
        number = c.toString(count / 1000000) + QLatin1Char('M');
    }
    return QStringLiteral("↓ %1 new %2").arg(number, count == 1 ? tr("line") : tr("lines"));
}

void NewLinesIndicator::setCount(qint64 count)
{
    if (m_count == count)
        return;
    m_count = count;
    setText(label(count));
    setToolTip(tr("%1 new line(s) below. Click to go to the first one.")
                   .arg(numberLocale().toString(count)));
    if (sizeHint() != size())
        resize(sizeHint());
    update();
}

QSize NewLinesIndicator::sizeHint() const
{
    const QFontMetrics fm(font());
    // Поля — как у кнопок CompactStyle: плотно, но кнопкой.
    const int hMargin = style()->pixelMetric(QStyle::PM_ButtonMargin, nullptr, this) + fm.height() / 2;
    const int vMargin = qMax(3, fm.height() / 4);
    return QSize(fm.horizontalAdvance(text()) + 2 * hMargin, fm.height() + 2 * vMargin);
}

void NewLinesIndicator::changeEvent(QEvent* event)
{
    QAbstractButton::changeEvent(event);
    // Размер меняется на месте; заново ставит плашку view — на ближайшем
    // пересчёте геометрии или изменении счёта.
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
        resize(sizeHint());
}

void NewLinesIndicator::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QPalette& pal = palette();
    QColor fill = pal.color(QPalette::Highlight);
    if (isDown())
        fill = fill.darker(120);
    else if (underMouse())
        fill = fill.lighter(112);

    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal radius = r.height() / 2;
    QPainterPath path;
    path.addRoundedRect(r, radius, radius);
    painter.fillPath(path, fill);
    // Тонкая кромка цветом окна отделяет плашку от строк лога любой темы.
    painter.setPen(QPen(pal.color(QPalette::Window), 1));
    painter.drawPath(path);

    painter.setPen(pal.color(QPalette::HighlightedText));
    painter.drawText(rect(), Qt::AlignCenter, text());
}
