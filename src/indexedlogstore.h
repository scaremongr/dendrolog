#ifndef INDEXEDLOGSTORE_H
#define INDEXEDLOGSTORE_H

#include "lineindex.h"
#include "logpattern.h"
#include "logstore.h"
#include "textchunkcache.h"

#include <QFuture>
#include <QSet>
#include <atomic>

class QTimer;

// ============================================================================
// IndexedLogStore — бэкенд очень больших файлов: текст на диске, в памяти
// только LineIndex (~10 байт/строку). Строки адресуются RowRef =
// (fileId << 40) | lineNo; для одно-файловой вкладки список всех строк НЕ
// материализуется (тождественное отображение row == line — доминирующий
// 20-ГБ случай стоит 0 байт), при нескольких файлах строки сливаются по
// времени в m_allRefs. Пустой активный фильтр — тоже тождество
// (m_identityVisible), фильтр всегда считается АСИНХРОННО (он читает диск).
//
// Мутации приходят от LogViewWidget по сигналам LogIndexer:
//   attachFile → appendIndexedRows(батчи) → … reload: appendIndexedRows с
//   перекрытием одной строки (переиндексация предварительного хвоста) или
//   resetFileIndex (файл заменён).
// ============================================================================
class IndexedLogStore final : public LogStore {
public:
    using RowRef = quint64;
    static constexpr int kFileBits = 40; // строк на файл: 2^40

    explicit IndexedLogStore(LogModel& model);
    ~IndexedLogStore() override;

    Backend backend() const override { return Backend::Indexed; }

    // ---- Подключение файлов (GUI-поток, из LogViewWidget) --------------------
    int attachFile(const LogFilePtr& logFile, std::shared_ptr<LineIndex> index);
    // Очередной опубликованный диапазон строк файла. Перекрытие с уже
    // показанными строками (переиндексированный предварительный хвост)
    // обрабатывается как dataChanged первой строки + вставка остальных.
    // markNew — пометить строки зелёным маркером гаттера (tail-догрузка).
    void appendIndexedRows(const LogFilePtr& logFile, qint64 firstLine,
                           qint64 count, bool markNew);
    // Файл заменён: сброс его строк и индекса (свежий index начнёт с нуля).
    void resetFileIndex(const LogFilePtr& logFile, std::shared_ptr<LineIndex> fresh);
    std::shared_ptr<LineIndex> indexForFile(const QString& filePath) const;

    // Схема полей: для отображения/фильтрации извлекается по требованию.
    void setFieldPattern(const QString& patternString, bool extractionEnabled);
    void setTextCacheBudget(qint64 bytes);

    // ---- Виртуальный интерфейс LogStore ---------------------------------------
    qint64 allCount() const override;
    int visibleCount() const override;
    int uniqueSourceFileCount() const override { return int(m_files.size()); }
    std::shared_ptr<LogEntry> entryAt(int visibleRow) const override;
    LogEntryMeta visibleMetaAt(int row) const override;
    QDateTime visibleTimestampAt(int row) const override;
    LogLevel visibleLevelAt(int row) const override;
    QString messageAt(int visibleRow) const override;
    int rawTextLengthAt(int visibleRow) const override;
    bool isNewAt(int visibleRow) const override;
    int firstVisibleRowAtOrAfter(const QDateTime& t) const override;
    QPair<QDateTime, QDateTime> fullTimeRange() const override;
    QStringList sampleMessages(int maxCount) const override;
    QVector<std::shared_ptr<LogEntry>> logicalRecordLines(
        const std::shared_ptr<LogEntry>& line, int maxLines) const override;
    int rowForEntry(int logicalEntryId, const LogFile* sourceFile) const override;
    int nearestVisibleRow(int logicalEntryId, const LogFile* sourceFile) const override;
    int findNextOccurrence(const QString& text, int startRow,
                           Qt::CaseSensitivity cs, bool wrapAround) const override;
    int findPreviousOccurrence(const QString& text, int startRow,
                               Qt::CaseSensitivity cs, bool wrapAround) const override;
    LogScanSnapshot scanSnapshot(bool filteredOnly) const override;
    void applyFilter() override;
    void cancelPendingFilter(bool wait) override;
    void reapplyFilterIfStale() override;

private:
    struct IndexedFile {
        LogFilePtr logFile;
        std::shared_ptr<LineIndex> index;
        int cacheFileId = -1;
    };

    static RowRef makeRef(int fileId, qint64 line)
    {
        return (RowRef(quint32(fileId)) << kFileBits) | RowRef(line);
    }
    static int refFile(RowRef ref) { return int(ref >> kFileBits); }
    static qint64 refLine(RowRef ref) { return qint64(ref & ((RowRef(1) << kFileBits) - 1)); }

    bool identityAll() const { return m_allRefs.isEmpty(); }
    RowRef rowToRef(int visibleRow) const;
    // Глобальный порядок строк — зеркало LogEntry::operator< по метаданным.
    bool lessRef(RowRef a, RowRef b) const;
    bool hasActiveFilterSettings() const;
    // Проверка одной строки текущими настройками (GUI, точечно: хвост).
    bool refPassesFiltersNow(RowRef ref) const;
    void materializeAllRefs();
    void startFilterJob(bool fullRescan, qint64 rangeFirst = -1, qint64 rangeCount = 0,
                        int rangeFileId = -1);
    void insertVisibleSorted(const QVector<RowRef>& passing);
    void startNextPendingRange();
    std::shared_ptr<LogEntry> materializeEntry(RowRef ref) const;

    QVector<IndexedFile> m_files;
    mutable TextChunkCache m_textCache;

    // Строк ПОКАЗАНО модели (тождественный режим одного файла). Индекс растёт
    // в воркере раньше уведомления — rowCount() обязан отражать состояние
    // между begin/endInsertRows, а не живой lineCount() индекса.
    qint64 m_shownAllCount = 0;

    QVector<RowRef> m_allRefs;     // пуст для одного файла (тождество)
    QVector<RowRef> m_visibleRefs; // действителен, когда !m_identityVisible
    bool m_identityVisible = true; // нет активного фильтра — видно всё

    QSet<RowRef> m_newRefs;        // строки последнего tail-батча (IsNewRole)

    LogPattern m_fieldPattern;
    bool m_extractionEnabled = false;

    // ---- Асинхронная фильтрация (поколение/cancel — как у резидентного) ------
    struct PendingRange { int fileId; qint64 first; qint64 count; };
    int m_filterGeneration = 0;
    bool m_filterJobActive = false;
    bool m_filteredListStale = false;
    std::shared_ptr<std::atomic_bool> m_filterJobCancel;
    QFuture<QVector<RowRef>> m_filterJobFuture;
    QVector<PendingRange> m_pendingRanges; // FIFO инкрементальных диапазонов
    std::shared_ptr<std::atomic<int>> m_filterProgress;
    QTimer* m_progressTimer = nullptr; // parent — LogModel
    std::shared_ptr<int> m_aliveGuard = std::make_shared<int>(0);
};

#endif // INDEXEDLOGSTORE_H
