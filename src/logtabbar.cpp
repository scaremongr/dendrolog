#include "logtabbar.h"

#include <QApplication>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>

LogTabBar::LogTabBar(QWidget* parent)
    : QTabBar(parent)
{
    // Сами разбираем drag заголовков (merge vs reorder), поэтому встроенное
    // перемещение выключено. Курсор-«рука» — привычный намёк, что вкладку
    // можно тянуть.
    setMovable(false);
    setCursor(Qt::ArrowCursor);
}

void LogTabBar::resetDragState()
{
    m_dragging    = false;
    m_pressIndex  = -1;
    m_mergeTarget = -1;
    m_insertIndex = -1;
}

void LogTabBar::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_pressIndex  = tabAt(event->pos());
        m_pressPos    = event->pos();
        m_dragging    = false;
        m_mergeTarget = -1;
        m_insertIndex = -1;
        // Выбор вкладки делаем сами (базовый обработчик не вызываем, чтобы не
        // тянуть его внутреннее состояние перетаскивания).
        if (m_pressIndex >= 0)
            setCurrentIndex(m_pressIndex);
        event->accept();
        return;
    }
    if (event->button() == Qt::MiddleButton) {
        m_middleIndex = tabAt(event->pos());
        event->accept();
        return;
    }
    QTabBar::mousePressEvent(event);
}

void LogTabBar::mouseMoveEvent(QMouseEvent* event)
{
    if (!(event->buttons() & Qt::LeftButton) || m_pressIndex < 0) {
        QTabBar::mouseMoveEvent(event);
        return;
    }

    if (!m_dragging) {
        if ((event->pos() - m_pressPos).manhattanLength()
                < QApplication::startDragDistance())
            return;
        m_dragging = true;
    }

    updateDropTarget(event->pos());
    update();
    event->accept();
}

void LogTabBar::updateDropTarget(const QPoint& pos)
{
    m_mergeTarget = -1;
    m_insertIndex = -1;

    const int n = count();
    if (n == 0)
        return;

    // Курсор над центральной зоной другой вкладки — это объединение.
    const int over = tabAt(pos);
    if (over >= 0 && over != m_pressIndex) {
        const QRect r = tabRect(over);
        const int margin = r.width() * 3 / 10;   // «центральная» зона = средние 40%
        if (pos.x() >= r.left() + margin && pos.x() <= r.right() - margin) {
            m_mergeTarget = over;
            return;
        }
    }

    // Иначе — переупорядочивание: слот вставки = число вкладок, чей центр
    // левее курсора.
    int slot = 0;
    for (int i = 0; i < n; ++i) {
        if (pos.x() > tabRect(i).center().x())
            slot = i + 1;
    }
    m_insertIndex = slot;
}

void LogTabBar::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton) {
        const int idx = tabAt(event->pos());
        if (idx >= 0 && idx == m_middleIndex)
            emit tabCloseRequested(idx);
        m_middleIndex = -1;
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        const bool wasDragging  = m_dragging;
        const int  from         = m_pressIndex;
        const int  mergeTarget  = m_mergeTarget;
        const int  insertIndex  = m_insertIndex;
        resetDragState();
        update();

        if (wasDragging && from >= 0) {
            if (mergeTarget >= 0 && mergeTarget != from) {
                emit mergeTabsRequested(from, mergeTarget);
            } else if (insertIndex >= 0) {
                // insertIndex — позиция в исходной нумерации; после «изъятия»
                // перетаскиваемой вкладки цель слева сдвигается на единицу.
                const int to = (from < insertIndex) ? insertIndex - 1 : insertIndex;
                if (to != from && to >= 0 && to < count())
                    moveTab(from, to);   // QTabWidget синхронизирует страницы
            }
        }
        event->accept();
        return;
    }

    QTabBar::mouseReleaseEvent(event);
}

void LogTabBar::contextMenuEvent(QContextMenuEvent* event)
{
    const int idx = tabAt(event->pos());
    if (idx >= 0) {
        emit tabContextMenuRequested(idx, event->globalPos());
        event->accept();
        return;
    }
    QTabBar::contextMenuEvent(event);
}

void LogTabBar::paintEvent(QPaintEvent* event)
{
    QTabBar::paintEvent(event);

    if (!m_dragging)
        return;

    QPainter p(this);
    const QColor accent = palette().color(QPalette::Highlight);

    if (m_mergeTarget >= 0) {
        // Объединение — подсветить целевую вкладку рамкой + лёгкой заливкой.
        const QRect r = tabRect(m_mergeTarget).adjusted(1, 1, -2, -1);
        QColor fill = accent;
        fill.setAlpha(60);
        p.fillRect(r, fill);
        p.setPen(QPen(accent, 2));
        p.drawRect(r);
    } else if (m_insertIndex >= 0) {
        // Переупорядочивание — вертикальная линия-вставка в слоте.
        const int n = count();
        int x;
        if (m_insertIndex >= n)
            x = tabRect(n - 1).right();
        else
            x = tabRect(m_insertIndex).left();
        p.setPen(QPen(accent, 2));
        p.drawLine(x, 0, x, height());
    }
}
