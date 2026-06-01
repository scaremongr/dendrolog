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
};

// Однопроходный O(n) сканер синтаксической подсветки лог-строк.
// Без регулярных выражений, без состояния (потокобезопасен).
//
// Чтобы добавить новое правило:
//   1. Написать функцию  int ruleXxx(...)  в syntaxhighlighter.cpp.
//   2. Добавить её в массив constexpr kRules[].
namespace SyntaxHighlighter {
    QList<HighlightToken> tokenize(const QString& text);
}
