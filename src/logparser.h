#ifndef LOGPARSER_H
#define LOGPARSER_H

#include "logentry.h"
#include "logpattern.h"
#include <QVector>
#include <QString>
#include <QDateTime>
#include <QRegularExpression>
#include <memory>
#include <QObject>

class LogParser : public QObject
{
    Q_OBJECT

public:
    explicit LogParser(QObject* parent = nullptr);

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

    // Optional: configure a Log4cxx / Log4j ConversionPattern so that the
    // parser extracts structured fields (thread id, logger name, message body,
    // NDC, source location) into LogEntry::fields for every primary line.
    // Passing an empty string clears the pattern (no extraction).
    void setPattern(const QString& conversionPattern);
    const LogPattern& pattern() const noexcept { return m_pattern; }

public slots:
    void startParsing(const LogFilePtr& logFile);

signals:
    void parsingStarted(const LogFilePtr& logFile);
    void entriesParsed(const QVector<std::shared_ptr<LogEntry>>& entriesBatch, const LogFilePtr& logFile);
    void parsingFinished(int totalEntries, const LogFilePtr& logFile);
    void parsingFailed(const LogFilePtr& logFile);
    void parsingProgress(int progressPercentage, const LogFilePtr& logFile);

private:
    bool detectTimestamp(const QString &line, QDateTime &ts);
    bool detectLogLevel(const QString &line, LogLevel &level) const;

    void doParse(const LogFilePtr& logFile);

    const QRegularExpression m_timestampRegex;
    QStringList m_timeFormats;
    const QRegularExpression m_levelRegex;
    LogPattern m_pattern; // Optional ConversionPattern for structured field extraction
};

#endif // LOGPARSER_H
