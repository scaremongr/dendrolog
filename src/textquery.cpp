#include "textquery.h"

#include "userregex.h"

namespace {

// Символ слова — как TextToken::isWordChar (QChar::isLetterOrNumber или '_'):
// буквы с их диакритикой, цифры, подчёркивание.
const QLatin1String kNotAfterWord("(?<![\\p{L}\\p{M}\\p{N}_])");
const QLatin1String kNotBeforeWord("(?![\\p{L}\\p{M}\\p{N}_])");

} // namespace

QString TextQuery::patternText() const
{
    if (!wholeWord)
        return text;
    return kNotAfterWord + QStringLiteral("(?:")
         + (regex ? text : QRegularExpression::escape(text))
         + QLatin1Char(')') + kNotBeforeWord;
}

bool TextQuery::isValid(QString* error, int* offset) const
{
    if (error)
        error->clear();
    if (offset)
        *offset = -1;
    if (text.isEmpty() || !needsRegex())
        return true;
    // Сначала текст пользователя сам по себе: позиция ошибки — в том, что он
    // набрал, а не в выражении с границами слова.
    if (regex) {
        const QRegularExpression own = UserRegex::compile(text, caseSensitivity());
        if (!own.isValid()) {
            if (error)
                *error = own.errorString();
            if (offset)
                *offset = int(own.patternErrorOffset());
            return false;
        }
    }
    if (wholeWord) {
        // Выражение, годное само по себе, может не пережить обёртку
        // (незакрытое «\Q…» съедает её скобку) — позиции у такой ошибки нет.
        const QRegularExpression wrapped = UserRegex::compile(patternText(), caseSensitivity());
        if (!wrapped.isValid()) {
            if (error)
                *error = wrapped.errorString();
            return false;
        }
    }
    return true;
}

bool TextMatcher::hasNonEmptyMatch(QStringView line) const
{
    // Редкий путь: выражение, умеющее совпадать с пустым, — ищем непустое.
    auto it = m_regex.globalMatchView(line);
    while (it.hasNext()) {
        if (it.next().capturedLength() > 0)
            return true;
    }
    return false;
}

TextMatcher::TextMatcher(const TextQuery& query)
    : m_text(query.text)
    , m_cs(query.caseSensitivity())
    , m_useRegex(query.needsRegex())
{
    if (query.isEmpty())
        return;
    if (m_useRegex) {
        // Тот же диалект, что у Text Filters, подсветки и сканера каталога.
        m_regex = UserRegex::compile(query.patternText(), query.caseSensitivity());
        // Компиляция (и JIT) — уже здесь, в compile(), а не на первой строке скана.
        m_usable = m_regex.isValid();
    } else {
        m_usable = true;
    }
}
