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
                                 const ResumeContext& context,
                                 QStringConverter::Encoding encoding)
{
    const LogPattern patternSnapshot = m_pattern;
    const bool extraction = m_extractionEnabled;
    (void)QtConcurrent::run(&m_pool, [this, logFile, startOffset, context,
                                      patternSnapshot, extraction, encoding]() {
        this->parseFrom(logFile, startOffset, context, patternSnapshot, extraction,
                        /*initial=*/false, encoding);
    });
}

void LogParser::setPattern(const QString& schemaString)
{
    m_pattern.setPattern(schemaString);
}

void LogParser::doParse(const LogFilePtr& logFile, const LogPattern& pattern, bool extraction)
{
    parseFrom(logFile, 0, ResumeContext(), pattern, extraction, /*initial=*/true,
              QStringConverter::Utf8);
}

namespace {

// Кодировки с переводом строки шире байта: ширина кодовой единицы и порядок
// байтов. Обобщённые Utf16/Utf32 (без BOM) — порядок байтов машины.
struct CodeUnit {
    int width = 1;
    bool bigEndian = false;
};

CodeUnit codeUnitOf(QStringConverter::Encoding encoding)
{
    const bool hostBig = QSysInfo::ByteOrder == QSysInfo::BigEndian;
    switch (encoding) {
    case QStringConverter::Utf16:   return {2, hostBig};
    case QStringConverter::Utf16LE: return {2, false};
    case QStringConverter::Utf16BE: return {2, true};
    case QStringConverter::Utf32:   return {4, hostBig};
    case QStringConverter::Utf32LE: return {4, false};
    case QStringConverter::Utf32BE: return {4, true};
    default:                        return {};
    }
}

// Смещение в chunk (он начинается на границе единицы) сразу ЗА последним
// переводом строки, либо -1.
qsizetype afterLastNewline(const QByteArray& chunk, CodeUnit unit)
{
    if (unit.width == 1) {
        const qsizetype newline = chunk.lastIndexOf('\n');
        return newline < 0 ? -1 : newline + 1;
    }
    const qsizetype low = unit.bigEndian ? unit.width - 1 : 0; // байт с '\n'
    for (qsizetype at = (chunk.size() / unit.width - 1) * unit.width; at >= 0;
         at -= unit.width) {
        bool newline = chunk[at + low] == '\n';
        for (qsizetype i = 0; newline && i < unit.width; ++i)
            newline = i == low || chunk[at + i] == 0;
        if (newline)
            return at + unit.width;
    }
    return -1;
}

} // namespace

void LogParser::parseFrom(const LogFilePtr& logFile, qint64 startOffset,
                          const ResumeContext& context, const LogPattern& pattern,
                          bool extraction, bool initial,
                          QStringConverter::Encoding tailEncoding)
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
    // Первая загрузка узнаёт UTF-16/32 по BOM (автоопределение QTextStream).
    // У дозаписи BOM нет — она читается в кодировке, в которой файл уже
    // прочитан; иначе хвост UTF-16 декодировался бы как UTF-8.
    if (!initial && tailEncoding != QStringConverter::Utf8) {
        in.setAutoDetectUnicode(false);
        in.setEncoding(tailEncoding);
    }
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
    // заново с её начала. Всё считается по сырым байтам, в кодовых единицах
    // кодировки: в UTF-16 перевод строки — два байта, а порция может
    // оборваться посреди единицы — такой огрызок не считается прочитанным
    // (единицы выровнены от начала файла: BOM — ровно одна единица).
    const QStringConverter::Encoding encoding = in.encoding();
    const CodeUnit unit = codeUnitOf(encoding);
    qint64 endOffset = in.pos();
    if (endOffset < 0)
        endOffset = file.size();
    endOffset -= endOffset % unit.width;
    bool lastLinePartial = false;
    qint64 lastLineStart = -1;
    // Строки начинаются после BOM: перечитывание с нуля отдало бы его
    // декодеру без автоопределения.
    const qint64 firstLineStart = (startOffset == 0 && unit.width > 1) ? unit.width : startOffset;
    if (totalParsedEntries > 0 && endOffset > firstLineStart) {
        QFile raw(logFile->filePath);
        if (raw.open(QIODevice::ReadOnly) && raw.seek(endOffset - unit.width)
            && afterLastNewline(raw.read(unit.width), unit) < 0) {
            lastLinePartial = true;
            lastLineStart = firstLineStart;
            constexpr qint64 kChunk = 64 * 1024;
            for (qint64 pos = endOffset; pos > firstLineStart;) {
                const qint64 from = qMax(firstLineStart, pos - kChunk);
                if (!raw.seek(from))
                    break;
                const qsizetype after = afterLastNewline(raw.read(pos - from), unit);
                if (after >= 0) {
                    lastLineStart = from + after;
                    break;
                }
                pos = from;
            }
        }
    }
    emit tailState(logFile, endOffset, lastLinePartial, lastLineStart, int(encoding));
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

        const bool hasTimestamp = m_classifier.detectTimestamp(line, currentTs);
        if (hasTimestamp) {
            if (!firstTsFound) {
                stats.firstEntryTimestamp = currentTs;
                firstTsFound = true;
            }
            stats.lastEntryTimestamp = currentTs; // Keep updating last timestamp
        }

        // Счётчики — по ЗАПИСЯМ, как во вкладке без схемы: уровень даёт только
        // строка с меткой времени (isPrimaryLine). Слово «err» или «critical»
        // в строке-продолжении (стек-трейс) ошибкой не считается.
        if (hasTimestamp && m_classifier.detectLogLevel(line, currentLevel)) {
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
