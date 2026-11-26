#include "lineclassifier.h"
#include "logfile.h"
#include "logpattern.h"

#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include <cstring>

#include <initializer_list>
#include <limits>

// ============================================================================
// Ручные сканеры вместо QRegularExpression. Оба метода зовутся на КАЖДУЮ
// строку лога; на десятках миллионов строк два вызова pcre2-матчера стоили
// минуты — ручной проход по UTF-16 юнитам на порядок дешевле.
//
// Семантика — бит-в-бит эквивалент регексов (закреплено тестом
// в tests/lineindex_smoke):
//   isoTimestampDetectPattern():    dddd-dd-dd или dddd/dd/dd, [ T], dd:dd:dd
//   accessTimestampDetectPattern(): dd/Mmm/dddd:dd:dd:dd (access-лог)
//   syslogTimestampDetectPattern(): ^Mmm dd hh:mm:ss, день может быть « 8»
//                                   или «8» (RFC 3164, без года)
//   clockTimestampDetectPattern():  ^[?hh:mm:ss, за секундами не цифра и не
//                                   «:», дробь — без зоны (F-11, только с
//                                   контекстом файла — ClockDates)
//   за секундами любой формы — timestampSuffixDetectPattern() (дробь, зона)
//   levelDetectPattern():         \b(слово из kLevelWords, logentry.h)\b,
//                                 регистр сворачивается только для ASCII
// Где в строке: метка на позиции 0 (любая форма, в том числе фолбэк-формы
// dd/MM/yyyy, MM/dd/yyyy, dd.MM.yyyy и syslog) главнее метки в середине;
// иначе — самая левая из форм с полной датой (ISO, dddd/dd/dd, access),
// которые, как ISO, ищутся по всей строке. Форма без года (syslog) — только
// с начала строки: в тексте сообщения её нечем отличить от метки. Найденная
// форма с невалидной датой — не метка, без отката к следующей; формы
// позиции 0 с невалидной датой просто не совпали.
// Месяцы — английские сокращения Jan…Dec, регистр сворачивается для ASCII.
// Год метки без года — по опорной дате (LineClassifier::YearReference).
// Важные детали эквивалентности:
//   • \d и \w у QRegularExpression без UseUnicodePropertiesOption — только
//     ASCII; не-ASCII символы (включая суррогаты) для сканера — разделители;
//   • регистр сворачивается только для ASCII, и в эталонном регексе тоже
//     (классы [Ss] вместо опции CaseInsensitive): в словах есть S, а «ſ»
//     (U+017F) опция приравняла бы к ней. Для сканера «ſ» — разделитель.
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

// Номер месяца 1..12 по английскому сокращению из трёх букв с позиции at
// (регистр сворачивается для ASCII), иначе 0.
int monthAt(const QChar* d, qsizetype n, qsizetype at)
{
    if (at + 3 > n)
        return 0;
    const char16_t a = toUpperAscii(d[at].unicode());
    const char16_t b = toUpperAscii(d[at + 1].unicode());
    const char16_t c = toUpperAscii(d[at + 2].unicode());
    switch (a) {
    case u'J':
        if (b == u'A' && c == u'N') return 1;
        if (b == u'U' && c == u'N') return 6;
        if (b == u'U' && c == u'L') return 7;
        return 0;
    case u'F': return (b == u'E' && c == u'B') ? 2 : 0;
    case u'M':
        if (b == u'A' && c == u'R') return 3;
        if (b == u'A' && c == u'Y') return 5;
        return 0;
    case u'A':
        if (b == u'P' && c == u'R') return 4;
        if (b == u'U' && c == u'G') return 8;
        return 0;
    case u'S': return (b == u'E' && c == u'P') ? 9 : 0;
    case u'O': return (b == u'C' && c == u'T') ? 10 : 0;
    case u'N': return (b == u'O' && c == u'V') ? 11 : 0;
    case u'D': return (b == u'E' && c == u'C') ? 12 : 0;
    default:   return 0;
    }
}

bool digitsAt(const QChar* d, qsizetype at, std::initializer_list<int> offsets)
{
    for (int o : offsets)
        if (!isAsciiDigit(d[at + o].unicode()))
            return false;
    return true;
}

// Формы с полной датой, которые ищутся по всей строке.
enum class MidForm { None, Iso, Access };

