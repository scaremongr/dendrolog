#include "logmodel.h"
#include "indexedlogstore.h"
#include "residentlogstore.h"

#include <QDebug>

// ============================================================================
// LogModel — тонкий фасад: роли/сигналы/настройки здесь, данные и фильтрация —
// в LogStore (см. logstore.h). Поведение резидентного пути идентично
// историческому LogModel до выделения хранилища.
// ============================================================================

LogModel::LogModel(QObject* parent)
    : QAbstractListModel(parent)
    , m_store(std::make_unique<ResidentLogStore>(*this))
    , m_nextColorIndex(0)
    // m_filterStartTime / m_filterEndTime default-constructed невалидными —
    // фильтр по времени выключен. Пустой m_activeLogLevels = «показывать все».
{
    // Палитра цветов для плашек файлов (при загрузке нескольких файлов в таб).
    m_predefinedFileColors << QColor(Qt::cyan).darker(220)
                           << QColor(Qt::magenta).darker(220)
                           << QColor(Qt::yellow).darker(250)
                           << QColor(Qt::green).darker(220)
                           << QColor(Qt::blue).darker(220)
                           << QColor(Qt::darkCyan).darker(220)
                           << QColor(Qt::darkMagenta).darker(220)
                           << QColor(Qt::darkYellow).darker(220)
                           << QColor(Qt::darkGreen).darker(220)
                           << QColor(Qt::darkBlue).darker(220)
                           << QColor(Qt::darkRed).darker(220);
}

LogModel::~LogModel()
{
    // Обрываем фоновый фильтр-джоб. Ждать не нужно: воркер владеет своими
    // снапшотами и не трогает модель, а watcher (наш child) умрёт вместе с нами.
    cancelPendingFilter(false);
}

ResidentLogStore* LogModel::resident() const
{
    Q_ASSERT(m_store->backend() == LogStore::Backend::Resident);
    return static_cast<ResidentLogStore*>(m_store.get());
}

ResidentLogStore* LogModel::residentOrNull() const
{
    return m_store->backend() == LogStore::Backend::Resident
        ? static_cast<ResidentLogStore*>(m_store.get())
        : nullptr;
}

// ---------------------------------------------------------------------------
// Мутации (резидентный путь LogParser). После конвертации вкладки в индексный
// бэкенд от резидентного парсера могут долететь хвостовые батчи — молча
// отбрасываем: файл целиком перечитывает индексатор.
// ---------------------------------------------------------------------------

void LogModel::setEntries(const QVector<std::shared_ptr<LogEntry>>& entries)
{
    if (auto* r = residentOrNull())
        r->setEntries(entries);
}

void LogModel::appendEntries(const QVector<std::shared_ptr<LogEntry>>& entries)
{
    if (auto* r = residentOrNull())
        r->appendEntries(entries);
}

void LogModel::mergeEntries(const QVector<std::shared_ptr<LogEntry>>& sortedBatch)
{
    if (auto* r = residentOrNull())
        r->mergeEntries(sortedBatch);
}

void LogModel::removeEntriesForFile(const QString& filePath)
{
    if (auto* r = residentOrNull())
        r->removeEntriesForFile(filePath);
}

// ---------------------------------------------------------------------------
// Цвета файлов (презентация)
// ---------------------------------------------------------------------------

void LogModel::ensureFileColor(const QString& filePath)
{
    if (m_fileColors.contains(filePath))
        return;
    m_fileColors[filePath] = m_predefinedFileColors[m_nextColorIndex];
    m_nextColorIndex = (m_nextColorIndex + 1) % m_predefinedFileColors.size();
}

void LogModel::resetFileColors()
{
    m_fileColors.clear();
    m_nextColorIndex = 0;
}

QColor LogModel::getColorForFile(const QString& filePath) const
{
    return m_fileColors.value(filePath, Qt::darkGray);
}

