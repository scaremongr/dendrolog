#include "ansiescape.h"

#include "textencoding.h"

#include <QVarLengthArray>

#include <cstring>

namespace {

// Разбор последовательностей над единицами U (unsigned char — байты, char16_t
// — UTF-16). Текст между последовательностями отдаётся sink.text(from, to)
// по порядку, параметры SGR (CSI … 'm' без промежуточных) — sink.sgr(from, to).
template <class U, class Sink>
void scan(const U* p, qsizetype n, Sink& sink)
{
    constexpr U kEsc = U(0x1b);
    qsizetype runStart = 0;
    qsizetype i = 0;
    while (i < n) {
        if (p[i] != kEsc) {
            ++i;
            continue;
        }
        sink.text(runStart, i);
        const qsizetype j = i + 1;
        if (j >= n) {
            // Одиночный ESC в конце строки.
            i = n;
            runStart = n;
            break;
        }
        const unsigned c = unsigned(p[j]);
        if (c == '[') {
            // CSI: параметры 0x30–0x3F, промежуточные 0x20–0x2F, финальный 0x40–0x7E.
            qsizetype k = j + 1;
            const qsizetype paramsBegin = k;
            while (k < n && unsigned(p[k]) >= 0x30 && unsigned(p[k]) <= 0x3F)
                ++k;
            const qsizetype paramsEnd = k;
            while (k < n && unsigned(p[k]) >= 0x20 && unsigned(p[k]) <= 0x2F)
                ++k;
            if (k < n && unsigned(p[k]) >= 0x40 && unsigned(p[k]) <= 0x7E) {
                if (unsigned(p[k]) == 'm' && paramsEnd == k)
                    sink.sgr(paramsBegin, paramsEnd);
                i = k + 1;
            } else {
                // Оборвана: снято разобранное, посторонний символ — текст.
                i = k;
            }
        } else if (c == ']' || c == 'P' || c == 'X' || c == '^' || c == '_') {
            // Строковые (OSC, DCS, SOS, PM, APC): до BEL или ST (ESC \).
            qsizetype k = j + 1;
            for (;;) {
                if (k >= n)
                    break;
                const unsigned u = unsigned(p[k]);
                if (u == 0x07) {
                    ++k;
                    break;
                }
                if (u == 0x1b) {
                    if (k + 1 < n && unsigned(p[k + 1]) == '\\')
                        k += 2;
                    // ESC без '\' — обрыв: он начинает следующую последовательность.
                    break;
                }
                if (u == '\n')
                    break;
                ++k;
            }
            i = k;
        } else if (c >= 0x20 && c <= 0x2F) {
            // nF: промежуточные, затем финальный 0x30–0x7E (ESC ( B).
            qsizetype k = j + 1;
            while (k < n && unsigned(p[k]) >= 0x20 && unsigned(p[k]) <= 0x2F)
                ++k;
            i = (k < n && unsigned(p[k]) >= 0x30 && unsigned(p[k]) <= 0x7E) ? k + 1 : k;
        } else if (c >= 0x30 && c <= 0x7E) {
            // Fe / Fp / Fs: два символа.
            i = j + 1;
        } else {
            // Одиночный ESC перед символом, не начинающим последовательность.
            i = j;
        }
        runStart = i;
    }
    sink.text(runStart, n);
}

template <class U>
struct CopySink {
    const U* in;
    U* out;
    qsizetype written = 0;
    void text(qsizetype from, qsizetype to)
    {
        if (to > from) {
            std::memmove(out + written, in + from, size_t(to - from) * sizeof(U));
            written += to - from;
        }
    }
    void sgr(qsizetype, qsizetype) {}
};

// Состояние SGR и сборка спанов.
class StyleBuilder
{
public:
    void applySgr(const QVarLengthArray<int, 16>& params, const QVarLengthArray<bool, 16>& colonAfter)
    {
        const qsizetype n = params.size();
        if (n == 0) {
            reset();
            return;
        }
        for (qsizetype i = 0; i < n; ++i) {
            const int p = params[i];
            // Подпараметры через ':' (ITU T.416) принадлежат своему коду, а не
            // отдельные коды: 4:0 — подчёркивание выключено, 4:3 — волнистое.
            qsizetype groupEnd = i + 1;
            while (groupEnd < n && colonAfter[groupEnd - 1])
                ++groupEnd;
            const bool hasSub = groupEnd > i + 1;
            if (p == 38 || p == 48 || p == 58) {
                quint32 color = AnsiColor::kDefault;
                i = extendedColor(params, colonAfter, i, color);
                // 58 — цвет подчёркивания: операнды съедены, цвет не рисуем.
                if (p != 58 && color != AnsiColor::kDefault)
                    (p == 38 ? m_fg : m_bg) = color;
                continue;
            }
            if (hasSub) {
                if (p == 4) {
                    if (params[i + 1] == 0)
                        m_attrs &= quint8(~AnsiSpan::Underline);
                    else
                        m_attrs |= AnsiSpan::Underline;
                }
                i = groupEnd - 1;
                continue;
            }
            switch (p) {
            case 0: reset(); break;
            case 1: m_attrs |= AnsiSpan::Bold; break;
            case 2: m_attrs |= AnsiSpan::Dim; break;
            case 3: m_attrs |= AnsiSpan::Italic; break;
            case 4: m_attrs |= AnsiSpan::Underline; break;
            case 7: m_attrs |= AnsiSpan::Inverse; break;
            case 21: m_attrs |= AnsiSpan::Underline; break; // двойное подчёркивание
            case 22: m_attrs &= quint8(~(AnsiSpan::Bold | AnsiSpan::Dim)); break;
            case 23: m_attrs &= quint8(~AnsiSpan::Italic); break;
            case 24: m_attrs &= quint8(~AnsiSpan::Underline); break;
            case 27: m_attrs &= quint8(~AnsiSpan::Inverse); break;
            case 39: m_fg = AnsiColor::kDefault; break;
            case 49: m_bg = AnsiColor::kDefault; break;
            default:
                if (p >= 30 && p <= 37)
                    m_fg = AnsiColor::indexed(p - 30);
                else if (p >= 40 && p <= 47)
                    m_bg = AnsiColor::indexed(p - 40);
                else if (p >= 90 && p <= 97)
                    m_fg = AnsiColor::indexed(p - 90 + 8);
                else if (p >= 100 && p <= 107)
                    m_bg = AnsiColor::indexed(p - 100 + 8);
                break;
            }
        }
    }