// Самое левое вхождение формы с полной датой:
//   Iso    — dddd-dd-dd[ T]dd:dd:dd или dddd/dd/dd[ T]dd:dd:dd (nginx error);
//   Access — dd/Mmm/dddd:dd:dd:dd.
// Один проход: на каждой позиции сначала разделители — они отсеивают почти
// все позиции на первом же сравнении, цифры и месяц проверяются только у
// выживших кандидатов. Две формы не могут начаться в одной позиции (у ISO
// третий символ — цифра, у access — «/»).
MidForm scanMidForm(const QChar* d, qsizetype n, qsizetype& at)
{
    for (qsizetype i = 0; i + 19 <= n; ++i) {
        const char16_t s4 = d[i + 4].unicode();
        if ((s4 == u'-' || s4 == u'/') && d[i + 7].unicode() == s4
            && d[i + 13] == QLatin1Char(':') && d[i + 16] == QLatin1Char(':')) {
            const char16_t sep = d[i + 10].unicode();
            if ((sep == u' ' || sep == u'T')
                && digitsAt(d, i, { 0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18 })) {
                at = i;
                return MidForm::Iso;
            }
        }
        if (d[i + 2] == QLatin1Char('/') && i + 20 <= n && d[i + 6] == QLatin1Char('/')
            && d[i + 11] == QLatin1Char(':') && d[i + 14] == QLatin1Char(':')
            && d[i + 17] == QLatin1Char(':')
            && digitsAt(d, i, { 0, 1, 7, 8, 9, 10, 12, 13, 15, 16, 18, 19 })
            && monthAt(d, n, i + 3) != 0) {
            at = i;
            return MidForm::Access;
        }
    }
    return MidForm::None;
}

// Разбор найденной формы с полной датой (валидность НЕ проверена).
void parseMidForm(const QChar* d, qsizetype n, MidForm form, qsizetype at,
                  TsComponents& c)
{
    if (form == MidForm::Iso) {
        c.year   = num2(d, at) * 100 + num2(d, at + 2);
        c.month  = num2(d, at + 5);
        c.day    = num2(d, at + 8);
        c.hour   = num2(d, at + 11);
        c.minute = num2(d, at + 14);
        c.second = num2(d, at + 17);
        scanFractionAndZone(d, n, at + 19, c);
    } else {
        c.day    = num2(d, at);
        c.month  = monthAt(d, n, at + 3);
        c.year   = num2(d, at + 7) * 100 + num2(d, at + 9);
        c.hour   = num2(d, at + 12);
        c.minute = num2(d, at + 15);
        c.second = num2(d, at + 18);
        scanFractionAndZone(d, n, at + 20, c);
    }
}

// Ведущая ли найденная форма с полной датой (правило — у
// LineClassifier::isPrimaryLine): с позиции 0, с позиции 1 за «[» или форма
// access-лога за «[» в строке без ведущего пробельного отступа.
bool isLeadingMidForm(const QChar* d, MidForm form, qsizetype at)
{
    if (at == 0)
        return true;
    if (d[at - 1] != QLatin1Char('['))
        return false;
    if (at == 1)
        return true;
    return form == MidForm::Access && d[0] != QLatin1Char(' ') && d[0] != QLatin1Char('\t');
}

bool isValidComponents(const TsComponents& c)
{
    return QDate::isValid(c.year, c.month, c.day)
        && QTime::isValid(c.hour, c.minute, c.second, c.millis);
}

// Год метки без года: последний год, в котором её день не позже опорного
// (опорная дата + 1 день уже учтена в ref). 29 февраля — ближайший
// високосный; невозможный день (31 ноября) — 0.
int resolveYear(int month, int day, const LineClassifier::YearReference& ref)
{
    int year = (month < ref.month || (month == ref.month && day <= ref.day))
        ? ref.year : ref.year - 1;
    // Високосный год встречается хотя бы раз в 8 лет (2096 → 2104).
    for (int k = 0; k < 8; ++k, --year)
        if (QDate::isValid(year, month, day))
            return year;
    return 0;
}

// Метка syslog (RFC 3164) с позиции 0: «Mmm dd hh:mm:ss», день дополнен
// пробелом («Nov  8») или без него («Nov 8»). true — метка и валидна.
bool scanSyslogTimestamp(const QChar* d, qsizetype n,
                         const LineClassifier::YearReference& ref, TsComponents& c)
{
    if (n < 14 || d[3] != QLatin1Char(' '))
        return false;
    const int month = monthAt(d, n, 0);
    if (month == 0)
        return false;
    int day;
    qsizetype t;
    if (d[4] == QLatin1Char(' ') && isAsciiDigit(d[5].unicode()) && d[6] == QLatin1Char(' ')) {
        day = d[5].unicode() - u'0';
        t = 7;
    } else if (isAsciiDigit(d[4].unicode()) && isAsciiDigit(d[5].unicode())
               && d[6] == QLatin1Char(' ')) {
        day = num2(d, 4);
        t = 7;
    } else if (isAsciiDigit(d[4].unicode()) && d[5] == QLatin1Char(' ')) {
        day = d[4].unicode() - u'0';
        t = 6;
    } else {
        return false;
    }
    if (t + 8 > n || d[t + 2] != QLatin1Char(':') || d[t + 5] != QLatin1Char(':')
        || !digitsAt(d, t, { 0, 1, 3, 4, 6, 7 }))
        return false;
    // Без опорной даты — сегодняшняя; считается только у совпавшей формы.
    const int year = resolveYear(month, day,
                                 ref.isValid() ? ref : LineClassifier::yearReference(QDate()));
    if (year == 0)
        return false;
    c.year = year;
    c.month = month;
    c.day = day;
    c.hour = num2(d, t);
    c.minute = num2(d, t + 3);
    c.second = num2(d, t + 6);
    scanFractionAndZone(d, n, t + 8, c);
    return isValidComponents(c);
}