// ---------------------------------------------------------------------------
// QAbstractListModel
// ---------------------------------------------------------------------------

int LogModel::rowCount(const QModelIndex& parent) const
{
    // Плоская модель: у элементов нет детей.
    return parent.isValid() ? 0 : m_store->visibleCount();
}

QVariant LogModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_store->visibleCount())
        return QVariant();

    switch (role) {
    case Qt::DisplayRole: {
        const auto entry = m_store->entryAt(index.row());
        return entry ? QVariant(formatDisplayMessage(*entry)) : QVariant();
    }
    case FileBadgeRole: {
        // Плашка показывается только если загружено несколько файлов.
        if (m_store->uniqueSourceFileCount() <= 1)
            return QVariant();
        const LogEntryMeta meta = m_store->visibleMetaAt(index.row());
        if (!meta.sourceFile)
            return QVariant();
        QVariantMap badge;
        badge["text"]  = meta.sourceFile->shortName();
        badge["color"] = getColorForFile(meta.sourceFile->filePath);
        return badge;
    }
    case IsNewRole:
        return m_store->isNewAt(index.row());
    case RowMarkerColorRole: {
        if (m_rowMarkers.isEmpty())
            return QVariant();
        const QColor color = m_rowMarkers.firstMatchColor(m_store->messageAt(index.row()));
        return color.isValid() ? QVariant(color) : QVariant();
    }
    default:
        return QVariant();
    }
}

// ---------------------------------------------------------------------------
// Шов доступа к данным — делегирование хранилищу
// ---------------------------------------------------------------------------

std::shared_ptr<LogEntry> LogModel::entryAt(int visibleRow) const
{
    return m_store->entryAt(visibleRow);
}

LogModel::EntryKey LogModel::keyForRow(int visibleRow) const
{
    EntryKey key;
    if (visibleRow < 0 || visibleRow >= m_store->visibleCount())
        return key;
    const LogEntryMeta meta = m_store->visibleMetaAt(visibleRow);
    if (!meta.sourceFile)
        return key;
    key.logicalEntryId = meta.logicalEntryId;
    key.sourceFile = meta.sourceFile;
    return key;
}

QDateTime LogModel::visibleTimestampAt(int row) const
{
    return m_store->visibleTimestampAt(row);
}

LogLevel LogModel::visibleLevelAt(int row) const
{
    return m_store->visibleLevelAt(row);
}

int LogModel::firstVisibleRowAtOrAfter(const QDateTime& t) const
{
    return m_store->firstVisibleRowAtOrAfter(t);
}

QPair<QDateTime, QDateTime> LogModel::fullTimeRange() const
{
    return m_store->fullTimeRange();
}

QStringList LogModel::sampleMessages(int maxCount) const
{
    return m_store->sampleMessages(maxCount);
}

QString LogModel::messageAt(int visibleRow) const
{
    return m_store->messageAt(visibleRow);
}

QVector<std::shared_ptr<LogEntry>> LogModel::logicalRecordLines(
    const std::shared_ptr<LogEntry>& line, int maxLines) const
{
    return m_store->logicalRecordLines(line, maxLines);
}

void LogModel::seedFromVisible(const LogModel& source)
{
    // Модель-приёмник (панель результатов поиска) всегда резидентная.
    if (auto* r = source.residentOrNull()) {
        resident()->setEntries(r->visibleEntries());
        return;
    }

    // Индексный источник: материализуем видимые строки с капом — панель
    // результатов рассчитана на разумные выборки, а не на копию гигантского
    // лога в памяти.
    constexpr int kSeedCap = 200000;
    const int n = qMin(source.rowCount(), kSeedCap);
    if (n < source.rowCount())
        qWarning() << "seedFromVisible: indexed source truncated to" << n
                   << "of" << source.rowCount() << "rows";
    QVector<std::shared_ptr<LogEntry>> entries;
    entries.reserve(n);
    for (int i = 0; i < n; ++i) {
        if (auto e = source.entryAt(i))
            entries.append(std::move(e));
    }
    resident()->setEntries(entries);
}

