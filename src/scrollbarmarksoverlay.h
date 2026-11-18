#ifndef SCROLLBARMARKSOVERLAY_H
#define SCROLLBARMARKSOVERLAY_H

#include "scrollbarmarks.h"

#include <QRect>
#include <QVector>
#include <QWidget>
#include <array>

class LogListView;
class QScrollBar;

// ============================================================================
// ScrollbarMarksOverlay — отрисовка отметок R-6 (docs/design/R-6.md) поверх
// вертикального скроллбара LogListView. Прозрачный для мыши дочерний виджет
// скроллбара: сам скроллбар — обычный QScrollBar под CompactStyle, его класс,
// стиль и метрики не меняются.
//
// Строки набора (ScrollbarMarks::marks) переводятся в пиксели дорожки:
// строка → значение скроллбара (LogListView::markScrollValue, без O(N)) →
// ScrollbarMarks::trackPosition. Пиксели кэшируются по ключу (набор,
// диапазон и шаги скроллбара, дорожка и ползунок, высота контента, число
// строк): прокрутка их не пересчитывает, смена размера или высот — да,
// O(отметок). Цвета — AppTheme, читаются при каждой отрисовке.
//
// Видимость скроллбара включает расчёт (ScrollbarMarks::setEnabled): скрытая
// вкладка или лог, целиком помещающийся на экран, отметок не считает.
// ============================================================================
class ScrollbarMarksOverlay : public QWidget {
    Q_OBJECT

public:
    ScrollbarMarksOverlay(LogListView* view, ScrollbarMarks* marks);

    // Геометрия дорожки сейчас (координаты скроллбара).
    QRect grooveRect() const;
    QRect sliderRect() const;
    ScrollbarTrack track() const;
    // Пиксели отметок категории — смещения от начала дорожки, по возрастанию,
    // без повторов (тесты; при нужде пересчитываются).
    const QVector<int>& pixels(ScrollbarMarkSet::Category category) const;
    // Полоса категории по ширине скроллбара: [left, right) в его координатах.
    QPair<int, int> lane(ScrollbarMarkSet::Category category) const;
    // Высота штриха отметки, px.
    static constexpr int kMarkHeight = 2;

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Key {
        quint64 serial = ~quint64(0);
        int maximum = -1;
        int pageStep = -1;
        QRect groove;
        int sliderLength = -1;
        int rows = -1;
        qint64 geometry = -1;
        bool operator==(const Key& o) const
        {
            return serial == o.serial && maximum == o.maximum && pageStep == o.pageStep
                && groove == o.groove && sliderLength == o.sliderLength && rows == o.rows
                && geometry == o.geometry;
        }
    };
    Key currentKey() const;
    void ensurePixels() const;
    void syncEnabled();

    LogListView* m_view;
    QScrollBar* m_bar;
    ScrollbarMarks* m_marks;
    mutable Key m_key;
    mutable std::array<QVector<int>, ScrollbarMarkSet::CategoryCount> m_pixels;
};

#endif // SCROLLBARMARKSOVERLAY_H
