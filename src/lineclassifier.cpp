#include "lineclassifier.h"

#include <limits>

// ============================================================================
// Ручные сканеры вместо QRegularExpression. Оба метода зовутся на КАЖДУЮ
// строку лога; на десятках миллионов строк два вызова pcre2-матчера стоили
// минуты — ручной проход по UTF-16 юнитам на порядок дешевле.
//
// Семантика — бит-в-бит эквивалент прежних регексов (закреплено тестом
// в tests/lineindex_smoke):
//   isoTimestampDetectPattern():  (\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}:\d{2})([.,]\d+)?
//   levelDetectPattern():         \b(INFO|WARN|WARNING|ERROR|DEBUG|TRACE|FATAL)\b
//                                 (CaseInsensitive)
// Важные детали эквивалентности:
//   • \d и \w у QRegularExpression без UseUnicodePropertiesOption — только
//     ASCII; не-ASCII символы (включая суррогаты) для сканера — разделители;
//   • регистронезависимость достаточна ASCII-сворачиванием: ни один
//     не-ASCII символ simple-case-fold'ом не совпадает с буквами ключевых
//     слов (K и S, у которых такие пары есть, в словах не встречаются).
// ============================================================================

namespace {

inline bool isAsciiDigit(char16_t c)
{
    return c >= u'0' && c <= u'9';
}

// \w из PCRE2 без UCP: ASCII-буквы, цифры, подчёркивание.
inline bool isWordChar(char16_t c)
{
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z')
        || isAsciiDigit(c) || c == u'_';
}

inline char16_t toUpperAscii(char16_t c)
{
    return (c >= u'a' && c <= u'z') ? char16_t(c - (u'a' - u'A')) : c;
}

// Две ASCII-цифры по смещению → число 0..99 (цифры уже проверены).
inline int num2(const QChar* d, qsizetype off)
{
    return (d[off].unicode() - u'0') * 10 + (d[off + 1].unicode() - u'0');
}

struct TsComponents {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, millis = 0;
};

// Компоненты локального времени → мс epoch с кэшем конверсии по минуте.
// Конверсия локаль→epoch (QDateTime) на Windows стоит микросекунды; прежний
// код делал её до трёх раз на строку (setDate, setTime, toMSecsSinceEpoch) —
// это, а не регексы, было главной стоимостью индексации (~6 мкс/строку).
// Смещение зоны постоянно внутри минуты (переходы DST современных зон
// выровнены по минутам), поэтому значения бит-в-бит совпадают с прямой
// конверсией. Кэш thread_local — классификатор делят конкурентные воркеры.
qint64 cachedEpochMs(const TsComponents& c)
{
    const qint64 key = ((((qint64(c.year) * 16 + c.month) * 32 + c.day) * 32
                        + c.hour) * 64 + c.minute);
    thread_local qint64 lastKey = std::numeric_limits<qint64>::min();
    thread_local qint64 lastMinuteEpochMs = 0;
    if (key != lastKey) {
        lastMinuteEpochMs = QDateTime(QDate(c.year, c.month, c.day),
                                      QTime(c.hour, c.minute))
                                .toMSecsSinceEpoch();
        lastKey = key;
    }
    return lastMinuteEpochMs + c.second * 1000 + c.millis;
}

// Самое левое вхождение формы dddd-dd-dd[ T]dd:dd:dd (+ [.,]цифры).
// Возвращает: 1 — форма найдена и разобрана (валидность даты/времени НЕ
// проверена), -1 — форма найдена, но отвергнута (переполнение миллисекунд,
// как у прежнего QString::toInt — отказ без отката к другим форматам),
// 0 — формы в строке нет.
int scanIsoTimestamp(const QString& line, TsComponents& c)
{
    const QChar* d = line.constData();
    const qsizetype n = line.size();

    // Сначала разделители — они отсеивают почти все позиции на первом же
    // сравнении, цифры проверяются только у выживших кандидатов.
    qsizetype at = -1;
    for (qsizetype i = 0; i + 19 <= n; ++i) {
        if (d[i + 4] != QLatin1Char('-') || d[i + 7] != QLatin1Char('-')
            || d[i + 13] != QLatin1Char(':') || d[i + 16] != QLatin1Char(':'))
            continue;
        const char16_t sep = d[i + 10].unicode();
        if (sep != u' ' && sep != u'T')
            continue;
        static constexpr int kDigitOffsets[14] =
            { 0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18 };
        bool digitsOk = true;
        for (int o : kDigitOffsets) {
            if (!isAsciiDigit(d[i + o].unicode())) {
                digitsOk = false;
                break;
            }
        }
        if (digitsOk) {
            at = i;
            break;
        }
    }
    if (at < 0)
        return 0;

    c.year   = num2(d, at) * 100 + num2(d, at + 2);
    c.month  = num2(d, at + 5);
    c.day    = num2(d, at + 8);
    c.hour   = num2(d, at + 11);
    c.minute = num2(d, at + 14);
    c.second = num2(d, at + 17);

    c.millis = 0;
    const qsizetype dot = at + 19;
    if (dot + 1 < n
        && (d[dot] == QLatin1Char('.') || d[dot] == QLatin1Char(','))
        && isAsciiDigit(d[dot + 1].unicode())) {
        // ([.,]\d+) — жадно вся цепочка цифр.
        qint64 v = 0;
        bool overflow = false;
        for (qsizetype k = dot + 1; k < n && isAsciiDigit(d[k].unicode()); ++k) {
            if (!overflow) {
                v = v * 10 + (d[k].unicode() - u'0');
                overflow = v > qint64(std::numeric_limits<int>::max());
            }
        }
        if (overflow)
            return -1;
        c.millis = int(v);
    }
    return 1;
}

// Ручные фолбэк-форматы (dd/MM/yyyy, MM/dd/yyyy, dd.MM.yyyy — с позиции 0).
// true — c заполнен И дата/время валидны (проверка per-format, как раньше).
bool scanFallbackFormats(const QString& line, const QStringList& formats,
                         TsComponents& c)
{
    const QStringView lineRef{line};
    bool convOk = true;

    for (const QString& formatString : formats) {
        if (formatString.startsWith(QStringLiteral("yyyy-MM-dd"))) {
            continue; // Эти должны были быть пойманы сканером ISO-формы
        }

        int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, millis = 0;
        convOk = true;

        if (formatString == QLatin1String("dd/MM/yyyy HH:mm:ss")) {
            if (lineRef.length() < 19) continue; // Минимальная длина
            if (lineRef.at(2) != QLatin1Char('/') || lineRef.at(5) != QLatin1Char('/') ||
                lineRef.at(10) != QLatin1Char(' ') || lineRef.at(13) != QLatin1Char(':') || lineRef.at(16) != QLatin1Char(':')) {
                continue;
            }
            day    = lineRef.mid(0, 2).toInt(&convOk); if (!convOk) continue;
            month  = lineRef.mid(3, 2).toInt(&convOk); if (!convOk) continue;
            year   = lineRef.mid(6, 4).toInt(&convOk); if (!convOk) continue;
            hour   = lineRef.mid(11, 2).toInt(&convOk); if (!convOk) continue;
            minute = lineRef.mid(14, 2).toInt(&convOk); if (!convOk) continue;
            second = lineRef.mid(17, 2).toInt(&convOk); if (!convOk) continue;
            // Миллисекунды не предусмотрены этим форматом
        } else if (formatString == QLatin1String("MM/dd/yyyy HH:mm:ss")) {
            if (lineRef.length() < 19) continue;
            if (lineRef.at(2) != QLatin1Char('/') || lineRef.at(5) != QLatin1Char('/') ||
                lineRef.at(10) != QLatin1Char(' ') || lineRef.at(13) != QLatin1Char(':') || lineRef.at(16) != QLatin1Char(':')) {
                continue;
            }
            month  = lineRef.mid(0, 2).toInt(&convOk); if (!convOk) continue;
            day    = lineRef.mid(3, 2).toInt(&convOk); if (!convOk) continue;
            year   = lineRef.mid(6, 4).toInt(&convOk); if (!convOk) continue;
            hour   = lineRef.mid(11, 2).toInt(&convOk); if (!convOk) continue;
            minute = lineRef.mid(14, 2).toInt(&convOk); if (!convOk) continue;
            second = lineRef.mid(17, 2).toInt(&convOk); if (!convOk) continue;
        } else if (formatString == QLatin1String("dd.MM.yyyy HH:mm:ss")) {
            if (lineRef.length() < 19) continue;
             if (lineRef.at(2) != QLatin1Char('.') || lineRef.at(5) != QLatin1Char('.') ||
                lineRef.at(10) != QLatin1Char(' ') || lineRef.at(13) != QLatin1Char(':') || lineRef.at(16) != QLatin1Char(':')) {
                continue;
            }
            day    = lineRef.mid(0, 2).toInt(&convOk); if (!convOk) continue;
            month  = lineRef.mid(3, 2).toInt(&convOk); if (!convOk) continue;
            year   = lineRef.mid(6, 4).toInt(&convOk); if (!convOk) continue;
            hour   = lineRef.mid(11, 2).toInt(&convOk); if (!convOk) continue;
            minute = lineRef.mid(14, 2).toInt(&convOk); if (!convOk) continue;
            second = lineRef.mid(17, 2).toInt(&convOk); if (!convOk) continue;
        } else {
            continue; // Формат без ручного парсера
        }

        if (convOk && QDate::isValid(year, month, day) && QTime::isValid(hour, minute, second, millis)) {
            c.year = year; c.month = month; c.day = day;
            c.hour = hour; c.minute = minute; c.second = second; c.millis = millis;
            return true;
        }
    }
    return false;
}

} // namespace

