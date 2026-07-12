#ifndef LOGENTRY_H
#define LOGENTRY_H

#include <QString>
#include <QDateTime>
#include "logfile.h"
#include "logfield.h"

enum class LogLevel {
    Unknown,
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal
};

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

    // Structured fields extracted from message by LogPattern::extractFields().
    // Offsets are into `message()`; isEmpty() == true for continuation lines.
    const LogEntryFields& fields() const { return m_fields; }
    // Единственный мутатор: переизвлечение полей при смене схемы.
    void setFields(LogEntryFields fields) { m_fields = std::move(fields); }

    // Строка «свободного текста»: парсер не извлёк ни таймстампа, ни уровня,
    // ни структурных полей. У continuation-строк настоящей записи таймстамп и
    // уровень унаследованы от её первой строки, поэтому true возможен только
    // там, где группировка в логические записи номинальна (не-лог файл целиком
    // слипается в запись #0; преамбула до первой настоящей записи — туда же).
    bool isPlainText() const {
        return !m_timestamp.isValid() && m_level == LogLevel::Unknown && m_fields.isEmpty();
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
            // Сравниваем по пути к файлу для детерминированного порядка
            if (m_sourceFile->filePath != other.m_sourceFile->filePath) {
                return m_sourceFile->filePath < other.m_sourceFile->filePath;
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
    QString m_message;
    LogFilePtr m_sourceFile;
    LogEntryFields m_fields;
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

// Преобразование QString → enum (регистронезависимо)
inline LogLevel StrToLevel(const QString &s) {
    const QString u = s.trimmed().toUpper();
    if (u == QLatin1String("TRACE"))   return LogLevel::Trace;
    if (u == QLatin1String("DEBUG"))   return LogLevel::Debug;
    if (u == QLatin1String("INFO"))    return LogLevel::Info;
    if (u == QLatin1String("WARN") || u == QLatin1String("WARNING")) return LogLevel::Warn;
    if (u == QLatin1String("ERROR"))   return LogLevel::Error;
    if (u == QLatin1String("FATAL"))   return LogLevel::Fatal;
    return LogLevel::Unknown;
}


#endif // LOGENTRY_H
