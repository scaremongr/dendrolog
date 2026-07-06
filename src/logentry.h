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

struct LogEntry {
    int logicalEntryId;      // ID логической записи (группирует многострочные сообщения)
    int originalLineNumber;  // Порядковый номер строки в исходном файле
    QDateTime timestamp;     // Время записи (первой строки логической записи)
    LogLevel level;         // Уровень логирования (первой строки логической записи)
    QString message;        // Текст этой конкретной строки
    LogFilePtr sourceFile;  // Источник (файл)

    // Structured fields extracted from message by LogPattern::extractFields().
    // Offsets are into `message`; isEmpty() == true for continuation lines.
    LogEntryFields fields;

    LogEntry(int lId, int lineNum, const QDateTime &ts, LogLevel lvl, const QString &msg, LogFilePtr sFile)
        : logicalEntryId(lId)
        , originalLineNumber(lineNum)
        , timestamp(ts)
        , level(lvl)
        , message(msg)
        , sourceFile(sFile)
    {}

    bool operator<(const LogEntry& other) const {
        // 1. Сравнение по времени логической записи
        if (timestamp.isValid() && other.timestamp.isValid()) {
            if (timestamp != other.timestamp) {
                return timestamp < other.timestamp;
            }
        } else if (timestamp.isValid()) { // this валиден, other нет
            return true;
        } else if (other.timestamp.isValid()) { // other валиден, this нет
            return false;
        }
        // Временные метки "равны" (или обе невалидны)

        // 2. Сравнение по файлу источнику для стабильности
        if (sourceFile && other.sourceFile && sourceFile != other.sourceFile) {
            // Сравниваем по пути к файлу для детерминированного порядка
            if (sourceFile->filePath != other.sourceFile->filePath) {
                return sourceFile->filePath < other.sourceFile->filePath;
            }
        } else if (sourceFile && !other.sourceFile) { // Только у this есть sourceFile
            return true;
        } else if (!sourceFile && other.sourceFile) { // Только у other есть sourceFile
            return false;
        }
        // Временные метки "равны" И sourceFile "равны" (или оба nullptr)

        // 3. Сравнение по ID логической записи (внутри одного файла или для записей с одинаковым временем из разных файлов)
        if (logicalEntryId != other.logicalEntryId) {
            return logicalEntryId < other.logicalEntryId;
        }
        // Временные метки, sourceFile и logicalEntryId "равны"

        // 4. Сравнение по исходному номеру строки для сохранения порядка внутри логической записи или файла
        return originalLineNumber < other.originalLineNumber;
    }
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