// Обратное к daysFromCivil (civil_from_days, H. Hinnant).
void civilFromDays(qint64 z, int& y, int& m, int& d)
{
    z += 719468;
    const qint64 era = (z >= 0 ? z : z - 146096) / 146097;
    const int doe = int(z - era * 146097);
    const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y = int(yoe + era * 400) + (m <= 2);
    if (y <= 0)
        --y; // к нумерации QDate: нулевого года нет
}

// Время без даты (F-11): hh:mm:ss с позиции 0 или 1 за «[», за секундами не
// цифра и не «:», затем необязательная дробь (без зоны). Время суток в мс,
// -1 — формы нет или время невалидно.
qint32 clockTimeAt(const QChar* d, qsizetype n)
{
    const qsizetype t = (n > 0 && d[0] == QLatin1Char('[')) ? 1 : 0;
    if (t + 8 > n || d[t + 2] != QLatin1Char(':') || d[t + 5] != QLatin1Char(':')
        || !digitsAt(d, t, { 0, 1, 3, 4, 6, 7 }))
        return -1;
    if (t + 8 < n && (isAsciiDigit(d[t + 8].unicode()) || d[t + 8] == QLatin1Char(':')))
        return -1;
    const int hour = num2(d, t), minute = num2(d, t + 3), second = num2(d, t + 6);
    if (hour > 23 || minute > 59 || second > 59)
        return -1;
    int millis = 0;
    qsizetype pos = t + 8;
    if (pos + 1 < n && (d[pos] == QLatin1Char('.') || d[pos] == QLatin1Char(','))
        && isAsciiDigit(d[pos + 1].unicode())) {
        int digits = 0;
        for (++pos; pos < n && digits < 3 && isAsciiDigit(d[pos].unicode()); ++pos, ++digits)
            millis = millis * 10 + (d[pos].unicode() - u'0');
        for (; digits < 3; ++digits)
            millis *= 10;
    }
    return ((hour * 60 + minute) * 60 + second) * 1000 + millis;
}

// Местный момент дня day (от эпохи) и времени суток timeMs.
qint64 localMomentMs(qint64 day, qint32 timeMs)
{
    TsComponents c;
    civilFromDays(day, c.year, c.month, c.day);
    c.hour = timeMs / 3600000;
    c.minute = timeMs / 60000 % 60;
    c.second = timeMs / 1000 % 60;
    c.millis = timeMs % 1000;
    return epochMs(c);
}

// Ручные фолбэк-форматы (dd/MM/yyyy, MM/dd/yyyy, dd.MM.yyyy — с позиции 0),
// за секундами — те же дробная часть и зона, что у ISO-формы.
// true — c заполнен И дата/время валидны (проверка per-format, как раньше).
bool scanFallbackFormats(QStringView line, const QStringList& formats,
                         TsComponents& c)
{
    // У всех форм d[2] == d[5] — «/» или «.»: строка без этого отсекается до
    // перебора форматов по именам (горячий путь).
    if (line.size() < 19 || (line[2] != QLatin1Char('/') && line[2] != QLatin1Char('.'))
        || line[5] != line[2])
        return false;
    const QStringView lineRef = line;
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

// ---- Длина метки с позиции (F-17, вид поля шаблона «time») ----
// ТОЛЬКО форма, без проверки даты и диапазона зоны — бит-в-бит регекс
// PatternHeuristics::timestampAtDetectPattern() (закреплено в
// tests/lineindex_smoke). Конец дробной части и зоны после секунд с pos —
// по раскладке timestampSuffixDetectPattern(), с её откатами.
qsizetype suffixEnd(const QChar* d, qsizetype n, qsizetype pos)
{
    if (pos + 1 < n && (d[pos] == QLatin1Char('.') || d[pos] == QLatin1Char(','))
        && isAsciiDigit(d[pos + 1].unicode())) {
        pos += 2;
        while (pos < n && isAsciiDigit(d[pos].unicode()))
            ++pos;
    }
    if (pos >= n)
        return pos;
    const char16_t ch = d[pos].unicode();
    if (ch == u'Z' || ch == u'z')
        return (pos + 1 < n && isWordChar(d[pos + 1].unicode())) ? pos : pos + 1;
    if (ch == u'+' || ch == u'-') {
        // ([+-]\d{2})(?::?(\d{2}))?(?!\d)
        if (!digitAt(d, n, pos + 1) || !digitAt(d, n, pos + 2))
            return pos;
        const qsizetype a = pos + 3;
        const qsizetype m = (a < n && d[a] == QLatin1Char(':')) ? a + 1 : a;
        if (digitAt(d, n, m) && digitAt(d, n, m + 1) && !digitAt(d, n, m + 2))
            return m + 2;
        return digitAt(d, n, a) ? pos : a;
    }
    if (ch == u' ' && pos + 1 < n) {
        const char16_t next = d[pos + 1].unicode();
        if (next == u'+' || next == u'-') {
            // ([+-]\d{2}):?(\d{2})(?!\d) через пробел
            if (!digitAt(d, n, pos + 2) || !digitAt(d, n, pos + 3))
                return pos;
            const qsizetype a = pos + 4;
            const qsizetype m = (a < n && d[a] == QLatin1Char(':')) ? a + 1 : a;
            if (digitAt(d, n, m) && digitAt(d, n, m + 1) && !digitAt(d, n, m + 2))
                return m + 2;
            return pos;
        }
        if (pos + 4 <= n) {
            const char16_t b = d[pos + 2].unicode(), e = d[pos + 3].unicode();
            const bool utc = next == u'U' && b == u'T' && e == u'C';
            const bool gmt = next == u'G' && b == u'M' && e == u'T';
            if (utc || gmt) {
                const qsizetype after = pos + 4;
                if (after < n) {
                    const char16_t x = d[after].unicode();
                    if (isWordChar(x) || x == u'+' || x == u'-')
                        return pos;
                }
                return after;
            }
        }
    }
    return pos;
}

} // namespace

