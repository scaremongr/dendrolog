#ifndef LOGMODEL_H
#define LOGMODEL_H

#include "logentry.h"
#include "logscan.h"
#include "filterruleset.h"
#include "textmatchhighlighter.h"
#include <QAbstractListModel>
#include <QColor>
#include <QDateTime>
#include <QHash>
#include <QSet>
#include <memory>

class LogStore;
class ResidentLogStore;
class IndexedLogStore;

// ============================================================================
// LogModel — фасад QAbstractListModel над хранилищем записей (LogStore).
// Сам держит только презентационное состояние (настройки фильтров, выбор
// полей, маркеры, цвета файлов); данные и фильтрация живут в бэкенде:
//   • ResidentLogStore — прежний резидентный путь (по умолчанию);
//   • IndexedLogStore — блочный индекс для очень больших файлов.
// Вся сигнальная хореография (beginInsertRows/reset/modelFiltered) остаётся
// на GUI-потоке; store дёргает её через дружественный доступ.
// ============================================================================
class LogModel : public QAbstractListModel {
    Q_OBJECT

public:
    explicit LogModel(QObject* parent = nullptr);
    ~LogModel() override;

    // ---- Мутации резидентного бэкенда (путь LogParser) -----------------------
    void setEntries(const QVector<std::shared_ptr<LogEntry>>& entries);
    // Обновление без reset модели для tail-догрузки (auto-reload): сливает батч
    // в данные (как mergeEntries) и помечает его записи «новыми» — IsNewRole,
    // зелёный маркер в гаттере. Сортировка батча не требуется.
    void appendEntries(const QVector<std::shared_ptr<LogEntry>>& entries);
    // Слияние отсортированного батча в отсортированный список БЕЗ reset модели:
    // видимые строки вставляются через beginInsertRows, поэтому выделение и
    // позиция скролла сохраняются самим view. Записи могут вставать в середину
    // (слияние нескольких файлов по времени). Батч должен быть отсортирован
    // компаратором logEntryPtrLess.
    void mergeEntries(const QVector<std::shared_ptr<LogEntry>>& sortedBatch);
    // Drop every entry sourced from `filePath` and reset the view.
    void removeEntriesForFile(const QString& filePath);

    // ---- Доступ к данным (шов хранилища) --------------------------------------
    // Запись видимой (отфильтрованной) строки; nullptr при выходе за границы.
    std::shared_ptr<LogEntry> entryAt(int visibleRow) const;

    // Идентичность записи видимой строки: пара (logicalEntryId, sourceFile) —
    // id уникален только внутри одного файла. Вне границ — {-1, nullptr}.
    struct EntryKey {
        int logicalEntryId = -1;
        const LogFile* sourceFile = nullptr;
    };
    EntryKey keyForRow(int visibleRow) const;

    // Метаданные видимой строки без обращения к тексту; O(1).
    QDateTime visibleTimestampAt(int row) const;
    LogLevel visibleLevelAt(int row) const;

    // Первая видимая строка с timestamp >= t (список отсортирован по времени,
    // строки без валидной метки — в конце). Может вернуть rowCount().
    int firstVisibleRowAtOrAfter(const QDateTime& t) const;

    // Диапазон валидных таймстампов по ВСЕМ записям (не только видимым).
    QPair<QDateTime, QDateTime> fullTimeRange() const;

    // Первые maxCount непустых строк лога — сэмплы для эвристики схемы.
    QStringList sampleMessages(int maxCount) const;

    // Сырой текст видимой строки (прокси к LogStore::messageAt).
    QString messageAt(int visibleRow) const;

    // Все строки логической записи, которой принадлежит line (включая её саму).
    QVector<std::shared_ptr<LogEntry>> logicalRecordLines(
        const std::shared_ptr<LogEntry>& line, int maxLines = 2000) const;

    // Полная замена данных содержимым ВИДИМОГО списка source (посев панели
    // результатов поиска текущей выдачей активной вкладки).
    void seedFromVisible(const LogModel& source);

    // Снапшот для последовательного скана в воркере (статистика, таймлайн).
    LogScanSnapshot scanSnapshot(bool filteredOnly) const;

    // ЕДИНСТВЕННЫЙ легальный доступ к записям как к мутируемому набору —
    // для переизвлечения полей схемы (MainWindow::applyPatternToAllViews).
    // Осмыслен только для резидентного бэкенда.
    QVector<std::shared_ptr<LogEntry>> residentEntriesForFieldMutation() const;

    // ---- Бэкенды ---------------------------------------------------------------
    // Резидентный (по умолчанию) или индексный (очень большие файлы).
    bool isIndexedBackend() const;
    // Заменить хранилище индексным (данные текущего отбрасываются — вкладка
    // перезагружает файлы через LogIndexer). Возвращает store для привязки
    // файлов; повторный вызов возвращает существующий.
    IndexedLogStore* convertToIndexedBackend();
    // Текущий индексный бэкенд; nullptr для резидентного.
    IndexedLogStore* indexedOrNull() const;
    // Вернуться к резидентному хранилищу (fallback UTF-16/32: индексный
    // бэкенд не декодирует эти кодировки). Данные отбрасываются — файл
    // перечитывается через LogParser.
    void convertToResidentBackend();
    // --------------------------------------------------------------------------

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;

