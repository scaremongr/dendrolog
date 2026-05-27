#include "logmodel.h"

LogModel::LogModel(QObject* parent)
    : QAbstractListModel(parent)
    , m_nextColorIndex(0) // Initialize nextColorIndex
    // m_filterStartTime and m_filterEndTime are default-constructed to be invalid (isNull() == true)
{
    qRegisterMetaType<LogFilePtr>();
    // Initialize m_activeLogLevels to include all levels by default, so no filtering initially.
    // Or leave it empty and handle empty set as "show all" in applyFilter.
    // For now, let's leave it empty and handle in applyFilter.

    // Initialize LogLevel colors
    m_logLevelColors[LogLevel::Trace] = DIM_TRACE_COLOR;
    m_logLevelColors[LogLevel::Debug] = DIM_DEBUG_COLOR;
    m_logLevelColors[LogLevel::Info] = DIM_INFO_COLOR;
    m_logLevelColors[LogLevel::Warn] = DIM_WARN_COLOR;
    m_logLevelColors[LogLevel::Error] = DIM_ERROR_COLOR;
    m_logLevelColors[LogLevel::Fatal] = DIM_FATAL_COLOR;
    m_logLevelColors[LogLevel::Unknown] = DEFAULT_BACKGROUND_COLOR; // Or some other default

    // Initialize predefined colors
    m_predefinedFileColors << QColor(Qt::cyan).darker(220)
                           << QColor(Qt::magenta).darker(220)
                           << QColor(Qt::yellow).darker(250) // Lighter yellow for better contrast with dark text
                           << QColor(Qt::green).darker(220)
                           << QColor(Qt::blue).darker(220)
                           << QColor(Qt::darkCyan).darker(220)
                           << QColor(Qt::darkMagenta).darker(220)
                           << QColor(Qt::darkYellow).darker(220)
                           << QColor(Qt::darkGreen).darker(220)
                           << QColor(Qt::darkBlue).darker(220)
                           << QColor(Qt::darkRed).darker(220);
}

void LogModel::setEntries(const QVector<std::shared_ptr<LogEntry>>& entries)
{
    beginResetModel();
    m_allEntries = entries;
    m_cachedUniqueSourceFileCount = -1; // Инвалидация кэша

    // Clear existing file colors to re-assign if file set changes (though typically we append)
    // For a full refresh, this is appropriate. If appending, might need different logic.
    // For now, let's assume setEntries is a full refresh.
    m_fileColors.clear();
    m_nextColorIndex = 0; // Reset color index for fresh assignment

    QSet<QString> processedFilePaths;
    for (const auto& entry : m_allEntries) {
        if (entry && entry->sourceFile) {
            const QString& filePath = entry->sourceFile->filePath;
            if (!processedFilePaths.contains(filePath)) {
                if (!m_fileColors.contains(filePath)) { // Should be true due to clear above, but good check
                    if (!m_predefinedFileColors.isEmpty()) {
                        m_fileColors[filePath] = m_predefinedFileColors[m_nextColorIndex];
                        m_nextColorIndex = (m_nextColorIndex + 1) % m_predefinedFileColors.size();
                    } else {
                        m_fileColors[filePath] = Qt::gray; // Fallback if no predefined colors
                    }
                }
                processedFilePaths.insert(filePath);
            }
        }
    }

    clearRowDependentCaches();
    rebuildFilteredEntries();
    endResetModel();
    emit modelFiltered(m_filteredEntries.size());
}

int LogModel::rowCount(const QModelIndex&) const
{
    return m_filteredEntries.size(); // Use filtered entries
}

