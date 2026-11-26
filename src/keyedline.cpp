#include "keyedline.h"

#include <QtAlgorithms>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define KEYEDLINE_SSE2 1
#else
#define KEYEDLINE_SSE2 0
#endif

namespace {

inline bool isJsonSpace(QChar c)
{
    const char16_t u = c.unicode();
    return u == u' ' || u == u'\t' || u == u'\r' || u == u'\n';
}

inline bool isLogfmtSpace(QChar c)
{
    const char16_t u = c.unicode();
    return u == u' ' || u == u'\t' || u == u'\r' || u == u'\n';
}

// Ключ logfmt: всё выше пробела, кроме «=» и «"» (как у go-logfmt).
inline bool isLogfmtKeyChar(QChar c)
{
    const char16_t u = c.unicode();
    return u > u' ' && u != u'=' && u != u'"';
}

// Строка в кавычках с позиции открывающей; возвращает позицию за
// закрывающей или -1 (строка оборвана).
// Первая «"» или «\\» с позиции i (n — если нет). Горячий путь разбора: по
// 8 юнитов за шаг на SSE2 (x86-64 — всегда есть), остаток — по одному.
qsizetype findQuoteOrBackslash(const char16_t* d, qsizetype i, qsizetype n)
{
#if KEYEDLINE_SSE2
    const __m128i quote = _mm_set1_epi16(short(u'"'));
    const __m128i slash = _mm_set1_epi16(short(u'\\'));
    for (; i + 8 <= n; i += 8) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(d + i));
        const int mask = _mm_movemask_epi8(
            _mm_or_si128(_mm_cmpeq_epi16(v, quote), _mm_cmpeq_epi16(v, slash)));
        if (mask)
            return i + qCountTrailingZeroBits(quint32(mask)) / 2;
    }
#endif
    for (; i < n; ++i)
        if (d[i] == u'"' || d[i] == u'\\')
            return i;
    return n;
}

// Конец ключа logfmt с позиции i: первый юнит не isLogfmtKeyChar (≤ ' ',
// «=» или «"»); n — если нет. SSE2 — как findQuoteOrBackslash; сравнение
// «≤ ' '» беззнаковое: знаковое сравнение после сдвига на 0x8000.
qsizetype findLogfmtKeyEnd(const char16_t* d, qsizetype i, qsizetype n)
{
#if KEYEDLINE_SSE2
    const __m128i bias = _mm_set1_epi16(short(0x8000));
    const __m128i above = _mm_set1_epi16(short(0x8000 + 0x21));   // u ≤ ' ' ⇔ u + 0x8000 < 0x8021
    const __m128i equals = _mm_set1_epi16(short(u'='));
    const __m128i quote = _mm_set1_epi16(short(u'"'));
    for (; i + 8 <= n; i += 8) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(d + i));
        const __m128i low = _mm_cmplt_epi16(_mm_xor_si128(v, bias), above);
        const int mask = _mm_movemask_epi8(_mm_or_si128(
            low, _mm_or_si128(_mm_cmpeq_epi16(v, equals), _mm_cmpeq_epi16(v, quote))));
        if (mask)
            return i + qCountTrailingZeroBits(quint32(mask)) / 2;
    }
#endif
    for (; i < n; ++i)
        if (!isLogfmtKeyChar(QChar(d[i])))
            return i;
    return n;
}

// Первый разделитель logfmt (isLogfmtSpace) с позиции i; n — если нет.
qsizetype findLogfmtSpace(const char16_t* d, qsizetype i, qsizetype n)
{
#if KEYEDLINE_SSE2
    const __m128i space = _mm_set1_epi16(short(u' '));
    const __m128i tab = _mm_set1_epi16(short(u'\t'));
    const __m128i cr = _mm_set1_epi16(short(u'\r'));
    const __m128i lf = _mm_set1_epi16(short(u'\n'));
    for (; i + 8 <= n; i += 8) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(d + i));
        const int mask = _mm_movemask_epi8(
            _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi16(v, space), _mm_cmpeq_epi16(v, tab)),
                         _mm_or_si128(_mm_cmpeq_epi16(v, cr), _mm_cmpeq_epi16(v, lf))));
        if (mask)
            return i + qCountTrailingZeroBits(quint32(mask)) / 2;
    }
