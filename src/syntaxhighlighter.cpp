#include "syntaxhighlighter.h"
#include "apptheme.h"

// =============================================================================
// Правила подсветки синтаксиса
//
// Каждое правило — функция с сигнатурой ScanRule.
// Возвращает новую позицию (> pos) при совпадении, pos — при промахе.
// Пишет цвет в outColor только при совпадении.
//
// Приоритет (порядок в kRules):
//   log-keyword → quoted-string → URL → path → timestamp → GUID → hex → number
// =============================================================================
namespace {

// Цвета загружаются один раз из AppTheme::instance() при вызове tokenize(),
// затем передаются в каждое правило — без повторных обращений к синглтону.
struct RuleColors {
    QColor string;
    QColor number;   // десятичные числа, IP-адреса
    QColor hex;      // шестнадцатеричные литералы
    QColor url;
    QColor path;
    QColor guid;     // UUID/GUID
    QColor timestamp;// временные метки
    // ключевые слова уровней лога
    QColor kwFatal;
    QColor kwError;
    QColor kwWarn;
    QColor kwInfo;
    QColor kwDebug;
    QColor kwTrace;
};

using ScanRule = int(*)(const QString& text, int pos, const RuleColors& colors, QColor& outColor);

// Вспомогательная: pos является началом токена (не внутри идентификатора)?
inline bool isTokenStart(const QString& text, int pos)
{
    if (pos == 0) return true;
    const QChar prev = text[pos - 1];
    return !prev.isLetterOrNumber() && prev != '_';
}

// ── Строка в кавычках: "…" или '…' ──────────────────────────────────────────
int ruleQuotedString(const QString& text, int pos, const RuleColors& colors, QColor& outColor)
{
    const QChar q = text[pos];
    if (q != '"' && q != '\'') return pos;
    const int len = text.length();
    for (int i = pos + 1; i < len; ++i) {
        if (text[i] == q && text[i - 1] != '\\') {
            outColor = colors.string;
            return i + 1;
        }
        if (text[i] == '\n') break;  // однострочные кавычки
    }
    return pos;  // незакрытая кавычка — не подсвечиваем
}

// ── URL: произвольная схема вида  scheme://… ─────────────────────────────────
int ruleUrl(const QString& text, int pos, const RuleColors& colors, QColor& outColor)
{
    if (!text[pos].isLetter()) return pos;
    const int len = text.length();

    // Scheme: [a-zA-Z][a-zA-Z0-9+\-.]*
    int schemeEnd = pos + 1;
    while (schemeEnd < len
           && (text[schemeEnd].isLetterOrNumber()
               || text[schemeEnd] == '+' || text[schemeEnd] == '-' || text[schemeEnd] == '.'))
        ++schemeEnd;

    // Должно следовать "://"
    if (schemeEnd + 2 >= len
        || text[schemeEnd]     != ':'
        || text[schemeEnd + 1] != '/'
        || text[schemeEnd + 2] != '/')
        return pos;

    // Тело: до первого пробела или символа, недопустимого в URL
    int i = schemeEnd + 3;
    while (i < len && !text[i].isSpace()
           && text[i] != '<' && text[i] != '>'
           && text[i] != '{' && text[i] != '}'
           && text[i] != '|'  && text[i] != '^') ++i;

    // Обрезаем типичную замыкающую пунктуацию
    while (i > schemeEnd + 3 && QStringLiteral(",;.)'\"<>]").contains(text[i - 1])) --i;

    if (i == schemeEnd + 3) return pos;  // пустое тело

    outColor = colors.url;
    return i;
}

// ── Шестнадцатеричный литерал: 0x[0-9a-fA-F]+ ───────────────────────────────
int ruleHexLiteral(const QString& text, int pos, const RuleColors& colors, QColor& outColor)
{
    if (text[pos] != '0' || !isTokenStart(text, pos)) return pos;
    const int len = text.length();
    if (pos + 1 >= len) return pos;
    const QChar x = text[pos + 1];
    if (x != 'x' && x != 'X') return pos;

    int i = pos + 2;
    while (i < len
           && (text[i].isDigit()
               || (text[i] >= 'a' && text[i] <= 'f')
               || (text[i] >= 'A' && text[i] <= 'F')))
        ++i;

    if (i == pos + 2) return pos;  // "0x" без цифр

    outColor = colors.hex;
    return i;
}

// ── Сырое hex-значение без префикса 0x: адреса, хеши, GUID ────────────────
// Требует: начало с цифры, не менее 6 символов, хотя бы одна hex-буква [a-fA-F].
int ruleRawHex(const QString& text, int pos, const RuleColors& colors, QColor& outColor)
{
    if (!text[pos].isDigit()) return pos;
    if (!isTokenStart(text, pos)) return pos;
    if (pos > 0 && text[pos - 1] == '.') return pos;

    const int len = text.length();
    int i = pos;
    bool hasHexLetter = false;
    while (i < len) {
        const QChar c = text[i];
        if (c.isDigit()) { ++i; continue; }
        if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) { hasHexLetter = true; ++i; continue; }
        break;
    }

