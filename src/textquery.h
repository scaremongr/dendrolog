#ifndef TEXTQUERY_H
#define TEXTQUERY_H

#include "textmatchhighlighter.h"

#include <QColor>
#include <QRegularExpression>
#include <QString>
#include <QStringView>

// ============================================================================
// TextQuery / TextMatcher — запрос быстрого поиска (поле в тулбаре, F3) и его
// предикат «строка подходит».
//
// Запрос — текст и три флага: регистр, «слово целиком», регулярное выражение.
// Семантика записана ОДИН раз, здесь, и сводится к паре «подстрока или
// регекс», которую понимают все потребители:
//   • скан строк (LogScanSnapshot::findRowWithText — фоновый QuickSearch и
//     синхронный LogModel::findOccurrence) — через TextMatcher;
//   • подсветка найденного в строке (LogListView::showSearchMatch) и правило
//     панели Search («Find All») — через patternText() + needsRegex().
// Поэтому строка, на которой остановился F3, — ровно та, где что-то
// подсвечено, и ровно та, что попадёт в список Find All.
//
// Регекс — тот же диалект и те же правила, что у правил Text Filters:
// UserRegex (\w, \d, \s, \b — по Юникоду), без якорей («найдено где угодно
// в строке»), из опций пользователя — только регистр.
//
// «Слово целиком»: совпадение не должно примыкать к символу слова ни слева,
// ни справа. Символ слова — буква, цифра или '_' (то же, что двойной клик
// считает словом, TextToken). Это просмотр назад/вперёд, а не \b: запрос
// «[error]» находится между пробелами, а \b требовал бы букву вплотную к
// скобке. Для запроса, который начинается и кончается буквой, правило
// совпадает с \b на тексте из букв любого алфавита без комбинируемых знаков
// (кириллица — тест user_regex). На комбинируемых знаках (ударение U+0301) и
// соединительной пунктуации кроме '_' они могут разойтись: состав \w с
// Юникод-классами зависит от версии PCRE2 (Mn — с 10.43). Пустое совпадение регекса не считается находкой. Работает и вместе с регексом: «\d+» целым словом — число, не
// примыкающее к буквам. Записывается регексом с границами (patternText()),
// поэтому передаётся дальше без отдельного флага.
// ============================================================================
struct TextQuery {
    QString text;
    bool caseSensitive = false;
    bool wholeWord = false;
    bool regex = false;

    TextQuery() = default;
    TextQuery(const QString& queryText, Qt::CaseSensitivity cs)
        : text(queryText), caseSensitive(cs == Qt::CaseSensitive) {}

    bool isEmpty() const { return text.isEmpty(); }
    Qt::CaseSensitivity caseSensitivity() const
    {
        return caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    }

    // Запрос как «подстрока или регекс»: needsRegex() — как читать
    // patternText(). Простой запрос — сам текст; «слово целиком» — регекс с
    // границами вокруг текста (экранированного, если это не регекс).
    bool needsRegex() const { return regex || wholeWord; }
    QString patternText() const;

    // Годен ли запрос к поиску. Невалидное регулярное выражение — false;
    // error — сообщение движка, offset — позиция ошибки в text (-1, если
    // позиции нет). Пустой запрос годен (искать по нему просто нечего).
    bool isValid(QString* error = nullptr, int* offset = nullptr) const;

    // Паттерн подсветки найденного (LogListView::showSearchMatch): заливает
    // ровно то, что нашёл TextMatcher.
    HighlightPattern highlightPattern(const QColor& color) const
    {
        HighlightPattern pattern;
        pattern.text = patternText();
        pattern.isRegex = needsRegex();
        pattern.caseSensitivity = caseSensitivity();
        pattern.color = color;
        return pattern;
    }

    bool operator==(const TextQuery& other) const
    {
        return text == other.text && caseSensitive == other.caseSensitive
            && wholeWord == other.wholeWord && regex == other.regex;
    }
    bool operator!=(const TextQuery& other) const { return !(*this == other); }
};

// Скомпилированный запрос. Создаётся один раз на скан; matches() можно звать
// из воркера (объект принадлежит одному потоку).
class TextMatcher {
public:
    explicit TextMatcher(const TextQuery& query);

    // false — запрос пуст или регекс не скомпилировался: matches() всегда false.
    bool isUsable() const { return m_usable; }
    // Пустое совпадение регекса («x*», «^») не считается: показать и
    // подсветить в строке нечего (TextMatchHighlighter его тоже пропускает).
    bool matches(QStringView line) const
    {
        if (!m_usable)
            return false;
        if (!m_useRegex)
            return line.contains(m_text, m_cs);
        const QRegularExpressionMatch first = m_regex.matchView(line);
        if (!first.hasMatch())
            return false;
        return first.capturedLength() > 0 || hasNonEmptyMatch(line);
    }

private:
    bool hasNonEmptyMatch(QStringView line) const;

    QString m_text;
    Qt::CaseSensitivity m_cs = Qt::CaseInsensitive;
    QRegularExpression m_regex;
    bool m_useRegex = false;
    bool m_usable = false;
};

#endif // TEXTQUERY_H
