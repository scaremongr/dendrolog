// logviewwidget.h
#ifndef LOGVIEWWIDGET_H
#define LOGVIEWWIDGET_H

#include <QWidget>
#include <QVBoxLayout>
#include "logmodel.h"
#include "logparser.h"
#include <QVector>
#include "logfile.h"
#include "LogListView.h"

class LogViewWidget : public QWidget {
    Q_OBJECT
public:
    explicit LogViewWidget(QWidget *parent = nullptr);
    ~LogViewWidget();

    LogModel*    model()    const { return m_model; }
    LogListView* view()     const { return m_view;  }

    // Добавляет файл в текущий view
    void addLogFile(const QString &filePath);
    
    // Возвращает список загруженных файлов
    QList<LogFilePtr> loadedFiles() const { return m_loadedFiles.toList(); }
    
    // Возвращает количество загруженных файлов
    int fileCount() const { return m_loadedFiles.size(); }

    // Задаёт ConversionPattern для парсера (применяется к следующим загружаемым файлам).
    void setParserPattern(const QString& pattern);
    QString parserPattern() const;

    // Search methods
    void searchTextNext(const QString& term, bool caseSensitive);
    void searchTextPrevious(const QString& term, bool caseSensitive);

signals: // Сигналы, которые LogViewWidget будет пробрасывать для MainWindow
    void fileParsingStarted(const LogFilePtr& logFile);
    void fileParsingProgress(const LogFilePtr& logFile, int progressPercentage);
    void fileParsingFinished(const LogFilePtr& logFile, int totalEntries);
    void fileParsingFailed(const LogFilePtr& logFile);

    // Новые сигналы для информации о строках
    void totalRowCountChanged(int totalRows);
    void currentRowChanged(int currentRow, int totalRows); // Передает и текущую, и общую
    void modelFiltered(int totalRowsAfterFilter); // Signal when model has been filtered

private slots:
    void handleEntriesParsed(const QVector<std::shared_ptr<LogEntry>>& entriesBatch, const LogFilePtr& parsedLogFile);
    void handleParsingFinished(int totalEntries, const LogFilePtr& parsedLogFile);
    void handleParsingFailed(const LogFilePtr& parsedLogFile);
    void handleParsingProgress(int progressPercentage, const LogFilePtr& parsedLogFile);
    void handleModelFilteredRelay(int totalRowsAfterFilter);

private:
    LogListView *m_view;
    LogModel    *m_model;
    QVector<LogFilePtr> m_loadedFiles;  // Список загруженных файлов

    LogParser* m_logParser;
};

#endif // LOGVIEWWIDGET_H
