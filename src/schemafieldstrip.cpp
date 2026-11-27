#include "schemafieldstrip.h"

#include "cardframe.h"
#include "highlightpalette.h"

#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

namespace {

constexpr int kPadX = 9;        // поля блока по горизонтали
constexpr int kPadY = 5;
constexpr int kBar = 4;         // цветная полоска слева
constexpr int kGap = 4;         // между блоками
constexpr int kRowGap = 6;      // между рядами
constexpr int kMargin = 2;
constexpr int kMaxNameChars = 28;

QFont nameFont(const QFont& base)
{
    QFont f = base;
    f.setBold(true);
    return f;
}

QFont captionFont(const QFont& base)
{
    QFont f = base;
    if (f.pointSizeF() > 0)
        f.setPointSizeF(qMax(7.0, f.pointSizeF() - 1.0));
    return f;
}

QFont literalFont(const QFont& base)
{
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (base.pointSizeF() > 0)
        f.setPointSizeF(base.pointSizeF());
    return f;
}

QString elided(const QString& text)
{
    return text.size() > kMaxNameChars ? text.left(kMaxNameChars - 1) + QChar(0x2026) : text;
}

// Элемент без колонки, который можно выбрать: текст между полями или
// скрытая часть строки.
bool pieceRectIndexValid(const QVector<SchemaFieldStrip::Item>& items, int piece)
{
    if (piece < 0)
        return false;
    for (const SchemaFieldStrip::Item& item : items) {
        if (item.type != SchemaFieldStrip::Item::Type::Add && item.piece == piece && item.column < 0)
            return true;
    }
    return false;
}

} // namespace

SchemaFieldStrip::SchemaFieldStrip(QWidget* parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
}

QColor SchemaFieldStrip::fieldColor(int column, const QPalette& palette)
{
    if (column < 0)
        return CardFrame::mixedColor(palette.color(QPalette::Base), palette.color(QPalette::Text), 0.10);
    return HighlightPalette::colorAt(column, palette.color(QPalette::Base));
}

QString SchemaFieldStrip::visibleLiteral(const QString& text)
{
    QString out = text;
    out.replace(QLatin1Char(' '), QChar(0x2423));    // ␣
    out.replace(QLatin1Char('\t'), QChar(0x21E5));   // ⇥
    return out;
}

void SchemaFieldStrip::setItems(const QVector<Item>& items)
{
    m_items = items;
    m_hover = -1;
    if (indexOfColumn(m_current) < 0)
        m_current = -1;
    if (!pieceRectIndexValid(m_items, m_currentPiece))
        m_currentPiece = -1;
    layoutItems(width(), &m_rects);
    updateGeometry();
    update();
}

void SchemaFieldStrip::setPlaceholderText(const QString& text)
{
    m_placeholder = text;
    update();
}

void SchemaFieldStrip::setCurrentColumn(int column)
{
    if (indexOfColumn(column) < 0)
        column = -1;
    if (m_current == column)
        return;
    m_current = column;
    update();
}

void SchemaFieldStrip::setCurrentPiece(int piece)
{
    if (!pieceRectIndexValid(m_items, piece))
        piece = -1;
    if (m_currentPiece == piece)
        return;
    m_currentPiece = piece;
    update();
}

QRect SchemaFieldStrip::pieceRect(int piece) const
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].type != Item::Type::Add && m_items[i].piece == piece && piece >= 0)
            return itemRect(i);
    }
    return QRect();
}

QRect SchemaFieldStrip::addRect() const
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].type == Item::Type::Add)
            return itemRect(i);
    }
    return QRect();
}

int SchemaFieldStrip::indexOfColumn(int column) const
{
    if (column < 0)
        return -1;
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].type == Item::Type::Field && m_items[i].column == column)
            return i;
    }
    return -1;
}

QRect SchemaFieldStrip::itemRect(int index) const
{
    return index >= 0 && index < m_rects.size() ? m_rects[index] : QRect();
}

QRect SchemaFieldStrip::columnRect(int column) const
{
    return itemRect(indexOfColumn(column));
}

int SchemaFieldStrip::itemAt(const QPoint& pos) const
{
    for (int i = 0; i < m_rects.size(); ++i) {
        if (m_rects[i].contains(pos))
            return i;
    }
    return -1;
}

QSize SchemaFieldStrip::itemSize(const Item& item) const
{
    const QFontMetrics name(nameFont(font()));
    const QFontMetrics caption(captionFont(font()));
    const int height = name.height() + caption.height() + 2 * kPadY;
    if (item.type == Item::Type::Literal) {
        const QFontMetrics mono(literalFont(font()));
        return QSize(mono.horizontalAdvance(visibleLiteral(item.text)) + 10, height);
    }
    if (item.type == Item::Type::Add)
        return QSize(QFontMetrics(font()).horizontalAdvance(QStringLiteral("+ ") + item.text) + 2 * kPadX, height);
    const QString shown = item.name.isEmpty() ? tr("(hidden)") : elided(item.name);
    QString sub = item.caption;
    if (item.optional)
        sub = sub.isEmpty() ? tr("optional") : tr("%1 · optional").arg(sub);
    const int text = qMax(name.horizontalAdvance(shown), caption.horizontalAdvance(sub));
    return QSize(kBar + 2 * kPadX + text, height);
}