LineClassifier::LineClassifier()
{
    m_timeFormats = {
        "yyyy-MM-dd HH:mm:ss,zzz",
        "yyyy-MM-dd HH:mm:ss.zzz",
        "yyyy-MM-dd HH:mm:ss",
        "dd/MM/yyyy HH:mm:ss",
        "MM/dd/yyyy HH:mm:ss",
        "dd.MM.yyyy HH:mm:ss"
    };
}

bool LineClassifier::detectTimestamp(const QString &line, QDateTime &ts) const
{
    // QDateTime строится ИЗ мс epoch (одна конверсия, и та из кэша по минуте)
    // вместо прежних setDate+setTime (две конверсии локаль→epoch на строку).
    TsComponents c;
    const int iso = scanIsoTimestamp(line, c);
    if (iso != 0) {
        if (iso > 0 && QDate::isValid(c.year, c.month, c.day)
            && QTime::isValid(c.hour, c.minute, c.second, c.millis)) {
            ts = QDateTime::fromMSecsSinceEpoch(cachedEpochMs(c));
            return true;
        }
        return false; // невалидная дата/время, несмотря на совпадение формы
    }

    if (scanFallbackFormats(line, m_timeFormats, c)) {
        ts = QDateTime::fromMSecsSinceEpoch(cachedEpochMs(c));
        return true;
    }

    ts = QDateTime();
    return false;
}

