#include "logparser.h"
#include "patternheuristics.h"
#include <QFile>
#include <QTextStream>
#include <QtConcurrent> // Для QtConcurrent::run
#include <QFileInfo>    // Для получения размера файла (для progress)
#include <QStringView>
#include <QDebug>       // Для qWarning()

LogParser::LogParser(QObject* parent)
    : QObject(parent)
    // Классификация строк (таймстамп/уровень/primary) делегируется
    // LineClassifier — общему с LogIndexer источнику истины.
{
}

LogParser::~LogParser()
{
    // Tell any running parse to stop, drop not-yet-started tasks, then block
    // until the workers have actually returned. This guarantees no worker is
    // touching m_pattern / the regex members while they are being destroyed —
    // the use-after-free that crashed on close.
    m_abort.store(true);
    m_pool.clear();
    m_pool.waitForDone();
}

void LogParser::startParsing(const LogFilePtr& logFile)
{
    // Snapshot the schema + flag on the GUI thread so the worker never reads
    // the mutable members (which setPattern may rebuild concurrently). The
    // task runs on our own pool, which the destructor joins.
    const LogPattern patternSnapshot = m_pattern;
    const bool extraction = m_extractionEnabled;
    (void)QtConcurrent::run(&m_pool, [this, logFile, patternSnapshot, extraction]() {
        this->doParse(logFile, patternSnapshot, extraction);
    });
}

void LogParser::startParsingFrom(const LogFilePtr& logFile, qint64 startOffset, int startLogicalEntryId)
{
    const LogPattern patternSnapshot = m_pattern;
    const bool extraction = m_extractionEnabled;
    (void)QtConcurrent::run(&m_pool, [this, logFile, startOffset, startLogicalEntryId,
                                      patternSnapshot, extraction]() {
        this->doParseFrom(logFile, startOffset, startLogicalEntryId, patternSnapshot, extraction);
    });
}

void LogParser::doParseFrom(const LogFilePtr& logFile, qint64 startOffset, int startLogicalEntryId,
                            const LogPattern& pattern, bool extraction)
{
    if (!logFile || logFile->filePath.isEmpty()) {
        emit parsingFinished(0, logFile);
        return;
    }

    QFile file(logFile->filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        emit parsingFailed(logFile);
        return;
    }

    // Seek to the continuation point.
    if (startOffset > 0 && !file.seek(startOffset)) {
        emit parsingFinished(0, logFile);
        return;
    }

    QTextStream in(&file);
    QVector<std::shared_ptr<LogEntry>> batchEntries;
    const int BATCH_SIZE = 5000;
    batchEntries.reserve(BATCH_SIZE);

    int logicalEntryIdCounter = startLogicalEntryId;
    int currentLogicalEntryId = logicalEntryIdCounter - 1; // will be set on first primary line
    QDateTime currentLogicalEntryTimestamp;
    LogLevel currentLogicalEntryLevel = LogLevel::Unknown;
    int fileLineNumber = 0; // relative line counter within the new block
    int totalParsedEntries = 0;

    QString line;
    while (!in.atEnd()) {
        if (m_abort.load(std::memory_order_relaxed))
            return; // Parser is shutting down — drop the rest.
        line = in.readLine();
        // Skip empty lines: either the phantom EOF line produced after a trailing
        // newline, or the completing \n of a line that had no newline at initial-parse time.
        if (line.isEmpty())
            continue;
        fileLineNumber++;

        QDateTime lineTs;
        LogLevel lineLevel = LogLevel::Unknown;

        bool hasTimestamp = m_classifier.detectTimestamp(line, lineTs);
        bool hasLevel = m_classifier.detectLogLevel(line, lineLevel);
        LogEntryFields extractedFields;
        const bool schemaMatched = (extraction && pattern.isValid())
            ? !(extractedFields = pattern.extractFields(line)).isEmpty()
            : false;

        std::shared_ptr<LogEntry> currentEntry;
        if (LineClassifier::isPrimaryLine(schemaMatched, hasTimestamp, hasLevel)) {
            currentLogicalEntryId = logicalEntryIdCounter++;
            currentLogicalEntryTimestamp = lineTs;
            currentLogicalEntryLevel = lineLevel;
            currentEntry = std::make_shared<LogEntry>(currentLogicalEntryId, fileLineNumber,
                currentLogicalEntryTimestamp, currentLogicalEntryLevel, line, logFile);
            currentEntry->setFields(extractedFields);
        } else {
            if (currentLogicalEntryId < startLogicalEntryId) {
                // No primary line yet — treat as its own entry
                currentLogicalEntryId = logicalEntryIdCounter++;
                currentLogicalEntryTimestamp = QDateTime();
                currentLogicalEntryLevel = LogLevel::Unknown;
            }
            currentEntry = std::make_shared<LogEntry>(currentLogicalEntryId, fileLineNumber,
                currentLogicalEntryTimestamp, currentLogicalEntryLevel, line, logFile);
        }
        batchEntries.push_back(currentEntry);
        totalParsedEntries++;

        if (batchEntries.size() >= BATCH_SIZE) {
            emit entriesParsed(batchEntries, logFile);
            batchEntries.clear();
            batchEntries.reserve(BATCH_SIZE);
        }
    }

    if (!batchEntries.isEmpty()) {
        emit entriesParsed(batchEntries, logFile);
    }

    emit parsingFinished(totalParsedEntries, logFile);
}

void LogParser::setPattern(const QString& schemaString)
{
    m_pattern.setPattern(schemaString);
}

void LogParser::doParse(const LogFilePtr& logFile, const LogPattern& pattern, bool extraction)
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
        if (m_abort.load(std::memory_order_relaxed))
            return; // Parser is shutting down — drop the rest.
        line = in.readLine();
        // Skip the phantom empty line that QTextStream produces after a trailing newline.
        if (line.isEmpty() && in.atEnd())
            break;
        bytesRead += line.length() + 1; // Приблизительный подсчет, +1 для \n
        fileLineNumber++;

        QDateTime lineTs;
        LogLevel lineLevel = LogLevel::Unknown;

        bool hasTimestamp = m_classifier.detectTimestamp(line, lineTs);
        bool hasLevel = m_classifier.detectLogLevel(line, lineLevel);
        LogEntryFields extractedFields;
        const bool schemaMatched = (extraction && pattern.isValid())
            ? !(extractedFields = pattern.extractFields(line)).isEmpty()
            : false;

        std::shared_ptr<LogEntry> currentEntry;
        if (LineClassifier::isPrimaryLine(schemaMatched, hasTimestamp, hasLevel)) {
            currentLogicalEntryId = logicalEntryIdCounter++;
            currentLogicalEntryTimestamp = lineTs;
            currentLogicalEntryLevel = lineLevel;
            currentEntry = std::make_shared<LogEntry>(currentLogicalEntryId, fileLineNumber, currentLogicalEntryTimestamp, currentLogicalEntryLevel, line, logFile);
            currentEntry->setFields(extractedFields);
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

        if (m_classifier.detectTimestamp(line, currentTs)) {
            if (!firstTsFound) {
                stats.firstEntryTimestamp = currentTs;
                firstTsFound = true;
            }
            stats.lastEntryTimestamp = currentTs; // Keep updating last timestamp
        }

        if (m_classifier.detectLogLevel(line, currentLevel)) {
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
