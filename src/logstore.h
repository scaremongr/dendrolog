#ifndef LOGSTORE_H
#define LOGSTORE_H

#include "logentry.h"
#include "logscan.h"
#include "recordid.h"
#include "textquery.h"
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
    // МЕСТО записи среди видимых строк: запись видна — её строка
    // (rowForEntry), *visible = true; скрыта фильтром — число видимых строк,
    // стоящих перед ней в порядке вкладки (от 0 до числа видимых
    // включительно), *visible = false. В отличие от nearestVisibleRow место
    // не прижимается к последней строке, поэтому «перед последней видимой» и
    // «после всех видимых» различимы: от места стартует быстрый поиск.
    // -1 — записи нет в данных.
    virtual int placeAmongVisible(int logicalEntryId, const LogFile* sourceFile,
                                  bool* visible = nullptr) const = 0;
    // Видимая строка для строки lineNumber (с 1) файла sourceFile — переход
    // Ctrl+G. Номер за концом файла — его последняя строка. Строка скрыта
    // фильтром — следующая видимая за её местом в порядке вкладки, иначе
    // последняя (как nearestVisibleRow). -1 — файла нет во вкладке или не
    // видно ни одной строки. Индексный бэкенд — двоичный поиск.
    virtual int nearestVisibleRowForLine(qint64 lineNumber,
                                         const LogFile* sourceFile) const = 0;
    // ---- Ключ строки (R-1) ------------------------------------------------------
    // Ключ видимой строки; невалидный — строки нет. Резидентный бэкенд —
    // O(1), индексный — два чтения индекса.
    virtual RecordKey keyAt(int visibleRow) const = 0;
    // Видимая строка ключа; -1 — строка скрыта фильтром или ключ не
    // действителен (строки нет в документе с теми же id записи и меткой,
    // см. recordid.h). O(log N) на обоих бэкендах; индексный в тождественном
    // режиме — O(1).
    virtual int rowForKey(const RecordKey& key) const = 0;
    // Ближайшая видимая строка к строке ключа — та же семантика, что у
    // nearestVisibleRow и nearestVisibleRowForLine: видна — она сама; скрыта
    // фильтром — следующая видимая за её МЕСТОМ в порядке вкладки, а за
    // концом — последняя видимая. -1 — ключ не действителен или не видно ни
    // одной строки. Цена — как у rowForKey.
    virtual int nearestRowForKey(const RecordKey& key) const = 0;

    // Синхронный поиск текста (быстрый поиск: подстрока, слово целиком,
    // регекс) в видимых строках; -1 — не найдено. Одна реализация на оба
    // бэкенда — LogScanSnapshot::findRowWithText, та же, что у фонового
    // QuickSearch. Индексный бэкенд читает диск (своими ридерами скана, кэш
    // вьюпорта не вымывается): на огромной вкладке — только из фона.
    int findOccurrence(const TextQuery& query, int startRow, bool forward,
                       bool wrapAround) const
    {
        return int(scanSnapshot(/*filteredOnly=*/true)
                       .findRowWithText(query, startRow, forward, wrapAround));
    }

    // Ближайшая видимая строка, с которой начинается запись уровня из levels
    // (F8 / Shift+F8), -1 — нет. Семантика и цена — LogScanSnapshot::
    // findRowWithLevel: одна реализация на оба бэкенда поверх их снапшотов,
    // только метаданные (индексный бэкенд текст с диска не читает). Проход
    // линейный — на огромной вкладке звать из фона (QuickSearch::startLevel),
    // а не с GUI-потока.
    int findRowWithLevel(LogLevelMask levels, int startRow, bool forward,
                         bool wrapAround) const
    {
        return int(scanSnapshot(/*filteredOnly=*/true)
                       .findRowWithLevel(levels, startRow, forward, wrapAround));
    }

    // ---- Сканы ------------------------------------------------------------------
    virtual LogScanSnapshot scanSnapshot(bool filteredOnly) const = 0;

    // ---- Фильтрация ---------------------------------------------------------------
    // Применить ТЕКУЩИЕ настройки фильтров модели (store читает их у неё).
    virtual void applyFilter() = 0;
    virtual void cancelPendingFilter(bool wait) = 0;
    virtual void reapplyFilterIfStale() = 0;
    // Фоновый фильтр-джоб в полёте: видимый список ещё догоняет настройки
    // (или новые строки ещё проверяются).
    virtual bool isFiltering() const = 0;

protected:
    LogModel& m_model;
};

#endif // LOGSTORE_H