static_assert(LogFile::kNoClockState == ClockDateState{}.packed(),
              "LogFile::clockState starts as an unset ClockDateState");

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

LineClassifier::YearReference LineClassifier::yearReference(const QDate& referenceDate)
{
    // Опорный момент только по дате — её полдень: последняя строка с временем
    // без даты получает именно эту дату.
    if (!referenceDate.isValid())
        return referenceAt(QDateTime());
    YearReference ref = referenceAt(QDateTime(referenceDate, QTime(12, 0)));
    return ref;
}

LineClassifier::YearReference LineClassifier::referenceAt(const QDateTime& moment)
{
    const QDateTime local = (moment.isValid() ? moment : QDateTime::currentDateTime()).toLocalTime();
    const QDate next = local.date().addDays(1);
    YearReference ref;
    ref.year = next.year();
    ref.month = next.month();
    ref.day = next.day();
    ref.momentMs = local.toMSecsSinceEpoch();
    return ref;
}

LineClassifier::YearReference LineClassifier::yearReferenceForFile(const QString& filePath)
{
    const QFileInfo info(filePath);
    const QDateTime modified = info.exists() ? info.lastModified() : QDateTime();
    return referenceAt(modified);
}

qint32 LineClassifier::clockTimeOfDay(QStringView line)
{
    return clockTimeAt(line.data(), line.size());
}

qint32 LineClassifier::clockLastDay(qint32 timeMs, const YearReference& reference)
{
    const YearReference ref = hasTimestampMs(reference.momentMs) ? reference : referenceAt(QDateTime());
    // Последняя дата, при которой момент раньше опорного + 12 ч. Опорная
    // дата + 1 — первая проверяемая (ref хранит её для года syslog).
    const qint64 limit = ref.momentMs + 12 * 3600 * 1000;
    qint64 day = daysFromCivil(ref.year, ref.month, ref.day);
    for (int k = 0; k < 3 && localMomentMs(day, timeMs) >= limit; ++k)
        --day;
    return qint32(day);
}

ClockRun LineClassifier::clockRunInFile(const QString& filePath, qint64 offset,
                                        std::optional<QStringConverter::Encoding> wide,
                                        bool autoDetectBom, const std::atomic_bool* abort)
{
    ClockRun run;
    const auto aborted = [abort] { return abort && abort->load(std::memory_order_relaxed); };
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return run;
    constexpr qsizetype kHead = 64;
    QChar head[kHead];
    if (wide) {
        QTextStream in(&file);
        in.setAutoDetectUnicode(autoDetectBom);
        if (!autoDetectBom)
            in.setEncoding(*wide);
        if (offset > 0 && !in.seek(offset))
            return run;
        QString line;
        while (in.readLineInto(&line)) {
            if (aborted())
                return ClockRun();
            const qint32 t = clockTimeAt(line.constData(), qMin(line.size(), kHead));
            if (t >= 0)
                run.feed(t);
        }
        return run;
    }
    if (!file.seek(offset))
        return run;
    // Голова каждой строки — до 64 байтов: время без даты — ASCII в начале
    // строки в любой однобайтной кодировке и в UTF-8.
    QByteArray buf(1 << 20, Qt::Uninitialized);
    char carry[kHead];
    qsizetype carryLen = 0;
    bool inLine = false;   // в carry — начало строки, конец которой ещё не встречен
    const auto finishLine = [&](const char* data, qsizetype len) {
        for (qsizetype k = 0; k < len; ++k)
            head[k] = QLatin1Char(data[k]);
        const qint32 t = clockTimeAt(head, len);
        if (t >= 0)
            run.feed(t);
    };
    for (;;) {
        if (aborted())
            return ClockRun();
        const qint64 got = file.read(buf.data(), buf.size());
        if (got <= 0)
            break;
        const char* p = buf.constData();
        qsizetype pos = 0;
        while (pos < got) {
            const char* nl = static_cast<const char*>(std::memchr(p + pos, '\n', size_t(got - pos)));
            const qsizetype end = nl ? nl - p : qsizetype(got);
            const qsizetype take = qMin<qsizetype>(kHead - carryLen, end - pos);
            if (take > 0) {
                std::memcpy(carry + carryLen, p + pos, size_t(take));
                carryLen += take;
            }
            if (!nl) {
                inLine = true;
                break;
            }
            finishLine(carry, carryLen);
            carryLen = 0;
            inLine = false;
            pos = end + 1;
        }
    }
    if (inLine && carryLen > 0)
        finishLine(carry, carryLen);   // последняя строка без '\n' — её видит и проход
    return run;
}

