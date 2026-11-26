#include "linetemplate.h"

#include "lineclassifier.h"

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <functional>

// Устройство разбора — в начале linetemplate.h. Элементы лежат ПЛОСКИМ
// списком: необязательный участок — заголовок (Type::Optional, end) и его
// элементы сразу за ним до end. Поэтому «что идёт следом» — всегда
// следующий номер: и внутри участка (дойдя до end, разбор выходит из него),
// и снаружи; проверка следом идущего элемента (probe) у заголовка участка
// пробует две ветви — участок взят и участок пропущен.

namespace {

inline bool isAsciiDigit(QChar c)
{
    return c.unicode() >= u'0' && c.unicode() <= u'9';
}

inline bool isHexDigit(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f') || (u >= u'A' && u <= u'F');
}

// \w без UCP — ASCII-буквы, цифры, подчёркивание (как у LineClassifier).
inline bool isWordChar(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z') || (u >= u'0' && u <= u'9')
        || u == u'_';
}

inline bool isBlank(QChar c)
{
    return c == QLatin1Char(' ') || c == QLatin1Char('\t');
}

// За концом токена не символ слова.
inline bool wordEndAt(QStringView s, qsizetype end)
{
    return end >= s.size() || !isWordChar(s[end]);
}

qsizetype digitsFrom(QStringView s, qsizetype p)
{
    qsizetype q = p;
    while (q < s.size() && isAsciiDigit(s[q]))
        ++q;
    return q - p;
}

qsizetype hexFrom(QStringView s, qsizetype p, qsizetype max = -1)
{
    qsizetype q = p;
    while (q < s.size() && isHexDigit(s[q]) && (max < 0 || q - p < max))
        ++q;
    return q - p;
}

// Символ куска пути: [^\r\n\s<>:"|?*\\/] (\s без UCP — ASCII-пробельные).
inline bool isPathChunkChar(QChar c)
{
    switch (c.unicode()) {
    case u'\r': case u'\n': case u' ': case u'\t': case u'\v': case u'\f':
    case u'<': case u'>': case u':': case u'"': case u'|': case u'?': case u'*':
    case u'\\': case u'/':
        return false;
    default:
        return true;
    }
}

inline bool isPathSep(QChar c)
{
    return c == QLatin1Char('/') || c == QLatin1Char('\\');
}

qsizetype pathFrom(QStringView s, qsizetype p)
{
    const qsizetype n = s.size();
    qsizetype q = p;
    while (q < n && isPathSep(s[q]))
        ++q;
    const qsizetype chunkStart = q;
    while (q < n && isPathChunkChar(s[q]))
        ++q;
    if (q == chunkStart)
        return -1;
    for (;;) {
        qsizetype r = q;
        while (r < n && isPathSep(s[r]))
            ++r;
        if (r == q)
            break;
        const qsizetype c0 = r;
        while (r < n && isPathChunkChar(s[r]))
            ++r;
        if (r == c0)
            break;
        q = r;
    }
    return q - p;
}

// IPv4 с позиции p: d{1,3}(.d{1,3}){3}, за ним не цифра, не «.цифра» и не
// символ слова. Длина или -1.
qsizetype ipv4From(QStringView s, qsizetype pos)
{
    const qsizetype n = s.size();
    qsizetype p = pos;
    for (int g = 0; g < 4; ++g) {
        if (g > 0) {
            if (p >= n || s[p] != QLatin1Char('.'))
                return -1;
            ++p;
        }
        const qsizetype d = digitsFrom(s, p);
        if (d == 0 || d > 3)
            return -1;
        p += d;
    }
    if (!wordEndAt(s, p) || (p + 1 < n && s[p] == QLatin1Char('.') && isAsciiDigit(s[p + 1])))
        return -1;
    return p - pos;
}

// IPv6 на отрезке [p, e): группы 1–4 hex через «:», не больше одного «::»,
// 8 групп без «::» или до 7 с ним; последней может быть IPv4 (две группы).
bool isIpv6(QStringView s, qsizetype p, qsizetype e)
{
    int groups = 0;
    int colons = 0;
    bool doubleColon = false;
    qsizetype q = p;
    if (q + 1 < e && s[q] == QLatin1Char(':') && s[q + 1] == QLatin1Char(':')) {
        doubleColon = true;
        q += 2;
        colons += 2;
        if (q == e)
            return true;   // «::»
    } else if (q < e && s[q] == QLatin1Char(':')) {
        return false;
    }
    while (q < e) {
        // IPv4 в конце: d.d.d.d
        qsizetype h = hexFrom(s, q, 5);
        if (h == 0 || h > 4) {
            return false;
        }
        if (q + h < e && s[q + h] == QLatin1Char('.')) {
            // Хвост — IPv4.
            const qsizetype v4 = ipv4From(s.first(e), q);
            if (v4 <= 0 || q + v4 != e)
                return false;
            groups += 2;
            q = e;
            break;
        }
        ++groups;
        q += h;
        if (q == e)
            break;
        if (s[q] != QLatin1Char(':'))
            return false;
        ++q;
        ++colons;
        if (q < e && s[q] == QLatin1Char(':')) {
            if (doubleColon)
                return false;
            doubleColon = true;
            ++q;
            ++colons;
            if (q == e)
                break;
        } else if (q == e) {
            return false;   // «:» в конце
        }
    }
    if (colons < 2)
        return false;
    return doubleColon ? groups <= 7 : groups == 8;
}

inline QStringView trimmedRange(QStringView s, qsizetype& a, qsizetype& b)
{
    while (a < b && isBlank(s[a]))
        ++a;
    while (b > a && isBlank(s[b - 1]))
        --b;
    return s.mid(a, b - a);
}

} // namespace

