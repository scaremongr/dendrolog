#ifndef LINEWRAPPER_H
#define LINEWRAPPER_H

#include <QStringView>
#include <QChar>
#include <algorithm>
#include "tabexpander.h"

// ---------------------------------------------------------------------------
// LineWrapper — перенос строки по словам/конструкциям (word wrap).
//
// Разбивает логическую строку на визуальные строки шириной не более colsPerLine
// колонок. Ширина считается в КОЛОНКАХ через TabExpander, поэтому табуляция
// учитывается корректно. Перенос выполняется по ГРАНИЦАМ СЛОВ: точка переноса
// разрешена только ПОСЛЕ «не-словесного» символа (пробел, пунктуация, слэши,
// скобки, любой разделитель). Непрерывная последовательность словесных символов
// (буквы / цифры / '_') не разрывается. Исключения, обеспечивающие прогресс:
//   * слово шире строки и не влезает целиком → ломается посимвольно (break-anywhere);
//   * ведущий отступ строки (пробелы/табы до первого слова) не создаёт точку
//     переноса — иначе первое слово оторвалось бы от своего отступа.
//
// Класс — единственный источник ПРАВИЛА переноса (isWordChar). Чистая логика без
// пикселей/виджета: тривиально тестируется и переиспользуется. Геометрия (rect,
// Y фрагментов) остаётся на стороне вызывающего: wrap() лишь сообщает диапазоны
// [start, length] через callback, без промежуточных аллокаций.
// ---------------------------------------------------------------------------
class LineWrapper {
public:
    explicit LineWrapper(const TabExpander& expander) noexcept : m_expander(expander) {}

    // Символ, который НЕ разрывает слово (буква, цифра, '_'). Перенос возможен
    // только после символов, для которых это false.
    static bool isWordChar(QChar ch) noexcept {
        return ch.isLetterOrNumber() || ch == u'_';
    }

    // Разбивает text на визуальные строки и вызывает sink(start, length) для
    // каждой по порядку. Диапазоны идут подряд и покрывают весь текст без
    // пропусков. colsPerLine — максимальная ширина строки в колонках (>= 1).
    // ВАЖНО: callback назван sink, а не emit — emit является макросом Qt
    // (раскрывается в пустоту), из-за чего вызовы попросту исчезали бы.
    template <typename Sink>
    void wrap(QStringView text, int colsPerLine, Sink&& sink) const {
        const int len = int(text.size());
        const int maxCols = std::max(1, colsPerLine);
        int pos = 0;
        while (pos < len) {
            int col = 0;          // ширина набираемой визуальной строки в колонках
            int breakPos = -1;    // куда переносить: начало след. строки (после разделителя)
            bool sawWord = false; // встречался ли словесный символ в этой строке
            int i = pos;
            while (i < len) {
                const QChar ch = text[i];
                const int next = m_expander.advance(col, ch);
                if (next > maxCols && i > pos)
                    break;        // символ не влезает — закрываем визуальную строку
                col = next;
                if (isWordChar(ch))
                    sawWord = true;
                else if (sawWord)
                    breakPos = i + 1;  // точку переноса даёт только разделитель ПОСЛЕ слова
                ++i;
            }

            int lineEnd;
            if (i >= len)
                lineEnd = len;            // последняя строка — остаток влез целиком
            else if (breakPos > pos)
                lineEnd = breakPos;       // нормальный перенос по границе слова
            else
                lineEnd = i;              // длинное слово / отступ — break-anywhere

            sink(pos, lineEnd - pos);
            pos = lineEnd;
        }
    }

private:
    const TabExpander& m_expander;
};

#endif // LINEWRAPPER_H
