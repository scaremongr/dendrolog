#include "lineclassifier.h"

#include <limits>

// ============================================================================
// Ручные сканеры вместо QRegularExpression. Оба метода зовутся на КАЖДУЮ
// строку лога; на десятках миллионов строк два вызова pcre2-матчера стоили
// минуты — ручной проход по UTF-16 юнитам на порядок дешевле.
//
// Семантика — бит-в-бит эквивалент регексов (закреплено тестом
// в tests/lineindex_smoke):
//   isoTimestampDetectPattern():  (\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}:\d{2})
//                                 + timestampSuffixDetectPattern() (дробь, зона)
//   levelDetectPattern():         \b(INFO|WARN|WARNING|ERROR|DEBUG|TRACE|FATAL)\b
//                                 (CaseInsensitive)
// Важные детали эквивалентности:
//   • \d и \w у QRegularExpression без UseUnicodePropertiesOption — только
//     ASCII; не-ASCII символы (включая суррогаты) для сканера — разделители;
//   • регистронезависимость достаточна ASCII-сворачиванием: ни один
//     не-ASCII символ simple-case-fold'ом не совпадает с буквами ключевых
//     слов (K и S, у которых такие пары есть, в словах не встречаются).
//
// Политика времени: метка без зоны — локальное время машины; явная зона
// (Z, ±hh[:mm] вплотную, ±hh[:]mm или UTC/GMT через пробел, в пределах
// ±14:00) задаёт момент точно. Дробная часть любой длины усекается до
// миллисекунд (".1" — 100 мс). Текст строки не меняется — зона влияет только
// на момент: порядок, фильтр по времени, таймлайн. Сверка с заранее
// известными моментами — testTimestampGolden в tests/lineindex_smoke.
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
    bool hasZone = false;   // иначе — локальное время машины
    int offsetMinutes = 0;  // к востоку от UTC
};

inline bool digitAt(const QChar* d, qsizetype n, qsizetype at)
{
    return at < n && isAsciiDigit(d[at].unicode());
}

// Две ASCII-цифры по смещению → 0..99, иначе -1.
inline int twoDigitsAt(const QChar* d, qsizetype n, qsizetype at)
{
    return (digitAt(d, n, at) && digitAt(d, n, at + 1)) ? num2(d, at) : -1;
}

// Реальные зоны лежат в пределах −12:00…+14:00; дальше ±14:00 — не зона.
constexpr int kMaxOffsetMinutes = 14 * 60;

// Зона сразу после секунд (или дробной части) с позиции pos. Порядок
// разбора повторяет регекс timestampSuffixDetectPattern() вместе с его
// откатами: у «вплотную» длинная форма ±hh[:]mm берётся, только если за ней
// не цифра, иначе ±hh; через пробел — только полная форма. Синтаксически
// зона, но невозможное смещение (mm > 59 или дальше ±14:00) — как метка
// без зоны.
void scanZone(const QChar* d, qsizetype n, qsizetype pos, TsComponents& c)
{
    if (pos >= n)
        return;
    const char16_t ch = d[pos].unicode();
    int sign = 0, hh = -1, mm = 0;
    if (ch == u'Z' || ch == u'z') {
        if (pos + 1 < n && isWordChar(d[pos + 1].unicode()))
            return;
        c.hasZone = true;
        return;
    }
    if (ch == u'+' || ch == u'-') {
        hh = twoDigitsAt(d, n, pos + 1);
        if (hh < 0)
            return;
        const qsizetype afterHours = pos + 3;
        const qsizetype mmAt = (afterHours < n && d[afterHours] == QLatin1Char(':'))
            ? afterHours + 1 : afterHours;
        const int minutes = twoDigitsAt(d, n, mmAt);
        if (minutes >= 0 && !digitAt(d, n, mmAt + 2))
            mm = minutes;
        else if (digitAt(d, n, afterHours))
            return;
        sign = ch == u'+' ? 1 : -1;
    } else if (ch == u' ' && pos + 1 < n) {
        const char16_t next = d[pos + 1].unicode();
        if (next == u'+' || next == u'-') {
            hh = twoDigitsAt(d, n, pos + 2);
            if (hh < 0)
                return;
            const qsizetype mmAt = (pos + 4 < n && d[pos + 4] == QLatin1Char(':'))
                ? pos + 5 : pos + 4;
            mm = twoDigitsAt(d, n, mmAt);
            if (mm < 0 || digitAt(d, n, mmAt + 2))
                return;
            sign = next == u'+' ? 1 : -1;
        } else {
            // Самый частый случай горячего пути: «метка, пробел, уровень» —
            // отсекаем по первой букве, без сравнения строк.
            if ((next != u'U' && next != u'G') || pos + 4 > n)
                return;
            const char16_t b = d[pos + 2].unicode(), e = d[pos + 3].unicode();
            const bool utc = next == u'U' && b == u'T' && e == u'C';
            const bool gmt = next == u'G' && b == u'M' && e == u'T';
            if (!utc && !gmt)
                return;
            const qsizetype after = pos + 4;
            if (after < n) {
                const char16_t a = d[after].unicode();
                if (isWordChar(a) || a == u'+' || a == u'-')
                    return; // "UTC+3" и подобное — не разбираем, как без зоны
            }
            c.hasZone = true;
            return;
        }
    } else {
        return;
    }
    if (mm > 59 || hh * 60 + mm > kMaxOffsetMinutes)
        return;
    c.hasZone = true;
    c.offsetMinutes = sign * (hh * 60 + mm);
}