int SchemaFieldStrip::layoutItems(int width, QVector<QRect>* rects) const
{
    if (rects)
        rects->resize(m_items.size());
    const int available = qMax(60, width - 2 * kMargin);
    int x = kMargin;
    int y = kMargin;
    int rowHeight = 0;
    for (int i = 0; i < m_items.size(); ++i) {
        const QSize size = itemSize(m_items[i]);
        // Литерал не начинает ряд в одиночку, если за ним поле: переносится
        // блок, а разделитель остаётся с предыдущим.
        if (x > kMargin && x + size.width() > kMargin + available) {
            x = kMargin;
            y += rowHeight + kRowGap;
            rowHeight = 0;
        }
        if (rects)
            (*rects)[i] = QRect(QPoint(x, y), size);
        x += size.width() + kGap;
        rowHeight = qMax(rowHeight, size.height());
    }
    if (m_items.isEmpty()) {
        const QFontMetrics name(nameFont(font()));
        const QFontMetrics caption(captionFont(font()));
        rowHeight = name.height() + caption.height() + 2 * kPadY;
    }
    return y + rowHeight + kMargin;
}

QSize SchemaFieldStrip::sizeHint() const
{
    return QSize(400, heightForWidth(width() > 0 ? width() : 400));
}

QSize SchemaFieldStrip::minimumSizeHint() const
{
    return QSize(120, layoutItems(100000, nullptr));
}

int SchemaFieldStrip::heightForWidth(int width) const
{
    return layoutItems(width, nullptr);
}

void SchemaFieldStrip::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    layoutItems(width(), &m_rects);
}

void SchemaFieldStrip::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QPalette pal = palette();
    const QColor muted = CardFrame::mutedTextColor(pal);
    const QColor border = CardFrame::mutedBorderColor(pal);

    if (m_items.isEmpty()) {
        p.setPen(muted);
        p.drawText(rect().adjusted(kMargin + 2, 0, -kMargin, 0), Qt::AlignVCenter | Qt::AlignLeft | Qt::TextWordWrap,
                   m_placeholder);
        return;
    }

    const QFont fName = nameFont(font());
    const QFont fCaption = captionFont(font());
    const QFont fLiteral = literalFont(font());
    const QFontMetrics mName(fName);
    const QFontMetrics mCaption(fCaption);

    for (int i = 0; i < m_items.size(); ++i) {
        const Item& item = m_items[i];
        const QRectF r = QRectF(m_rects[i]).adjusted(0.5, 0.5, -0.5, -0.5);
        if (item.type == Item::Type::Literal) {
            // Текст между полями — тоже элемент схемы: под курсором и выбранный
            // обведён, щелчок даёт его поправить.
            const bool chosen = item.piece == m_currentPiece && m_currentPiece >= 0;
            if (chosen || i == m_hover) {
                QPen pen(chosen ? pal.color(QPalette::Highlight) : border);
                pen.setWidthF(chosen ? 2.0 : 1.0);
                p.setPen(pen);
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(r.adjusted(0, 4, 0, -4), 3, 3);
            }
            p.setFont(fLiteral);
            p.setPen(i == m_hover || chosen ? pal.color(QPalette::Text) : muted);
            p.drawText(m_rects[i], Qt::AlignCenter, visibleLiteral(item.text));
            continue;
        }
        if (item.type == Item::Type::Add) {
            const bool hot = i == m_hover;
            QPen pen(hot ? pal.color(QPalette::Highlight) : border);
            pen.setStyle(Qt::DashLine);
            p.setPen(pen);
            p.setBrush(hot ? CardFrame::mixedColor(pal.color(QPalette::Window), pal.color(QPalette::Highlight), 0.12)
                           : QBrush(Qt::NoBrush));
            p.drawRoundedRect(r, 4, 4);
            p.setFont(font());
            p.setPen(hot ? pal.color(QPalette::Text) : muted);
            p.drawText(m_rects[i], Qt::AlignCenter, QStringLiteral("+ ") + item.text);
            continue;
        }
        const bool current = (item.column >= 0 && item.column == m_current)
                             || (item.column < 0 && item.piece >= 0 && item.piece == m_currentPiece);
        const bool hover = i == m_hover;
        const QColor fill = fieldColor(item.column, pal);
        // Заливка — цвет поля; без колонки — нейтральная и пунктиром.
        QPen pen(current ? pal.color(QPalette::Highlight) : hover ? pal.color(QPalette::Text) : border);
        pen.setWidthF(current ? 2.0 : 1.0);
        if (item.column < 0 || item.optional)
            pen.setStyle(Qt::DashLine);
        p.setPen(pen);
        p.setBrush(fill);
        p.drawRoundedRect(current ? r.adjusted(0.5, 0.5, -0.5, -0.5) : r, 4, 4);
        if (item.column >= 0) {
            // Полоска слева — тот же цвет гуще: блок узнаётся и на бледной теме.
            p.setPen(Qt::NoPen);
            p.setBrush(CardFrame::mixedColor(fill, pal.color(QPalette::Text), 0.35));
            p.drawRoundedRect(QRectF(r.left() + 1.5, r.top() + 3, kBar - 1, r.height() - 6), 1.5, 1.5);
        }
        const int textX = m_rects[i].left() + kBar + kPadX;
        const int textW = m_rects[i].width() - kBar - 2 * kPadX;
        QFont nf = fName;
        if (item.name.isEmpty())
            nf.setItalic(true);
        p.setFont(nf);
        p.setPen(item.column < 0 ? muted : pal.color(QPalette::Text));
        p.drawText(QRect(textX, m_rects[i].top() + kPadY, textW, mName.height()), Qt::AlignLeft | Qt::AlignVCenter,
                   item.name.isEmpty() ? tr("(hidden)") : elided(item.name));
        QString sub = item.caption;
        if (item.optional)
            sub = sub.isEmpty() ? tr("optional") : tr("%1 · optional").arg(sub);
        p.setFont(fCaption);
        p.setPen(CardFrame::mixedColor(pal.color(QPalette::Text), fill, 0.30));
        p.drawText(QRect(textX, m_rects[i].top() + kPadY + mName.height(), textW, mCaption.height()),
                   Qt::AlignLeft | Qt::AlignVCenter, sub);
    }

    if (hasFocus() && m_current >= 0) {
        const QRect r = columnRect(m_current);
        if (r.isValid()) {
            QPen pen(pal.color(QPalette::Highlight));
            pen.setStyle(Qt::DotLine);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(r).adjusted(-1.5, -1.5, 1.5, 1.5), 5, 5);
        }
    }
}

