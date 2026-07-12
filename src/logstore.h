#ifndef LOGSTORE_H
#define LOGSTORE_H

#include "logentry.h"
#include "logscan.h"
#include <QDateTime>
#include <QPair>
#include <QStringList>
#include <QVector>
#include <functional>
#include <memory>

class LogModel;

// ============================================================================
// LogStore — способ хранения записей за фасадом LogModel. Два бэкенда:
//   • ResidentLogStore — прежнее резидентное хранилище (вектора shared_ptr,
//     весь текст в памяти) — путь по умолчанию для «обычных» файлов;
//   • IndexedLogStore — блочный индекс строк + чтение текста с диска по
//     требованию — для очень больших файлов.
//
// Store НЕ QObject: всю сигнальную хореографию QAbstractItemModel
// (beginInsertRows/reset/dataChanged/modelFiltered) он ведёт через владеющую
// LogModel (дружественный доступ) и ТОЛЬКО на GUI-потоке; воркеры доставляют
// результаты queued-вызовами в контексте модели.
//
// Мутации данных (слияние батчей, привязка индексируемых файлов) у бэкендов
// разные и в виртуальный интерфейс не входят — LogModel делегирует их
// конкретному типу store.
// ============================================================================
class LogStore {
public:
    enum class Backend { Resident, Indexed };

    explicit LogStore(LogModel& model) : m_model(model) {}
    virtual ~LogStore() = default;

    virtual Backend backend() const = 0;

    // ---- Счётчики -------------------------------------------------------------
    virtual qint64 allCount() const = 0;
    virtual int visibleCount() const = 0;
    virtual int uniqueSourceFileCount() const = 0;

    // ---- Точечный доступ к видимым строкам ------------------------------------
    // Запись строки; индексный бэкенд материализует её по требованию.
    virtual std::shared_ptr<LogEntry> entryAt(int visibleRow) const = 0;
    virtual LogEntryMeta visibleMetaAt(int row) const = 0;
    virtual QDateTime visibleTimestampAt(int row) const = 0;
    virtual LogLevel visibleLevelAt(int row) const = 0;
    // Сырой текст строки (маркеры, поиск).
    virtual QString messageAt(int visibleRow) const = 0;
    // Длина сырого текста; у индексного бэкенда — оценка по байтовой длине
    // (view уточняет высоты лениво при отрисовке).
    virtual int rawTextLengthAt(int visibleRow) const = 0;
    // Строка из последнего tail-батча (зелёный маркер гаттера, IsNewRole).
    virtual bool isNewAt(int visibleRow) const = 0;

    // ---- Навигация/выборки ------------------------------------------------------
    virtual int firstVisibleRowAtOrAfter(const QDateTime& t) const = 0;
    virtual QPair<QDateTime, QDateTime> fullTimeRange() const = 0;
    virtual QStringList sampleMessages(int maxCount) const = 0;
    virtual QVector<std::shared_ptr<LogEntry>> logicalRecordLines(
        const std::shared_ptr<LogEntry>& line, int maxLines) const = 0;
    virtual int rowForEntry(int logicalEntryId, const LogFile* sourceFile) const = 0;
    virtual int nearestVisibleRow(int logicalEntryId, const LogFile* sourceFile) const = 0;
    // Синхронный поиск подстроки в видимых строках; -1 — не найдено.
    virtual int findNextOccurrence(const QString& text, int startRow,
                                   Qt::CaseSensitivity cs, bool wrapAround) const = 0;
    virtual int findPreviousOccurrence(const QString& text, int startRow,
                                       Qt::CaseSensitivity cs, bool wrapAround) const = 0;

    // ---- Сканы ------------------------------------------------------------------
    virtual LogScanSnapshot scanSnapshot(bool filteredOnly) const = 0;

    // ---- Фильтрация ---------------------------------------------------------------
    // Применить ТЕКУЩИЕ настройки фильтров модели (store читает их у неё).
    virtual void applyFilter() = 0;
    virtual void cancelPendingFilter(bool wait) = 0;
    virtual void reapplyFilterIfStale() = 0;

protected:
    LogModel& m_model;
};

#endif // LOGSTORE_H