    void text(int textStart, int textLength)
    {
        if (textLength <= 0 || (m_fg == AnsiColor::kDefault && m_bg == AnsiColor::kDefault
                                && m_attrs == 0))
            return;
        if (!m_spans.isEmpty()) {
            AnsiSpan& last = m_spans.last();
            if (last.start + last.length == textStart && last.fg == m_fg && last.bg == m_bg
                && last.attrs == m_attrs) {
                last.length += textLength;
                return;
            }
        }
        AnsiSpan span;
        span.start = textStart;
        span.length = textLength;
        span.fg = m_fg;
        span.bg = m_bg;
        span.attrs = m_attrs;
        m_spans.append(span);
    }

    AnsiStyles take(int textSize)
    {
        // Позиции UTF-8 по кускам могут разойтись с целым декодированием,
        // только если ESC разорвал символ; спаны не выходят за текст.
        for (qsizetype i = 0; i < m_spans.size();) {
            AnsiSpan& s = m_spans[i];
            if (s.start >= textSize) {
                m_spans.resize(i);
                break;
            }
            s.length = qMin(s.length, textSize - s.start);
            ++i;
        }
        return std::move(m_spans);
    }

private:
    void reset()
    {
        m_fg = AnsiColor::kDefault;
        m_bg = AnsiColor::kDefault;
        m_attrs = 0;
    }

    // 38/48 с позиции i: «;5;n», «;2;r;g;b» или те же через ':' (38:5:n,
    // 38:2::r:g:b, 38:2:cs:r:g:b, 38:2:r:g:b). Возвращает индекс последнего
    // поглощённого параметра.
    static qsizetype extendedColor(const QVarLengthArray<int, 16>& params,
                                   const QVarLengthArray<bool, 16>& colonAfter, qsizetype i,
                                   quint32& color)
    {
        const qsizetype n = params.size();
        if (colonAfter[i]) {
            // Подпараметры через ':' — до первого ';'.
            qsizetype end = i + 1;
            while (end < n && colonAfter[end - 1])
                ++end;
            // [i+1, end) — подпараметры.
            const qsizetype count = end - (i + 1);
            if (count >= 2 && params[i + 1] == 5) {
                color = AnsiColor::indexed(qBound(0, params[i + 2], 255));
            } else if (params.value(i + 1) == 2 && count >= 4) {
                const qsizetype rgbAt = count >= 5 ? i + 3 : i + 2;
                color = AnsiColor::rgb(qBound(0, params[rgbAt], 255),
                                       qBound(0, params[rgbAt + 1], 255),
                                       qBound(0, params[rgbAt + 2], 255));
            }
            return end - 1;
        }
        if (i + 2 < n && params[i + 1] == 5) {
            color = AnsiColor::indexed(qBound(0, params[i + 2], 255));
            return i + 2;
        }
        if (i + 4 < n && params[i + 1] == 2) {
            color = AnsiColor::rgb(qBound(0, params[i + 2], 255), qBound(0, params[i + 3], 255),
                                   qBound(0, params[i + 4], 255));
            return i + 4;
        }
        // Неполная форма — остаток параметров не разбирается.
        return n - 1;
    }

