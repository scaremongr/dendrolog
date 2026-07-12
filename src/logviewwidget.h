// logviewwidget.h
#ifndef LOGVIEWWIDGET_H
#define LOGVIEWWIDGET_H

#include <QWidget>
#include <QVBoxLayout>
#include "logmodel.h"
#include "logparser.h"
#include "logindexer.h"
#include <QVector>
#include "logfile.h"
#include "LogListView.h"
#include "filechangedetector.h"

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

    // Проверяет, изменились ли файлы на диске, и дочитывает новые данные.
    // Возвращает true если хотя бы один файл был обновлён.
    bool reloadChangedFiles();

    // Per-tab auto-reload. Default is taken from AppSettings at construction time.
    bool autoReload() const { return m_autoReload; }
    void setAutoReload(bool enabled) { m_autoReload = enabled; }

    // Задаёт схему извлечения блоков для парсера (применяется к следующим загрузкам).
    void setParserPattern(const QString& pattern);
    void setExtractionEnabled(bool enabled);
    QString parserPattern() const;
    QStringList parserFieldNames() const;

    // Search methods
    void searchTextNext(const QString& term, bool caseSensitive);
    void searchTextPrevious(const QString& term, bool caseSensitive);

signals: // Сигналы, которые LogViewWidget будет пробрасывать для MainWindow
    void fileParsingStarted(const LogFilePtr& logFile);
    void fileParsingProgress(const LogFilePtr& logFile, int progressPercentage);
    void fileParsingFinished(const LogFilePtr& logFile, int totalEntries);
    void fileParsingFailed(const LogFilePtr& logFile);

    // Emitted when an incremental reload finishes (new entry count passed).
    void reloadFinished(int newEntriesCount);

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
    // Incremental reload: append new entries without model reset
    void handleIncrementalEntriesParsed(const QVector<std::shared_ptr<LogEntry>>& batch, const LogFilePtr& logFile);
    void handleIncrementalParsingFinished(int totalEntries, const LogFilePtr& logFile);
    // Индексный бэкенд: приём опубликованных порций строк и завершения.
    void handleIndexBatchReady(const LogFilePtr& logFile, qint64 firstLine, qint64 count);
    void handleIndexingFinished(qint64 newLines, const LogFilePtr& logFile);
    void handleIndexingFailed(const LogFilePtr& logFile);
    void handleResidentFallback(const LogFilePtr& logFile, const QString& reason);

private:
    // Ленивая инициализация LogIndexer с подключением сигналов.
    void ensureIndexer();
    // Запуск полной индексации файла (store уже индексный).
    void startIndexedLoad(const LogFilePtr& logFile);

    LogListView *m_view;
    LogModel    *m_model;
    QVector<LogFilePtr> m_loadedFiles;  // Список загруженных файлов

    LogParser* m_logParser;

    // Per-file reload state: the anchor fingerprinting the already-consumed
    // prefix (used to tell appends from rewrites) and the next logicalEntryId.
    struct FileReloadState {
        FileChangeDetector::Anchor anchor;        // prefix size + boundary fingerprint
        int    nextLogicalEntryId = 0;
        bool   initialLoadDone = false;           // false = still doing the initial parse
        // Индексация файла в полёте: не запускать вторую по тому же индексу
        // (LineIndex — single-writer).
        bool   indexingInFlight = false;
    };
    QHash<QString, FileReloadState> m_fileReloadStates; // key = filePath

    // Parser used exclusively for incremental reads (so it doesn't interfere with
    // the initial parse that uses m_logParser).
    LogParser* m_reloadParser = nullptr;

    // Индексатор больших файлов (ленивый: создаётся при первом индексном файле).
    LogIndexer* m_indexer = nullptr;
    // Зеркало флага извлечения полей (для индексатора/индексного store).
    bool m_extractionEnabled = false;

    // Per-tab auto-reload flag. Defaults to the global AppSettings value at construction.
    bool m_autoReload = false;
};

#endif // LOGVIEWWIDGET_H
