#include "logparser.h"
#include <QFile>
#include <QTextStream>
#include <QtConcurrent> // Для QtConcurrent::run
#include <QFileInfo>    // Для получения размера файла (для progress)
#include <QStringRef>
#include <QDebug>       // Для qWarning()

LogParser::LogParser(QObject* parent)
    : QObject(parent),
      m_timestampRegex(R"((\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})([.,]\d+)?)"),
      m_levelRegex(QRegularExpression(R"(\b(INFO|WARN|WARNING|ERROR|DEBUG|TRACE|FATAL)\b)",
                                      QRegularExpression::CaseInsensitiveOption))
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

bool LogParser::detectTimestamp(const QString &line, QDateTime &ts)
{
    auto match = m_timestampRegex.match(line);

    if (match.hasMatch()) {
        const auto dateTimePartRef = match.capturedView(1); // "YYYY-MM-DD HH:MM:SS"
        const auto millisPartRef = match.capturedView(2);   // "[.,]ddd" или пустая

        bool ok = true;
        int year = dateTimePartRef.mid(0, 4).toInt(&ok);
        if (!ok) return false;
        int month = dateTimePartRef.mid(5, 2).toInt(&ok);
        if (!ok) return false;
        int day = dateTimePartRef.mid(8, 2).toInt(&ok);
        if (!ok) return false;
        int hour = dateTimePartRef.mid(11, 2).toInt(&ok);
        if (!ok) return false;
        int minute = dateTimePartRef.mid(14, 2).toInt(&ok);
        if (!ok) return false;
        int second = dateTimePartRef.mid(17, 2).toInt(&ok);
        if (!ok) return false;
        
        int millis = 0;
        if (!millisPartRef.isEmpty()) {
            millis = millisPartRef.mid(1).toInt(&ok); // Пропускаем '.' или ','
            if (!ok) return false;
        }

        if (QDate::isValid(year, month, day) && QTime::isValid(hour, minute, second, millis)) {
            ts.setDate(QDate(year, month, day));
            ts.setTime(QTime(hour, minute, second, millis));
            // Какой формат был использован, здесь уже не так важно для m_timeFormats.move, 
            // т.к. мы его определили напрямую и успешно.
            // Можно для консистентности переместить один из "yyyy-MM-dd..." форматов наверх, если хочется.
            // Например, определить, был ли millisPart с запятой, точкой или отсутствовал,
            // и переместить соответствующий из m_timeFormats.at(0), .at(1) или .at(2).
            // Но основной выигрыш уже получен.
            return true;
        }
        return false; // Невалидная дата/время, несмотря на совпадение с regex
    } else {
        // m_timestampRegex не нашел совпадения. Пробуем другие форматы вручную.
        // Используем QStringRef для line для эффективности
        const QStringRef lineRef = QStringRef(&line);
        bool convOk = true;

        // Порядок m_timeFormats важен для проверки.
        // Он должен соответствовать списку в конструкторе.
        // "dd/MM/yyyy HH:mm:ss" 
        // "MM/dd/yyyy HH:mm:ss"
        // "dd.MM.yyyy HH:mm:ss"

        for (const QString& formatString : m_timeFormats) { // Проходим по всем форматам
            if (formatString.startsWith(QStringLiteral("yyyy-MM-dd"))) {
                continue; // Эти должны были быть пойманы regex
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
                // Миллисекунды не предусмотрены этим форматом в m_timeFormats
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
                // Неизвестный формат для ручного парсинга, или формат yyyy-MM-dd (который должен был быть пойман regex)
                // В качестве крайнего средства, можно использовать QDateTime::fromString,
                // но это то, от чего мы пытаемся уйти.
                // Если все форматы в m_timeFormats покрыты выше, этот else не нужен.
                // Для безопасности, если какой-то формат не yyyy-MM-dd и не один из трех выше,
                // он здесь не будет обработан.
                continue; // Пропускаем формат, для которого нет ручного парсера
            }

            if (convOk && QDate::isValid(year, month, day) && QTime::isValid(hour, minute, second, millis)) {
                ts.setDate(QDate(year, month, day));
                ts.setTime(QTime(hour, minute, second, millis));
                return true;
            }
        }
    }

    ts = QDateTime(); 
    return false;
}

bool LogParser::detectLogLevel(const QString &line, LogLevel &level) const
{
    auto match = m_levelRegex.match(line);
    if (match.hasMatch()) {
        level = StrToLevel(match.captured(1).toUpper());
        return true;
    }
    return false;
}

void LogParser::startParsing(const LogFilePtr& logFile)
{
    // Запускаем doParse в отдельном потоке из пула потоков Qt
    // Передаем копию shared_ptr logFile
    QtConcurrent::run([this, logFile]() {
        this->doParse(logFile);
    });
}

void LogParser::setPattern(const QString& conversionPattern)
{
    m_pattern.setPattern(conversionPattern);
}