void SchemaFieldStrip::activate(int index)
{
    if (index < 0 || index >= m_items.size())
        return;
    const Item& item = m_items[index];
    if (item.type == Item::Type::Add) {
        emit addRequested();
        return;
    }
    if (item.type == Item::Type::Literal) {
        emit literalActivated(item.piece);
        return;
    }
    if (item.column >= 0) {
        setCurrentColumn(item.column);
        emit columnActivated(item.column);
    } else {
        emit pieceActivated(item.piece);
    }
}

void SchemaFieldStrip::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        setFocus(Qt::MouseFocusReason);
        activate(itemAt(event->pos()));
    }
    QWidget::mousePressEvent(event);
}

void SchemaFieldStrip::mouseMoveEvent(QMouseEvent* event)
{
    const int hover = itemAt(event->pos());
    if (hover != m_hover) {
        m_hover = hover;
        setCursor(hover >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void SchemaFieldStrip::leaveEvent(QEvent* event)
{
    if (m_hover != -1) {
        m_hover = -1;
        update();
    }
    QWidget::leaveEvent(event);
}

void SchemaFieldStrip::keyPressEvent(QKeyEvent* event)
{
    QVector<int> columns;   // индексы блоков с колонкой, по порядку
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].type == Item::Type::Field && m_items[i].column >= 0)
            columns.append(i);
    }
    const int at = int(columns.indexOf(indexOfColumn(m_current)));
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_Right: {
        if (columns.isEmpty())
            break;
        int next = at < 0 ? 0 : at + (event->key() == Qt::Key_Right ? 1 : -1);
        next = qBound(0, next, int(columns.size()) - 1);
        activate(columns[next]);
        return;
    }
    case Qt::Key_Home:
        if (!columns.isEmpty())
            activate(columns.first());
        return;
    case Qt::Key_End:
        if (!columns.isEmpty())
            activate(columns.last());
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        if (at >= 0)
            activate(columns[at]);
        else if (!columns.isEmpty())
            activate(columns.first());
        return;
    case Qt::Key_Plus:
    case Qt::Key_Insert:
        if (addRect().isValid()) {
            emit addRequested();
            return;
        }
        break;
    case Qt::Key_Menu:
        if (at >= 0) {
            emit menuRequested(m_current, mapToGlobal(columnRect(m_current).bottomLeft()));
            return;
        }
        break;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

void SchemaFieldStrip::contextMenuEvent(QContextMenuEvent* event)
{
    const int index = itemAt(event->pos());
    if (index >= 0 && m_items[index].type == Item::Type::Field && m_items[index].column >= 0) {
        setCurrentColumn(m_items[index].column);
        emit columnActivated(m_items[index].column);
        emit menuRequested(m_items[index].column, event->globalPos());
        event->accept();
        return;
    }
    QWidget::contextMenuEvent(event);
}

bool SchemaFieldStrip::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(event);
        const int index = itemAt(help->pos());
        if (index >= 0 && !m_items[index].toolTip.isEmpty()) {
            QToolTip::showText(help->globalPos(), m_items[index].toolTip, this, m_rects[index]);
            return true;
        }
        QToolTip::hideText();
        event->ignore();
        return true;
    }
    if (event->type() == QEvent::FontChange) {
        layoutItems(width(), &m_rects);
        updateGeometry();
    }
    return QWidget::event(event);
}
