#ifndef LOGMODEL_H
#define LOGMODEL_H

#include "logentry.h"
#include "filterruleset.h"
#include "textmatchhighlighter.h"
#include <QAbstractListModel>
#include <QColor>
#include <QDateTime>
#include <QFuture>
#include <QHash>
#include <QSet>
#include <atomic>
#include <memory>

class LogModel : public QAbstractListModel {
    Q_OBJECT

public:
    explicit LogModel(QObject* parent = nullptr);
    ~LogModel() override;

    void setEntries(const QVector<std::shared_ptr<LogEntry>>& entries);
    // Обновление без reset модели для tail-догрузки (auto-reload): сливает батч
    // в данные (как mergeEntries) и помечает его записи «новыми» — IsNewRole,
    // зелёный маркер в гаттере. Сортировка батча не требуется.
    void appendEntries(const QVector<std::shared_ptr<LogEntry>>& entries);
    // Слияние отсортированного батча в отсортированный список БЕЗ reset модели:
    // видимые строки вставляются через beginInsertRows, поэтому выделение и
    // позиция скролла сохраняются самим view. Записи могут вставать в середину
    // (слияние нескольких файлов по времени). Батч должен быть отсортирован
    // компаратором logEntryPtrLess. В отличие от appendEntries() не помечает
    // записи «новыми» (IsNewRole) — это путь первичной загрузки, а не tail-append.
    void mergeEntries(const QVector<std::shared_ptr<LogEntry>>& sortedBatch);
    // Drop every entry sourced from `filePath` and reset the view. Used when a
    // watched file is replaced/truncated and must be re-parsed from scratch,
    // without disturbing entries that belong to other files in the same tab.
    void removeEntriesForFile(const QString& filePath);
    const QVector<std::shared_ptr<LogEntry>>& allEntries() const { return m_allEntries; }
    const QVector<std::shared_ptr<LogEntry>>& filteredEntries() const { return m_filteredEntries; }
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;

    enum Roles {
        // Возвращает QVariantMap{"text": QString, "color": QColor} для Info-плашки.
        // Возвращает QVariant() (invalid) если загружен только один файл.
        // Вся логика "показывать/не показывать" и цвет хранятся здесь — в модели.
        FileBadgeRole = Qt::UserRole + 1,
        // true для записей из последнего батча appendEntries(); подсветка живёт
        // до следующего батча или до полного reset (setEntries/removeEntriesForFile).
        // Used by LogListView to paint the gutter marker green for newly loaded rows.
        IsNewRole,
        // Цвет фона строки от недеструктивного row-маркера (Row Highlighters).
        // QColor если строка совпала с одним из маркеров, иначе invalid QVariant.
        RowMarkerColorRole
    };

    // ---- Dynamic field-visibility control -----------------------------------
    // The parser provides an ordered list of available field names. The model
    // stores the active list and the subset selected by the UI when field
    // filtering is enabled.
    void setAvailableFields(const QStringList& fieldNames);
    QStringList availableFields() const { return m_availableFieldNames; }
    void setFieldDisplaySelection(bool enabled, const QVector<int>& visibleIndexes);
    bool fieldDisplayFilterEnabled() const noexcept { return m_fieldFilterEnabled; }
    QVector<int> visibleFieldIndexes() const { return m_visibleFieldIndexes; }
    /// Force a display refresh without changing the schema or selection — call
    /// after re-extracting structured fields on already-loaded entries.
    void refreshDisplay();
    // -------------------------------------------------------------------------

    void setLogLevelFilter(const QSet<LogLevel>& levels);
    QSet<LogLevel> logLevelFilter() const { return m_activeLogLevels; }

    void setTimeRangeFilter(const QDateTime& start, const QDateTime& end);
    QDateTime startTimeFilter() const { return m_filterStartTime; }
    QDateTime endTimeFilter() const { return m_filterEndTime; }

    // ---- Текстовая фильтрация (Include/Exclude + AND/OR + колонки) ----------
    // Набор должен прийти уже привязанным к схеме (FilterRuleSet::bindFields).
    void setFilterRules(const FilterRuleSet& rules);
    const FilterRuleSet& filterRules() const { return m_filterRules; }

    // ---- Недеструктивные row-маркеры ----------------------------------------
    // Строки не скрываются — совпавшие получают цвет фона через
    // RowMarkerColorRole. Смена маркеров не перефильтровывает модель.
    void setRowMarkers(const QVector<HighlightPattern>& markers);
    QVector<HighlightPattern> rowMarkers() const { return m_rowMarkers.patterns(); }

