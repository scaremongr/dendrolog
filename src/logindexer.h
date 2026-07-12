#ifndef LOGINDEXER_H
#define LOGINDEXER_H

#include "lineclassifier.h"
#include "lineindex.h"
#include "logfile.h"
#include "logpattern.h"
#include <QObject>
#include <QThreadPool>
#include <atomic>
#include <memory>

// ============================================================================
// LogIndexer — асинхронное построение LineIndex по файлу лога. Аналог
// LogParser для индексного бэкенда: та же модель жизненного цикла (приватный
// пул, atomic-abort, деструктор ждёт воркеров), тот же LineClassifier и та же
// семантика первичных/continuation-строк, но текст строк НЕ материализуется.
//
// Критические инварианты:
//   • файл открывается БЕЗ QIODevice::Text — все смещения сырые байтовые
//     (совместимы с якорями FileChangeDetector); QTextStream не используется
//     (он не даёт байтовых позиций);
//   • разрез по '\n', '\r' перед ним учитывается в eolBytes и исключается
//     из текста; хвост файла без '\n' индексируется «предварительной»
//     строкой (eolBytes = 0) и переиндексируется при дозаписи;
//   • UTF-8 BOM пропускается (строки начинаются после него); UTF-16/32 BOM →
//     needsResidentFallback (эти кодировки умеет только резидентный путь);
//   • декодирование по строке QString::fromUtf8 (UTF-8 самосинхронизирующийся,
//     битые байты — replacement char, как у QTextStream).
//
// Дозапись (startIndexingFrom) наследует контекст логической записи из
// самого индекса: continuation-строки после границы дозаписи присоединяются
// к последней записи (резидентный инкрементальный парсер контекст терял —
// здесь поведение строго лучше).
// ============================================================================
class LogIndexer : public QObject
{
    Q_OBJECT

public:
    explicit LogIndexer(QObject* parent = nullptr);
    ~LogIndexer() override;

    // Схема полей: используется ТОЛЬКО для решения primary/continuation
    // (спаны полей в индексе не сохраняются — извлекаются по требованию).
    void setPattern(const QString& schemaString);
    void setExtractionEnabled(bool enabled) { m_extractionEnabled = enabled; }
    const LogPattern& pattern() const noexcept { return m_pattern; }

public slots:
    // Полная индексация с нуля; index должен быть свежесозданным (пустым).
    void startIndexing(const LogFilePtr& logFile,
                       const std::shared_ptr<LineIndex>& index);
    // Дозапись хвоста с index->endOffset(). reindexProvisionalTail — сначала
    // переиндексировать последнюю строку, если она была без '\n'
    // (truncateFrom + чтение с её начала); store обязан согласованно убрать
    // её строку из модели до применения новых батчей.
    void startIndexingFrom(const LogFilePtr& logFile,
                           const std::shared_ptr<LineIndex>& index,
                           bool reindexProvisionalTail);

signals:
    void indexingStarted(const LogFilePtr& logFile);
    // Опубликована очередная порция: строки [firstLine, firstLine + count).
    void indexBatchReady(const LogFilePtr& logFile, qint64 firstLine, qint64 count);
    void indexingProgress(int progressPercentage, const LogFilePtr& logFile);
    void indexingFinished(qint64 newLines, const LogFilePtr& logFile);
    void indexingFailed(const LogFilePtr& logFile);
    // Файл не в UTF-8 (BOM UTF-16/32) — открыть через резидентный путь.
    void needsResidentFallback(const LogFilePtr& logFile, const QString& reason);

private:
    void doIndex(const LogFilePtr& logFile, std::shared_ptr<LineIndex> index,
                 const LogPattern& pattern, bool extraction,
                 bool initial, bool reindexProvisionalTail);

    const LineClassifier m_classifier;
    LogPattern m_pattern;
    bool m_extractionEnabled = false;

    // Задачи индексации живут в приватном пуле; деструктор ждёт их завершения,
    // чтобы воркер не пережил объект (паттерн LogParser).
    QThreadPool      m_pool;
    std::atomic_bool m_abort{false};
};

#endif // LOGINDEXER_H