    quint32 m_fg = AnsiColor::kDefault;
    quint32 m_bg = AnsiColor::kDefault;
    quint8 m_attrs = 0;
    AnsiStyles m_spans;
};

template <class U>
void parseSgr(const U* p, qsizetype from, qsizetype to, StyleBuilder& builder)
{
    // Частная форма (`ESC [ ? … m` и т. п.) — не SGR.
    if (from < to) {
        const unsigned c = unsigned(p[from]);
        if (c == '<' || c == '=' || c == '>' || c == '?')
            return;
    }
    QVarLengthArray<int, 16> params;
    QVarLengthArray<bool, 16> colonAfter;
    if (from == to) {
        builder.applySgr(params, colonAfter);
        return;
    }
    int value = 0;
    for (qsizetype k = from; k <= to; ++k) {
        const unsigned c = k < to ? unsigned(p[k]) : unsigned(';');
        if (c >= '0' && c <= '9') {
            if (value < 100000)
                value = value * 10 + int(c - '0');
        } else if (c == ';' || c == ':') {
            params.append(value);
            colonAfter.append(c == ':' && k < to);
            value = 0;
        } else {
            return; // посторонний параметр — последовательность не понимаем
        }
    }
    builder.applySgr(params, colonAfter);
}

template <class U, class TextLength>
struct StyleSink {
    const U* p;
    StyleBuilder& builder;
    TextLength textLength;
    int textPos = 0;
    void text(qsizetype from, qsizetype to)
    {
        if (to <= from)
            return;
        const int len = textLength(p + from, to - from);
        builder.text(textPos, len);
        textPos += len;
    }
    void sgr(qsizetype from, qsizetype to) { parseSgr(p, from, to, builder); }
};

// Последовательность с ESC на позиции k закончена до n? (та же грамматика,
// что у scan: обрыв посторонним символом — тоже конец.)
template <class U>
bool terminatedBefore(const U* p, qsizetype k, qsizetype n)
{
    const qsizetype j = k + 1;
    if (j >= n)
        return false;
    const unsigned c = unsigned(p[j]);
    if (c == '[') {
        qsizetype m = j + 1;
        while (m < n && unsigned(p[m]) >= 0x20 && unsigned(p[m]) <= 0x3F)
            ++m;
        return m < n;
    }
    if (c == ']' || c == 'P' || c == 'X' || c == '^' || c == '_') {
        for (qsizetype m = j + 1; m < n; ++m) {
            const unsigned u = unsigned(p[m]);
            if (u == 0x07 || u == '\n')
                return true;
            if (u == 0x1b)
                return m + 1 < n;
        }
        return false;
    }
    if (c >= 0x20 && c <= 0x2F) {
        qsizetype m = j + 1;
        while (m < n && unsigned(p[m]) >= 0x20 && unsigned(p[m]) <= 0x2F)
            ++m;
        return m < n;
    }
    return true;
}

template <class U>
qsizetype completeEndOf(const U* p, qsizetype n)
{
    const qsizetype from = qMax<qsizetype>(0, n - AnsiEscape::kMaxCutLookBack);
    for (qsizetype k = n - 1; k >= from; --k) {
        if (unsigned(p[k]) == 0x1b)
            return terminatedBefore(p, k, n) ? n : k;
    }
    return n;
}

} // namespace

namespace AnsiEscape {

qsizetype completeEnd(QByteArrayView data)
{
    return completeEndOf(reinterpret_cast<const unsigned char*>(data.data()), data.size());
}

qsizetype completeEnd(QStringView data)
{
    return completeEndOf(data.utf16(), data.size());
}

qsizetype stripBytes(const char* in, qsizetype len, char* out)
{
    const auto* u = reinterpret_cast<const unsigned char*>(in);
    CopySink<unsigned char> sink{u, reinterpret_cast<unsigned char*>(out)};
    scan(u, len, sink);
    return sink.written;
}

QByteArray strip(QByteArrayView bytes)
{
    if (!hasEscape(bytes))
        return bytes.toByteArray();
    QByteArray out(bytes.size(), Qt::Uninitialized);
    out.resize(stripBytes(bytes.data(), bytes.size(), out.data()));
    return out;
}

void stripInPlace(QString& text)
{
    if (!hasEscape(QStringView(text)))
        return;
    const qsizetype n = text.size();
    char16_t* data = reinterpret_cast<char16_t*>(text.data());
    CopySink<char16_t> sink{data, data};
    scan(static_cast<const char16_t*>(data), n, sink);
    text.truncate(sink.written);
}

AnsiStyles styles(QByteArrayView bytes, const TextDecoder& decoder)
{
    if (!hasEscape(bytes))
        return {};
    const auto* u = reinterpret_cast<const unsigned char*>(bytes.data());
    StyleBuilder builder;
    const auto length = [&decoder](const unsigned char* run, qsizetype len) {
        const char* c = reinterpret_cast<const char*>(run);
        if (EncodingDetector::firstHighByte(c, len) < 0)
            return int(len);
        return int(decoder.decode(c, len).size());
    };
    StyleSink<unsigned char, decltype(length)> sink{u, builder, length};
    scan(u, bytes.size(), sink);
    return builder.take(int(decoder.decode(bytes).size()));
}

AnsiStyles styles(QStringView text)
{
    if (!hasEscape(text))
        return {};
    const char16_t* p = text.utf16();
    StyleBuilder builder;
    const auto length = [](const char16_t*, qsizetype len) { return int(len); };
    StyleSink<char16_t, decltype(length)> sink{p, builder, length};
    scan(p, text.size(), sink);
    return builder.take(sink.textPos);
}

} // namespace AnsiEscape
