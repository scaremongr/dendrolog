#include "texttoken.h"
#include "patternheuristics.h"

#include <QRegularExpression>
#include <QStringView>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// Детекторы токенов для двойного клика
//
// Каждая функция возвращает {start, end} при успехе или {-1, -1} при промахе.
// "end" — позиция за последним символом (полуоткрытый интервал, как в Qt).
//
// Приоритет: quoted-string > URL > file-path > hex-literal > IPv4 >
//            simple-filename > decimal-number > word (fallback)
// ─────────────────────────────────────────────────────────────────────────────
namespace {

bool isWordChar(QChar c)    { return c.isLetterOrNumber() || c == '_'; }
bool isUrlBodyChar(QChar c) {
    static const QString kExtra = QStringLiteral("_-~./?#[]@!$&()*+,;=:%");
    return c.isLetterOrNumber() || kExtra.contains(c);
}

// Расширяет диапазон влево и вправо от pos, пока pred == true.
template<typename Pred>
std::pair<int,int> expandFromPos(const QString& text, int pos, Pred pred)
{
    int start = pos, end = pos;
    while (start > 0             && pred(text[start - 1])) --start;
    while (end   < text.length() && pred(text[end]))       ++end;
    return {start, end};
}

// ── Строка в кавычках: "..." или '...' (включая сами кавычки) ───────────────
std::pair<int,int> detectQuotedString(const QString& text, int pos)
{
    if (pos >= text.length()) return {-1, -1};

    const auto findClosing = [&](int open, QChar q) -> std::pair<int,int> {
        for (int i = open + 1; i < text.length(); ++i) {
            if (text[i] == q && (i == 0 || text[i - 1] != '\\')) return {open, i + 1};
            if (text[i] == '\n') break;
        }
        return {-1, -1};
    };

    const QChar ch = text[pos];
    if (ch == '"' || ch == '\'') return findClosing(pos, ch);

    // Поиск открывающей кавычки влево (в пределах строки)
    for (int i = pos - 1; i >= 0; --i) {
        const QChar c = text[i];
        if (c == '\n') break;
        if (c == '"' || c == '\'') return findClosing(i, c);
    }
    return {-1, -1};
}

// ── URL: произвольная схема вида  scheme://… ─────────────────────────────────
std::pair<int,int> detectUrl(const QString& text, int pos)
{
    if (pos >= text.length()) return {-1, -1};
    auto [start, end] = expandFromPos(text, pos, isUrlBodyChar);
    if (start == end) return {-1, -1};

    const QStringView candidate = QStringView{text}.mid(start, end - start);
    static const QRegularExpression kSchemeRe(QStringLiteral("[a-zA-Z][a-zA-Z0-9+\\-.]*://"));
    if (!kSchemeRe.match(candidate).hasMatch()) return {-1, -1};

    // Обрезаем замыкающую пунктуацию, которая не является частью URL
    while (end > start && QStringLiteral(",;.)'\"<>").contains(text[end - 1])) --end;
    return {start, end};
}

// ── Путь к файлу: содержит '/' или '\' (либо Windows-абсолютный путь) ────────
std::pair<int,int> detectFilePath(const QString& text, int pos)
{
    if (pos >= text.length()) return {-1, -1};

    const auto isPathChar = [](QChar c) {
        return c.isLetterOrNumber() || QStringLiteral("_-.~/\\:@").contains(c);
    };
    auto [start, end] = expandFromPos(text, pos, isPathChar);
    if (start == end) return {-1, -1};

    const QString candidate = text.mid(start, end - start);
    const bool hasSlash     = candidate.contains('/') || candidate.contains('\\');
    const bool isWinAbsPath = candidate.length() >= 3
                              && candidate[1] == ':'
                              && (candidate[2] == '/' || candidate[2] == '\\');
    if (!hasSlash && !isWinAbsPath) return {-1, -1};

    // Обрезаем замыкающую пунктуацию, которая не входит в путь
    while (end > start && QStringLiteral(".,;:").contains(text[end - 1])) --end;
    // Обрезаем суффикс номера строки вида ":324"
    int e = end;
    while (e > start && text[e - 1].isDigit()) --e;
    if (e < end && e > start && text[e - 1] == ':') end = e - 1;

    // Если позиция клика оказалась в отрезанном суффиксе — это не путь
    if (pos >= end) return {-1, -1};

    return {start, end};
}

// ── Шестнадцатеричный литерал: [−]0x[0-9a-fA-F]+ ────────────────────────────
std::pair<int,int> detectHexLiteral(const QString& text, int pos)
{
    if (pos >= text.length()) return {-1, -1};
    const auto isHexOrPrefix = [](QChar c) {
        return (c >= '0' && c <= '9')
            || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')
            || c == 'x' || c == 'X';
    };
    if (!isHexOrPrefix(text[pos])) return {-1, -1};

    auto [start, end] = expandFromPos(text, pos, isHexOrPrefix);
    if (start > 0 && text[start - 1] == '-') --start;  // опциональный унарный минус

    const QString cand = text.mid(start, end - start);
    const int     base = cand.startsWith('-') ? 1 : 0;
    if (cand.length() > base + 2
        && cand[base] == '0'
        && (cand[base + 1] == 'x' || cand[base + 1] == 'X'))
    {
        return {start, end};
    }
    return {-1, -1};
}

// ── IPv4-адрес с опциональным портом: N.N.N.N[:port] ─────────────────────────
std::pair<int,int> detectIpAddress(const QString& text, int pos)
{
    if (pos >= text.length()) return {-1, -1};
    if (!text[pos].isDigit() && text[pos] != '.') return {-1, -1};

    auto [start, end] = expandFromPos(text, pos, [](QChar c) {
        return c.isDigit() || c == '.';
    });
    const QStringView cand = QStringView{text}.mid(start, end - start);
    static const QRegularExpression kIpv4(QStringLiteral("^(\\d{1,3}\\.){3}\\d{1,3}$"));
    if (!kIpv4.match(cand).hasMatch()) return {-1, -1};

    // Расширяем до :port если присутствует
    if (end < text.length() && text[end] == ':') {
        int portEnd = end + 1;
        while (portEnd < text.length() && text[portEnd].isDigit()) ++portEnd;
        if (portEnd > end + 1) end = portEnd;
    }
    return {start, end};
}

// ── Простое имя файла: слово.расширение (расш. = 1–5 букв) ──────────────────
std::pair<int,int> detectSimpleFilename(const QString& text, int pos)
{
    if (pos >= text.length()) return {-1, -1};

    const auto isFilenameChar = [](QChar c) {
        return c.isLetterOrNumber() || c == '_' || c == '-' || c == '.';
    };
    auto [start, end] = expandFromPos(text, pos, isFilenameChar);
    if (start == end) return {-1, -1};

    const QString cand     = text.mid(start, end - start);
    const int     lastDot  = cand.lastIndexOf('.');
    if (lastDot < 0) return {-1, -1};

    const QStringView ext = QStringView{cand}.mid(lastDot + 1);
    if (ext.isEmpty() || ext.length() > 5) return {-1, -1};
    for (const QChar c : ext) if (!c.isLetter()) return {-1, -1};

    return {start, end};
}

// ── Десятичное число: [-]цифры[.цифры] ───────────────────────────────────────
std::pair<int,int> detectDecimalNumber(const QString& text, int pos)
{
    if (pos >= text.length()) return {-1, -1};
    if (!text[pos].isDigit() && text[pos] != '.') return {-1, -1};

    auto [start, end] = expandFromPos(text, pos, [](QChar c) {
        return c.isDigit() || c == '.';
    });
    if (start > 0 && text[start - 1] == '-') --start;

    const QString cand = text.mid(start, end - start);
    const int     base = cand.startsWith('-') ? 1 : 0;
    if (cand.length() <= base || !cand[base].isDigit()) return {-1, -1};
    if (cand == QStringLiteral(".")) return {-1, -1};

    return {start, end};
}

// ── Таймстамп в различных форматах (опц. дата + обязательное время) ─────────
std::pair<int,int> detectTimestamp(const QString& text, int pos)
{
    if (pos >= text.length()) return {-1, -1};

    // Переиспользуем канонический regex таймстампа из эвристик схем, чтобы
    // распознавание здесь и в авто-детекте схемы не расходилось.
    static const QRegularExpression re(PatternHeuristics::timestampRegex());

    QRegularExpressionMatchIterator it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const int s = m.capturedStart();
        const int e = m.capturedEnd();
        if (pos >= s && pos < e) return {s, e};
        if (s > pos) break;  // совпадения идут по возрастанию — дальше уже мимо
    }
    return {-1, -1};
}