LogScanSnapshot LogModel::scanSnapshot(bool filteredOnly) const
{
    return m_store->scanSnapshot(filteredOnly);
}

QVector<std::shared_ptr<LogEntry>> LogModel::residentEntriesForFieldMutation() const
{
    return resident()->allEntries();
}

// ---------------------------------------------------------------------------
// Бэкенды
// ---------------------------------------------------------------------------

bool LogModel::isIndexedBackend() const
{
    return m_store->backend() == LogStore::Backend::Indexed;
}

IndexedLogStore* LogModel::indexedOrNull() const
{
    return isIndexedBackend() ? static_cast<IndexedLogStore*>(m_store.get())
                              : nullptr;
}

IndexedLogStore* LogModel::convertToIndexedBackend()
{
    if (auto* existing = indexedOrNull())
        return existing;

    // Старое хранилище гасится под reset: его данные вкладка перезагрузит
    // через LogIndexer. Отложенные watcher'ы старого store нейтрализует его
    // alive-guard.
    m_store->cancelPendingFilter(false);
    beginResetModel();
    auto indexed = std::make_unique<IndexedLogStore>(*this);
    IndexedLogStore* raw = indexed.get();
    m_store = std::move(indexed);
    endResetModel();
    emit modelFiltered(0);
    return raw;
}

void LogModel::convertToResidentBackend()
{
    if (!isIndexedBackend())
        return;
    m_store->cancelPendingFilter(false);
    beginResetModel();
    m_store = std::make_unique<ResidentLogStore>(*this);
    endResetModel();
    emit modelFiltered(0);
}

int LogModel::rowForEntry(int logicalEntryId, const LogFile* sourceFile) const
{
    return m_store->rowForEntry(logicalEntryId, sourceFile);
}

int LogModel::nearestVisibleRow(int logicalEntryId, const LogFile* sourceFile) const
{
    return m_store->nearestVisibleRow(logicalEntryId, sourceFile);
}

QModelIndex LogModel::findNextOccurrence(const QString& text, int startRow,
                                         Qt::CaseSensitivity cs, bool wrapAround) const
{
    const int row = m_store->findNextOccurrence(text, startRow, cs, wrapAround);
    return row >= 0 ? index(row, 0) : QModelIndex();
}

QModelIndex LogModel::findPreviousOccurrence(const QString& text, int startRow,
                                             Qt::CaseSensitivity cs, bool wrapAround) const
{
    const int row = m_store->findPreviousOccurrence(text, startRow, cs, wrapAround);
    return row >= 0 ? index(row, 0) : QModelIndex();
}

// ---------------------------------------------------------------------------
// Фильтры (настройки — здесь, исполнение — в хранилище)
// ---------------------------------------------------------------------------

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

void LogModel::setFilterRules(const FilterRuleSet& rules)
{
    m_filterRules = rules;
    applyFilter();
}

void LogModel::applyFilter()
{
    m_store->applyFilter();
}

void LogModel::cancelPendingFilter(bool wait)
{
    m_store->cancelPendingFilter(wait);
}

void LogModel::reapplyFilterIfStale()
{
    m_store->reapplyFilterIfStale();
}

void LogModel::setRowMarkers(const QVector<HighlightPattern>& markers)
{
    m_rowMarkers.setPatterns(markers);
    // Маркеры недеструктивны: состав строк не меняется, нужна только
    // перерисовка фона видимых строк.
    if (m_store->visibleCount() > 0) {
        emit dataChanged(index(0, 0),
                         index(m_store->visibleCount() - 1, 0),
                         {RowMarkerColorRole});
    }
}

// ---------------------------------------------------------------------------
// Отображение полей
// ---------------------------------------------------------------------------

