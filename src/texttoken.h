#pragma once

#include <QString>

// Распознавание токенов под курсором при двойном клике + классификация уже
// выделенного текста (для контекстных действий: открыть ссылку, открыть файл,
// подставить таймстамп в фильтр по времени).
namespace TextToken {

enum class TokenType {
    None,
    Word,
    Whitespace,
    Punctuation,
    QuotedString,
    Url,
    FilePath,
    HexLiteral,
    IpAddress,
    SimpleFilename,
    DecimalNumber,
    Timestamp,
};

struct Token {
    int       start = -1;
    int       end   = -1;                 // полуоткрытый интервал [start, end)
    TokenType type  = TokenType::None;

    bool isEmpty() const { return start < 0 || start == end; }
};

// Границы токена под позицией pos при двойном клике.
// На пустом месте за концом строки возвращает пустой токен.
Token findDoubleClickToken(const QString& text, int pos);

// Классифицирует уже выделенную строку целиком (для контекстного меню).
// Возвращает Url / FilePath / Timestamp, либо None, если ничего не распознано.
TokenType classify(const QString& text);

} // namespace TextToken
