#ifndef LINECLASSIFIER_H
#define LINECLASSIFIER_H

#include "keyedline.h"
#include "logentry.h"
#include <QDateTime>
#include <QString>
#include <QStringConverter>
#include <QStringList>
#include <atomic>
#include <functional>
#include <limits>
#include <optional>

// ============================================================================
// LineClassifier — распознавание таймстампа и уровня в строке лога плюс
// правило «первичной» строки. ЕДИНЫЙ источник истины для LogParser
// (резидентная загрузка) и LogIndexer (индексная): вынесен из LogParser,
// чтобы два пути загрузки никогда не разошлись в классификации.
//
// Оба детектора зовутся на каждую строку лога, поэтому реализованы ручными
// сканерами (см. lineclassifier.cpp) — эквивалент канонических паттернов
// PatternHeuristics::isoTimestampDetectPattern()/levelDetectPattern(),
// которые продолжает использовать редактор схем; эквивалентность закреплена
// тестом в tests/lineindex_smoke.
//
// Потокобезопасность: методы const и не мутируют состояние; экземпляр можно
// разделять между последовательными задачами одного владельца (как это делал
// LogParser), но проще держать по экземпляру на воркера.
// ============================================================================

// Опорная дата для меток без года (syslog RFC 3164: «Nov 18 13:55:36»).
// Метка получает последний год, в котором её день не позже СЛЕДУЮЩЕГО
// дня после опорной даты (сутки запаса — на чужой пояс и часы писателя);
// 29 февраля отступает к ближайшему високосному году. Опорная дата —
// локальная дата mtime файла: её строит yearReferenceForFile, а читатели
// файла (LogParser, LogIndexer, сканер каталога) только передают её.
// Пустая (по умолчанию) — сегодняшняя дата.
// Тот же момент (mtime) задаёт и дату последней строки с временем без даты
// (F-11, ClockDates ниже).
struct TimestampYearReference {
    int year = 0, month = 0, day = 0;   // опорная дата + 1 день
    qint64 momentMs = kNoTimestampMs;   // сам опорный момент (mtime), мс epoch
    bool isValid() const { return year != 0; }
};

// ============================================================================
// Время без даты (F-11, docs/design/F-11.md): «13:55:40.016 [main] …»,
// «[13:55:36 INF] …». Дата — по состоянию разбора файла: последняя такая
// строка получает дату, при которой её момент ближе всего к опорному
// (mtime; точнее — последнюю дату, при которой момент раньше опорного +
// 12 ч), к началу файла дата уменьшается на сутки, когда время соседних
// таких строк идёт назад больше чем на час. Вперёд по файлу: день строки =
// день последней − число переходов от неё до конца файла; это число считает
// предпросмотр (ClockDates::lookAhead), когда проход встречает первую такую
// строку, а дня у файла ещё нет.
// ============================================================================

// Состояние: день (от 1970-01-01, локальная дата) и время суток последней
// строки с временем без даты. Переносится через дозапись (LogFile::
// clockState — упаковано в одно атомарное слово) и хвост сканера
// (LogParser::StatsResume).
struct ClockDateState {
    static constexpr qint32 kNoDay = std::numeric_limits<qint32>::min();
    qint32 day = kNoDay;
    qint32 timeMs = 0;
    bool isSet() const { return day != kNoDay; }
    constexpr quint64 packed() const { return (quint64(quint32(day)) << 32) | quint32(timeMs); }
    static ClockDateState fromPacked(quint64 v)
    {
        ClockDateState s;
        s.day = qint32(quint32(v >> 32));
        s.timeMs = qint32(quint32(v));
        return s;
    }
    bool operator==(const ClockDateState& o) const { return day == o.day && timeMs == o.timeMs; }
};

// Счёт переходов через полночь — правило одно для прохода и предпросмотра.
struct ClockRun {
    int rollovers = 0;
    qint32 lastTimeMs = -1;   // -1: таких строк не было
    // Время пошло назад больше чем на час: ровно час — перевод часов, чуть
    // назад — строки потоков вперемешку.
    static bool isRollover(qint32 previousMs, qint32 currentMs)
    {
        return previousMs - currentMs > 3600 * 1000;
    }
    void feed(qint32 timeMs)
    {
        if (lastTimeMs >= 0 && isRollover(lastTimeMs, timeMs))
            ++rollovers;
        lastTimeMs = timeMs;
    }
};

// Контекст прохода по файлу: состояние и предпросмотр. Без него (nullptr в
// detectTimestamp) время без даты меткой не считается — даты взять неоткуда.
struct ClockDates {
    ClockDateState state;
    // Переходы и время последней строки с временем без даты от НАЧАЛА
    // ПРОХОДА до конца файла (до первой такой строки их в проходе нет, так
    // что это то же, что от неё). Зовётся один раз, когда первая такая
    // строка встретилась при пустом состоянии. Пусто — как если бы строка
    // была последней (сканер сдвигает итог сам, см. analyzeFileForStats).
    std::function<ClockRun()> lookAhead;
    // Выход: последний вызов detectTimestamp нашёл время без даты.
    bool matched = false;
};

