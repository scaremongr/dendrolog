#ifndef LOGMODEL_H
#define LOGMODEL_H

#include "logentry.h"
#include "logpattern.h"
#include "filterruleset.h"
#include "textmatchhighlighter.h"
#include <QAbstractListModel>
#include <QDateTime>
#include <QFontMetrics>
#include <QHash>
#include <QSet>
#include <memory>
#include <QColor>
#include <QMap>

// Define dimmer colors for log levels
const QColor DIM_TRACE_COLOR = QColor(220, 220, 220); // Light gray
const QColor DIM_DEBUG_COLOR = QColor(200, 200, 255); // Light blue-ish
const QColor DIM_INFO_COLOR = QColor(200, 255, 200);  // Light green-ish
const QColor DIM_WARN_COLOR = QColor(255, 240, 180);  // Pale yellow
const QColor DIM_ERROR_COLOR = QColor(255, 200, 200); // Pale red
const QColor DIM_FATAL_COLOR = QColor(230, 180, 180); // Darker pale red / pink-ish
const QColor DEFAULT_BACKGROUND_COLOR = Qt::white; // or Transparent, depending on desired effect

class LogModel : public QAbstractListModel {
    Q_OBJECT

public:
    explicit LogModel(QObject* parent = nullptr);

    void setEntries(const QVector<std::shared_ptr<LogEntry>>& entries);
    // Append-only update: adds new entries without resetting the model.
    // Preserves the current selection and scroll position in the view.
    // New entries must be >= all existing entries (appended at end of file).
    void appendEntries(const QVector<std::shared_ptr<LogEntry>>& entries);
    // Drop every entry sourced from `filePath` and reset the view. Used when a
    // watched file is replaced/truncated and must be re-parsed from scratch,
    // without disturbing entries that belong to other files in the same tab.
    void removeEntriesForFile(const QString& filePath);
    const QVector<std::shared_ptr<LogEntry>>& allEntries() const { return m_allEntries; }
    const QVector<std::shared_ptr<LogEntry>>& filteredEntries() const { return m_filteredEntries; }
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;

    QColor getColorForFile(const QString& filePath) const;

    enum Roles {
        TimestampRole = Qt::UserRole + 1,
        LevelRole,
        MessageRole,
        SourceFileRole,
        IsExpandedRole,
        // Возвращает QVariantMap{"text": QString, "color": QColor} для Info-плашки.
        // Возвращает QVariant() (invalid) если загружен только один файл.
        // Вся логика "показывать/не показывать" и цвет хранятся здесь — в модели.
        FileBadgeRole,
        // Returns the display-formatted message text respecting the current
        // field-visibility mask.  Identical to Qt::DisplayRole but exposed
        // as a named role for QML / delegate convenience.
        DisplayMessageRole,
        // Returns true for entries appended via appendEntries() within the last 15 seconds.
        // Used by LogListView to paint the gutter marker green for newly loaded rows.
        IsNewRole,
        // Цвет фона строки от недеструктивного row-маркера (Row Highlighters).
        // QColor если строка совпала с одним из маркеров, иначе invalid QVariant.
        // Отделён от Qt::BackgroundRole, чтобы не менять существующую отрисовку.
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

    // кеш сокращенных строк
    struct ElideCacheEntry {
        int viewWidth;        // ширина textRect
        QString elided;       // уже усечённая строка
        int hidden;           // сколько символов спрятано
    };

    // хранится:  row → ElideCacheEntry
    mutable QHash<int, ElideCacheEntry> m_elideCache;

    // Возвращает true, если был кеш; иначе заполняет и кеширует
    bool getOrBuildElide(int row,
                         const QString& msg,
                         const QFontMetrics& fm,
                         int available,
                         int badgeReserve,
                         QString& outElided,
                         int& outHidden) const;
    void clearElideCache();
    bool isRowExpanded(int row) const;
    int uniqueSourceFileCount() const;

    // Кэширование высоты развернутых строк
    int getCachedExpandedRowHeight(int row) const;
    void cacheExpandedRowHeight(int row, int height) const; // const, т.к. кэш mutable
    void clearExpandedRowHeightCache();
    void invalidateExpandedRowHeightCache(int row);

    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;

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

    QColor getLogLevelColor(LogLevel level) const;

    static QColor defaultColorForLevel(LogLevel level);

    // ---- Поиск записи в отфильтрованном представлении -----------------------
    // Идентификация записи — пара (logicalEntryId, sourceFile): logicalEntryId
    // уникален только внутри одного файла.
    // Точная строка записи в текущем отфильтрованном виде; -1 если запись
    // скрыта фильтром или отсутствует.
    int rowForEntry(int logicalEntryId, const void* sourceFile) const;
    // Ближайшая к записи видимая строка (сама запись может быть скрыта
    // фильтром). Возвращает -1, только если записи нет в m_allEntries или
    // отфильтрованный список пуст.
    int nearestVisibleRow(int logicalEntryId, const void* sourceFile) const;

    // Search methods
    QModelIndex findNextOccurrence(const QString& text, int startRow, Qt::CaseSensitivity cs, bool wrapAround = true);
    QModelIndex findPreviousOccurrence(const QString& text, int startRow, Qt::CaseSensitivity cs, bool wrapAround = true);

signals:
    void modelFiltered(int totalRowsAfterFilter);

protected:
    QHash<int, QByteArray> roleNames() const override;

private:
    void applyFilter();
    bool passesFilters(const std::shared_ptr<LogEntry>& entry) const;
    void rebuildFilteredEntries();
    void clearRowDependentCaches();

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

    QMap<QString, QColor> m_fileColors;
    QVector<QColor> m_predefinedFileColors;
    int m_nextColorIndex;

    QSet<int> m_expandedRows;
    mutable QHash<int, int> m_expandedRowHeightCache; // Кэш высот развернутых строк
    mutable int m_cachedUniqueSourceFileCount = -1;   // Кэш для количества уникальных файлов

    QMap<LogLevel, QColor> m_logLevelColors; // For background colors

    QStringList m_availableFieldNames;
    QVector<int> m_visibleFieldIndexes;
    bool m_fieldFilterEnabled = false;

    // Set of logicalEntryIds from the most recently appended batch.
    // Cleared and replaced on every appendEntries() call, and on setEntries().
    // Used by LogListView to paint the gutter marker green for the latest loaded rows.
    QSet<int> m_newEntryIds;
};

#endif // LOGMODEL_H
