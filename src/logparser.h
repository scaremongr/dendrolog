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

    // This method can be called from a separate thread for stat analysis
    FileStats analyzeFileForStats(const QString& filePath);

    // Optional: configure a dynamic block schema so that the parser extracts
    // structured fields into LogEntry::fields for every primary line.
    // The string may be either the new serialized schema format or a legacy
    // log4cxx/log4j conversion pattern, which will be migrated on read.
    // Passing an empty string clears structured extraction.
    void setPattern(const QString& schemaString);
    void setExtractionEnabled(bool enabled) { m_extractionEnabled = enabled; }
    const LogPattern& pattern() const noexcept { return m_pattern; }

public slots:
    void startParsing(const LogFilePtr& logFile);
    // Incremental parse: read only bytes starting at startOffset.
    // startLogicalEntryId is the ID to assign to the first new primary entry.
    void startParsingFrom(const LogFilePtr& logFile, qint64 startOffset, int startLogicalEntryId);

signals:
    void parsingStarted(const LogFilePtr& logFile);
    void entriesParsed(const QVector<std::shared_ptr<LogEntry>>& entriesBatch, const LogFilePtr& logFile);
    void parsingFinished(int totalEntries, const LogFilePtr& logFile);
    void parsingFailed(const LogFilePtr& logFile);
    void parsingProgress(int progressPercentage, const LogFilePtr& logFile);

private:
    // The worker methods receive an immutable snapshot of the schema and the
    // extraction flag taken on the GUI thread at launch time. They never read
    // the mutable m_pattern / m_extractionEnabled members, so reconfiguring
    // the parser (setPattern) while a parse is in flight is race-free.
    void doParse(const LogFilePtr& logFile, const LogPattern& pattern, bool extraction);
    void doParseFrom(const LogFilePtr& logFile, qint64 startOffset, int startLogicalEntryId,
                     const LogPattern& pattern, bool extraction);

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
