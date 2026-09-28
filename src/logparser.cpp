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

void LogParser::startParsingFrom(const LogFilePtr& logFile, qint64 startOffset,
                                 const ResumeContext& context)
{
    const LogPattern patternSnapshot = m_pattern;
    const bool extraction = m_extractionEnabled;
    (void)QtConcurrent::run(&m_pool, [this, logFile, startOffset, context,
                                      patternSnapshot, extraction]() {
        this->parseFrom(logFile, startOffset, context, patternSnapshot, extraction,
                        /*initial=*/false);
    });
}

void LogParser::setPattern(const QString& schemaString)
{
    m_pattern.setPattern(schemaString);
}

void LogParser::doParse(const LogFilePtr& logFile, const LogPattern& pattern, bool extraction)
{
    parseFrom(logFile, 0, ResumeContext(), pattern, extraction, /*initial=*/true);
}

void LogParser::parseFrom(const LogFilePtr& logFile, qint64 startOffset,
                          const ResumeContext& context, const LogPattern& pattern,
                          bool extraction, bool initial)
{
    if (initial)
        emit parsingStarted(logFile);

    if (!logFile || logFile->filePath.isEmpty()) {
        if (initial)
            emit parsingFailed(logFile);
        else
            emit parsingFinished(0, logFile);
        return;
    }

    QFile file(logFile->filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        emit parsingFailed(logFile);
        return;
    }
    if (startOffset > 0 && !file.seek(startOffset)) {
        emit parsingFinished(0, logFile);
        return;
    }

    QTextStream in(&file);
    QVector<std::shared_ptr<LogEntry>> batchEntries;
    const int BATCH_SIZE = 5000;
    batchEntries.reserve(BATCH_SIZE);

    // Состояние продолжается с того места, где кончилась прошлая порция: строка
    // продолжения в начале дозаписи принадлежит последней записи файла, а
    // номера строк идут дальше, а не с 1.
    int logicalEntryIdCounter = context.nextLogicalEntryId;
    int currentLogicalEntryId = context.currentLogicalEntryId;
    QDateTime currentLogicalEntryTimestamp = context.currentTimestamp;
    LogLevel currentLogicalEntryLevel = context.currentLevel;
    int fileLineNumber = context.nextLineNumber - 1;
    int totalParsedEntries = 0;

    const qint64 fileSize = initial ? QFileInfo(logFile->filePath).size() : 0;
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
            currentEntry = std::make_shared<LogEntry>(currentLogicalEntryId, fileLineNumber,
                currentLogicalEntryTimestamp, currentLogicalEntryLevel, line, logFile);
            currentEntry->setFields(extractedFields);
        } else {
            if (currentLogicalEntryId < 0) {
                // Ни одной записи ещё нет — строка становится собственной записью.
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

        if (fileSize > 0) {
            int progress = static_cast<int>((bytesRead * 100) / fileSize);
            if (progress != lastReportedProgress) {
                emit parsingProgress(progress, logFile);
                lastReportedProgress = progress;
            }
        }
    }

    if (!batchEntries.isEmpty())
        emit entriesParsed(batchEntries, logFile);
    if (initial && lastReportedProgress < 100 && fileSize > 0)
        emit parsingProgress(100, logFile);

    // Точный конец прочитанного (дальше дочитывание продолжит отсюда) и не
    // оборвана ли последняя строка: писатель мог не успеть дописать '\n'.
    // Такую строку вкладка показывает предварительно и при дозаписи читает
    // заново с её начала. Начало ищется по сырым байтам — только для
    // кодировок, где перевод строки — один байт '\n' (не UTF-16/32).
    qint64 endOffset = in.pos();
    if (endOffset < 0)
        endOffset = file.size();
    bool lastLinePartial = false;
    qint64 lastLineStart = -1;
    const QStringConverter::Encoding encoding = in.encoding();
    const bool byteNewlines = encoding != QStringConverter::Utf16
        && encoding != QStringConverter::Utf16LE && encoding != QStringConverter::Utf16BE
        && encoding != QStringConverter::Utf32 && encoding != QStringConverter::Utf32LE
        && encoding != QStringConverter::Utf32BE;
    if (byteNewlines && totalParsedEntries > 0 && endOffset > startOffset) {
        QFile raw(logFile->filePath);
        char last = '\n';
        if (raw.open(QIODevice::ReadOnly) && raw.seek(endOffset - 1) && raw.getChar(&last)
            && last != '\n') {
            lastLinePartial = true;
            lastLineStart = startOffset;
            constexpr qint64 kChunk = 64 * 1024;
            for (qint64 pos = endOffset; pos > startOffset;) {
                const qint64 from = qMax(startOffset, pos - kChunk);
                if (!raw.seek(from))
                    break;
                const QByteArray chunk = raw.read(pos - from);
                const qsizetype newline = chunk.lastIndexOf('\n');
                if (newline >= 0) {
                    lastLineStart = from + newline + 1;
                    break;
                }
                pos = from;
            }
        }
    }
    emit tailState(logFile, endOffset, lastLinePartial, lastLineStart);
    emit parsingFinished(totalParsedEntries, logFile);
}

LogParser::FileStats LogParser::analyzeFileForStats(const QString& filePath, qint64 fromOffset)
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

    if (fromOffset > 0 && !file.seek(fromOffset)) {
        stats.parseSuccess = false;
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
