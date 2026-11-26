#ifndef LOGENTRY_H
#define LOGENTRY_H

#include <QString>
#include <QDateTime>
#include <limits>
#include "logfile.h"
#include "logfield.h"
#include "ansiescape.h"
#include <memory>

enum class LogLevel {
    Unknown,
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal
};

// Набор уровней одной маской (бит уровня — levelBit): поиск строки по уровню
// сравнивает байт метаданных с маской, без QSet.
using LogLevelMask = quint32;
constexpr LogLevelMask levelBit(LogLevel level) { return LogLevelMask(1) << int(level); }

// Момент метки в мс epoch и признак «метки нет» — один на всё хранилище:
// индекс (блоки меток по id записи), снапшоты (LogEntryMeta), ключ строки
// (RecordKey), статистика и таймлайн (F-13). Не -1 и не «любое
// отрицательное»: метка до 1970 года — законный отрицательный момент, ровно
// эпоха — 0. Минимум qint64 классификатор дать не может (годы 0001–9999 —
// это ±2,6·10^14 мс), поэтому с законным моментом признак не совпадает, а
// разности законных моментов не переполняются.
constexpr qint64 kNoTimestampMs = std::numeric_limits<qint64>::min();
constexpr bool hasTimestampMs(qint64 ms) { return ms != kNoTimestampMs; }

// Одна физическая строка лога. Доступ к данным — только через аксессоры:
// это шов, за которым хранилище сможет стать нерезидентным (индекс + чтение
// текста с диска по требованию), не ломая потребителей.
class LogEntry {
public:
    LogEntry(int lId, int lineNum, const QDateTime &ts, LogLevel lvl, const QString &msg, LogFilePtr sFile)
        : m_logicalEntryId(lId)
        , m_originalLineNumber(lineNum)
        , m_timestamp(ts)
        , m_level(lvl)
        , m_message(msg)
        , m_sourceFile(sFile)
    {}

    // ID логической записи (группирует многострочные сообщения)
    int logicalEntryId() const { return m_logicalEntryId; }
    // Порядковый номер строки в исходном файле
    int originalLineNumber() const { return m_originalLineNumber; }
    // Время записи (первой строки логической записи)
    const QDateTime& timestamp() const { return m_timestamp; }
    // Уровень логирования (первой строки логической записи)
    LogLevel level() const { return m_level; }
    // Текст этой конкретной строки
    const QString& message() const { return m_message; }
    // Источник (файл)
    const LogFilePtr& sourceFile() const { return m_sourceFile; }

    // Первая строка логической записи этой строки (F-17, шаг 3); nullptr —
    // строка сама первая. Правило фильтра по полю решается по полям первой
    // строки записи (FieldRecordCursor), и строке-продолжению нужен её текст,
    // даже если первая строка не попала в проход (дозапись посреди записи,
    // поисковая база из видимых строк). Ставит парсер до публикации записи.
    // Полей в записи нет: поля — функция текста и схемы (FieldSource).
    const std::shared_ptr<const LogEntry>& recordHead() const { return m_head; }
    void setRecordHead(std::shared_ptr<const LogEntry> head) { m_head = std::move(head); }
    // Формат строк файла (JSON / logfmt, F-17, шаг 7) узнал строку. Только
    // для isPlainText; ставит парсер до публикации.
    void setFormatMatched(bool matched) { m_formatMatched = matched; }

    // Стиль ANSI строки (F-3): спаны SGR в координатах message(); nullptr —
    // в строке не было кодов с видимым стилем (у большинства строк). Ставит
    // только парсер до публикации записи; дальше не меняется.
    const AnsiStyles* ansiStyles() const { return m_ansi.get(); }
    void setAnsiStyles(AnsiStyles styles)
    {
        m_ansi = styles.isEmpty() ? nullptr
                                  : std::make_unique<const AnsiStyles>(std::move(styles));
    }

    // Строка «свободного текста»: парсер не извлёк ни таймстампа, ни уровня,
    // ни структурных полей. У continuation-строк настоящей записи таймстамп и
    // уровень унаследованы от её первой строки, поэтому true возможен только
    // там, где группировка в логические записи номинальна (не-лог файл целиком
    // слипается в запись #0; преамбула до первой настоящей записи — туда же).
    // «Структурные поля» — формат строк узнал строку (паритет с
    // индексом: LineIndex::isPlainTextLine). Запись неизменяема после
    // публикации: воркеры статистики и таймлайна читают это без синхронизации.
    bool isPlainText() const {
        return !m_timestamp.isValid() && m_level == LogLevel::Unknown && !m_formatMatched;
    }