    if (!hasHexLetter || (i - pos) < 6) return pos;
    if (i < len && (text[i].isLetterOrNumber() || text[i] == '_')) return pos;

    outColor = colors.hex;
    return i;
}

// ── Число: целое или вещественное / IP-адрес ─────────────────────────────────
// Не срабатывает внутри идентификаторов (foo42), после точки (foo.14bar),
// перед буквой (123TEXT) или вплотную к разделителям-склейкам (-, _, ,):
// строки вида "abc-123", "id_42", "1,234" не считаются числом.
// Точечная нотация без ограничения по числу точек покрывает IP-адреса (192.168.1.1).
inline bool isNumberGlue(QChar c)
{
    return c == '-' || c == '_' || c == ',';
}

int ruleNumber(const QString& text, int pos, const RuleColors& colors, QColor& outColor)
{
    if (!text[pos].isDigit()) return pos;
    if (!isTokenStart(text, pos)) return pos;
    if (pos > 0) {
        const QChar p = text[pos - 1];
        if (p == '.' || isNumberGlue(p)) return pos;  // .14 в foo.14bar, abc-123, id_42
    }

    const int len = text.length();
    int i = pos + 1;
    while (i < len && (text[i].isDigit() || text[i] == '.')) ++i;

    // Не включаем завершающую точку (напр. "42." → только "42")
    if (i > pos && text[i - 1] == '.') --i;

    // За числом не должно идти буквы/подчёркивания (123TEXT) или склеивающего
    // разделителя (123-456): это идентификатор/составной токен, а не число.
    if (i < len) {
        const QChar n = text[i];
        if (n.isLetter() || n == '_' || isNumberGlue(n)) return pos;
    }

    outColor = colors.number;
    return i;
}

// ── Путь к файлу: /unix/path, C:\win\path, ./rel/path ──────────────────
int ruleFilePath(const QString& text, int pos, const RuleColors& colors, QColor& outColor)
{
    // Старт пути должен быть началом токена: иначе "X" из "SOMETEX:\abc"
    // ошибочно опознаётся как драйв-буква.
    if (!isTokenStart(text, pos)) return pos;

    const QChar c0  = text[pos];
    const int   len = text.length();

    const bool isWinAbs = c0.isLetter() && pos + 2 < len
                          && text[pos + 1] == ':'
                          && (text[pos + 2] == '\\' || text[pos + 2] == '/');
    const bool isUnixAbs = c0 == '/' && pos + 1 < len && text[pos + 1] != '/';
    const bool isRelative = c0 == '.'
                            && pos + 1 < len
                            && (text[pos + 1] == '/'
                                || (text[pos + 1] == '.' && pos + 2 < len && text[pos + 2] == '/'));

    if (!isWinAbs && !isUnixAbs && !isRelative) return pos;

    const auto isPathChar = [](QChar c) {
        return c.isLetterOrNumber()
            || c == '/' || c == '\\' || c == '.'
            || c == '-' || c == '_'  || c == '@' || c == '~'
            || c == '(' || c == ')';  // скобки в путях: "Program Files (x86)"
        // ':' исключён — останавливаемся перед суффиксом вида :324
    };

    // Windows: драйв-префикс "C:\" / "C:/" уже проверен, сканируем с позиции pos+3
    const int scanStart = isWinAbs ? pos + 3 : pos + 1;
    int i = scanStart;
    // Пробел разрешён внутри пути, только если за ним сразу следует обычный
    // символ имени (буква/цифра): покрывает "C:\Program Files (x86)", но не
    // даёт пути «съесть» текст после завершающего пробела.
    while (i < len) {
        if (isPathChar(text[i])) { ++i; continue; }
        if (text[i] == ' ' && i + 1 < len
            && (text[i + 1].isLetterOrNumber())) { ++i; continue; }
        break;
    }

    if (isWinAbs) {
        if (i < pos + 4) return pos;  // минимум C:\a
    } else if (isRelative) {
        if (i < pos + 4) return pos;  // минимум ./a
    } else {
        // Unix absolute: требуем хотя бы один внутренний разделитель (/a/b)
        bool hasInternal = false;
        for (int j = pos + 1; j < i; ++j) {
            if (text[j] == '/' || text[j] == '\\') { hasInternal = true; break; }
        }
        if (!hasInternal) return pos;
    }

    // Обрезаем замыкающую пунктуацию и пробелы
    while (i > pos + 1 && QStringLiteral(".,;: ").contains(text[i - 1])) --i;

    outColor = colors.path;
    return i;
}