    // Длина текста, который вернёт DisplayRole для строки row, БЕЗ построения
    // самой строки. Для индексного бэкенда — оценка по байтовой длине (view
    // уточняет высоты лениво при отрисовке).
    int displayTextLength(int row) const;

    enum Roles {
        // Возвращает QVariantMap{"text": QString, "color": QColor} для Info-плашки.
        // Возвращает QVariant() (invalid) если загружен только один файл.
        FileBadgeRole = Qt::UserRole + 1,
        // true для записей из последнего батча appendEntries(); подсветка живёт
        // до следующего батча или до полного reset.
        IsNewRole,
        // Цвет фона строки от недеструктивного row-маркера (Row Highlighters).
        RowMarkerColorRole
    };

    // ---- Dynamic field-visibility control -----------------------------------
    void setAvailableFields(const QStringList& fieldNames);
    QStringList availableFields() const { return m_availableFieldNames; }
    void setFieldDisplaySelection(bool enabled, const QVector<int>& visibleIndexes);
    bool fieldDisplayFilterEnabled() const noexcept { return m_fieldFilterEnabled; }
    QVector<int> visibleFieldIndexes() const { return m_visibleFieldIndexes; }
    /// Force a display refresh without changing the schema or selection.
    void refreshDisplay();
    // -------------------------------------------------------------------------

    void setLogLevelFilter(const QSet<LogLevel>& levels);
    QSet<LogLevel> logLevelFilter() const { return m_activeLogLevels; }

    void setTimeRangeFilter(const QDateTime& start, const QDateTime& end);
    QDateTime startTimeFilter() const { return m_filterStartTime; }
    QDateTime endTimeFilter() const { return m_filterEndTime; }

    // ---- Текстовая фильтрация (Include/Exclude + AND/OR + колонки) ----------
    void setFilterRules(const FilterRuleSet& rules);
    const FilterRuleSet& filterRules() const { return m_filterRules; }

    // ---- Недеструктивные row-маркеры ----------------------------------------
    void setRowMarkers(const QVector<HighlightPattern>& markers);
    QVector<HighlightPattern> rowMarkers() const { return m_rowMarkers.patterns(); }

    // ---- Поиск записи в отфильтрованном представлении -----------------------
    int rowForEntry(int logicalEntryId, const LogFile* sourceFile) const;
    int nearestVisibleRow(int logicalEntryId, const LogFile* sourceFile) const;

    // Search methods
    QModelIndex findNextOccurrence(const QString& text, int startRow, Qt::CaseSensitivity cs, bool wrapAround = true) const;
    QModelIndex findPreviousOccurrence(const QString& text, int startRow, Qt::CaseSensitivity cs, bool wrapAround = true) const;

    // Отменяет фоновую перефильтрацию, если она идёт. wait=true — дождаться
    // фактической остановки воркера; обязательно перед конкурентной мутацией
    // LogEntry::fields (переизвлечение полей в MainWindow).
    void cancelPendingFilter(bool wait = false);
    // Перефильтровать, если отмена фонового джоба оставила видимый список
    // не соответствующим текущим настройкам фильтров.
    void reapplyFilterIfStale();

signals:
    void modelFiltered(int totalRowsAfterFilter);
    // Прогресс асинхронной фильтрации индексного бэкенда (он читает диск и
    // может занимать секунды); 100 — фильтр применён.
    void filterProgress(int progressPercentage);

private:
    // Хранилища ведут сигнальную хореографию модели и читают настройки
    // фильтров через дружественный доступ.
    friend class ResidentLogStore;
    friend class IndexedLogStore;

    // Текущий бэкенд как резидентный; assert при несоответствии.
    ResidentLogStore* resident() const;
    // Как resident(), но nullptr вместо assert — для мутаций, которые могут
    // прилететь от устаревшего резидентного парсера после смены бэкенда.
    ResidentLogStore* residentOrNull() const;

    void applyFilter();
    void ensureFileColor(const QString& filePath);
    void resetFileColors();
    QColor getColorForFile(const QString& filePath) const;
    // Отбрасывает индексы вне диапазона схемы и дубликаты, сохраняя порядок.
    QVector<int> sanitizeFieldIndexes(const QVector<int>& indexes) const;
    // Уведомляет view о смене отображаемого текста всех строк.
    void notifyDisplayChanged();

    // Returns the message text to display for entry, applying the active field
    // selection when field filtering is enabled.
    QString formatDisplayMessage(const LogEntry& entry) const;

    std::unique_ptr<LogStore> m_store;

    QSet<LogLevel> m_activeLogLevels;
    QDateTime m_filterStartTime;
    QDateTime m_filterEndTime;
    FilterRuleSet m_filterRules;
    TextMatchHighlighter m_rowMarkers;

    QHash<QString, QColor> m_fileColors;
    QVector<QColor> m_predefinedFileColors;
    int m_nextColorIndex;

    QStringList m_availableFieldNames;
    QVector<int> m_visibleFieldIndexes;
    bool m_fieldFilterEnabled = false;
};

#endif // LOGMODEL_H