#endif
    for (; i < n; ++i)
        if (isLogfmtSpace(QChar(d[i])))
            return i;
    return n;
}

qsizetype scanString(QStringView t, qsizetype pos, bool* escaped)
{
    const char16_t* const d = t.utf16();
    const qsizetype n = t.size();
    bool esc = false;
    qsizetype i = pos + 1;
    for (;;) {
        i = findQuoteOrBackslash(d, i, n);
        if (i >= n)
            break;
        if (d[i] == u'"') {
            *escaped = esc;
            return i + 1;
        }
        esc = true;
        i += 2;   // экранированный символ (в том числе «"») пропускается
    }
    *escaped = esc;
    return -1;
}

// Вложенный объект или массив с позиции открывающей скобки: счёт скобок с
// учётом строк, без рекурсии. Позиция за закрывающей или -1 (обрыв).
qsizetype scanNested(QStringView t, qsizetype pos)
{
    const qsizetype n = t.size();
    const QChar* d = t.data();
    int depth = 0;
    for (qsizetype i = pos; i < n; ++i) {
        const char16_t u = d[i].unicode();
        if (u == u'"') {
            bool esc = false;
            const qsizetype end = scanString(t, i, &esc);
            if (end < 0)
                return -1;
            i = end - 1;
        } else if (u == u'{' || u == u'[') {
            ++depth;
        } else if (u == u'}' || u == u']') {
            if (--depth == 0)
                return i + 1;
        }
    }
    return -1;
}

KeyedLine::ValueKind bareKind(QStringView token)
{
    // Свой цикл, а не сравнение с QLatin1String: на каждое голое значение.
    const char16_t* t = token.utf16();
    if (token.size() == 4 && t[0] == u'n' && t[1] == u'u' && t[2] == u'l' && t[3] == u'l')
        return KeyedLine::ValueKind::Null;
    const char16_t c = token.front().unicode();
    if ((c >= u'0' && c <= u'9') || c == u'-')
        return KeyedLine::ValueKind::Number;
    return KeyedLine::ValueKind::Literal;
}

// Голое значение logfmt: число — только токен вида [-]цифры[.цифры], той
// же формы, что читает LineClassifier::epochFromValue. В logfmt голым пишут
// и текст, начинающийся с цифры: «ts=2026-11-18T10:00:00Z», «status=5xx» —
// это не числа, иначе timestampFromValue и levelFromValue не прочли бы их
// как текст (S-17).
KeyedLine::ValueKind logfmtBareKind(QStringView token)
{
    const KeyedLine::ValueKind kind = bareKind(token);
    if (kind != KeyedLine::ValueKind::Number)
        return kind;
    const char16_t* t = token.utf16();
    const qsizetype n = token.size();
    qsizetype i = t[0] == u'-' ? 1 : 0;
    const auto digits = [&]() {
        const qsizetype from = i;
        while (i < n && t[i] >= u'0' && t[i] <= u'9')
            ++i;
        return i > from;
    };
    bool ok = digits();
    if (ok && i < n && t[i] == u'.') {
        ++i;
        ok = digits();
    }
    return ok && i == n ? KeyedLine::ValueKind::Number : KeyedLine::ValueKind::Literal;
}

inline int hexValue(char16_t c)
{
    if (c >= u'0' && c <= u'9') return c - u'0';
    if (c >= u'a' && c <= u'f') return c - u'a' + 10;
    if (c >= u'A' && c <= u'F') return c - u'A' + 10;
    return -1;
}

} // namespace

