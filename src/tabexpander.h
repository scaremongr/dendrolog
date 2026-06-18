#ifndef TABEXPANDER_H
#define TABEXPANDER_H

#include <QStringView>
#include <QString>
#include <QChar>
#include <algorithm>

// ---------------------------------------------------------------------------
// TabExpander — раскрытие табуляции в визуальные колонки моноширинной сетки.
//
// Вся геометрия LogListView меряет текст в «колонках» — ячейках шириной в один
// моноширинный символ (m_charWidth пикселей). Обычный символ занимает ровно
// одну колонку, табуляция продвигает позицию до следующего табстопа (кратного
// tabWidth колонок). '\r' и прочие управляющие символы трактуются как одна
// колонка и рисуются пробелом.
//
// Класс — ЕДИНСТВЕННЫЙ источник правила табстопа (см. advance()). Логика чистая,
// без пикселей и без зависимости от шрифта/виджета: тривиально тестируется и
// переиспользуется. Все измерения ведутся от колонки 0 переданного QStringView;
// wrapped-фрагменты меряются каждый независимо от своего начала — это сохраняет
// согласованность отрисовки, hit-test и выделения (все используют один и тот же
// QStringView с одинаковым началом отсчёта).
// ---------------------------------------------------------------------------
class TabExpander {
public:
    explicit TabExpander(int tabWidth = 4) noexcept : m_tabWidth(std::max(1, tabWidth)) {}

    int tabWidth() const noexcept { return m_tabWidth; }

    // Продвижение колонки одним символом — единственное место с правилом табстопа.
    int advance(int column, QChar ch) const noexcept {
        if (ch == u'\t')
            return column - (column % m_tabWidth) + m_tabWidth;
        return column + 1;
    }

    // Колонка, с которой начинается символ index (== число визуальных колонок,
    // занятых текстом [0, index)). index зажимается в [0, size].
    int columnAt(QStringView text, int index) const noexcept {
        const int n = std::clamp(index, 0, int(text.size()));
        int col = 0;
        for (int i = 0; i < n; ++i)
            col = advance(col, text[i]);
        return col;
    }

    // Полное число визуальных колонок строки.
    int columns(QStringView text) const noexcept {
        return columnAt(text, int(text.size()));
    }

    // Индекс символа, ближайшего к визуальной колонке (snap к границе символа) —
    // для перевода координаты мыши в позицию курсора.
    int indexAtColumn(QStringView text, qreal column) const noexcept {
        if (column <= 0.0) return 0;
        int col = 0;
        const int n = int(text.size());
        for (int i = 0; i < n; ++i) {
            const int next = advance(col, text[i]);
            if (column < next)  // колонка попала в ячейку символа i — [col, next)
                return (column - col <= next - column) ? i : i + 1;
            col = next;
        }
        return n;
    }

    // Раскрывает табуляцию фрагмента в пробелы так, чтобы при отрисовке одной
    // строкой (QPainter::drawText) глифы попали ровно в колонки сетки. startColumn —
    // колонка, в которой начинается фрагмент: нужна для корректного выравнивания
    // табстопов, когда фрагмент рисуется не с начала строки. '\r' → пробел.
    QString expand(QStringView text, int startColumn = 0) const {
        QString out;
        out.reserve(text.size());
        int col = startColumn;
        for (QChar ch : text) {
            if (ch == u'\t') {
                const int stop = col - (col % m_tabWidth) + m_tabWidth;
                out.append(QString(stop - col, u' '));
                col = stop;
            } else if (ch == u'\r') {
                out.append(u' ');
                ++col;
            } else {
                out.append(ch);
                ++col;
            }
        }
        return out;
    }

private:
    int m_tabWidth;
};

// ---------------------------------------------------------------------------
// ColumnCursor — курсор для МОНОТОННОГО обхода строки слева направо.
//
// При отрисовке строки X каждого цветного сегмента запрашивается в порядке
// возрастания индекса. Курсор продвигается только вперёд, поэтому суммарная
// стоимость всех запросов columnAt() за один проход — O(длина строки), а не
// O(длина × число токенов), как было бы при независимом вызове TabExpander.
// ---------------------------------------------------------------------------
class ColumnCursor {
public:
    ColumnCursor(QStringView text, const TabExpander& expander) noexcept
        : m_text(text), m_expander(expander) {}

    // Колонка символа index. Требование: index не убывает между вызовами.
    int columnAt(int index) noexcept {
        const int n = std::min(index, int(m_text.size()));
        while (m_index < n) {
            m_column = m_expander.advance(m_column, m_text[m_index]);
            ++m_index;
        }
        return m_column;
    }

private:
    QStringView m_text;
    const TabExpander& m_expander;
    int m_index = 0;
    int m_column = 0;
};

#endif // TABEXPANDER_H