struct LineFormatReader;

// Классификация строки при загрузке — LineClassifier::classifyLine.
struct LineClassification {
    bool matched = false;        ///< Строку распознал формат строк (Json / Logfmt, keyedline.h).
    bool hasTs = false;
    qint64 tsMs = kNoTimestampMs;
    bool leading = false;        ///< Метка ведущая (только у эвристики по строке).
    bool hasLevel = false;
    LogLevel level = LogLevel::Unknown;
    bool tsFromKey = false;      ///< Момент взят из значения ключа, а не эвристикой.
    bool levelFromKey = false;   ///< Уровень взят из значения ключа.
    bool primary = false;        ///< Строка начинает запись (см. classifyLine).
    bool isPrimary() const { return primary; }
};

class LineClassifier {
public:
    LineClassifier();

    // Опорная дата для меток без года — см. TimestampYearReference выше.
    using YearReference = TimestampYearReference;
    static YearReference yearReference(const QDate& referenceDate);
    // По mtime файла; файла нет — сегодняшняя дата.
    static YearReference yearReferenceForFile(const QString& filePath);
    // Опорный момент целиком (дата для года + сам момент для F-11);
    // невалидный — сейчас.
    static YearReference referenceAt(const QDateTime& moment);

    // F-11. Время суток формы «HH:mm:ss[.fff]» с позиции 0 или 1 за «[», мс;
    // -1 — формы нет (эталон — PatternHeuristics::clockTimestampDetectPattern).
    // Головы строки в 64 символа достаточно.
    static qint32 clockTimeOfDay(QStringView line);
    // День (от 1970-01-01) последней строки с временем timeMs по опорному
    // моменту — см. блок F-11 выше.
    static qint32 clockLastDay(qint32 timeMs, const YearReference& reference);
    // Предпросмотр: строки файла с offset (начало строки) до конца. Без
    // wide — сырые байты (UTF-8 и однобайтные: голова строки — ASCII), с
    // wide — QTextStream в этой кодировке (UTF-16/32); autoDetectBom — поток
    // с начала файла узнаёт BOM сам. ANSI-коды в голове строки снимаются
    // (F-3, S-25): предпросмотр видит тот же текст, что проход.
    // abort (может быть nullptr) проверяется на каждом мегабайте и строке
    // потока: поднятый — пустой результат (воркер закрывается).
    static ClockRun clockRunInFile(const QString& filePath, qint64 offset,
                                   std::optional<QStringConverter::Encoding> wide = std::nullopt,
                                   bool autoDetectBom = false,
                                   const std::atomic_bool* abort = nullptr);

    // Распознать таймстамп в строке. true — ts заполнен. Метка без зоны —
    // локальное время машины; с явной зоной (Z, ±hh:mm, UTC…) — точный
    // момент. Дробная часть усекается до миллисекунд. Формы и где в строке
    // они ищутся — в начале lineclassifier.cpp.
    // leading (если задан) — метка ВЕДУЩАЯ, см. isPrimaryLine.
    // clock — контекст разбора файла для времени без даты (F-11); метка
    // этой формы ведущая и главнее формы с полной датой дальше в строке.
    bool detectTimestamp(QStringView line, QDateTime& ts,
                         const YearReference& year = YearReference(),
                         bool* leading = nullptr, ClockDates* clock = nullptr) const;
    // Как detectTimestamp, но сразу мс epoch БЕЗ построения QDateTime —
    // горячий путь индексатора. Конверсия локаль→epoch кэшируется по минуте:
    // на Windows каждая конверсия стоит микросекунды, и именно она (а не
    // поиск формы) доминировала в стоимости индексации. Метка с зоной
    // считается арифметикой и в кэш не ходит.
    bool detectTimestampMs(QStringView line, qint64& msecs,
                           const YearReference& year = YearReference(),
                           bool* leading = nullptr, ClockDates* clock = nullptr) const;
    // ---- Сканеры видов поля шаблона строки (F-17, linetemplate.h) ----
    // Длина метки, НАЧИНАЮЩЕЙСЯ ровно с pos, 0 — метки нет. Формы — все,
    // что читает detectTimestampMs (ISO и dddd/dd/dd, access, фолбэк-формы
    // dd/MM/yyyy и dd.MM.yyyy, syslog, время без даты), где угодно в строке,
    // с дробью и зоной после секунд. Только ФОРМА: дата и диапазон зоны не
    // проверяются (эталон — регекс PatternHeuristics::
    // timestampAtDetectPattern(), закреплено в tests/lineindex_smoke).
    static qsizetype timestampLengthAt(QStringView line, qsizetype pos);
    // Длина слова уровня (kLevelWords), начинающегося ровно с pos: целое
    // слово (\b с обеих сторон), регистр сворачивается для ASCII. 0 — нет.
    static qsizetype levelLengthAt(QStringView line, qsizetype pos, LogLevel* level = nullptr);
    // Распознать уровень логирования (словом). true — level заполнен.
    bool detectLogLevel(QStringView line, LogLevel& level) const;