// Дробная часть ([.,]\d+ — жадно все цифры) и зона после секунд с позиции
// pos. Миллисекунды — первые три цифры, дополненные нулями: ".1" = 100 мс,
// ".123456789" = 123 мс.
void scanFractionAndZone(const QChar* d, qsizetype n, qsizetype pos, TsComponents& c)
{
    c.millis = 0;
    c.hasZone = false;
    c.offsetMinutes = 0;
    if (pos + 1 < n && (d[pos] == QLatin1Char('.') || d[pos] == QLatin1Char(','))
        && isAsciiDigit(d[pos + 1].unicode())) {
        int digits = 0;
        for (++pos; pos < n && isAsciiDigit(d[pos].unicode()); ++pos, ++digits) {
            if (digits < 3)
                c.millis = c.millis * 10 + (d[pos].unicode() - u'0');
        }
        for (; digits < 3; ++digits)
            c.millis *= 10;
    }
    scanZone(d, n, pos, c);
}

// Дней от 1970-01-01 в пролептическом григорианском календаре
// (days_from_civil, H. Hinnant). Год — в нумерации QDate: нулевого года нет,
// -1 — это 1 г. до н. э. (фолбэк-форматы через toInt пропускают знак).
qint64 daysFromCivil(int y, int m, int d)
{
    if (y < 0)
        ++y; // к астрономической нумерации, где 1 г. до н. э. — нулевой
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return qint64(era) * 146097 + doe - 719468;
}

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

// Момент метки в мс epoch: с явной зоной — арифметикой, без QDateTime и
// без базы зон; без зоны — локальное время через кэш по минуте.
qint64 epochMs(const TsComponents& c)
{
    if (!c.hasZone)
        return cachedEpochMs(c);
    const qint64 seconds = daysFromCivil(c.year, c.month, c.day) * 86400
        + c.hour * 3600 + c.minute * 60 + c.second - c.offsetMinutes * 60;
    return seconds * 1000 + c.millis;
}

// Самое левое вхождение формы dddd-dd-dd[ T]dd:dd:dd, затем дробная часть
// и зона. true — форма найдена и разобрана (валидность даты/времени НЕ
// проверена: невалидная форма — отказ без отката к другим форматам);
// false — формы в строке нет.
bool scanIsoTimestamp(const QString& line, TsComponents& c)
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
        return false;

    c.year   = num2(d, at) * 100 + num2(d, at + 2);
    c.month  = num2(d, at + 5);
    c.day    = num2(d, at + 8);
    c.hour   = num2(d, at + 11);
    c.minute = num2(d, at + 14);
    c.second = num2(d, at + 17);
    scanFractionAndZone(d, n, at + 19, c);
    return true;
}

// Ручные фолбэк-форматы (dd/MM/yyyy, MM/dd/yyyy, dd.MM.yyyy — с позиции 0),
// за секундами — те же дробная часть и зона, что у ISO-формы.
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
            c.hour = hour; c.minute = minute; c.second = second;
            scanFractionAndZone(line.constData(), line.size(), 19, c);
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
    qint64 msecs = 0;
    if (detectTimestampMs(line, msecs)) {
        ts = QDateTime::fromMSecsSinceEpoch(msecs);
        return true;
    }
    ts = QDateTime();
    return false;
}

bool LineClassifier::detectTimestampMs(const QString &line, qint64 &msecs) const
{
    // Без построения QDateTime вовсе — горячий путь индексатора, которому
    // нужны только мс epoch.
    TsComponents c;
    if (scanIsoTimestamp(line, c)) {
        if (QDate::isValid(c.year, c.month, c.day)
            && QTime::isValid(c.hour, c.minute, c.second, c.millis)) {
            msecs = epochMs(c);
            return true;
        }
        return false; // невалидная дата/время, несмотря на совпадение формы
    }

    if (scanFallbackFormats(line, m_timeFormats, c)) {
        msecs = epochMs(c);
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