// ── Вспомогательная: hex-цифра ──────────────────────────────────────────────
inline bool isHexDigit(QChar c)
{
    return c.isDigit() || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// ── UUID / GUID: 8-4-4-4-12 hex, опционально в фигурных скобках {…} ──────────
// Пример: 550e8400-e29b-41d4-a716-446655440000
int ruleGuid(const QString& text, int pos, const RuleColors& colors, QColor& outColor)
{
    if (!isTokenStart(text, pos)) return pos;
    const int len = text.length();

    int p = pos;
    const bool brace = (text[p] == '{');
    if (brace) ++p;

    // Читает ровно n hex-цифр, продвигая p; иначе false.
    const auto readHex = [&](int n) -> bool {
        if (p + n > len) return false;
        for (int k = 0; k < n; ++k)
            if (!isHexDigit(text[p + k])) return false;
        p += n;
        return true;
    };
    const auto dash = [&]() -> bool {
        if (p < len && text[p] == '-') { ++p; return true; }
        return false;
    };

    if (!readHex(8) || !dash()
        || !readHex(4) || !dash()
        || !readHex(4) || !dash()
        || !readHex(4) || !dash()
        || !readHex(12))
        return pos;

    if (brace) {
        if (p < len && text[p] == '}') ++p;
        else return pos;
    }

    // За GUID не должно идти буквы/цифры/подчёркивания.
    if (p < len && (text[p].isLetterOrNumber() || text[p] == '_')) return pos;

    outColor = colors.guid;
    return p;
}

// ── Временная метка: дата и/или время ───────────────────────────────────────
// Поддерживает: 2024-01-15, 2024/01/15, 10:30:45, 10:30:45.123,
//               2024-01-15T10:30:45.123Z, "2024-01-15 10:30:45".
// Срабатывает раньше правила числа, чтобы метка не дробилась на куски.
int ruleTimestamp(const QString& text, int pos, const RuleColors& colors, QColor& outColor)
{
    if (!text[pos].isDigit()) return pos;
    if (!isTokenStart(text, pos)) return pos;

    const int len = text.length();
    int i = pos;

    // Читает ровно n десятичных цифр, продвигая i.
    const auto digits = [&](int n) -> bool {
        if (i + n > len) return false;
        for (int k = 0; k < n; ++k)
            if (!text[i + k].isDigit()) return false;
        i += n;
        return true;
    };

    // Время HH:MM:SS[.ms | ,ms][Z | ±HH[:MM]] начиная с i.
    const auto parseTime = [&]() -> bool {
        const int start = i;
        if (!digits(2)) { i = start; return false; }
        if (i >= len || text[i] != ':') { i = start; return false; } ++i;
        if (!digits(2)) { i = start; return false; }
        if (i >= len || text[i] != ':') { i = start; return false; } ++i;
        if (!digits(2)) { i = start; return false; }
        // Дробная часть секунд: .123 или ,123
        if (i < len && (text[i] == '.' || text[i] == ',')) {
            const int dot = i; ++i;
            const int fs = i;
            while (i < len && text[i].isDigit()) ++i;
            if (i == fs) i = dot;  // точка без цифр — откатываем
        }
        // Часовой пояс
        if (i < len && (text[i] == 'Z' || text[i] == 'z')) {
            ++i;
        } else if (i < len && (text[i] == '+' || text[i] == '-')) {
            const int tz = i; ++i;
            if (digits(2)) {
                if (i < len && text[i] == ':') ++i;
                digits(2);  // минуты пояса — необязательно
            } else {
                i = tz;
            }
        }
        return true;
    };

    bool matched = false;

    // Дата YYYY[-/]MM[-/]DD?
    if (pos + 4 < len && (text[pos + 4] == '-' || text[pos + 4] == '/')) {
        const QChar sep = text[pos + 4];
        i = pos;
        if (digits(4)
            && i < len && text[i] == sep && (++i, digits(2))
            && i < len && text[i] == sep && (++i, digits(2))) {
            matched = true;
            // Необязательное время после 'T' или пробела
            if (i < len && (text[i] == 'T' || text[i] == ' ')) {
                const int save = i; ++i;
                if (!parseTime()) i = save;
            }
        } else {
            i = pos;
        }
    }

    // Время без даты HH:MM:SS
    if (!matched) {
        i = pos;
        matched = parseTime();
    }

    if (!matched) return pos;

    // За меткой не должно идти буквы/цифры/подчёркивания.
    if (i < len && (text[i].isLetterOrNumber() || text[i] == '_')) return pos;

    outColor = colors.timestamp;
    return i;
}

// ── Ключевые слова уровней лога: FATAL, ERROR, WARN(ING), INFO, DEBUG, TRACE ─────
int ruleLogKeyword(const QString& text, int pos, const RuleColors& colors, QColor& outColor)
{
    if (!isTokenStart(text, pos)) return pos;
    const QChar c0 = text[pos];
    if (c0 != 'D' && c0 != 'E' && c0 != 'F' && c0 != 'I' && c0 != 'T' && c0 != 'W')
        return pos;

    const int len = text.length();
    // Проверяет слово kw в позиции pos; возвращает конец токена или -1
    const auto tryKw = [&](QLatin1String kw, const QColor& col) -> int {
        const int kwLen = kw.size();
        if (pos + kwLen > len) return -1;
        if (QStringView(text).sliced(pos, kwLen) != kw) return -1;
        const int end = pos + kwLen;
        if (end < len && (text[end].isLetterOrNumber() || text[end] == '_')) return -1;
        outColor = col;
        return end;
    };

    int r;
    if ((r = tryKw(QLatin1String("FATAL"),   colors.kwFatal)) >= 0) return r;
    if ((r = tryKw(QLatin1String("ERROR"),   colors.kwError)) >= 0) return r;
    if ((r = tryKw(QLatin1String("WARNING"), colors.kwWarn))  >= 0) return r;
    if ((r = tryKw(QLatin1String("WARN"),    colors.kwWarn))  >= 0) return r;
    if ((r = tryKw(QLatin1String("INFO"),    colors.kwInfo))  >= 0) return r;
    if ((r = tryKw(QLatin1String("DEBUG"),   colors.kwDebug)) >= 0) return r;
    if ((r = tryKw(QLatin1String("TRACE"),   colors.kwTrace)) >= 0) return r;
    return pos;
}

// ── Реестр правил (порядок = приоритет) ─────────────────────────────────────
constexpr ScanRule kRules[] = {
    ruleLogKeyword,    // высший приоритет: ERROR/WARN/… до кавычек
    ruleQuotedString,
    ruleUrl,
    ruleFilePath,
    ruleTimestamp,     // до GUID/hex/числа: метка не должна дробиться
    ruleGuid,          // до rawHex: GUID содержит дефисы, hex — нет
    ruleHexLiteral,
    ruleRawHex,        // сырой hex без 0x: адреса, хеши (≥06 символов + hex-буква)
    ruleNumber,
};

} // namespace

// =============================================================================
namespace SyntaxHighlighter {

QList<HighlightToken> tokenize(const QString& text)
{
    const AppTheme& theme = AppTheme::instance();
    const RuleColors colors {
        theme.syntaxString,
        theme.syntaxNumber,
        theme.syntaxHex,
        theme.syntaxUrl,
        theme.syntaxPath,
        theme.syntaxGuid,
        theme.syntaxTime,
        theme.logFatal,
        theme.logError,
        theme.logWarn,
        theme.logInfo,
        theme.logDebug,
        theme.logTrace,
    };

    QList<HighlightToken> result;
    const int len = text.length();
    int pos = 0;
    while (pos < len) {
        QColor color;
        bool matched = false;
        for (const ScanRule rule : kRules) {
            const int newPos = rule(text, pos, colors, color);
            if (newPos > pos) {
                result.append({pos, newPos, color});
                pos = newPos;
                matched = true;
                break;
            }
        }
        if (!matched) ++pos;
    }
    return result;
}

} // namespace SyntaxHighlighter