    // ---- Поиск записи в отфильтрованном представлении -----------------------
    // Идентификация записи — пара (logicalEntryId, sourceFile): logicalEntryId
    // уникален только внутри одного файла.
    // Точная строка записи в текущем отфильтрованном виде; -1 если запись
    // скрыта фильтром или отсутствует.
    int rowForEntry(int logicalEntryId, const LogFile* sourceFile) const;
    // Ближайшая к записи видимая строка (сама запись может быть скрыта
    // фильтром). Возвращает -1, только если записи нет в m_allEntries или
    // отфильтрованный список пуст.
    int nearestVisibleRow(int logicalEntryId, const LogFile* sourceFile) const;

    // Search methods
    QModelIndex findNextOccurrence(const QString& text, int startRow, Qt::CaseSensitivity cs, bool wrapAround = true) const;
    QModelIndex findPreviousOccurrence(const QString& text, int startRow, Qt::CaseSensitivity cs, bool wrapAround = true) const;

    // Отменяет фоновую перефильтрацию, если она идёт. wait=true — дождаться
    // фактической остановки воркера; обязательно перед конкурентной мутацией
    // LogEntry::fields (переизвлечение полей в MainWindow), т.к. воркер их читает.
    void cancelPendingFilter(bool wait = false);
    // Перефильтровать, если отмена фонового джоба оставила видимый список
    // не соответствующим текущим настройкам фильтров.
    void reapplyFilterIfStale();

signals:
    void modelFiltered(int totalRowsAfterFilter);

private:
    void applyFilter();
    // Запускает пересчёт m_filteredEntries в пуле потоков; результат приедет
    // queued-вызовом и применится, если поколение не устарело.
    void startFilterJob();
    bool passesFilters(const std::shared_ptr<LogEntry>& entry) const;
    void rebuildFilteredEntries();
    // Общее ядро appendEntries()/mergeEntries(): слияние отсортированного батча
    // в m_allEntries и вставка прошедших фильтр строк в m_filteredEntries.
    void mergeSortedBatch(const QVector<std::shared_ptr<LogEntry>>& sortedBatch);
    // Вставка отсортированного списка записей в отсортированный m_filteredEntries
    // сериями (run) с beginInsertRows на каждую серию.
    void insertFilteredSorted(const QVector<std::shared_ptr<LogEntry>>& passing);
    // Назначает файлу следующий цвет из палитры, если у него ещё нет цвета.
    void ensureFileColor(const QString& filePath);
    QColor getColorForFile(const QString& filePath) const;
    int uniqueSourceFileCount() const;
    // Отбрасывает индексы вне диапазона схемы и дубликаты, сохраняя порядок.
    QVector<int> sanitizeFieldIndexes(const QVector<int>& indexes) const;
    // Уведомляет view о смене отображаемого текста всех строк.
    void notifyDisplayChanged();

    // Returns the message text to display for entry, applying the active field
    // selection when field filtering is enabled.
    // Returns entry.message directly (no allocation) when no filtering needed.
    QString formatDisplayMessage(const LogEntry& entry) const;

    QVector<std::shared_ptr<LogEntry>> m_allEntries;
    QVector<std::shared_ptr<LogEntry>> m_filteredEntries;
    QSet<LogLevel> m_activeLogLevels;
    QDateTime m_filterStartTime;
    QDateTime m_filterEndTime;
    FilterRuleSet m_filterRules;
    TextMatchHighlighter m_rowMarkers;

    QHash<QString, QColor> m_fileColors;
    QVector<QColor> m_predefinedFileColors;
    int m_nextColorIndex;

    mutable int m_cachedUniqueSourceFileCount = -1; // Кэш для количества уникальных файлов

    QStringList m_availableFieldNames;
    QVector<int> m_visibleFieldIndexes;
    bool m_fieldFilterEnabled = false;

    // Записи из последнего батча appendEntries() — подсвечиваются зелёным
    // маркером в гаттере (IsNewRole). Идентичность — по адресу LogEntry:
    // logicalEntryId уникален только внутри одного файла и для этого не годится.
    // Очищается при setEntries()/removeEntriesForFile() и на каждом новом батче.
    QSet<const LogEntry*> m_newEntries;

    // ---- Фоновая фильтрация --------------------------------------------------
    // До порога фильтр применяется синхронно (мгновенно и без мерцания);
    // выше — в пуле потоков, чтобы не замораживать GUI на больших логах.
    // Воркер работает со снапшотами (COW-копиями) данных и настроек фильтра
    // и никогда не трогает члены модели. Поколение защищает от применения
    // устаревшего результата; cancel-флаг обрывает ненужный воркер досрочно.
    static constexpr int kAsyncFilterThreshold = 100000;
    int m_filterGeneration = 0;                        // текущее поколение настроек/данных
    bool m_filterJobActive = false;                    // есть ли актуальный джоб в полёте
    bool m_filteredListStale = false;                  // список ещё не догнал настройки
    std::shared_ptr<std::atomic_bool> m_filterJobCancel; // флаг отмены текущего джоба
    QFuture<QVector<std::shared_ptr<LogEntry>>> m_filterJobFuture; // для cancelPendingFilter(wait)
};

#endif // LOGMODEL_H