// ---------------------------------------------------------------------------
// KeyedDefinition

QStringList KeyedDefinition::defaultTimeKeys()
{
    return {QStringLiteral("ts"), QStringLiteral("time"), QStringLiteral("timestamp"),
            QStringLiteral("@timestamp"), QStringLiteral("@t"), QStringLiteral("t")};
}

QStringList KeyedDefinition::defaultLevelKeys()
{
    return {QStringLiteral("level"), QStringLiteral("lvl"), QStringLiteral("severity"),
            QStringLiteral("loglevel"), QStringLiteral("levelname"), QStringLiteral("log.level"),
            QStringLiteral("@l")};
}

QStringList KeyedDefinition::defaultMessageKeys()
{
    return {QStringLiteral("msg"), QStringLiteral("message"), QStringLiteral("@m"),
            QStringLiteral("@mt"), QStringLiteral("event")};
}

QStringList KeyedDefinition::columnNames() const
{
    QStringList names{QStringLiteral("Timestamp"), QStringLiteral("Level"),
                      QStringLiteral("Message")};
    // Ключ колонки ищется без пробелов по краям (Lookup) — и имя то же:
    // правила фильтров привязаны к колонкам по имени.
    for (const QString& extra : extraColumns)
        names.append(extra.trimmed());
    return names;
}

QString KeyedDefinition::formatName(Format f)
{
    return f == Format::Logfmt ? QStringLiteral("logfmt") : QStringLiteral("json");
}

bool KeyedDefinition::formatFromName(const QString& name, Format* f)
{
    const QString n = name.trimmed().toLower();
    if (n == QLatin1String("json"))
        *f = Format::JsonObject;
    else if (n == QLatin1String("logfmt"))
        *f = Format::Logfmt;
    else
        return false;
    return true;
}

QString KeyedDefinition::epochUnitName(EpochUnit u)
{
    switch (u) {
    case EpochUnit::Auto:    return QStringLiteral("auto");
    case EpochUnit::Seconds: return QStringLiteral("s");
    case EpochUnit::Millis:  return QStringLiteral("ms");
    case EpochUnit::Micros:  return QStringLiteral("us");
    case EpochUnit::Nanos:   return QStringLiteral("ns");
    }
    return QStringLiteral("auto");
}

bool KeyedDefinition::epochUnitFromName(const QString& name, EpochUnit* u)
{
    const QString n = name.trimmed().toLower();
    if (n.isEmpty() || n == QLatin1String("auto"))
        *u = EpochUnit::Auto;
    else if (n == QLatin1String("s"))
        *u = EpochUnit::Seconds;
    else if (n == QLatin1String("ms"))
        *u = EpochUnit::Millis;
    else if (n == QLatin1String("us"))
        *u = EpochUnit::Micros;
    else if (n == QLatin1String("ns"))
        *u = EpochUnit::Nanos;
    else
        return false;
    return true;
}

QString KeyedDefinition::numericLevelsName(NumericLevels n)
{
    switch (n) {
    case NumericLevels::Auto:   return QStringLiteral("auto");
    case NumericLevels::Syslog: return QStringLiteral("syslog");
    case NumericLevels::Python: return QStringLiteral("python");
    }
    return QStringLiteral("auto");
}

bool KeyedDefinition::numericLevelsFromName(const QString& name, NumericLevels* n)
{
    const QString s = name.trimmed().toLower();
    if (s.isEmpty() || s == QLatin1String("auto"))
        *n = NumericLevels::Auto;
    else if (s == QLatin1String("syslog"))
        *n = NumericLevels::Syslog;
    else if (s == QLatin1String("python"))
        *n = NumericLevels::Python;
    else
        return false;
    return true;
}