namespace TokenScan {

qsizetype timeAt(QStringView line, qsizetype pos)
{
    const qsizetype n = LineClassifier::timestampLengthAt(line, pos);
    return n > 0 ? n : -1;
}

qsizetype levelAt(QStringView line, qsizetype pos)
{
    const qsizetype n = LineClassifier::levelLengthAt(line, pos);
    return n > 0 ? n : -1;
}

qsizetype intAt(QStringView s, qsizetype pos)
{
    if (pos < 0 || pos >= s.size())
        return -1;
    qsizetype p = pos;
    if (s[p] == QLatin1Char('+') || s[p] == QLatin1Char('-'))
        ++p;
    const qsizetype d = digitsFrom(s, p);
    if (d == 0 || !wordEndAt(s, p + d))
        return -1;
    return p + d - pos;
}

qsizetype numAt(QStringView s, qsizetype pos)
{
    if (pos < 0 || pos >= s.size())
        return -1;
    qsizetype p = pos;
    if (s[p] == QLatin1Char('+') || s[p] == QLatin1Char('-'))
        ++p;
    const qsizetype d = digitsFrom(s, p);
    if (d == 0)
        return -1;
    p += d;
    if (p + 1 < s.size() && s[p] == QLatin1Char('.') && isAsciiDigit(s[p + 1]))
        p += 1 + digitsFrom(s, p + 1);
    if (p < s.size() && (s[p] == QLatin1Char('e') || s[p] == QLatin1Char('E'))) {
        qsizetype q = p + 1;
        if (q < s.size() && (s[q] == QLatin1Char('+') || s[q] == QLatin1Char('-')))
            ++q;
        const qsizetype ed = digitsFrom(s, q);
        if (ed > 0)
            p = q + ed;
    }
    if (!wordEndAt(s, p))
        return -1;
    return p - pos;
}

qsizetype hexAt(QStringView s, qsizetype pos)
{
    if (pos < 0 || pos >= s.size())
        return -1;
    if (pos + 2 < s.size() && s[pos] == QLatin1Char('0')
        && (s[pos + 1] == QLatin1Char('x') || s[pos + 1] == QLatin1Char('X'))) {
        const qsizetype h = hexFrom(s, pos + 2);
        if (h > 0 && wordEndAt(s, pos + 2 + h))
            return 2 + h;
    }
    const qsizetype h = hexFrom(s, pos);
    if (h == 0 || !wordEndAt(s, pos + h))
        return -1;
    return h;
}

qsizetype uuidAt(QStringView s, qsizetype pos)
{
    static constexpr int kGroups[] = { 8, 4, 4, 4, 12 };
    if (pos < 0)
        return -1;
    qsizetype p = pos;
    for (int g = 0; g < 5; ++g) {
        if (g > 0) {
            if (p >= s.size() || s[p] != QLatin1Char('-'))
                return -1;
            ++p;
        }
        if (hexFrom(s, p, kGroups[g]) != kGroups[g])
            return -1;
        p += kGroups[g];
    }
    if (!wordEndAt(s, p))
        return -1;
    return p - pos;
}

qsizetype ipAt(QStringView s, qsizetype pos)
{
    const qsizetype n = s.size();
    if (pos < 0 || pos >= n)
        return -1;
    const qsizetype v4 = ipv4From(s, pos);
    if (v4 > 0)
        return v4;
    // IPv6: самый длинный отрезок из hex, «:» и «.»; не сошёлся — без «:» и «.»
    // в конце («2001:db8::1:» перед текстом).
    qsizetype e = pos;
    while (e < n && (isHexDigit(s[e]) || s[e] == QLatin1Char(':') || s[e] == QLatin1Char('.')))
        ++e;
    if (e == pos || !wordEndAt(s, e))
        return -1;
    if (isIpv6(s, pos, e))
        return e - pos;
    qsizetype t = e;
    while (t > pos && (s[t - 1] == QLatin1Char(':') || s[t - 1] == QLatin1Char('.')))
        --t;
    if (t != e && t > pos && isIpv6(s, pos, t))
        return t - pos;
    return -1;
}

qsizetype pathAt(QStringView s, qsizetype pos)
{
    const qsizetype n = s.size();
    if (pos < 0 || pos >= n)
        return -1;
    // (?:[A-Za-z]:)? — с откатом: без пути за диском диск не берётся.
    const char16_t c = s[pos].unicode();
    if (pos + 1 < n && ((c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z'))
        && s[pos + 1] == QLatin1Char(':')) {
        const qsizetype rest = pathFrom(s, pos + 2);
        if (rest > 0)
            return 2 + rest;
    }
    return pathFrom(s, pos);
}

qsizetype quotedAt(QStringView s, qsizetype pos, bool* escaped)
{
    const qsizetype n = s.size();
    if (escaped)
        *escaped = false;
    if (pos < 0 || pos >= n || s[pos] != QLatin1Char('"'))
        return -1;
    for (qsizetype p = pos + 1; p < n; ++p) {
        if (s[p] == QLatin1Char('\\')) {
            if (escaped)
                *escaped = true;
            ++p;
            continue;
        }
        if (s[p] == QLatin1Char('"'))
            return p + 1 - pos;
    }
    return -1;
}

} // namespace TokenScan

// ============================================================================
// Компиляция
// ============================================================================

LineTemplate::LineTemplate(const QString& text)
    : m_text(text)
{
    m_valid = parse() && resolve();
    if (!m_valid) {
        m_elements.clear();
        m_columns.clear();
        m_columnTop.clear();
        m_tailColumn = -1;
        m_tailElement = -1;
        m_hasRegex = false;
    }
}

bool LineTemplate::fail(LineTemplateError::Code code, int position, const QString& field,
                        const QString& message)
{
    m_error.code = code;
    m_error.position = position;
    m_error.field = field;
    m_error.message = message;
    return false;
}

QString LineTemplate::escapeLiteral(const QString& text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        out += c;
        if (c == QLatin1Char('{') || c == QLatin1Char('}'))
            out += c;
    }
    return out;
}

QString LineTemplate::kindName(Element::Kind kind)
{
    using K = Element::Kind;
    switch (kind) {
    case K::Time: return QStringLiteral("time");
    case K::Level: return QStringLiteral("level");
    case K::Int: return QStringLiteral("int");
    case K::Num: return QStringLiteral("num");
    case K::Hex: return QStringLiteral("hex");
    case K::Uuid: return QStringLiteral("uuid");
    case K::Ip: return QStringLiteral("ip");
    case K::Path: return QStringLiteral("path");
    case K::Quoted: return QStringLiteral("quoted");
    case K::Of: return QStringLiteral("of");
    case K::Regex: return QStringLiteral("regex");
    case K::Word: return QStringLiteral("word");
    case K::Text: return QStringLiteral("text");
    case K::Greedy: return QStringLiteral("greedy");
    case K::Padded: return QStringLiteral("padded");
    case K::Width: return QStringLiteral("width");
    case K::Words: return QStringLiteral("words");
    case K::Rest: return QStringLiteral("rest");
    }
    return QString();
}

namespace {

// Аргументы в скобках (of, width, words): до парной «)», «\)», «\|», «\\» —
// сами знаки. Значения of — по «|». false — скобка не закрыта.
bool readArguments(const QString& s, int& i, QStringList* parts)
{
    QString cur;
    for (; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (c == QLatin1Char('\\') && i + 1 < s.size()) {
            const QChar next = s.at(i + 1);
            if (next == QLatin1Char(')') || next == QLatin1Char('|') || next == QLatin1Char('\\')) {
                cur += next;
                ++i;
                continue;
            }
            cur += c;
            continue;
        }
        if (c == QLatin1Char(')')) {
            parts->append(cur);
            ++i;
            return true;
        }
        if (c == QLatin1Char('|')) {
            parts->append(cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    return false;
}

} // namespace

bool LineTemplate::parse()
{
    using E = Element;
    using C = LineTemplateError::Code;
    const QString& s = m_text;
    const int n = int(s.size());
    QString literal;
    int literalPos = 0;
    int openOptional = -1;   // номер заголовка открытого участка

    const auto flushLiteral = [&]() {
        if (literal.isEmpty())
            return;
        E e;
        e.type = E::Type::Literal;
        e.text = literal;
        e.sourcePos = literalPos;
        m_elements.append(e);
        literal.clear();
    };
    const auto addLiteralChar = [&](QChar c, int at) {
        if (literal.isEmpty())
            literalPos = at;
        literal += c;
    };

    int i = 0;
    while (i < n) {
        const QChar c = s.at(i);
        const QChar next = i + 1 < n ? s.at(i + 1) : QChar();
        if (c == QLatin1Char('{') && next == QLatin1Char('{')) {
            addLiteralChar(c, i);
            i += 2;
            continue;
        }
        if (c == QLatin1Char('}') && next == QLatin1Char('}')) {
            addLiteralChar(c, i);
            i += 2;
            continue;
        }
        if (c == QLatin1Char('{') && next == QLatin1Char('?')) {
            if (openOptional >= 0)
                return fail(C::NestedOptional, i, QString(),
                            QStringLiteral("An optional part cannot contain another optional part."));
            flushLiteral();
            E e;
            e.type = E::Type::Optional;
            e.sourcePos = i;
            openOptional = int(m_elements.size());
            m_elements.append(e);
            i += 2;
            continue;
        }
        if (c == QLatin1Char('?') && next == QLatin1Char('}') && openOptional >= 0) {
            flushLiteral();
            if (int(m_elements.size()) == openOptional + 1)
                return fail(C::EmptyOptional, m_elements[openOptional].sourcePos, QString(),
                            QStringLiteral("The optional part is empty."));
            m_elements[openOptional].end = int(m_elements.size());
            openOptional = -1;
            i += 2;
            continue;
        }
        if (c == QLatin1Char('}'))
            return fail(C::UnmatchedBrace, i, QString(),
                        QStringLiteral("“}” without “{” — write “}}” for the brace itself."));
        if (isBlank(c)) {
            flushLiteral();
            E e;
            e.type = E::Type::Space;
            e.sourcePos = i;
            while (i < n && isBlank(s.at(i)))
                ++i;
            if (m_elements.isEmpty() || m_elements.constLast().type != E::Type::Space)
                m_elements.append(e);
            continue;
        }
        if (c != QLatin1Char('{')) {
            addLiteralChar(c, i);
            ++i;
            continue;
        }

        // Поле.
        flushLiteral();
        const int start = i;
        E e;
        e.type = E::Type::Field;
        e.sourcePos = start;
        ++i;
        QString name;
        while (i < n && s.at(i) != QLatin1Char(':') && s.at(i) != QLatin1Char('}')
               && s.at(i) != QLatin1Char('{'))
            name += s.at(i++);
        name = name.trimmed();
        if (i >= n || s.at(i) == QLatin1Char('{'))
            return fail(C::UnclosedField, start, name,
                        QStringLiteral("Field “%1”: “{” is not closed with “}”.").arg(name));
        if (name == QLatin1String("_"))
            name.clear();
        e.name = name;
        if (s.at(i) == QLatin1Char(':')) {
            ++i;
            while (i < n && isBlank(s.at(i)))
                ++i;
            if (i < n && s.at(i) == QLatin1Char('/')) {
                // /регекс/: до неэкранированной «/».
                ++i;
                QString rx;
                bool closed = false;
                for (; i < n; ++i) {
                    if (s.at(i) == QLatin1Char('\\') && i + 1 < n) {
                        if (s.at(i + 1) == QLatin1Char('/')) {
                            rx += QLatin1Char('/');
                        } else {
                            rx += s.at(i);
                            rx += s.at(i + 1);
                        }
                        ++i;
                        continue;
                    }
                    if (s.at(i) == QLatin1Char('/')) {
                        closed = true;
                        ++i;
                        break;
                    }
                    rx += s.at(i);
                }
                if (!closed)
                    return fail(C::UnclosedField, start, name,
                                QStringLiteral("Field “%1”: the regex is not closed with “/”.").arg(name));
                e.kind = E::Kind::Regex;
                e.kindGiven = true;
                e.regexSource = rx;
                e.regex = QRegularExpression(rx);
                if (!e.regex.isValid())
                    return fail(C::BadRegex, start, name,
                                QStringLiteral("Field “%1”: invalid regex — %2.")
                                    .arg(name, e.regex.errorString()));
            } else {
                QString kind;
                while (i < n && s.at(i).isLetter())
                    kind += s.at(i++);
                QStringList args;
                bool hasArgs = false;
                if (i < n && s.at(i) == QLatin1Char('(')) {
                    ++i;
                    hasArgs = true;
                    if (!readArguments(s, i, &args))
                        return fail(C::UnclosedField, start, name,
                                    QStringLiteral("Field “%1”: “(” is not closed with “)”.").arg(name));
                }
                static const QHash<QString, E::Kind> kKinds = {
                    { QStringLiteral("time"), E::Kind::Time },     { QStringLiteral("level"), E::Kind::Level },
                    { QStringLiteral("int"), E::Kind::Int },       { QStringLiteral("num"), E::Kind::Num },
                    { QStringLiteral("hex"), E::Kind::Hex },       { QStringLiteral("uuid"), E::Kind::Uuid },
                    { QStringLiteral("ip"), E::Kind::Ip },         { QStringLiteral("path"), E::Kind::Path },
                    { QStringLiteral("quoted"), E::Kind::Quoted }, { QStringLiteral("of"), E::Kind::Of },
                    { QStringLiteral("word"), E::Kind::Word },     { QStringLiteral("text"), E::Kind::Text },
                    { QStringLiteral("greedy"), E::Kind::Greedy }, { QStringLiteral("padded"), E::Kind::Padded },
                    { QStringLiteral("width"), E::Kind::Width },   { QStringLiteral("words"), E::Kind::Words },
                    { QStringLiteral("rest"), E::Kind::Rest },
                };
                const auto it = kKinds.constFind(kind);
                if (it == kKinds.constEnd())
                    return fail(C::UnknownKind, start, name,
                                QStringLiteral("Field “%1”: unknown kind “%2”.").arg(name, kind));
                e.kind = *it;
                e.kindGiven = true;
                const bool wantsArgs = e.kind == E::Kind::Of || e.kind == E::Kind::Width
                    || e.kind == E::Kind::Words;
                if (hasArgs != wantsArgs)
                    return fail(C::BadArgument, start, name,
                                wantsArgs ? QStringLiteral("Field “%1”: “%2” needs an argument in brackets.")
                                                .arg(name, kind)
                                          : QStringLiteral("Field “%1”: “%2” takes no argument.")
                                                .arg(name, kind));
                if (e.kind == E::Kind::Of) {
                    for (const QString& v : args) {
                        if (v.isEmpty())
                            return fail(C::BadArgument, start, name,
                                        QStringLiteral("Field “%1”: an empty value in the list.").arg(name));
                    }
                    e.values = args;
                    e.valuesByLength = args;
                    std::stable_sort(e.valuesByLength.begin(), e.valuesByLength.end(),
                                     [](const QString& a, const QString& b) { return a.size() > b.size(); });
                } else if (wantsArgs) {
                    bool ok = false;
                    const int count = args.size() == 1 ? args.first().trimmed().toInt(&ok) : 0;
                    if (!ok || count <= 0 || count > 100000)
                        return fail(C::BadArgument, start, name,
                                    QStringLiteral("Field “%1”: “%2” needs a positive number.").arg(name, kind));
                    e.count = count;
                }
            }
            while (i < n && isBlank(s.at(i)))
                ++i;
        }
        if (i >= n || s.at(i) != QLatin1Char('}'))
            return fail(C::UnclosedField, start, name,
                        QStringLiteral("Field “%1”: “{” is not closed with “}”.").arg(name));
        ++i;
        m_elements.append(e);
    }
    flushLiteral();
    if (openOptional >= 0)
        return fail(C::UnclosedOptional, m_elements[openOptional].sourcePos, QString(),
                    QStringLiteral("The optional part “{?” is not closed with “?}”."));
    bool any = false;
    for (const Element& e : m_elements)
        any = any || e.type != E::Type::Space;
    if (!any)
        return fail(C::Empty, 0, QString(), QStringLiteral("The template is empty."));
    return true;
}

bool LineTemplate::resolve()
{
    using E = Element;
    using C = LineTemplateError::Code;
    const int count = int(m_elements.size());

    // Элемент верхнего уровня у каждого.
    for (int i = 0; i < count; ++i) {
        E& e = m_elements[i];
        if (e.top >= 0)
            continue;
        e.top = i;
        if (e.type == E::Type::Optional) {
            for (int j = i + 1; j < e.end; ++j)
                m_elements[j].top = i;
        }
    }

    // Что следом за полем i (плоский порядок): конец, литерал, участок,
    // поле с формой, поле без формы — вплотную или через пробел.
    enum class Next { End, Literal, Optional, Form, Plain };
    struct Follow {
        Next next = Next::End;
        bool space = false;
    };
    const auto followOf = [&](int i) {
        Follow f;
        int j = i + 1;
        if (j < count && m_elements[j].type == E::Type::Space) {
            f.space = true;
            ++j;
        }
        if (j >= count) {
            f.next = Next::End;
            return f;
        }
        const E& e = m_elements[j];
        switch (e.type) {
        case E::Type::Literal: f.next = Next::Literal; break;
        case E::Type::Optional: f.next = Next::Optional; break;
        case E::Type::Field: f.next = (e.kindGiven && E::isFormKind(e.kind)) ? Next::Form : Next::Plain; break;
        case E::Type::Space: f.next = Next::End; break;   // не бывает: пробелы слиты
        }
        return f;
    };
    // Литералы, на которых слово кончается раньше пробела: литерал вплотную
    // следом, первый литерал участка вплотную следом и то, что за участком.
    const auto stopsOf = [&](int i) {
        QStringList stops;
        int j = i + 1;
        while (j < count) {
            const E& e = m_elements[j];
            if (e.type == E::Type::Literal) {
                stops.append(e.text);
                break;
            }
            if (e.type != E::Type::Optional)
                break;
            if (e.end > j + 1 && m_elements[j + 1].type == E::Type::Literal)
                stops.append(m_elements[j + 1].text);
            j = e.end;
        }
        return stops;
    };

    // С элемента j по какой-либо ветви участков первым непробельным
    // попадается поле без формы — конец text перед ним не определён.
    std::function<bool(int)> plainReach = [&](int j) -> bool {
        while (j < count && m_elements[j].type == E::Type::Space)
            ++j;
        if (j >= count)
            return false;
        const E& e = m_elements[j];
        switch (e.type) {
        case E::Type::Field: return !(e.kindGiven && E::isFormKind(e.kind));
        case E::Type::Optional: return plainReach(j + 1) || plainReach(e.end);
        default: return false;
        }
    };

    QSet<QString> names;
    for (int i = 0; i < count; ++i) {
        E& e = m_elements[i];
        if (e.type != E::Type::Field)
            continue;
        Follow f = followOf(i);
        if (f.next == Next::Optional && plainReach(i + 1))
            f.next = Next::Plain;   // участок открывает (или пропускает к) полю без формы
        const QString shown = e.name.isEmpty() ? QStringLiteral("_") : e.name;
        if (!e.kindGiven) {
            switch (f.next) {
            case Next::End: e.kind = E::Kind::Rest; break;
            case Next::Literal:
            case Next::Optional:
            case Next::Form: e.kind = E::Kind::Text; break;
            case Next::Plain:
                if (!f.space)
                    return fail(C::UndefinedEnd, e.sourcePos, shown,
                                QStringLiteral("Field “%1”: it is not said where it ends — another field "
                                               "follows right after it. Put a separator between them or "
                                               "choose a kind for the field.").arg(shown));
                e.kind = E::Kind::Word;
                break;
            }
        }
        switch (e.kind) {
        case E::Kind::Text:
        case E::Kind::Greedy:
            if (f.next == Next::Plain)
                return fail(C::UndefinedEnd, e.sourcePos, shown,
                            f.space ? QStringLiteral("Field “%1”: it is not said where it ends — only a space "
                                                     "follows it. Choose one word, N words, a width, a wide "
                                                     "gap or a list of values.").arg(shown)
                                    : QStringLiteral("Field “%1”: it is not said where it ends — another "
                                                     "field follows right after it.").arg(shown));
            e.toEnd = f.next == Next::End;
            e.directForm = !f.space && f.next == Next::Form;
            break;
        case E::Kind::Rest:
            if (f.next != Next::End)
                return fail(C::RestNotLast, e.sourcePos, shown,
                            QStringLiteral("Field “%1”: “rest” takes the rest of the line — nothing can "
                                           "follow it.").arg(shown));
            break;
        case E::Kind::Word:
        case E::Kind::Words:
            e.stops = stopsOf(i);
            break;
        case E::Kind::Regex:
            m_hasRegex = true;
            break;
        default:
            break;
        }
        if (!e.name.isEmpty()) {
            const QString key = e.name.toLower();
            if (names.contains(key))
                return fail(C::DuplicateName, e.sourcePos, e.name,
                            QStringLiteral("Field “%1”: there is already a column with this name.").arg(e.name));
            names.insert(key);
            e.column = int(m_columns.size());
            m_columns.append(e.name);
            m_columnTop.append(e.top);
        }
    }
    // Хвостовая колонка — поле rest в конце шаблона.
    for (int i = count - 1; i >= 0; --i) {
        const E& e = m_elements[i];
        if (e.type == E::Type::Space || e.type == E::Type::Optional)
            continue;
        if (e.type == E::Type::Field && e.kind == E::Kind::Rest) {
            m_tailColumn = e.column;
            m_tailElement = i;
        }
        break;
    }
    return true;
}

// ============================================================================
// Разбор
// ============================================================================

struct LineTemplate::Ctx {
    QStringView s;
    qsizetype n = 0;
    LogEntryFields* out = nullptr;
    qint64* steps = nullptr;
    bool distinctive = false;

    void step(qint64 k = 1) const
    {
        if (steps)
            *steps += k;
    }
    qsizetype blanks(qsizetype p) const
    {
        qsizetype q = p;
        while (q < n && isBlank(s[q]))
            ++q;
        step(q - p + 1);
        return q - p;
    }
    bool literalAt(const QString& lit, qsizetype p) const
    {
        step();
        return p + lit.size() <= n && s.mid(p, lit.size()) == lit;
    }
    void set(const Element& e, qsizetype a, qsizetype b, bool trim,
             FieldSpan::Escape escape = FieldSpan::Escape::None) const
    {
        if (e.column < 0 || !out)
            return;
        if (trim)
            trimmedRange(s, a, b);
        out->spans[e.column].set(int(a), int(b - a), escape);
    }
};

qsizetype LineTemplate::formLength(const Element& e, qsizetype pos, Ctx& c, bool* escaped) const
{
    using K = Element::Kind;
    if (escaped)
        *escaped = false;
    qsizetype len = -1;
    switch (e.kind) {
    case K::Time: len = TokenScan::timeAt(c.s, pos); break;
    case K::Level: len = TokenScan::levelAt(c.s, pos); break;
    case K::Int: len = TokenScan::intAt(c.s, pos); break;
    case K::Num: len = TokenScan::numAt(c.s, pos); break;
    case K::Hex: len = TokenScan::hexAt(c.s, pos); break;
    case K::Uuid: len = TokenScan::uuidAt(c.s, pos); break;
    case K::Ip: len = TokenScan::ipAt(c.s, pos); break;
    case K::Path: len = TokenScan::pathAt(c.s, pos); break;
    case K::Quoted: len = TokenScan::quotedAt(c.s, pos, escaped); break;
    case K::Of:
        for (const QString& v : e.valuesByLength) {
            c.step();
            if (pos + v.size() > c.n || c.s.mid(pos, v.size()) != v)
                continue;
            // Значение, кончающееся символом слова, не обрывает слово строки.
            if (isWordChar(v.back()) && pos + v.size() < c.n && isWordChar(c.s[pos + v.size()]))
                continue;
            len = v.size();
            break;
        }
        break;
    case K::Regex: {
        if (pos > c.n)
            return -1;
        const QRegularExpressionMatch m = e.regex.matchView(
            c.s, pos, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
        len = m.hasMatch() ? m.capturedLength(0) : -1;
        break;
    }
    default:
        return -1;
    }
    c.step(len > 0 ? len : 1);
    return len;
}

// Проверка «то, что следом, сходится с pos» (правила 3 и 4): пробелы и
// литералы — целиком, участок — в обе ветви, первое поле: с формой — его
// форма, без формы — да. Конец шаблона — да.
bool LineTemplate::probe(int idx, qsizetype pos, Ctx& c, bool forText) const
{
    const int count = int(m_elements.size());
    while (idx < count) {
        const Element& e = m_elements[idx];
        c.step();
        switch (e.type) {
        case Element::Type::Space: {
            const qsizetype w = c.blanks(pos);
            if (w == 0 && pos < c.n)
                return false;
            pos += w;
            ++idx;
            break;
        }
        case Element::Type::Literal:
            if (!c.literalAt(e.text, pos))
                return false;
            pos += e.text.size();
            ++idx;
            break;
        case Element::Type::Optional:
            if (probe(idx + 1, pos, c, forText))
                return true;
            idx = e.end;
            break;
        case Element::Type::Field:
            if (!e.hasForm())
                return true;
            return formLength(e, pos, c, nullptr) >= 0;
        }
    }
    // Конец шаблона. Для text — это конец строки (дальше только пробелы);
    // для участка (правило 4) — да: что осталось, решает сам разбор.
    if (!forText)
        return true;
    while (pos < c.n && isBlank(c.s[pos]))
        ++pos;
    return pos == c.n;
}

bool LineTemplate::stopAt(const Element& e, qsizetype pos, Ctx& c) const
{
    for (const QString& lit : e.stops) {
        if (c.literalAt(lit, pos))
            return true;
    }
    return false;
}

// Конец поля text / greedy, начатого с start; -1 — то, что следом, не нашлось.
qsizetype LineTemplate::textEnd(int i, qsizetype start, Ctx& c) const
{
    const Element& e = m_elements[i];
    if (e.toEnd)
        return c.n;
    const auto boundary = [&](qsizetype p) {
        if (!e.directForm || p == start || p >= c.n)
            return true;
        return isWordChar(c.s[p - 1]) != isWordChar(c.s[p]);
    };
    if (e.kind == Element::Kind::Greedy) {
        for (qsizetype p = c.n; p >= start; --p) {
            c.step();
            if (boundary(p) && probe(i + 1, p, c, true))
                return p;
        }
        return -1;
    }
    for (qsizetype p = start; p <= c.n; ++p) {
        c.step();
        if (boundary(p) && probe(i + 1, p, c, true))
            return p;
    }
    return -1;
}

bool LineTemplate::matchElement(int i, qsizetype& pos, Ctx& c) const
{
    using K = Element::Kind;
    const Element& e = m_elements[i];
    switch (e.type) {
    case Element::Type::Space: {
        const qsizetype w = c.blanks(pos);
        if (w == 0 && pos < c.n)
            return false;
        pos += w;
        return true;
    }
    case Element::Type::Literal:
        if (!c.literalAt(e.text, pos))
            return false;
        pos += e.text.size();
        if (e.text.size() >= 3)
            c.distinctive = true;
        return true;
    case Element::Type::Optional:
        return false;   // разбирается в match()
    case Element::Type::Field:
        break;
    }

    if (e.hasForm()) {
        bool escaped = false;
        const qsizetype len = formLength(e, pos, c, &escaped);
        if (len < 0)
            return false;
        // quoted: только \" и \\ (строка лога, не JSON — «C:\temp» как есть).
        if (e.kind == K::Quoted)
            c.set(e, pos + 1, pos + len - 1, false,
                  escaped ? FieldSpan::Escape::Quoted : FieldSpan::Escape::None);
        else
            c.set(e, pos, pos + len, false);
        pos += len;
        // Пустое совпадение (регекс «x?») — не свидетельство формата.
        if (len > 0)
            c.distinctive = true;
        return true;
    }

    switch (e.kind) {
    case K::Word: {
        qsizetype p = pos;
        while (p < c.n && !isBlank(c.s[p]) && !stopAt(e, p, c))
            ++p;
        c.step(p - pos);
        if (p == pos)
            return false;
        c.set(e, pos, p, false);
        pos = p;
        return true;
    }
    case K::Words: {
        qsizetype p = pos;
        for (int k = 0; k < e.count; ++k) {
            if (k > 0) {
                const qsizetype w = c.blanks(p);
                if (w == 0)
                    return false;
                p += w;
            }
            const qsizetype from = p;
            const bool last = k == e.count - 1;
            while (p < c.n && !isBlank(c.s[p]) && !(last && stopAt(e, p, c)))
                ++p;
            c.step(p - from);
            if (p == from)
                return false;
        }
        c.set(e, pos, p, false);
        pos = p;
        return true;
    }
    case K::Padded: {
        qsizetype p = pos;
        while (p < c.n) {
            if (c.s[p] == QLatin1Char('\t')
                || (c.s[p] == QLatin1Char(' ') && p + 1 < c.n && isBlank(c.s[p + 1])))
                break;
            ++p;
        }
        c.step(p - pos + 1);
        qsizetype a = pos, b = p;
        trimmedRange(c.s, a, b);
        if (a == b)
            return false;
        c.set(e, pos, p, true);
        pos = p;
        return true;
    }
    case K::Width: {
        if (pos + e.count > c.n)
            return false;
        c.step(e.count);
        c.set(e, pos, pos + e.count, true);
        pos += e.count;
        return true;
    }
    case K::Text:
    case K::Greedy: {
        const qsizetype end = textEnd(i, pos, c);
        if (end < 0)
            return false;
        c.set(e, pos, end, true);
        pos = end;
        return true;
    }
    case K::Rest:
        c.set(e, pos, c.n, true);
        c.step();
        pos = c.n;
        return true;
    default:
        return false;
    }
}

LineTemplate::Outcome LineTemplate::match(QStringView line, LogEntryFields* out, int upTo,
                                          FieldExtractor::Stop* stop, MatchReport* report,
                                          qint64* steps) const
{
    if (stop)
        *stop = FieldExtractor::Stop();
    if (report)
        *report = MatchReport();
    const int columns = int(m_columns.size());
    if (out) {
        if (out->spans.size() != columns)
            out->spans.resize(columns);
        for (FieldSpan& span : out->spans)
            span = FieldSpan();
    }
    if (!m_valid)
        return Outcome::Rejected;

    Ctx c;
    c.s = line;
    c.n = line.size();
    c.out = out;
    c.steps = steps;

    const int count = int(m_elements.size());
    const int stopAfterTop = (upTo >= 0 && upTo < columns) ? m_columnTop[upTo] : -1;
    qsizetype pos = 0;
    int lastMatched = -1;   // последний сошедшийся элемент (верхнего уровня)
    int failed = -1;        // элемент, на котором разбор остановился
    int i = 0;
    while (i < count) {
        if (stopAfterTop >= 0 && i > stopAfterTop && c.distinctive)
            return Outcome::Full;   // upTo: дальше не нужно, ответ «принята» уже известен
        const Element& e = m_elements[i];
        if (e.type == Element::Type::Optional) {
            // Правило 4: участок берётся, если сошёлся сам и элемент за ним.
            const bool distinctiveBefore = c.distinctive;
            qsizetype p = pos;
            bool ok = true;
            for (int j = i + 1; j < e.end && ok; ++j)
                ok = matchElement(j, p, c);
            if (ok && probe(e.end, p, c)) {
                pos = p;
                lastMatched = e.end - 1;
            } else {
                c.distinctive = distinctiveBefore;
                if (out) {
                    for (int j = i + 1; j < e.end; ++j) {
                        if (m_elements[j].column >= 0)
                            out->spans[m_elements[j].column] = FieldSpan();
                    }
                }
            }
            i = e.end;
            continue;
        }
        if (!matchElement(i, pos, c)) {
            failed = i;
            break;
        }
        lastMatched = i;
        ++i;
    }

    const auto reject = [&]() {
        if (out) {
            for (FieldSpan& span : out->spans)
                span = FieldSpan();
        }
        return Outcome::Rejected;
    };

    Outcome result;
    if (failed >= 0) {
        // Правила 5 и 6.
        if (lastMatched < 0 || !c.distinctive) {
            result = reject();
        } else {
            if (m_tailElement > failed && m_tailColumn >= 0)
                c.set(m_elements[m_tailElement], pos, c.n, true);
            result = Outcome::Partial;
        }
        if (stop) {
            stop->afterElement = lastMatched;
            stop->offset = int(pos);
        }
    } else {
        qsizetype rest = pos;
        while (rest < c.n && isBlank(c.s[rest]))
            ++rest;
        if (rest == c.n) {
            result = Outcome::Full;
        } else {
            result = c.distinctive ? Outcome::Partial : reject();
            if (stop) {
                stop->afterElement = lastMatched;
                stop->offset = int(pos);
            }
        }
    }

    if (report) {
        report->partial = result == Outcome::Partial;
        if (result != Outcome::Full) {
            report->afterBlock = lastMatched;
            report->stopOffset = result == Outcome::Rejected && lastMatched < 0 ? 0 : int(pos);
            report->found = line.mid(report->stopOffset, 24).toString();
            report->expectedBlock = failed;
            using X = MatchReport::Expect;
            using K = Element::Kind;
            if (failed < 0) {
                report->expect = X::Nothing;   // шаблон кончился, в строке остался текст
            } else {
                const Element& f = m_elements[failed];
                switch (f.type) {
                case Element::Type::Literal:
                    report->expect = X::Literal;
                    report->literal = f.text;
                    break;
                case Element::Type::Space:
                    report->expect = X::Spaces;
                    break;
                case Element::Type::Optional:
                    break;
                case Element::Type::Field:
                    report->field = f.name;
                    switch (f.kind) {
                    case K::Time: report->expect = X::Timestamp; break;
                    case K::Level: report->expect = X::Level; break;
                    case K::Int: case K::Num: report->expect = X::Number; break;
                    case K::Hex: report->expect = X::Hex; break;
                    case K::Uuid: report->expect = X::Uuid; break;
                    case K::Ip: report->expect = X::Ip; break;
                    case K::Path: report->expect = X::Path; break;
                    case K::Quoted: report->expect = X::Quoted; break;
                    case K::Of:
                        report->expect = X::Value;
                        report->literal = f.values.join(QLatin1Char('|'));
                        break;
                    case K::Regex: report->expect = X::Regex; break;
                    case K::Word: case K::Words: case K::Padded: case K::Width:
                        report->expect = X::Word;
                        break;
                    case K::Text: case K::Greedy: case K::Rest: {
                        // Текст до литерала: литерала в остатке строки нет —
                        // его и не хватило.
                        report->expect = X::Text;
                        for (int j = failed + 1; j < count; ++j) {
                            const Element& g = m_elements[j];
                            if (g.type == Element::Type::Space)
                                continue;
                            if (g.type == Element::Type::Literal) {
                                report->expect = X::Literal;
                                report->literal = g.text;
                            }
                            break;
                        }
                        break;
                    }
                    }
                    break;
                }
            }
        }
    }
    return result;
}

// ============================================================================
// TemplateExtractor
// ============================================================================

TemplateExtractor::TemplateExtractor(std::shared_ptr<const LineTemplate> tpl)
    : m_template(tpl ? std::move(tpl) : std::make_shared<const LineTemplate>())
{
}

TemplateExtractor::TemplateExtractor(const QString& text)
    : m_template(std::make_shared<const LineTemplate>(text))
{
}

MatchReport TemplateExtractor::explain(QStringView line) const
{
    MatchReport report;
    LogEntryFields fields;
    m_template->match(line, &fields, -1, nullptr, &report);
    return report;
}

// ============================================================================
// Файл схемы версии 4
// ============================================================================

bool LineTemplateSchema::read(const QByteArray& json, LineTemplateSchema* out, QString* error)
{
    const auto fail = [error](const QString& why) {
        if (error)
            *error = why;
        return false;
    };
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (!doc.isObject())
        return fail(QStringLiteral("not a JSON object: %1").arg(parseError.errorString()));
    const QJsonObject obj = doc.object();
    if (obj.value(QStringLiteral("version")).toInt() != kVersion)
        return fail(QStringLiteral("version is not %1").arg(kVersion));
    if (obj.value(QStringLiteral("type")).toString() != QLatin1String("line-template"))
        return fail(QStringLiteral("type is not line-template"));
    LineTemplateSchema s;
    s.name = obj.value(QStringLiteral("name")).toString().trimmed();
    s.description = obj.value(QStringLiteral("description")).toString();
    s.templateText = obj.value(QStringLiteral("template")).toString();
    if (s.name.isEmpty() || s.templateText.isEmpty())
        return fail(QStringLiteral("name or template is missing"));
    if (out)
        *out = s;
    return true;
}

bool LineTemplateSchema::templateOf(const QString& definition, QString* templateText)
{
    if (!definition.trimmed().startsWith(QLatin1Char('{')))
        return false;
    const QJsonDocument doc = QJsonDocument::fromJson(definition.toUtf8());
    if (!doc.isObject())
        return false;
    const QJsonObject obj = doc.object();
    if (obj.value(QStringLiteral("version")).toInt() != kVersion
        || obj.value(QStringLiteral("type")).toString() != QLatin1String("line-template"))
        return false;
    const QString text = obj.value(QStringLiteral("template")).toString();
    if (text.isEmpty())
        return false;
    if (templateText)
        *templateText = text;
    return true;
}

QString LineTemplateSchema::definition(const QString& templateText)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("type"), QStringLiteral("line-template"));
    obj.insert(QStringLiteral("version"), kVersion);
    obj.insert(QStringLiteral("template"), templateText);
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}