    bool operator<(const LogEntry& other) const {
        // 1. Сравнение по времени логической записи
        if (m_timestamp.isValid() && other.m_timestamp.isValid()) {
            if (m_timestamp != other.m_timestamp) {
                return m_timestamp < other.m_timestamp;
            }
        } else if (m_timestamp.isValid()) { // this валиден, other нет
            return true;
        } else if (other.m_timestamp.isValid()) { // other валиден, this нет
            return false;
        }
        // Временные метки "равны" (или обе невалидны)

        // 2. Сравнение по файлу источнику для стабильности
        if (m_sourceFile && other.m_sourceFile && m_sourceFile != other.m_sourceFile) {
            // Детерминированный порядок — по ключу источника (F-2): у обычного
            // файла это путь, у сжатого — архив, а не временная копия.
            const QString& ka = m_sourceFile->orderKey();
            const QString& kb = other.m_sourceFile->orderKey();
            if (ka != kb) {
                return ka < kb;
            }
        } else if (m_sourceFile && !other.m_sourceFile) { // Только у this есть sourceFile
            return true;
        } else if (!m_sourceFile && other.m_sourceFile) { // Только у other есть sourceFile
            return false;
        }
        // Временные метки "равны" И sourceFile "равны" (или оба nullptr)

        // 3. Сравнение по ID логической записи (внутри одного файла или для записей с одинаковым временем из разных файлов)
        if (m_logicalEntryId != other.m_logicalEntryId) {
            return m_logicalEntryId < other.m_logicalEntryId;
        }
        // Временные метки, sourceFile и logicalEntryId "равны"

        // 4. Сравнение по исходному номеру строки для сохранения порядка внутри логической записи или файла
        return m_originalLineNumber < other.m_originalLineNumber;
    }

private:
    int m_logicalEntryId;
    int m_originalLineNumber;
    QDateTime m_timestamp;
    LogLevel m_level;
    bool m_formatMatched = false;   // встаёт в выравнивание после m_level
    QString m_message;
    LogFilePtr m_sourceFile;
    std::shared_ptr<const LogEntry> m_head;
    std::unique_ptr<const AnsiStyles> m_ansi;
};


// Компаратор для сортировки и слияния батчей записей (shared_ptr-обёртки).
// Null-указатели считаются «большими» и уходят в конец. Строгое слабое
// упорядочение: comp(x, x) == false, в том числе для двух null.
inline bool logEntryPtrLess(const std::shared_ptr<LogEntry>& a,
                            const std::shared_ptr<LogEntry>& b)
{
    if (!a || !b)
        return a && !b;
    return *a < *b;
}

// Преобразование enum → QString
inline QString LevelToStr(LogLevel lvl) {
    switch (lvl) {
    case LogLevel::Trace:   return QStringLiteral("TRACE");
    case LogLevel::Debug:   return QStringLiteral("DEBUG");
    case LogLevel::Info:    return QStringLiteral("INFO");
    case LogLevel::Warn:    return QStringLiteral("WARN");
    case LogLevel::Error:   return QStringLiteral("ERROR");
    case LogLevel::Fatal:   return QStringLiteral("FATAL");
    default:                return QStringLiteral("UNKNOWN");
    }
}

// Словарь уровней: какое слово в строке лога какой уровень означает.
// Единственное место, где он записан. Отсюда строятся регексы
// PatternHeuristics (блок Level схемы и эталон классификатора), StrToLevel и
// подсветка ключевых слов; LineClassifier::detectLogLevel повторяет его
// вручную ради скорости, а tests/lineindex_smoke сверяет их между собой и с
// заранее заданным соответствием. Меняя таблицу, править классификатор, тест
// и таблицу в обеих справках.
//
// Слова — только ASCII в верхнем регистре; сравнение везде регистронезависимое
// и только по ASCII («ſ» и знак Кельвина — не S и не K). Новых значений
// LogLevel словарь не заводит: чужие шкалы отображаются на шесть уровней.
//   CRITICAL/CRIT, EMERG — syslog, nginx, Python; SEVERE —
//   java.util.logging; NOTICE — syslog, PostgreSQL; VERBOSE — Android,
//   Serilog; ERR/WRN/INF/DBG/VRB/FTL — Serilog.
// Намеренно НЕ в словаре: ALERT, PANIC, FAIL, FINE/FINER/FINEST и CONFIG
// (java.util.logging). Это обычные слова сообщений и имён компонентов, а
// уровень задаёт САМОЕ ЛЕВОЕ слово словаря: «alert.manager - ERROR» стал бы
// Fatal вместо Error. Каждое новое слово несёт этот риск — добавлять только
// те, что вне меток уровня почти не встречаются.
struct LevelWord {
    const char* word;
    LogLevel level;
};

inline constexpr LevelWord kLevelWords[] = {
    { "FATAL", LogLevel::Fatal },    { "CRITICAL", LogLevel::Fatal },
    { "CRIT", LogLevel::Fatal },     { "SEVERE", LogLevel::Fatal },
    { "EMERG", LogLevel::Fatal },    { "FTL", LogLevel::Fatal },
    { "ERROR", LogLevel::Error },    { "ERR", LogLevel::Error },
    { "WARNING", LogLevel::Warn },   { "WARN", LogLevel::Warn },
    { "WRN", LogLevel::Warn },
    { "INFO", LogLevel::Info },      { "INF", LogLevel::Info },
    { "NOTICE", LogLevel::Info },
    { "DEBUG", LogLevel::Debug },    { "DBG", LogLevel::Debug },
    { "TRACE", LogLevel::Trace },    { "VERBOSE", LogLevel::Trace },
    { "VRB", LogLevel::Trace },
};

// Слово уровня → enum (регистронезависимо по ASCII, пробелы по краям не в счёт)
inline LogLevel StrToLevel(QStringView s) {
    s = s.trimmed();
    for (const LevelWord& w : kLevelWords) {
        qsizetype k = 0;
        for (; k < s.size() && w.word[k] != '\0'; ++k) {
            char16_t c = s[k].unicode();
            if (c >= u'a' && c <= u'z')
                c = char16_t(c - (u'a' - u'A'));
            if (c != char16_t(w.word[k]))
                break;
        }
        if (k == s.size() && w.word[k] == '\0' && k > 0)
            return w.level;
    }
    return LogLevel::Unknown;
}


#endif // LOGENTRY_H
