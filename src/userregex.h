#ifndef USERREGEX_H
#define USERREGEX_H

#include <QRegularExpression>
#include <QString>

// ============================================================================
// UserRegex — регулярное выражение из текста, который ввёл ПОЛЬЗОВАТЕЛЬ.
//
// Одно место, где решено, на каком диалекте понимается регекс пользователя.
// Зовут все, кто компилирует его текст: правила Text Filters и панели Search
// (FilterRuleSet), быстрый поиск и Find All (TextQuery / TextMatcher),
// подсветка совпадений и Row Highlighters (TextMatchHighlighter), фильтр
// сканера каталога по содержимому (ContentQuery), проверка валидности в
// карточке правила (FilterPanelWidget). Поэтому строка, которую нашёл поиск,
// — ровно та, где есть подсветка, и ровно та, которую находит сканер.
//
// Диалект: QRegularExpression (PCRE2) без якорей; из опций — регистр и
// Юникод-классы:
//   • \w, \d, \s, \b и POSIX-классы ([[:alpha:]]) работают по Юникоду —
//     «пользовател\w+» находит кириллицу, \b стоит между буквой любого
//     алфавита и не-буквой. \d принимает и не-ASCII цифры (٣, ३);
//   • сворачивание регистра — по Юникоду (PCRE2 в UTF-режиме делает его и
//     без опции: «ОШИБКА» находит «ошибка»).
//
// НЕ для схем полей (LogPattern) и детект-регексов PatternHeuristics /
// LineClassifier: там ASCII-семантика закреплена эквивалентностью ручному
// сканеру и полями встроенных схем.
// ============================================================================
namespace UserRegex {

// Опции компиляции регекса пользователя.
QRegularExpression::PatternOptions options(Qt::CaseSensitivity cs);

// Скомпилированный (ещё не оптимизированный) регекс пользователя.
QRegularExpression compile(const QString& pattern, Qt::CaseSensitivity cs);

} // namespace UserRegex

#endif // USERREGEX_H