void LogParser::doParse(const LogFilePtr& logFile)
{
    emit parsingStarted(logFile); // Испускаем сигнал о начале парсинга

    if (!logFile || logFile->filePath.isEmpty()) {
        emit parsingFailed(logFile);
        return;
    }

    QFile file(logFile->filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        emit parsingFailed(logFile);
        return;
    }

    QTextStream in(&file);
    QVector<std::shared_ptr<LogEntry>> batchEntries;
    const int BATCH_SIZE = 5000; // Отправляем по 500 записей за раз
    batchEntries.reserve(BATCH_SIZE);

    int logicalEntryIdCounter = 0;
    int currentLogicalEntryId = -1;
    QDateTime currentLogicalEntryTimestamp;
    LogLevel currentLogicalEntryLevel = LogLevel::Unknown;
    int fileLineNumber = 0;
    int totalParsedEntries = 0;

    qint64 fileSize = QFileInfo(logFile->filePath).size();
    qint64 bytesRead = 0;
    int lastReportedProgress = -1;

    QString line;
    while (!in.atEnd()) {
        line = in.readLine();
        bytesRead += line.length() + 1; // Приблизительный подсчет, +1 для \n
        fileLineNumber++;

        QDateTime lineTs;
        LogLevel lineLevel = LogLevel::Unknown;

        bool hasTimestamp = detectTimestamp(line, lineTs);
        bool hasLevel = detectLogLevel(line, lineLevel);

        std::shared_ptr<LogEntry> currentEntry;
        if (hasTimestamp && hasLevel) {
            currentLogicalEntryId = logicalEntryIdCounter++;
            currentLogicalEntryTimestamp = lineTs;
            currentLogicalEntryLevel = lineLevel;
            currentEntry = std::make_shared<LogEntry>(currentLogicalEntryId, fileLineNumber, currentLogicalEntryTimestamp, currentLogicalEntryLevel, line, logFile);
            // Extract structured fields if a pattern is configured
            if (m_pattern.isValid())
                currentEntry->fields = m_pattern.extractFields(currentEntry->message);
        } else {
            if (currentLogicalEntryId == -1) { 
                currentLogicalEntryId = logicalEntryIdCounter++;
                currentLogicalEntryTimestamp = QDateTime(); 
                currentLogicalEntryLevel = LogLevel::Unknown; 
            }
            currentEntry = std::make_shared<LogEntry>(currentLogicalEntryId, fileLineNumber, currentLogicalEntryTimestamp, currentLogicalEntryLevel, line, logFile);
        }
        batchEntries.push_back(currentEntry);
        totalParsedEntries++;

        if (batchEntries.size() >= BATCH_SIZE) {
            emit entriesParsed(batchEntries, logFile);
            batchEntries.clear();
            batchEntries.reserve(BATCH_SIZE);
        }

        if (fileSize > 0) {
            int progress = static_cast<int>((bytesRead * 100) / fileSize);
            if (progress != lastReportedProgress) {
                emit parsingProgress(progress, logFile);
                lastReportedProgress = progress;
            }
        }
    }

    // Отправляем оставшиеся записи, если они есть
    if (!batchEntries.isEmpty()) {
        emit entriesParsed(batchEntries, logFile);
    }

    // Убедимся, что финальный прогресс 100% отправлен
    if (lastReportedProgress < 100 && fileSize > 0) {
         emit parsingProgress(100, logFile);
    }

    emit parsingFinished(totalParsedEntries, logFile);
    // file.close() вызывается автоматически деструктором QFile
}

LogParser::FileStats LogParser::analyzeFileForStats(const QString& filePath)
{
    FileStats stats;
    QFile file(filePath);
    QFileInfo fileInfo(filePath);
    stats.fileSize = fileInfo.size();

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        stats.parseSuccess = false;
        qWarning() << "AnalyzeStats: Failed to open file:" << filePath;
        return stats;
    }

    QTextStream in(&file);
    QString line;
    QDateTime currentTs;
    LogLevel currentLevel;
    bool firstTsFound = false;

    while (!in.atEnd()) {
        line = in.readLine();
        stats.totalEntries++; // Count each line as a potential entry for simplicity in stats

        if (detectTimestamp(line, currentTs)) {
            if (!firstTsFound) {
                stats.firstEntryTimestamp = currentTs;
                firstTsFound = true;
            }
            stats.lastEntryTimestamp = currentTs; // Keep updating last timestamp
        }

        if (detectLogLevel(line, currentLevel)) {
            switch (currentLevel) {
            case LogLevel::Warn:
                stats.warnCount++;
                break;
            case LogLevel::Error:
                stats.errorCount++;
                break;
            case LogLevel::Fatal:
                stats.fatalCount++;
                break;
            default:
                break;
            }
        }
    }
    // file.close() will be called by QFile destructor
    return stats;
}