    // ---- Значение ключа формата строк JSON / logfmt (F-4, F-17, шаг 7) ----
    // Политика времени та же, что у строки: значение-строка читается тем же
    // сканером, что detectTimestampMs (ISO с зоной, access, syslog с годом по
    // опорной дате; время без даты — нет, контекста файла у значения нет).
    // Число или строка из одних цифр с необязательными «-» и дробью — эпоха
    // (epochFromValue). isNumber — значение было голым числом JSON / logfmt:
    // тогда только эпоха.
    bool timestampFromValue(QStringView value, bool isNumber,
                            KeyedDefinition::EpochUnit unit, const YearReference& year,
                            qint64& msecs) const;
    // Эпоха: единица из схемы или по величине |целой части| (Auto: < 1e11 —
    // секунды, < 1e14 — мс, < 1e17 — мкс, иначе нс). Момент — вниз до
    // миллисекунды (как дробь строки: -0,5 мс — это -1 мс); отрицательная —
    // метка до 1970 (F-13). Вне 0001–9999 года, больше 19 цифр целой части,
    // экспонента, мусор — false.
    static bool epochFromValue(QStringView value, KeyedDefinition::EpochUnit unit,
                               qint64& msecs);
    // Уровень: слово — detectLogLevel по значению (словарь kLevelWords один);
    // целое число (или строка из цифр) — по шкале: Auto — 0–7 syslog
    // (0–2 Fatal, 3 Error, 4 Warning, 5–6 Info, 7 Debug), от 10 — pino /
    // Bunyan по десяткам (10 Trace, 20 Debug, 30 Info, 40 Warning, 50 Error,
    // 60+ Fatal); Syslog — только 0–7; Python — 10 Debug, 20 Info,
    // 30 Warning, 40 Error, 50+ Fatal. Не по шкале — false.
    bool levelFromValue(QStringView value, bool isNumber,
                        KeyedDefinition::NumericLevels scale, LogLevel& level) const;
    // Классификация строки при ЗАГРУЗКЕ — одно место правила для LogParser
    // (резидентный путь и счётчики сканера) и LogIndexer (F-17, шаг 7):
    // формат строк файла (lineformat.h; nullptr — Plain). Json / Logfmt —
    // распознана ли строка (keyedline.h), момент и уровень из ключей ролей;
    // ключа нет или значение не читается — эвристика по строке
    // (detectTimestampMs с leading и clock, detectLogLevel), как у Plain.
    // primary — строка начинает запись: распознана форматом, ИЛИ
    // isPrimaryLine по метке и уровню, ИЛИ (Plain с levelStartsRecord)
    // строка начинается словом уровня с позиции 0 или 1 за «[».
    // fields (может быть nullptr) — поля распознанной строки по колонкам
    // ключей формата (без второго разбора); у нераспознанной — пусто.
    LineClassification classifyLine(const LineFormatReader* format, QStringView line,
                                    const YearReference& year = YearReference(),
                                    ClockDates* clock = nullptr,
                                    LogEntryFields* fields = nullptr) const;

    // Правило первичной строки логической записи (F-12, docs/design/F-12.md;
    // без схемы — F-17, шаг 7: формат строк добавляет своё в classifyLine).
    // Строка первична, если у неё есть метка и
    // слово уровня, ИЛИ метка ВЕДУЩАЯ — открывает строку:
    //   • стоит с позиции 0 (любая форма; syslog, фолбэк-формы и время без
    //     даты — F-11 — только так, время без даты ещё и с позиции 1 за «[»);
    //   • стоит с позиции 1 сразу за «[» («[2026-11-18 10:00:00] …»);
    //   • это форма access-лога сразу за «[» в строке без ведущего пробела
    //     («10.0.0.1 - frank [18/Nov/2026:13:55:36 +0000] "GET …"»).
    // Остальные строки — continuation текущей записи: стек-трейсы и тело
    // сообщения (JSON, SQL, XML) с меткой в середине или с отступом.
    // Правило построчное, без состояния: одно у LogParser, LogIndexer и
    // сканера каталога (analyzeFileForStats).
    static bool isPrimaryLine(bool hasTimestamp, bool leadingTimestamp, bool hasLevel)
    {
        return hasTimestamp && (leadingTimestamp || hasLevel);
    }

private:
    QStringList m_timeFormats;
};

#endif // LINECLASSIFIER_H
