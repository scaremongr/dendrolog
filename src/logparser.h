#ifndef LOGPARSER_H
#define LOGPARSER_H

#include "logentry.h"
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
};

#endif // LOGPARSER_H