qsizetype LineClassifier::timestampLengthAt(QStringView line, qsizetype pos)
{
    const QChar* d = line.constData();
    const qsizetype n = line.size();
    if (pos < 0 || pos >= n)
        return 0;
    const QChar* p = d + pos;
    const qsizetype m = n - pos;   // символов от pos
    // ISO: dddd-dd-dd или dddd/dd/dd, [ T], dd:dd:dd.
    if (m >= 19) {
        const char16_t s4 = p[4].unicode();
        if ((s4 == u'-' || s4 == u'/') && p[7].unicode() == s4
            && (p[10] == QLatin1Char(' ') || p[10] == QLatin1Char('T'))
            && p[13] == QLatin1Char(':') && p[16] == QLatin1Char(':')
            && digitsAt(p, 0, { 0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18 }))
            return suffixEnd(d, n, pos + 19) - pos;
        // Фолбэк-формы: dd/dd/dddd или dd.dd.dddd, пробел, dd:dd:dd.
        const char16_t s2 = p[2].unicode();
        if ((s2 == u'/' || s2 == u'.') && p[5].unicode() == s2 && p[10] == QLatin1Char(' ')
            && p[13] == QLatin1Char(':') && p[16] == QLatin1Char(':')
            && digitsAt(p, 0, { 0, 1, 3, 4, 6, 7, 8, 9, 11, 12, 14, 15, 17, 18 }))
            return suffixEnd(d, n, pos + 19) - pos;
    }
    // access: dd/Mmm/dddd:dd:dd:dd.
    if (m >= 20 && p[2] == QLatin1Char('/') && p[6] == QLatin1Char('/')
        && p[11] == QLatin1Char(':') && p[14] == QLatin1Char(':') && p[17] == QLatin1Char(':')
        && digitsAt(p, 0, { 0, 1, 7, 8, 9, 10, 12, 13, 15, 16, 18, 19 })
        && monthAt(d, n, pos + 3) != 0)
        return suffixEnd(d, n, pos + 20) - pos;
    // syslog: Mmm, пробел, « d» / «d» / «dd», пробел, dd:dd:dd.
    if (m >= 14 && p[3] == QLatin1Char(' ') && monthAt(d, n, pos) != 0) {
        qsizetype t = -1;
        if (p[4] == QLatin1Char(' ') && isAsciiDigit(p[5].unicode()) && p[6] == QLatin1Char(' '))
            t = 7;
        else if (isAsciiDigit(p[4].unicode()) && isAsciiDigit(p[5].unicode()) && p[6] == QLatin1Char(' '))
            t = 7;
        else if (isAsciiDigit(p[4].unicode()) && p[5] == QLatin1Char(' '))
            t = 6;
        if (t > 0 && t + 8 <= m && p[t + 2] == QLatin1Char(':') && p[t + 5] == QLatin1Char(':')
            && digitsAt(p, t, { 0, 1, 3, 4, 6, 7 }))
            return suffixEnd(d, n, pos + t + 8) - pos;
    }
    // Время без даты: dd:dd:dd, за ним не цифра и не «:», дробь — без зоны.
    if (m >= 8 && p[2] == QLatin1Char(':') && p[5] == QLatin1Char(':')
        && digitsAt(p, 0, { 0, 1, 3, 4, 6, 7 })) {
        if (m > 8 && (isAsciiDigit(p[8].unicode()) || p[8] == QLatin1Char(':')))
            return 0;
        qsizetype e = pos + 8;
        if (e + 1 < n && (d[e] == QLatin1Char('.') || d[e] == QLatin1Char(','))
            && isAsciiDigit(d[e + 1].unicode())) {
            e += 2;
            while (e < n && isAsciiDigit(d[e].unicode()))
                ++e;
        }
        return e - pos;
    }
    return 0;
}

bool LineClassifier::detectTimestamp(QStringView line, QDateTime &ts,
                                     const YearReference& year, bool* leading,
                                     ClockDates* clock) const
{
    // QDateTime строится ИЗ мс epoch (одна конверсия, и та из кэша по минуте)
    // вместо прежних setDate+setTime (две конверсии локаль→epoch на строку).
    qint64 msecs = 0;
    if (detectTimestampMs(line, msecs, year, leading, clock)) {
        ts = QDateTime::fromMSecsSinceEpoch(msecs);
        return true;
    }
    ts = QDateTime();
    return false;
}