namespace KeyedLine {

// ---------------------------------------------------------------------------
// PairReader

PairReader::PairReader(QStringView text, KeyedDefinition::Format format)
    : m_text(text)
    , m_format(format)
{
    if (format == KeyedDefinition::Format::JsonObject) {
        m_structured = !text.isEmpty() && text.front() == QLatin1Char('{');
        m_pos = 1;
    } else {
        const qsizetype i = findLogfmtKeyEnd(text.utf16(), 0, text.size());
        m_structured = i > 0 && i < text.size() && text[i] == QLatin1Char('=');
        m_pos = 0;
        m_firstKeyEnd = i;   // первая пара — с этим концом ключа, без второго поиска
    }
    m_done = !m_structured;
}

bool PairReader::next(Pair* pair)
{
    if (m_done)
        return false;
    const bool ok = (m_format == KeyedDefinition::Format::JsonObject) ? nextJson(pair)
                                                                      : nextLogfmt(pair);
    if (!ok)
        m_done = true;
    return ok;
}

bool PairReader::nextJson(Pair* pair)
{
    const QStringView t = m_text;
    const qsizetype n = t.size();
    qsizetype pos = m_pos;
    // Пробелы и запятые между парами (лишняя запятая — терпимо).
    while (pos < n && (isJsonSpace(t[pos]) || t[pos] == QLatin1Char(',')))
        ++pos;
    if (pos >= n || t[pos] != QLatin1Char('"'))
        return false;   // конец объекта, обрыв или мусор

    bool keyEscaped = false;
    const qsizetype keyEnd = scanString(t, pos, &keyEscaped);
    if (keyEnd < 0)
        return false;
    const qsizetype keyStart = pos + 1;
    pos = keyEnd;
    while (pos < n && isJsonSpace(t[pos]))
        ++pos;
    if (pos >= n || t[pos] != QLatin1Char(':'))
        return false;
    ++pos;
    while (pos < n && isJsonSpace(t[pos]))
        ++pos;
    if (pos >= n)
        return false;

    Pair p;
    p.keyStart = int(keyStart);
    p.keyLength = int(keyEnd - 1 - keyStart);
    p.keyEscaped = keyEscaped;

    const char16_t c = t[pos].unicode();
    qsizetype end;
    if (c == u'"') {
        bool esc = false;
        end = scanString(t, pos, &esc);
        if (end < 0)
            return false;
        p.valueStart = int(pos + 1);
        p.valueLength = int(end - 1 - (pos + 1));
        p.valueEscaped = esc;
        p.kind = ValueKind::String;
    } else if (c == u'{' || c == u'[') {
        end = scanNested(t, pos);
        if (end < 0)
            return false;
        p.valueStart = int(pos);
        p.valueLength = int(end - pos);
        p.kind = (c == u'{') ? ValueKind::Object : ValueKind::Array;
    } else {
        end = pos;
        while (end < n) {
            const char16_t u = t[end].unicode();
            if (u == u',' || u == u'}' || u == u']' || isJsonSpace(t[end]))
                break;
            ++end;
        }
        if (end == pos)
            return false;   // «"a":,» — пары нет
        p.valueStart = int(pos);
        p.valueLength = int(end - pos);
        p.kind = bareKind(t.mid(pos, end - pos));
    }
    m_pos = end;
    *pair = p;
    return true;
}

bool PairReader::nextLogfmt(Pair* pair)
{
    const QStringView t = m_text;
    const qsizetype n = t.size();
    qsizetype pos = m_pos;
    for (;;) {
        while (pos < n && isLogfmtSpace(t[pos]))
            ++pos;
        if (pos >= n)
            return false;
        const qsizetype keyStart = pos;
        if (m_firstKeyEnd >= 0) {
            pos = m_firstKeyEnd;   // pos == 0: строка начинается с ключа (structured)
            m_firstKeyEnd = -1;
        } else {
            pos = findLogfmtKeyEnd(t.utf16(), pos, n);
        }
        if (pos == keyStart || pos >= n || t[pos] != QLatin1Char('=')) {
            // Не пара (голый ключ, «"…"» без ключа) — токен пропускается.
            if (pos < n && t[pos] == QLatin1Char('"')) {
                bool esc = false;
                const qsizetype end = scanString(t, pos, &esc);
                if (end < 0)
                    return false;
                pos = end;
            }
            pos = findLogfmtSpace(t.utf16(), pos, n);
            continue;
        }
        Pair p;
        p.keyStart = int(keyStart);
        p.keyLength = int(pos - keyStart);
        ++pos;   // «=»
        if (pos < n && t[pos] == QLatin1Char('"')) {
            bool esc = false;
            const qsizetype end = scanString(t, pos, &esc);
            if (end < 0)
                return false;   // оборванное значение в кавычках
            p.valueStart = int(pos + 1);
            p.valueLength = int(end - 1 - (pos + 1));
            p.valueEscaped = esc;
            p.kind = ValueKind::String;
            pos = end;
        } else {
            const qsizetype start = pos;
            pos = findLogfmtSpace(t.utf16(), pos, n);
            p.valueStart = int(start);
            p.valueLength = int(pos - start);
            p.kind = p.valueLength > 0 ? logfmtBareKind(t.mid(start, pos - start))
                                       : ValueKind::Literal;
        }
        m_pos = pos;
        *pair = p;
        return true;
    }
}

// ---------------------------------------------------------------------------
// unescape

QString unescape(QStringView s, bool keepControls)
{
    QString out;
    out.reserve(s.size());
    const qsizetype n = s.size();
    for (qsizetype i = 0; i < n; ++i) {
        const QChar c = s[i];
        if (c != QLatin1Char('\\') || i + 1 >= n) {
            out += c;
            continue;
        }
        const qsizetype seqStart = i;
        const char16_t e = s[i + 1].unicode();
        char16_t decoded = 0;
        qsizetype seqLen = 2;
        bool known = true;
        switch (e) {
        case u'"':  decoded = u'"'; break;
        case u'\\': decoded = u'\\'; break;
        case u'/':  decoded = u'/'; break;
        case u'b':  decoded = u'\b'; break;
        case u'f':  decoded = u'\f'; break;
        case u'n':  decoded = u'\n'; break;
        case u'r':  decoded = u'\r'; break;
        case u't':  decoded = u'\t'; break;
        case u'u': {
            int v = 0;
            bool ok = i + 5 < n;
            for (int k = 0; ok && k < 4; ++k) {
                const int h = hexValue(s[i + 2 + k].unicode());
                if (h < 0)
                    ok = false;
                else
                    v = v * 16 + h;
            }
            if (!ok) {
                known = false;
                break;
            }
            decoded = char16_t(v);
            seqLen = 6;
            break;
        }
        default:
            known = false;
            break;
        }
        if (!known) {
            // Неизвестная или битая последовательность — как в файле.
            out += c;
            continue;
        }
        if (keepControls && (decoded < 0x20 || decoded == 0x7f))
            out += s.mid(seqStart, seqLen);
        else
            out += QChar(decoded);   // суррогаты 😀 складываются сами — QString в UTF-16
        i += seqLen - 1;
    }
    return out;
}

qsizetype unescapedSize(QStringView s, bool keepControls)
{
    // Тот же проход, что unescape, но без строки: длина для раскладки view.
    qsizetype size = 0;
    const qsizetype n = s.size();
    for (qsizetype i = 0; i < n; ++i) {
        ++size;
        if (s[i] != QLatin1Char('\\') || i + 1 >= n)
            continue;
        const char16_t e = s[i + 1].unicode();
        char16_t decoded = 0;
        qsizetype seqLen = 2;
        switch (e) {
        case u'"': case u'\\': case u'/':
            decoded = e;
            break;
        case u'b': decoded = u'\b'; break;
        case u'f': decoded = u'\f'; break;
        case u'n': decoded = u'\n'; break;
        case u'r': decoded = u'\r'; break;
        case u't': decoded = u'\t'; break;
        case u'u': {
            int v = 0;
            bool ok = i + 5 < n;
            for (int k = 0; ok && k < 4; ++k) {
                const int h = hexValue(s[i + 2 + k].unicode());
                ok = h >= 0;
                v = v * 16 + h;
            }
            if (!ok)
                continue;   // как в файле: «\» посчитан, остальное — следующими шагами
            decoded = char16_t(v);
            seqLen = 6;
            break;
        }
        default:
            continue;
        }
        if (keepControls && (decoded < 0x20 || decoded == 0x7f))
            size += seqLen - 1;   // последовательность остаётся как есть
        i += seqLen - 1;
    }
    return size;
}

QString valueText(QStringView text, const Pair& pair)
{
    const QStringView v = text.mid(pair.valueStart, pair.valueLength);
    return pair.valueEscaped ? unescape(v) : v.toString();
}

// ---------------------------------------------------------------------------
// Lookup

Lookup::Lookup(const KeyedDefinition& def)
    : m_format(def.format)
    , m_byLength(kMaxBucketedKey + 1)
    , m_firstChars(kMaxBucketedKey + 1)
{
    const auto add = [this](const QString& key, const QStringList& path, int column, int rank) {
        Entry e;
        e.key = key;
        e.path = path;
        e.column = column;
        e.rank = rank;
        const int bucket = qMin(int(key.size()), int(kMaxBucketedKey));
        m_byLength[bucket].append(e);
        const char16_t first = key.isEmpty() ? 0 : key.front().unicode();
        FirstChars& mask = m_firstChars[bucket];
        if (first >= 128)
            mask.nonAscii = true;
        else
            mask.bits[first / 64] |= quint64(1) << (first % 64);
    };
    const auto column = [&](const QStringList& keys) {
        const int col = m_columnCount++;
        int candidate = 0;
        for (const QString& raw : keys) {
            const QString key = raw.trimmed();
            if (key.isEmpty())
                continue;
            add(key, QStringList(), col, candidate * 2);
            // Путь во вложенные объекты — только у JSON: в logfmt «a.b» —
            // просто имя ключа.
            if (def.format == KeyedDefinition::Format::JsonObject && key.contains(QLatin1Char('.'))) {
                const QStringList parts = key.split(QLatin1Char('.'));
                bool ok = parts.size() > 1;
                for (const QString& part : parts)
                    ok = ok && !part.isEmpty();
                if (ok)
                    add(parts.front(), parts, col, candidate * 2 + 1);
            }
            ++candidate;
        }
    };
    column(def.timeKeys);
    column(def.levelKeys);
    column(def.messageKeys);
    for (const QString& extra : def.extraColumns)
        column(QStringList{extra});
}

namespace {

bool usableValue(const Pair& p)
{
    return p.kind != ValueKind::Null && p.valueLength > 0;
}

// Значение по пути segments[from..] внутри объекта obj (base — смещение obj
// в строке). Первый совпавший ключ уровня решает, как у верхнего уровня.
bool findPath(QStringView obj, int base, const QStringList& segments, int from, Hit* hit)
{
    for (int level = from; level < segments.size(); ++level) {
        PairReader reader(obj, KeyedDefinition::Format::JsonObject);
        Pair q;
        bool descended = false;
        while (reader.next(&q)) {
            const QStringView raw = obj.mid(q.keyStart, q.keyLength);
            const bool same = q.keyEscaped ? unescape(raw) == segments[level]
                                           : raw == segments[level];
            if (!same)
                continue;
            if (level + 1 == segments.size()) {
                if (!usableValue(q))
                    return false;
                hit->start = base + q.valueStart;
                hit->length = q.valueLength;
                hit->escaped = q.valueEscaped;
                hit->kind = q.kind;
                return true;
            }
            if (q.kind != ValueKind::Object)
                return false;
            base += q.valueStart;
            obj = obj.mid(q.valueStart, q.valueLength);
            descended = true;
            break;
        }
        if (!descended)
            return false;
    }
    return false;
}

} // namespace

namespace {

// Ключи короткие: свой цикл дешевле вызова memcmp / equalStrings.
inline bool sameKey(QStringView a, const QString& b)
{
    const qsizetype n = a.size();
    if (n != b.size())
        return false;
    const char16_t* x = a.utf16();
    const QChar* y = b.constData();
    for (qsizetype i = 0; i < n; ++i)
        if (x[i] != y[i].unicode())
            return false;
    return true;
}

} // namespace

void Lookup::consider(QStringView line, QStringView key, const Pair& pair, Hits* hits,
                      int neededColumns, int* settled) const
{
    const int length = qMin(int(key.size()), int(kMaxBucketedKey));
    // Ключ, не начинающийся ни с одной первой буквы кандидатов своей длины, —
    // мимо без прохода по корзине (большинство ключей строки — не колонки).
    const FirstChars& mask = m_firstChars[length];
    const char16_t first = key.isEmpty() ? 0 : key.front().unicode();
    if (first < 128 ? !(mask.bits[first / 64] & (quint64(1) << (first % 64))) : !mask.nonAscii)
        return;
    const QVector<Entry>& bucket = m_byLength[length];
    for (const Entry& e : bucket) {
        Hit& best = (*hits)[e.column];
        if (best.isValid() && e.rank >= best.rank)
            continue;   // у колонки уже есть кандидат не хуже; повторный ключ — первый
        if (!sameKey(key, e.key))
            continue;
        if (e.path.isEmpty()) {
            if (!usableValue(pair))
                continue;   // null или пусто — поля нет
            if (e.rank == 0 && e.column < neededColumns)
                ++*settled;   // лучше первого кандидата не будет
            best.start = pair.valueStart;
            best.length = pair.valueLength;
            best.escaped = pair.valueEscaped;
            best.kind = pair.kind;
            best.rank = e.rank;
        } else if (pair.kind == ValueKind::Object) {
            Hit nested;
            if (findPath(line.mid(pair.valueStart, pair.valueLength), pair.valueStart, e.path, 1,
                         &nested)) {
                nested.rank = e.rank;
                best = nested;
            }
        }
    }
}

bool Lookup::scan(QStringView line, Hits* hits, int neededColumns) const
{
    if (neededColumns <= 0 || neededColumns > m_columnCount)
        neededColumns = m_columnCount;
    hits->resize(m_columnCount);
    for (Hit& h : *hits)
        h = Hit();
    PairReader reader(line, m_format);
    if (!reader.structured())
        return false;
    // Нужные колонки, у которых уже найден первый кандидат: когда это все,
    // дальше строку читать незачем — ни один следующий ключ их не поменяет.
    int settled = 0;
    Pair p;
    while (settled < neededColumns && reader.next(&p)) {
        const QStringView key = line.mid(p.keyStart, p.keyLength);
        if (p.keyEscaped)
            consider(line, unescape(key), p, hits, neededColumns, &settled);
        else
            consider(line, key, p, hits, neededColumns, &settled);
    }
    for (int c = 0; c < KeyedDefinition::kRoleColumns && c < hits->size(); ++c)
        if ((*hits)[c].isValid())
            return true;
    return false;
}

} // namespace KeyedLine

QVector<std::pair<QString, QString>> KeyedLine::keyValuePairs(QStringView line,
                                                              KeyedDefinition::Format format)
{
    QVector<std::pair<QString, QString>> pairs;
    PairReader reader(line, format);
    Pair p;
    while (reader.next(&p)) {
        const QStringView key = line.mid(p.keyStart, p.keyLength);
        pairs.append({p.keyEscaped ? unescape(key) : key.toString(), valueText(line, p)});
    }
    return pairs;
}
