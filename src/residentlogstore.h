#ifndef RESIDENTLOGSTORE_H
#define RESIDENTLOGSTORE_H

#include "logstore.h"
#include <QFuture>
#include <QSet>
#include <atomic>

// ============================================================================
// ResidentLogStore — прежнее резидентное хранилище LogModel, перенесённое за
// интерфейс LogStore БЕЗ изменения поведения: два вектора shared_ptr
// (все записи + видимые), синхронный фильтр до порога и фоновый джоб выше,
// слияние отсортированных батчей без reset модели.
// ============================================================================
class ResidentLogStore final : public LogStore {
public:
    explicit ResidentLogStore(LogModel& model) : LogStore(model) {}

    Backend backend() const override { return Backend::Resident; }

    // ---- Виртуальный интерфейс чтения -----------------------------------------
    qint64 allCount() const override { return m_allEntries.size(); }
    int visibleCount() const override { return int(m_filteredEntries.size()); }
    int uniqueSourceFileCount() const override;
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

    // ---- Резидентные мутации (вызываются фасадом LogModel) ----------------------
    void setEntries(const QVector<std::shared_ptr<LogEntry>>& entries);
    void appendEntries(const QVector<std::shared_ptr<LogEntry>>& entries);
    void mergeEntries(const QVector<std::shared_ptr<LogEntry>>& sortedBatch);
    void removeEntriesForFile(const QString& filePath);

    // Прямой доступ для переизвлечения полей и посева результатов поиска —
    // только внутри слоя хранения/фасада.
    const QVector<std::shared_ptr<LogEntry>>& allEntries() const { return m_allEntries; }
    const QVector<std::shared_ptr<LogEntry>>& visibleEntries() const { return m_filteredEntries; }

private:
    bool passesFilters(const std::shared_ptr<LogEntry>& entry) const;
    void rebuildFilteredEntries();
    void mergeSortedBatch(const QVector<std::shared_ptr<LogEntry>>& sortedBatch);
    void insertFilteredSorted(const QVector<std::shared_ptr<LogEntry>>& passing);
    void startFilterJob();

    QVector<std::shared_ptr<LogEntry>> m_allEntries;
    QVector<std::shared_ptr<LogEntry>> m_filteredEntries;

    mutable int m_cachedUniqueSourceFileCount = -1;

    // Записи из последнего батча appendEntries() — подсвечиваются зелёным
    // маркером в гаттере (IsNewRole). Идентичность — по адресу LogEntry:
    // logicalEntryId уникален только внутри одного файла.
    QSet<const LogEntry*> m_newEntries;

    // ---- Фоновая фильтрация (см. комментарий в logmodel.h истории) -------------
    // До порога фильтр применяется синхронно; выше — в пуле потоков со
    // снапшотами данных и настроек, поколением и cancel-флагом.
    static constexpr int kAsyncFilterThreshold = 100000;
    int m_filterGeneration = 0;
    bool m_filterJobActive = false;
    bool m_filteredListStale = false;
    std::shared_ptr<std::atomic_bool> m_filterJobCancel;
    QFuture<QVector<std::shared_ptr<LogEntry>>> m_filterJobFuture;
    // Живучесть store для колбэков watcher'ов: при смене бэкенда (store
    // умирает раньше модели) отложенный finished не должен трогать память.
    std::shared_ptr<int> m_aliveGuard = std::make_shared<int>(0);
};

#endif // RESIDENTLOGSTORE_H