bool LineClassifier::detectTimestampMs(QStringView line, qint64 &msecs,
                                       const YearReference& year, bool* leading,
                                       ClockDates* clock) const
{
    if (leading)
        *leading = false;
    if (clock)
        clock->matched = false;
    // Без построения QDateTime вовсе — горячий путь индексатора, которому
    // нужны только мс epoch. Порядок форм — в начале файла.
    const QChar* d = line.constData();
    const qsizetype n = line.size();
    TsComponents c;
    // Сначала формы позиции 0: фолбэк-форматы, затем syslog. Ни
    // одна из них не начинается так же, как форма с полной датой (у ISO
    // d[2] — цифра, у access d[5] — буква), поэтому порядок проверок не
    // меняет результат «позиция 0 главнее», а строке syslog не нужен проход
    // по всей строке.
    if (scanFallbackFormats(line, m_timeFormats, c) || scanSyslogTimestamp(d, n, year, c)) {
        msecs = epochMs(c);
        if (leading)
            *leading = true;   // формы позиции 0 — ведущие по определению
        return true;
    }
    // Время без даты (F-11) — тоже форма позиции 0 (или 1 за «[»); с
    // формами выше не пересекается (у них d[2] — «/» или «.», либо буква).
    if (clock) {
        const qint32 t = clockTimeAt(d, n);
        if (t >= 0) {
            ClockDateState& st = clock->state;
            if (!st.isSet()) {
                ClockRun ahead = clock->lookAhead ? clock->lookAhead() : ClockRun();
                if (ahead.lastTimeMs < 0)
                    ahead = ClockRun{0, t};   // предпросмотра нет (или файл уже другой)
                st.day = clockLastDay(ahead.lastTimeMs, year) - ahead.rollovers;
            } else if (ClockRun::isRollover(st.timeMs, t)) {
                ++st.day;
            }
            st.timeMs = t;
            clock->matched = true;
            msecs = localMomentMs(st.day, t);
            if (leading)
                *leading = true;
            return true;
        }
    }
    qsizetype at = -1;
    const MidForm mid = scanMidForm(d, n, at);
    if (mid == MidForm::None)
        return false;
    parseMidForm(d, n, mid, at, c);
    if (!isValidComponents(c))
        return false; // невалидная дата/время, несмотря на совпадение формы
    msecs = epochMs(c);
    if (leading)
        *leading = isLeadingMidForm(d, mid, at);
    return true;
}

namespace {

// Уровень по целому слову w[0..len) — словарь kLevelWords (logentry.h),
// повторённый вручную: разбор по длине слова, затем по первой букве — на
// слово текста обычно приходится одно сравнение длины и одно — буквы.
// Совпадение со словарём закреплено в tests/lineindex_smoke.
LogLevel levelOfWord(const QChar* w, qsizetype len)
{
    LogLevel found = LogLevel::Unknown;
    if (len >= 3 && len <= 8) {
        // Хвост слова (со второй буквы) против kw; длины равны.
        const auto rest = [w, len](const char16_t* kw) {
            for (qsizetype k = 1; k < len; ++k)
                if (toUpperAscii(w[k].unicode()) != kw[k])
                    return false;
            return true;
        };
        const char16_t first = toUpperAscii(w[0].unicode());
        switch (len) {
        case 3:
            switch (first) {
            case u'E': if (rest(u"ERR")) found = LogLevel::Error; break;
            case u'W': if (rest(u"WRN")) found = LogLevel::Warn;  break;
            case u'I': if (rest(u"INF")) found = LogLevel::Info;  break;
            case u'D': if (rest(u"DBG")) found = LogLevel::Debug; break;
            case u'V': if (rest(u"VRB")) found = LogLevel::Trace; break;
            case u'F': if (rest(u"FTL")) found = LogLevel::Fatal; break;
            default: break;
            }
            break;
        case 4:
            switch (first) {
            case u'I': if (rest(u"INFO")) found = LogLevel::Info;  break;
            case u'W': if (rest(u"WARN")) found = LogLevel::Warn;  break;
            case u'C': if (rest(u"CRIT")) found = LogLevel::Fatal; break;
            default: break;
            }
            break;
        case 5:
            switch (first) {
            case u'E':
                if (rest(u"ERROR"))      found = LogLevel::Error;
                else if (rest(u"EMERG")) found = LogLevel::Fatal;
                break;
            case u'D': if (rest(u"DEBUG")) found = LogLevel::Debug; break;
            case u'T': if (rest(u"TRACE")) found = LogLevel::Trace; break;
            case u'F': if (rest(u"FATAL")) found = LogLevel::Fatal; break;
            default: break;
            }
            break;
        case 6:
            switch (first) {
            case u'S': if (rest(u"SEVERE")) found = LogLevel::Fatal; break;
            case u'N': if (rest(u"NOTICE")) found = LogLevel::Info;  break;
            default: break;
            }
            break;
        case 7:
            switch (first) {
            case u'W': if (rest(u"WARNING")) found = LogLevel::Warn;  break;
            case u'V': if (rest(u"VERBOSE")) found = LogLevel::Trace; break;
            default: break;
            }
            break;
        case 8:
            if (first == u'C' && rest(u"CRITICAL")) found = LogLevel::Fatal;
            break;
        default:
            break;
        }
    }
    return found;
}

} // namespace