QVariant LogModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_filteredEntries.size()) // Use filtered entries
        return QVariant();

    const std::shared_ptr<LogEntry>& entry = m_filteredEntries.at(index.row()); // Changed type, use const&
    switch (role) {
    case TimestampRole:
        return entry->timestamp; // Use ->
    case LevelRole:
        return static_cast<int>(entry->level); // Use ->
    case MessageRole:
        return entry->message; // Raw, unfiltered — use for search / copy
    case DisplayMessageRole:
        return formatDisplayMessage(*entry);
    case SourceFileRole:
        return QVariant::fromValue<LogFilePtr>(entry->sourceFile); // Use ->
    case IsExpandedRole:
        return m_expandedRows.contains(index.row());
    case FileBadgeRole: {
        // Плашка показывается только если загружено несколько файлов.
        // Вся логика (показывать/нет, текст, цвет) инкапсулирована здесь.
        if (uniqueSourceFileCount() <= 1 || !entry->sourceFile)
            return QVariant();
        QVariantMap badge;
        badge["text"]  = entry->sourceFile->shortName();
        badge["color"] = getColorForFile(entry->sourceFile->filePath);
        return badge;
    }
    case Qt::BackgroundRole: // Handle background color
        return getLogLevelColor(entry->level);
    case Qt::DisplayRole:
        return formatDisplayMessage(*entry);
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> LogModel::roleNames() const
{
    return {
        {TimestampRole,      "timestamp"},
        {LevelRole,          "level"},
        {MessageRole,        "message"},
        {DisplayMessageRole, "displayMessage"},
        {SourceFileRole,     "sourceFile"},
        {IsExpandedRole,     "isExpanded"},
        {FileBadgeRole,      "fileBadge"}
    };
}

bool LogModel::getOrBuildElide(int row,
                               const QString& msg,
                               const QFontMetrics& fm,
                               int available,
                               int badgeReserve,
                               QString& outElided,
                               int& outHidden) const
{
    auto it = m_elideCache.constFind(row);
    if (it != m_elideCache.cend() && it->viewWidth == available - badgeReserve) {
        outElided = it->elided;
        outHidden = it->hidden;
        return true;
    }
    // строим заново
    QString el = fm.elidedText(msg, Qt::ElideRight, available - badgeReserve);
    int hid   = msg.length() - el.length();
    m_elideCache[row] = { available - badgeReserve, el, hid };
    outElided = el;
    outHidden = hid;
    return false;
}

void LogModel::clearElideCache()
{
    m_elideCache.clear();
}

bool LogModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || index.row() >= m_filteredEntries.size())
        return false;

    if (role == IsExpandedRole) {
        bool expanded = value.toBool();
        if (expanded)
            m_expandedRows.insert(index.row());
        else {
            m_expandedRows.remove(index.row());
            m_expandedRowHeightCache.remove(index.row());
        }
        emit dataChanged(index, index, {IsExpandedRole, Qt::SizeHintRole});
        return true;
    }
    return QAbstractListModel::setData(index, value, role);
}

bool LogModel::isRowExpanded(int row) const
{
    return m_expandedRows.contains(row);
}

int LogModel::uniqueSourceFileCount() const
{
    if (m_cachedUniqueSourceFileCount != -1) {
        return m_cachedUniqueSourceFileCount;
    }

    QSet<LogFilePtr> uniqueFiles;
    for (const auto& entry : m_allEntries) { // entry is now std::shared_ptr<LogEntry>
        if (entry->sourceFile) { // Use ->
            uniqueFiles.insert(entry->sourceFile); // Use ->
        }
    }
    m_cachedUniqueSourceFileCount = uniqueFiles.size();
    return m_cachedUniqueSourceFileCount;
}

int LogModel::getCachedExpandedRowHeight(int row) const
{
    return m_expandedRowHeightCache.value(row, -1);
}

void LogModel::cacheExpandedRowHeight(int row, int height) const
{
    m_expandedRowHeightCache.insert(row, height);
}

void LogModel::clearExpandedRowHeightCache()
{
    m_expandedRowHeightCache.clear();
}

void LogModel::invalidateExpandedRowHeightCache(int row)
{
    m_expandedRowHeightCache.remove(row);
}

void LogModel::setLogLevelFilter(const QSet<LogLevel>& levels)
{
    m_activeLogLevels = levels;
    applyFilter();
}

void LogModel::setTimeRangeFilter(const QDateTime& start, const QDateTime& end)
{
    m_filterStartTime = start;
    m_filterEndTime = end;
    applyFilter();
}

void LogModel::setMessageFilterRules(const QVector<MessageFilterRule>& rules)
{
    m_messageFilterRules = rules;
    applyFilter();
}

bool LogModel::passesFilters(const std::shared_ptr<LogEntry>& entry) const
{
    if (!entry) {
        return false;
    }

    if (!m_activeLogLevels.isEmpty() && !m_activeLogLevels.contains(entry->level)) {
        return false;
    }

    if (m_filterStartTime.isValid() && entry->timestamp < m_filterStartTime) {
        return false;
    }
    if (m_filterEndTime.isValid() && entry->timestamp > m_filterEndTime) {
        return false;
    }

    if (!m_messageFilterRules.isEmpty()) {
        for (const auto& rule : m_messageFilterRules) {
            if (entry->message.contains(rule.substring, rule.caseSensitivity)) {
                return true;
            }
        }
        return false;
    }

    return true;
}

void LogModel::rebuildFilteredEntries()
{
    m_filteredEntries.clear();
    m_filteredEntries.reserve(m_allEntries.size());

    for (const auto& entry : m_allEntries) {
        if (passesFilters(entry)) {
            m_filteredEntries.push_back(entry);
        }
    }
}

void LogModel::clearRowDependentCaches()
{
    // Эти кэши индексируются по row в текущем filtered view.
    // После любого reset соответствие row -> entry меняется полностью.
    m_expandedRows.clear();
    m_elideCache.clear();
    m_expandedRowHeightCache.clear();
}

