#ifndef LOGMODEL_H
#define LOGMODEL_H

#include "logentry.h"
#include <QAbstractListModel>
#include <QFontMetrics>
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
    QVector<std::shared_ptr<LogEntry>> allEntries() const { return m_allEntries; }
    QVector<std::shared_ptr<LogEntry>> filteredEntries() const { return m_filteredEntries; }
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
        FileBadgeRole
    };

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

    struct MessageFilterRule {
        QString substring;
        Qt::CaseSensitivity caseSensitivity;
    };
    void setMessageFilterRules(const QVector<MessageFilterRule>& rules);
    QVector<MessageFilterRule> messageFilterRules() const { return m_messageFilterRules; }

    QColor getLogLevelColor(LogLevel level) const;

    static QColor defaultColorForLevel(LogLevel level);

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

    QVector<std::shared_ptr<LogEntry>> m_allEntries;
    QVector<std::shared_ptr<LogEntry>> m_filteredEntries;
    QSet<LogLevel> m_activeLogLevels;
    QDateTime m_filterStartTime;
    QDateTime m_filterEndTime;
    QVector<MessageFilterRule> m_messageFilterRules;

    QMap<QString, QColor> m_fileColors;
    QVector<QColor> m_predefinedFileColors;
    int m_nextColorIndex;

    QSet<int> m_expandedRows;
    mutable QHash<int, int> m_expandedRowHeightCache; // Кэш высот развернутых строк
    mutable int m_cachedUniqueSourceFileCount = -1;   // Кэш для количества уникальных файлов

    QMap<LogLevel, QColor> m_logLevelColors; // For background colors
};

#endif // LOGMODEL_H