bool LineClassifier::detectLogLevel(QStringView line, LogLevel &level) const
{
    const QChar* d = line.constData();
    const qsizetype n = line.size();

    // Самое левое ЦЕЛОЕ слово (границы — не-\w символы), равное одному из
    // слов словаря kLevelWords (logentry.h) — levelOfWord.
    qsizetype i = 0;
    while (i < n) {
        if (!isWordChar(d[i].unicode())) {
            ++i;
            continue;
        }
        qsizetype j = i + 1;
        while (j < n && isWordChar(d[j].unicode()))
            ++j;
        const LogLevel found = levelOfWord(d + i, j - i);
        if (found != LogLevel::Unknown) {
            level = found;
            return true;
        }
        i = j;
    }
    return false;
}

qsizetype LineClassifier::levelLengthAt(QStringView line, qsizetype pos, LogLevel* level)
{
    const QChar* d = line.constData();
    const qsizetype n = line.size();
    if (pos < 0 || pos >= n || !isWordChar(d[pos].unicode())
            || (pos > 0 && isWordChar(d[pos - 1].unicode())))
        return 0;
    qsizetype j = pos + 1;
    while (j < n && isWordChar(d[j].unicode()))
        ++j;
    const LogLevel found = levelOfWord(d + pos, j - pos);
    if (found == LogLevel::Unknown)
        return 0;
    if (level)
        *level = found;
    return j - pos;
}

// ============================================================================
// Ключевая схема (F-4): значения ключей. Правило — в lineclassifier.h.
// ============================================================================

namespace {

// Границы моментов: 0001-01-01T00:00:00Z … 9999-12-31T23:59:59.999Z.
constexpr qint64 kMinEpochMs = -62135596800000LL;
constexpr qint64 kMaxEpochMs = 253402300799999LL;

// Целое без знака из одних ASCII-цифр. До 19 цифр — quint64 (до ~1,8e19)
// не переполняется; нужны все 19: эпоха в наносекундах после 1973 года.
bool parseDigits(QStringView s, quint64& out)
{
    if (s.isEmpty() || s.size() > 19)
        return false;
    quint64 v = 0;
    for (const QChar c : s) {
        if (!isAsciiDigit(c.unicode()))
            return false;
        v = v * 10 + quint64(c.unicode() - u'0');
    }
    out = v;
    return true;
}

} // namespace

bool KeyedClassification::isPrimary() const
{
    return LineClassifier::isPrimaryLine(matched, hasTs, leading, hasLevel);
}

bool LineClassifier::epochFromValue(QStringView v, KeyedDefinition::EpochUnit unit, qint64& msecs)
{
    using Unit = KeyedDefinition::EpochUnit;
    bool negative = false;
    if (!v.isEmpty() && v.front() == QLatin1Char('-')) {
        negative = true;
        v = v.mid(1);
    }
    const qsizetype dot = v.indexOf(QLatin1Char('.'));
    const QStringView intText = dot < 0 ? v : v.left(dot);
    const QStringView fracText = dot < 0 ? QStringView() : v.mid(dot + 1);
    quint64 whole = 0;
    if (!parseDigits(intText, whole))
        return false;
    if (dot >= 0 && fracText.isEmpty())
        return false;   // «12.» — не число
    for (const QChar c : fracText)
        if (!isAsciiDigit(c.unicode()))
            return false;

    if (unit == Unit::Auto) {
        unit = whole < 100000000000ULL         ? Unit::Seconds
             : whole < 100000000000000ULL      ? Unit::Millis
             : whole < 100000000000000000ULL   ? Unit::Micros
                                               : Unit::Nanos;
    }

    // Момент в мс вниз: scaled — целая часть в мс, rest — есть ли ещё
    // ненулевой остаток (у отрицательного он сдвигает момент на мс вниз).
    quint64 scaled = 0;
    bool rest = false;
    const auto anyNonZero = [](QStringView digits) {
        for (const QChar c : digits)
            if (c != QLatin1Char('0'))
                return true;
        return false;
    };
    switch (unit) {
    case Unit::Seconds: {
        if (whole > 9000000000000000ULL / 1000)
            return false;
        int ms = 0;
        for (int k = 0; k < 3; ++k)
            ms = ms * 10 + (k < fracText.size() ? fracText[k].unicode() - u'0' : 0);
        scaled = whole * 1000 + quint64(ms);
        rest = fracText.size() > 3 && anyNonZero(fracText.mid(3));
        break;
    }
    case Unit::Millis:
        scaled = whole;
        rest = anyNonZero(fracText);
        break;
    case Unit::Micros:
        scaled = whole / 1000;
        rest = whole % 1000 != 0 || anyNonZero(fracText);
        break;
    case Unit::Nanos:
        scaled = whole / 1000000;
        rest = whole % 1000000 != 0 || anyNonZero(fracText);
        break;
    case Unit::Auto:
        return false;
    }
    if (scaled > quint64(std::numeric_limits<qint64>::max() / 2))
        return false;
    const qint64 ms = negative ? -qint64(scaled) - (rest ? 1 : 0) : qint64(scaled);
    if (ms < kMinEpochMs || ms > kMaxEpochMs)
        return false;
    msecs = ms;
    return true;
}