bool LineClassifier::detectTimestampMs(const QString &line, qint64 &msecs) const
{
    // Как detectTimestamp, но без построения QDateTime вовсе — для горячего
    // пути индексатора, которому нужны только мс epoch.
    TsComponents c;
    const int iso = scanIsoTimestamp(line, c);
    if (iso != 0) {
        if (iso > 0 && QDate::isValid(c.year, c.month, c.day)
            && QTime::isValid(c.hour, c.minute, c.second, c.millis)) {
            msecs = cachedEpochMs(c);
            return true;
        }
        return false;
    }

    if (scanFallbackFormats(line, m_timeFormats, c)) {
        msecs = cachedEpochMs(c);
        return true;
    }
    return false;
}

bool LineClassifier::detectLogLevel(const QString &line, LogLevel &level) const
{
    const QChar* d = line.constData();
    const qsizetype n = line.size();

    // Самое левое ЦЕЛОЕ слово (границы — не-\w символы), равное одному из
    // ключевых. Альтернативы регекса различимы по длине — неоднозначности нет.
    qsizetype i = 0;
    while (i < n) {
        if (!isWordChar(d[i].unicode())) {
            ++i;
            continue;
        }
        qsizetype j = i + 1;
        while (j < n && isWordChar(d[j].unicode()))
            ++j;
        const qsizetype len = j - i;

        LogLevel found = LogLevel::Unknown;
        if (len >= 4 && len <= 7) {
            char16_t w[7];
            for (qsizetype k = 0; k < len; ++k)
                w[k] = toUpperAscii(d[i + k].unicode());
            const auto is = [&](const char16_t* kw) {
                for (qsizetype k = 0; k < len; ++k)
                    if (w[k] != kw[k])
                        return false;
                return true;
            };
            switch (len) {
            case 4:
                if (is(u"INFO"))         found = LogLevel::Info;
                else if (is(u"WARN"))    found = LogLevel::Warn;
                break;
            case 5:
                if (is(u"ERROR"))        found = LogLevel::Error;
                else if (is(u"DEBUG"))   found = LogLevel::Debug;
                else if (is(u"TRACE"))   found = LogLevel::Trace;
                else if (is(u"FATAL"))   found = LogLevel::Fatal;
                break;
            case 7:
                if (is(u"WARNING"))      found = LogLevel::Warn;
                break;
            default:
                break;
            }
        }
        if (found != LogLevel::Unknown) {
            level = found;
            return true;
        }
        i = j;
    }
    return false;
}