void LogModel::applyFilter()
{
    beginResetModel();
    rebuildFilteredEntries();
    clearRowDependentCaches();
    endResetModel();
    emit modelFiltered(m_filteredEntries.size());
}

QColor LogModel::getColorForFile(const QString& filePath) const
{
    return m_fileColors.value(filePath, Qt::darkGray); // Return darkGray if not found
}

QColor LogModel::getLogLevelColor(LogLevel level) const
{
    return m_logLevelColors.value(level, DEFAULT_BACKGROUND_COLOR);
}

#include "logcolors.h"

QColor LogModel::defaultColorForLevel(LogLevel level) {
    return LogColors::forLevel(level);
}

// ---------------------------------------------------------------------------
// setVisibleFields — controls which structured fields appear in the display.
// ---------------------------------------------------------------------------

void LogModel::setVisibleFields(FieldVisibilityMask mask)
{
    if (m_visibleFields == mask)
        return;

    m_visibleFields = mask;
    clearElideCache(); // Elide cache stores text derived from the display message

    // Notify the view that display data has changed for every visible row.
    if (!m_filteredEntries.isEmpty()) {
        emit dataChanged(index(0, 0),
                         index(m_filteredEntries.size() - 1, 0),
                         {Qt::DisplayRole, DisplayMessageRole});
    }
}

void LogModel::refreshDisplay()
{
    clearElideCache();
    if (!m_filteredEntries.isEmpty()) {
        emit dataChanged(index(0, 0),
                         index(m_filteredEntries.size() - 1, 0),
                         {Qt::DisplayRole, DisplayMessageRole});
    }
}

// ---------------------------------------------------------------------------
// formatDisplayMessage — returns the text to show for a single entry row.
//
// Fast path: if all fields are visible (default) or the entry has no
// structured fields (continuation line or no pattern set), the raw
// entry.message is returned directly with no allocation.
//
// Slow path: builds a space-joined string from only the visible fields.
// Called for every painted row, so it must stay cheap.
// ---------------------------------------------------------------------------

QString LogModel::formatDisplayMessage(const LogEntry& entry) const
{
    // Fast path: all fields visible or no structured data extracted
    if (m_visibleFields == LogFieldAllMask || entry.fields.isEmpty())
        return entry.message;

    QStringList parts;
    parts.reserve(LogFieldCount);
    for (int i = 0; i < LogFieldCount; ++i) {
        if (!(m_visibleFields & (static_cast<FieldVisibilityMask>(1) << i)))
            continue;
        const QStringView sv = entry.fields.get(static_cast<LogField>(i), entry.message);
        if (!sv.isEmpty())
            parts << sv.toString();
    }

    // Fallback: if the mask matched nothing (e.g. fields not present in this
    // entry), show the full raw line so the row is never blank.
    return parts.isEmpty() ? entry.message : parts.join(QLatin1Char(' '));
}

QModelIndex LogModel::findNextOccurrence(const QString& text, int startRow, Qt::CaseSensitivity cs, bool wrapAround)
{
    if (text.isEmpty() || m_filteredEntries.isEmpty()) {
        return QModelIndex();
    }

    int currentRow = startRow + 1;
    int numRows = m_filteredEntries.size();

    // Search from startRow + 1 to the end
    for (int i = currentRow; i < numRows; ++i) {
        if (m_filteredEntries[i] && m_filteredEntries[i]->message.contains(text, cs)) {
            return index(i, 0);
        }
    }

    // If wrapAround is true and not found, search from the beginning to startRow
    if (wrapAround) {
        for (int i = 0; i <= startRow && i < numRows; ++i) {
            if (m_filteredEntries[i] && m_filteredEntries[i]->message.contains(text, cs)) {
                return index(i, 0);
            }
        }
    }
    return QModelIndex(); // Not found
}

QModelIndex LogModel::findPreviousOccurrence(const QString& text, int startRow, Qt::CaseSensitivity cs, bool wrapAround)
{
    if (text.isEmpty() || m_filteredEntries.isEmpty()) {
        return QModelIndex();
    }

    int numRows = m_filteredEntries.size();
    int currentRow = startRow - 1;
    if (startRow < 0 || startRow >= numRows) { // If startRow is out of bounds, start from the end
        currentRow = numRows - 1;
    }


    // Search from startRow - 1 to the beginning
    for (int i = currentRow; i >= 0; --i) {
        if (m_filteredEntries[i] && m_filteredEntries[i]->message.contains(text, cs)) {
            return index(i, 0);
        }
    }

    // If wrapAround is true and not found, search from the end to startRow
    if (wrapAround) {
        for (int i = numRows - 1; i >= startRow && i >= 0; --i) {
            if (m_filteredEntries[i] && m_filteredEntries[i]->message.contains(text, cs)) {
                return index(i, 0);
            }
        }
    }
    return QModelIndex(); // Not found
}