bool LineClassifier::timestampFromValue(QStringView value, bool isNumber,
                                        KeyedDefinition::EpochUnit unit,
                                        const YearReference& year, qint64& msecs) const
{
    if (value.isEmpty())
        return false;
    // Строка из цифр — тоже эпоха («"ts":"1700000000"»); у неё не бывает
    // формы с датой, так что сканеру строки её не отдать.
    const char16_t first = value.front().unicode();
    // «yyyy-» — дата, а не эпоха: epochFromValue её всё равно отверг бы.
    const bool numeric = (isAsciiDigit(first) || first == u'-')
        && !(value.size() > 4 && value[4] == u'-' && first != u'-');
    if (numeric && epochFromValue(value, unit, msecs))
        return true;
    if (isNumber)
        return false;
    return detectTimestampMs(value, msecs, year);
}

bool LineClassifier::levelFromValue(QStringView value, bool isNumber,
                                    KeyedDefinition::NumericLevels scale, LogLevel& level) const
{
    using Scale = KeyedDefinition::NumericLevels;
    quint64 n = 0;
    if (parseDigits(value, n)) {
        LogLevel found = LogLevel::Unknown;
        if (scale == Scale::Python) {
            static constexpr LogLevel byTens[] = {LogLevel::Unknown, LogLevel::Debug, LogLevel::Info,
                                                  LogLevel::Warn, LogLevel::Error};
            if (n >= 10)
                found = n >= 50 ? LogLevel::Fatal : byTens[n / 10];
        } else if (n <= 7) {
            static constexpr LogLevel syslog[] = {LogLevel::Fatal, LogLevel::Fatal, LogLevel::Fatal,
                                                  LogLevel::Error, LogLevel::Warn,  LogLevel::Info,
                                                  LogLevel::Info,  LogLevel::Debug};
            found = syslog[n];
        } else if (scale == Scale::Auto && n >= 10) {
            static constexpr LogLevel byTens[] = {LogLevel::Unknown, LogLevel::Trace, LogLevel::Debug,
                                                  LogLevel::Info,    LogLevel::Warn,  LogLevel::Error};
            found = n >= 60 ? LogLevel::Fatal : byTens[n / 10];
        }
        if (found == LogLevel::Unknown)
            return false;
        level = found;
        return true;
    }
    if (isNumber)
        return false;   // дробь, отрицательное — не уровень
    return detectLogLevel(value, level);
}

KeyedClassification LineClassifier::classifyKeyed(const LogPattern& pattern, QStringView line,
                                                  const YearReference& year,
                                                  ClockDates* clock) const
{
    return classifyLine(&pattern, line, year, clock);
}

KeyedClassification LineClassifier::classifyLine(const LogPattern* keyedSchema, QStringView line,
                                                 const YearReference& year, ClockDates* clock,
                                                 LogEntryFields* fields) const
{
    if (fields)
        *fields = LogEntryFields();
    KeyedClassification r;
    if (const KeyedDefinition* keyed = keyedSchema ? keyedSchema->keyed() : nullptr) {
        KeyedLine::Hits hits;
        // Без полей (индексатор, счётчики) нужны только метка и уровень:
        // разбор кончается, когда у обеих найден первый ключ-кандидат.
        static_assert(KeyedDefinition::kTimestampColumn < 2 && KeyedDefinition::kLevelColumn < 2,
                      "classification reads the first two columns only");
        r.matched = keyedSchema->scanKeyed(line, &hits, fields ? -1 : 2);
        if (r.matched) {
            if (fields)
                *fields = keyedSchema->keyedFields(hits);
            // Экранированное значение (редко: метка и уровень — ASCII без
            // кавычек внутри) раскодируется во временную строку.
            QString decoded;
            const auto valueOf = [&](const KeyedLine::Hit& hit) {
                if (!hit.escaped)
                    return hit.view(line);
                decoded = KeyedLine::unescape(hit.view(line));
                return QStringView(decoded);
            };
            const KeyedLine::Hit& ts = hits[KeyedDefinition::kTimestampColumn];
            if (ts.isValid()) {
                qint64 ms = 0;
                if (timestampFromValue(valueOf(ts), ts.kind == KeyedLine::ValueKind::Number,
                                       keyed->epochUnit, year, ms)) {
                    r.hasTs = r.tsFromKey = true;
                    r.tsMs = ms;
                }
            }
            const KeyedLine::Hit& lv = hits[KeyedDefinition::kLevelColumn];
            if (lv.isValid()) {
                LogLevel level = LogLevel::Unknown;
                if (levelFromValue(valueOf(lv), lv.kind == KeyedLine::ValueKind::Number,
                                   keyed->numericLevels, level)) {
                    r.hasLevel = r.levelFromKey = true;
                    r.level = level;
                }
            }
        }
    }
    // Ключа нет или значение не читается — эвристика по строке, как без схемы.
    if (!r.tsFromKey) {
        qint64 ms = 0;
        r.hasTs = detectTimestampMs(line, ms, year, &r.leading, clock);
        if (r.hasTs)
            r.tsMs = ms;
    } else if (clock) {
        clock->matched = false;
    }
    if (!r.levelFromKey)
        r.hasLevel = detectLogLevel(line, r.level);
    return r;
}
