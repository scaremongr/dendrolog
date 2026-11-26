#pragma once

#include <QColor>
#include <QList>
#include <QString>

// Токен подсветки — цветовой диапазон [start, end) в исходной строке.
struct HighlightToken {
    int    start;
    int    end;
    QColor color;    // цвет текста;  QColor() = использовать цвет строки по умолчанию
    QColor bgColor;  // цвет фона;    QColor() = без заливки (для поиска с подсветкой)
    // Начертание (F-3, стиль ANSI): AnsiSpan::Bold / Italic / Underline.
    quint8 fontAttrs = 0;
};

struct AnsiSpan;
class QPalette;

// Однопроходный O(n) сканер синтаксической подсветки лог-строк.
// Без регулярных выражений, без состояния (потокобезопасен).
//
// Чтобы добавить новое правило:
//   1. Написать функцию  int ruleXxx(...)  в syntaxhighlighter.cpp.
//   2. Добавить её в массив constexpr kRules[].
namespace SyntaxHighlighter {
    QList<HighlightToken> tokenize(const QString& text);

    // Раскраска строки с цветом ANSI (F-3): участки спанов ANSI — их цветом и
    // начертанием (цвета — AppTheme::ansiColor, текст читаем на своём фоне,
    // dim — смесь с фоном, inverse — обмен), токены syntax — только в
    // промежутках между ними. Результат отсортирован и не пересекается.
    QList<HighlightToken> withAnsi(const QVector<AnsiSpan>& ansi,
                                   const QList<HighlightToken>& syntax,
                                   const QPalette& palette);
}