// ── Fallback: слово / пробельный прогон / группа пунктуации ──────────────────
//
// В отличие от текстовых редакторов прошлой версии, клик по пробелам НЕ
// перескакивает на следующее слово — выделяется сам пробельный прогон.
// Идущие подряд разделители (не-буквенно-цифровые, не-пробельные) выделяются
// группой, как принято в большинстве редакторов.
TextToken::Token detectWordOrSpace(const QString& text, int pos)
{
    if (pos >= text.length())
        return { pos, pos, TextToken::TokenType::None };

    const QChar ch = text[pos];

    if (ch.isSpace()) {
        auto [s, e] = expandFromPos(text, pos, [](QChar c) { return c.isSpace(); });
        return { s, e, TextToken::TokenType::Whitespace };
    }
    if (isWordChar(ch)) {
        auto [s, e] = expandFromPos(text, pos, isWordChar);
        return { s, e, TextToken::TokenType::Word };
    }
    // Пунктуация: группируем подряд идущие разделители.
    auto [s, e] = expandFromPos(text, pos,
                                [](QChar c) { return !isWordChar(c) && !c.isSpace(); });
    return { s, e, TextToken::TokenType::Punctuation };
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────

namespace TextToken {

// Главный диспетчер — обходит цепочку детекторов по приоритету.
Token findDoubleClickToken(const QString& text, int pos)
{
    pos = std::clamp(pos, 0, static_cast<int>(text.length()));
    std::pair<int,int> r;
    if ((r = detectQuotedString  (text, pos)).first >= 0) return { r.first, r.second, TokenType::QuotedString  };
    if ((r = detectTimestamp     (text, pos)).first >= 0) return { r.first, r.second, TokenType::Timestamp      };
    if ((r = detectUrl           (text, pos)).first >= 0) return { r.first, r.second, TokenType::Url            };
    if ((r = detectFilePath      (text, pos)).first >= 0) return { r.first, r.second, TokenType::FilePath       };
    if ((r = detectHexLiteral    (text, pos)).first >= 0) return { r.first, r.second, TokenType::HexLiteral     };
    if ((r = detectIpAddress     (text, pos)).first >= 0) return { r.first, r.second, TokenType::IpAddress      };
    if ((r = detectSimpleFilename(text, pos)).first >= 0) return { r.first, r.second, TokenType::SimpleFilename };
    if ((r = detectDecimalNumber (text, pos)).first >= 0) return { r.first, r.second, TokenType::DecimalNumber  };
    return detectWordOrSpace(text, pos);
}

// Классификация выделенного текста для контекстных действий.
TokenType classify(const QString& text)
{
    const QString t = text.trimmed();
    if (t.isEmpty() || t.contains(QChar('\n')))
        return TokenType::None;

    static const QRegularExpression urlRe(
        QRegularExpression::anchoredPattern(QStringLiteral("[a-zA-Z][a-zA-Z0-9+\\-.]*://\\S+")));
    if (urlRe.match(t).hasMatch())
        return TokenType::Url;

    static const QRegularExpression tsRe(
        QRegularExpression::anchoredPattern(PatternHeuristics::timestampRegex()));
    if (tsRe.match(t).hasMatch())
        return TokenType::Timestamp;

    // Абсолютный Windows-путь или строка, содержащая разделитель каталогов.
    static const QRegularExpression winAbsRe(
        QRegularExpression::anchoredPattern(QStringLiteral("[A-Za-z]:[\\\\/].*")));
    if (winAbsRe.match(t).hasMatch() || t.contains(QChar('/')) || t.contains(QChar('\\')))
        return TokenType::FilePath;

    return TokenType::None;
}

} // namespace TextToken
