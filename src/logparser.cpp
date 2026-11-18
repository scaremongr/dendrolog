#include "logparser.h"
#include "patternheuristics.h"
#include <QFile>
#include <QStringDecoder>
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

    // Год меток без года (syslog) — по mtime файла на момент этого прохода:
    // дозапись после Нового года получает новый год (LineClassifier).
    const LineClassifier::YearReference yearRef =
        LineClassifier::yearReferenceForFile(logFile->filePath);

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

        bool hasTimestamp = m_classifier.detectTimestamp(line, lineTs, yearRef);
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

LogParser::StatsScan LogParser::analyzeFileForStats(const QString& filePath,
                                                    const StatsResume& from) const
{
    StatsScan scan;
    FileStats& stats = scan.stats;
    stats.fileSize = QFileInfo(filePath).size();

    // Без QIODevice::Text: смещения — сырые байты (инвариант 5), '\r' перед
    // '\n' снимается вручную.
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        stats.parseSuccess = false;
        scan.complete = stats;
        qWarning() << "AnalyzeStats: Failed to open file:" << filePath;
        return scan;
    }

    // Кодировка: с начала файла — по BOM, как автоопределение QTextStream у
    // разбора вкладки; хвост — в кодировке, известной по прошлому разбору.
    QStringConverter::Encoding encoding = from.encoding;
    qint64 offset = from.offset;
    if (offset == 0) {
        const QByteArray head = file.read(4);
        const auto detected = QStringConverter::encodingForData(head);
        if (detected) {
            // Строки начинаются за BOM: в UTF-8 он три байта, в UTF-16/32 —
            // одна кодовая единица.
            encoding = *detected;
            offset = encoding == QStringConverter::Utf8 ? 3 : codeUnitOf(encoding).width;
        } else {
            encoding = QStringConverter::Utf8;
        }
    }
    const CodeUnit unit = codeUnitOf(encoding);
    if (offset % unit.width != 0 || !file.seek(offset)) {
        stats.parseSuccess = false;
        scan.complete = stats;
        return scan;
    }

    QStringDecoder decoder(encoding, QStringConverter::Flag::Stateless);
    const LineClassifier::YearReference yearRef = LineClassifier::yearReferenceForFile(filePath);
    bool inRecord = from.inRecord;
    LogLevel recordLevel = LogLevel::Unknown;

    const auto countLine = [&](const QByteArray& bytes) {
        QString line = decoder.decode(bytes);
        if (line.endsWith(u'\r'))
            line.chop(1);
        QDateTime ts;
        LogLevel level = LogLevel::Unknown;
        const bool hasTimestamp = m_classifier.detectTimestamp(line, ts, yearRef);
        const bool hasLevel = m_classifier.detectLogLevel(line, level);
        // Схема полей здесь не участвует: вкладка решает по ней, только когда
        // включено извлечение полей, а по умолчанию — по метке и уровню.
        if (LineClassifier::isPrimaryLine(false, hasTimestamp, hasLevel)) {
            recordLevel = level;
            if (!stats.firstEntryTimestamp.isValid())
                stats.firstEntryTimestamp = ts;
            stats.lastEntryTimestamp = ts;
        } else if (!inRecord) {
            recordLevel = LogLevel::Unknown;   // строка до первой записи — своя запись
        } else {
            return;                            // продолжение текущей записи
        }
        inRecord = true;
        stats.totalEntries++;
        switch (recordLevel) {
        case LogLevel::Warn:  stats.warnCount++;  break;
        case LogLevel::Error: stats.errorCount++; break;
        case LogLevel::Fatal: stats.fatalCount++; break;
        default: break;
        }
    };

    // Поиск перевода строки — по кодовым единицам кодировки.
    const qsizetype low = unit.bigEndian ? unit.width - 1 : 0;
    const auto findNewline = [&](const QByteArray& buf, qsizetype fromPos) -> qsizetype {
        if (unit.width == 1)
            return buf.indexOf('\n', fromPos);
        for (qsizetype at = fromPos; at + unit.width <= buf.size(); at += unit.width) {
            if (buf[at + low] != '\n')
                continue;
            bool newline = true;
            for (qsizetype k = 0; newline && k < unit.width; ++k)
                newline = k == low || buf[at + k] == 0;
            if (newline)
                return at;
        }
        return -1;
    };

    constexpr qint64 kChunk = 1 << 20;
    QByteArray pending;           // байты начатой, ещё не законченной строки
    qint64 lineStart = offset;    // сырое смещение начала pending
    while (true) {
        const QByteArray chunk = file.read(kChunk);
        if (chunk.isEmpty())
            break;
        pending += chunk;
        qsizetype pos = 0;
        while (true) {
            const qsizetype newline = findNewline(pending, pos);
            if (newline < 0)
                break;
            countLine(pending.mid(pos, newline - pos));
            lineStart += newline + unit.width - pos;
            pos = newline + unit.width;
        }
        pending.remove(0, pos);
    }

    // Целые строки кончились: это точка продолжения. Оборванная последняя
    // строка (писатель не дописал '\n') считается в показанных цифрах, но не
    // в базе — при дозаписи она перечитается целиком.
    scan.complete = stats;
    scan.next.offset = lineStart;
    scan.next.encoding = encoding;
    scan.next.inRecord = inRecord;
    if (pending.size() >= unit.width)
        countLine(pending.left(pending.size() - pending.size() % unit.width));
    return scan;
}

LogParser::FileStats LogParser::mergeStats(const FileStats& head, const FileStats& tail)
{
    FileStats merged = head;
    merged.fileSize      = tail.fileSize;
    merged.totalEntries += tail.totalEntries;
    merged.warnCount    += tail.warnCount;
    merged.errorCount   += tail.errorCount;
    merged.fatalCount   += tail.fatalCount;
    merged.parseSuccess  = head.parseSuccess && tail.parseSuccess;
    if (!merged.firstEntryTimestamp.isValid())
        merged.firstEntryTimestamp = tail.firstEntryTimestamp;
    if (tail.lastEntryTimestamp.isValid())
        merged.lastEntryTimestamp = tail.lastEntryTimestamp;
    return merged;
}
