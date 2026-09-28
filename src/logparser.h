#ifndef LOGPARSER_H
#define LOGPARSER_H

#include "logentry.h"
#include "logpattern.h"
#include "lineclassifier.h"
#include <QVector>
#include <QString>
#include <QDateTime>
#include <QThreadPool>
#include <atomic>
#include <memory>
#include <QObject>

class LogParser : public QObject
{
    Q_OBJECT

public:
    explicit LogParser(QObject* parent = nullptr);
    ~LogParser() override;

    struct FileStats {
        qint64 fileSize = 0;
        int totalEntries = 0;
        QDateTime firstEntryTimestamp;
        QDateTime lastEntryTimestamp;
        int warnCount = 0;
        int errorCount = 0;
        int fatalCount = 0;
        bool parseSuccess = true; 
    };

    // This method can be called from a separate thread for stat analysis.
    // `fromOffset` > 0 analyses only the tail of the file starting at that byte
    // offset (which the caller must know to be a line boundary) — used by the
    // directory scanner to refresh an append-only log without re-reading it all.
    // The returned stats then describe the tail alone and have to be merged.
    FileStats analyzeFileForStats(const QString& filePath, qint64 fromOffset = 0);

    // Optional: configure a dynamic block schema so that the parser extracts
    // structured fields into LogEntry::fields for every primary line.
    // The string may be either the new serialized schema format or a legacy
    // log4cxx/log4j conversion pattern, which will be migrated on read.
    // Passing an empty string clears structured extraction.
    void setPattern(const QString& schemaString);
    void setExtractionEnabled(bool enabled) { m_extractionEnabled = enabled; }
    const LogPattern& pattern() const noexcept { return m_pattern; }

    // Where an incremental parse continues from: the file's last record (a
    // continuation line at the start of the new bytes belongs to it) and the
    // numbering. Default — the start of a file.
    struct ResumeContext {
        int nextLogicalEntryId = 0;         // id of the next primary line
        int currentLogicalEntryId = -1;     // -1: no record yet
        QDateTime currentTimestamp;         // of the current record
        LogLevel currentLevel = LogLevel::Unknown;
        int nextLineNumber = 1;             // 1-based line number in the file
    };

public slots:
    void startParsing(const LogFilePtr& logFile);
    // Incremental parse: read only bytes starting at startOffset (a line
    // boundary), continuing the numbering and the current record of context.
    void startParsingFrom(const LogFilePtr& logFile, qint64 startOffset,
                          const ResumeContext& context);

signals:
    void parsingStarted(const LogFilePtr& logFile);
    void entriesParsed(const QVector<std::shared_ptr<LogEntry>>& entriesBatch, const LogFilePtr& logFile);
    // Right before parsingFinished of a successful parse: the exact byte
    // offset reading stopped at (the next incremental parse starts there), and
    // whether the last line had no newline yet — then lastLineStart is its
    // byte offset, to read it again once the writer completes it.
    void tailState(const LogFilePtr& logFile, qint64 endOffset, bool lastLinePartial,
                   qint64 lastLineStart);
    void parsingFinished(int totalEntries, const LogFilePtr& logFile);
    void parsingFailed(const LogFilePtr& logFile);
    void parsingProgress(int progressPercentage, const LogFilePtr& logFile);

private:
    // The worker methods receive an immutable snapshot of the schema and the
    // extraction flag taken on the GUI thread at launch time. They never read
    // the mutable m_pattern / m_extractionEnabled members, so reconfiguring
    // the parser (setPattern) while a parse is in flight is race-free.
    void doParse(const LogFilePtr& logFile, const LogPattern& pattern, bool extraction);
    // The one parse loop for both the initial load (initial: progress and
    // parsingStarted/Failed) and an incremental read.
    void parseFrom(const LogFilePtr& logFile, qint64 startOffset,
                   const ResumeContext& context, const LogPattern& pattern,
                   bool extraction, bool initial);

    // Распознавание таймстампа/уровня и правило primary-строки; const-методы,
    // безопасно читается воркерами пула (см. LineClassifier).
    const LineClassifier m_classifier;
    LogPattern m_pattern; // Optional block schema for structured field extraction
    bool m_extractionEnabled = false; // Only extract fields when filter is active

    // Parse tasks run on this private pool, joined in the destructor so a
    // worker can never outlive the parser (and its members) it reads from.
    QThreadPool      m_pool;
    std::atomic_bool m_abort{false}; // Set on teardown to cut a running parse short.
};

#endif // LOGPARSER_H