QVector<int> LogModel::sanitizeFieldIndexes(const QVector<int>& indexes) const
{
    QVector<int> sanitized;
    sanitized.reserve(indexes.size());
    for (const int fieldIndex : indexes) {
        if (fieldIndex >= 0 && fieldIndex < m_availableFieldNames.size() && !sanitized.contains(fieldIndex))
            sanitized.push_back(fieldIndex);
    }
    return sanitized;
}

void LogModel::notifyDisplayChanged()
{
    if (m_store->visibleCount() > 0) {
        emit dataChanged(index(0, 0),
                         index(m_store->visibleCount() - 1, 0),
                         {Qt::DisplayRole});
    }
}

void LogModel::setAvailableFields(const QStringList& fieldNames)
{
    if (m_availableFieldNames == fieldNames)
        return;

    m_availableFieldNames = fieldNames;
    m_visibleFieldIndexes = sanitizeFieldIndexes(m_visibleFieldIndexes);
    notifyDisplayChanged();
}

void LogModel::setFieldDisplaySelection(bool enabled, const QVector<int>& visibleIndexes)
{
    QVector<int> sanitized = sanitizeFieldIndexes(visibleIndexes);
    if (m_fieldFilterEnabled == enabled && m_visibleFieldIndexes == sanitized)
        return;

    m_fieldFilterEnabled = enabled;
    m_visibleFieldIndexes = std::move(sanitized);
    notifyDisplayChanged();
}

void LogModel::refreshDisplay()
{
    notifyDisplayChanged();
}

// ---------------------------------------------------------------------------
// formatDisplayMessage — returns the text to show for a single entry row.
//
// Fast path: when field filtering is disabled or the entry has no extracted
// fields, return the original line with no allocation.
//
// Filtered path: concatenate only the selected block values in schema order.
// ---------------------------------------------------------------------------

QString LogModel::formatDisplayMessage(const LogEntry& entry) const
{
    // Lines that did not match the schema at all (continuation lines, junk)
    // have no fields — show their raw text so nothing silently disappears.
    if (!m_fieldFilterEnabled || entry.fields().isEmpty())
        return entry.message();

    QString result;
    bool first = true;
    for (const int fieldIndex : m_visibleFieldIndexes) {
        const QStringView value = entry.fields().get(fieldIndex, entry.message());
        if (value.isEmpty())
            continue;

        if (!first)
            result += QLatin1Char(' ');
        result += value;
        first = false;
    }

    // The line *did* match (some field is present), so show exactly the
    // selected fields. If none of them exist on this line — e.g. a line that
    // only filled the first few blocks of a longer schema — the row is left
    // blank on purpose.
    return result;
}

// Длина результата formatDisplayMessage() без конкатенации строк.
// Для резидентного бэкенда даёт ровно ту же длину, что построенный
// display-текст; для индексного — ОЦЕНКУ (байтовая длина сырой строки):
// точная длина потребовала бы чтения с диска и извлечения полей на каждую
// строку в горячих путях view, а оценки достаточно — высоты уточняются
// лениво при отрисовке.
int LogModel::displayTextLength(int row) const
{
    if (row < 0 || row >= m_store->visibleCount())
        return 0;

    if (!m_fieldFilterEnabled || m_store->backend() == LogStore::Backend::Indexed)
        return m_store->rawTextLengthAt(row);

    const auto entryPtr = m_store->entryAt(row);
    if (!entryPtr)
        return 0;
    const LogEntry& entry = *entryPtr;

    if (entry.fields().isEmpty())
        return int(entry.message().length());

    int length = 0;
    bool first = true;
    for (const int fieldIndex : m_visibleFieldIndexes) {
        const QStringView value = entry.fields().get(fieldIndex, entry.message());
        if (value.isEmpty())
            continue;
        length += (first ? 0 : 1) + static_cast<int>(value.length());
        first = false;
    }
    return length;
}
