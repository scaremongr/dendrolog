#include "logmodel.h"

#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>

LogModel::LogModel(QObject* parent)
    : QAbstractListModel(parent)
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

void LogModel::ensureFileColor(const QString& filePath)
{
    if (m_fileColors.contains(filePath))
        return;
    m_fileColors[filePath] = m_predefinedFileColors[m_nextColorIndex];
    m_nextColorIndex = (m_nextColorIndex + 1) % m_predefinedFileColors.size();
}

void LogModel::appendEntries(const QVector<std::shared_ptr<LogEntry>>& entries)
{
    if (entries.isEmpty())
        return;

    QVector<std::shared_ptr<LogEntry>> sortedBatch = entries;
    std::sort(sortedBatch.begin(), sortedBatch.end(), logEntryPtrLess);
    mergeSortedBatch(sortedBatch);

    // Подсветка «новых» строк переходит на этот батч: строки предыдущего
    // батча теряют зелёный маркер — перерисовываем гаттер.
    const bool hadPrevious = !m_newEntries.isEmpty();
    m_newEntries.clear();
    for (const auto& e : entries)
        if (e) m_newEntries.insert(e.get());
    if (hadPrevious && rowCount() > 0)
        emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {IsNewRole});
}

void LogModel::mergeEntries(const QVector<std::shared_ptr<LogEntry>>& sortedBatch)
{
    if (sortedBatch.isEmpty())
        return;
    mergeSortedBatch(sortedBatch);
}

void LogModel::mergeSortedBatch(const QVector<std::shared_ptr<LogEntry>>& sortedBatch)
{
    for (const auto& entry : sortedBatch) {
        if (entry && entry->sourceFile)
            ensureFileColor(entry->sourceFile->filePath);
    }
    m_cachedUniqueSourceFileCount = -1;

    // Полный список. Частый случай — батч целиком позже конца (загрузка одного
    // файла, tail-догрузка) — чистый append; иначе слияние отсортированного
    // хвоста с отсортированной головой на месте.
    const bool needMerge = !m_allEntries.isEmpty()
        && logEntryPtrLess(sortedBatch.first(), m_allEntries.last());
    const qsizetype oldAllCount = m_allEntries.size();
    m_allEntries.append(sortedBatch);
    if (needMerge) {
        std::inplace_merge(m_allEntries.begin(),
                           m_allEntries.begin() + oldAllCount,
                           m_allEntries.end(), logEntryPtrLess);
    }

    // Видимый список: прошедшие фильтр вставляются с уведомлениями view —
    // без reset, выделение и позиция скролла сохраняются.
    QVector<std::shared_ptr<LogEntry>> passing;
    passing.reserve(sortedBatch.size());
    for (const auto& e : sortedBatch) {
        if (passesFilters(e))
            passing.append(e);
    }

    if (!passing.isEmpty()) {
        if (m_filteredEntries.isEmpty()
            || !logEntryPtrLess(passing.first(), m_filteredEntries.last())) {
            const int first = m_filteredEntries.size();
            beginInsertRows(QModelIndex(), first, first + static_cast<int>(passing.size()) - 1);
            m_filteredEntries.append(passing);
            endInsertRows();
        } else {
            insertFilteredSorted(passing);
        }
        emit modelFiltered(m_filteredEntries.size());
    }

    // Фоновая перефильтрация (если шла) работала со снапшотом без этого
    // батча — перезапускаем её на актуальных данных.
    if (m_filterJobActive) {
        cancelPendingFilter(false);
        startFilterJob();
    }
}

void LogModel::insertFilteredSorted(const QVector<std::shared_ptr<LogEntry>>& passing)
{
    // Серии подряд идущих записей с общей точкой вставки; позиции считаются
    // относительно СТАРОГО списка. upper_bound даёт вставку ПОСЛЕ равных
    // существующих записей — та же стабильность, что у inplace_merge выше,
    // поэтому m_filteredEntries остаётся подпоследовательностью m_allEntries.
    struct Run { int pos; int first; int count; };
    QVector<Run> runs;
    int searchFrom = 0;
    for (int i = 0; i < passing.size(); ++i) {
        const auto it = std::upper_bound(m_filteredEntries.constBegin() + searchFrom,
                                         m_filteredEntries.constEnd(),
                                         passing[i], logEntryPtrLess);
        const int pos = static_cast<int>(it - m_filteredEntries.constBegin());
        if (!runs.isEmpty() && runs.last().pos == pos)
            ++runs.last().count;
        else
            runs.append({pos, i, 1});
        searchFrom = pos;
    }

    // Патологическое чередование строк двух файлов: сотни вставок в середину
    // дороже одной полной перестройки (и для модели, и для view).
    if (runs.size() > 64) {
        beginResetModel();
        rebuildFilteredEntries();
        endResetModel();
        return;
    }

    int shift = 0;
    for (const Run& run : runs) {
        const int first = run.pos + shift;
        beginInsertRows(QModelIndex(), first, first + run.count - 1);
        m_filteredEntries.insert(first, run.count, std::shared_ptr<LogEntry>());
        for (int k = 0; k < run.count; ++k)
            m_filteredEntries[first + k] = passing[run.first + k];
        endInsertRows();
        shift += run.count;
    }
}

void LogModel::removeEntriesForFile(const QString& filePath)
{
    if (m_allEntries.isEmpty())
        return;

    // Полная синхронная перестройка ниже даёт консистентный список сама —
    // фоновый джоб (если шёл) больше не нужен и не должен примениться.
    cancelPendingFilter(false);

    // Чистим до удаления записей, чтобы не держать висячие указатели.
    m_newEntries.clear();

    beginResetModel();
    m_allEntries.erase(
        std::remove_if(m_allEntries.begin(), m_allEntries.end(),
            [&filePath](const std::shared_ptr<LogEntry>& e) {
                return e && e->sourceFile && e->sourceFile->filePath == filePath;
            }),
        m_allEntries.end());

    m_cachedUniqueSourceFileCount = -1;
    rebuildFilteredEntries();
    endResetModel();
    m_filteredListStale = false; // перестроено по текущим настройкам
    emit modelFiltered(m_filteredEntries.size());
}

void LogModel::setEntries(const QVector<std::shared_ptr<LogEntry>>& entries)
{
    // Полная замена данных: фоновый джоб (если шёл) устарел, а старые
    // указатели в m_newEntries недействительны.
    cancelPendingFilter(false);
    m_newEntries.clear();

    beginResetModel();
    m_allEntries = entries;
    m_cachedUniqueSourceFileCount = -1;

    // Полное обновление: цвета файлов назначаются заново в порядке появления.
    m_fileColors.clear();
    m_nextColorIndex = 0;
    for (const auto& entry : m_allEntries) {
        if (entry && entry->sourceFile)
            ensureFileColor(entry->sourceFile->filePath);
    }

    rebuildFilteredEntries();
    endResetModel();
    m_filteredListStale = false; // перестроено по текущим настройкам
    emit modelFiltered(m_filteredEntries.size());
}

int LogModel::rowCount(const QModelIndex& parent) const
{
    // Плоская модель: у элементов нет детей.
    return parent.isValid() ? 0 : m_filteredEntries.size();
}

QVariant LogModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_filteredEntries.size())
        return QVariant();

    const std::shared_ptr<LogEntry>& entry = m_filteredEntries.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        return formatDisplayMessage(*entry);
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
    case IsNewRole:
        return m_newEntries.contains(entry.get());
    case RowMarkerColorRole: {
        if (m_rowMarkers.isEmpty())
            return QVariant();
        const QColor color = m_rowMarkers.firstMatchColor(entry->message);
        return color.isValid() ? QVariant(color) : QVariant();
    }
    default:
        return QVariant();
    }
}

int LogModel::uniqueSourceFileCount() const
{
    if (m_cachedUniqueSourceFileCount != -1) {
        return m_cachedUniqueSourceFileCount;
    }

    QSet<LogFilePtr> uniqueFiles;
    for (const auto& entry : m_allEntries) {
        if (entry && entry->sourceFile) {
            uniqueFiles.insert(entry->sourceFile);
        }
    }
    m_cachedUniqueSourceFileCount = uniqueFiles.size();
    return m_cachedUniqueSourceFileCount;
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

void LogModel::setFilterRules(const FilterRuleSet& rules)
{
    m_filterRules = rules;
    applyFilter();
}

void LogModel::setRowMarkers(const QVector<HighlightPattern>& markers)
{
    m_rowMarkers.setPatterns(markers);
    // Маркеры недеструктивны: состав строк не меняется, нужна только
    // перерисовка фона видимых строк.
    if (!m_filteredEntries.isEmpty()) {
        emit dataChanged(index(0, 0),
                         index(m_filteredEntries.size() - 1, 0),
                         {RowMarkerColorRole});
    }
}

// Общая проверка фильтров. Вынесена в свободную функцию, потому что фоновый
// воркер работает со СНАПШОТАМИ настроек и не имеет права читать члены модели.
static bool entryPassesFilters(const std::shared_ptr<LogEntry>& entry,
                               const QSet<LogLevel>& levels,
                               const QDateTime& startTime,
                               const QDateTime& endTime,
                               const FilterRuleSet& rules)
{
    if (!entry) {
        return false;
    }

    if (!levels.isEmpty() && !levels.contains(entry->level)) {
        return false;
    }

    if (startTime.isValid() && entry->timestamp < startTime) {
        return false;
    }
    if (endTime.isValid() && entry->timestamp > endTime) {
        return false;
    }

    if (rules.isActive() && !rules.matches(*entry)) {
        return false;
    }

    return true;
}

bool LogModel::passesFilters(const std::shared_ptr<LogEntry>& entry) const
{
    return entryPassesFilters(entry, m_activeLogLevels,
                              m_filterStartTime, m_filterEndTime, m_filterRules);
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

void LogModel::applyFilter()
{
    cancelPendingFilter(false);

    // Небольшие данные фильтруем синхронно: мгновенно и без промежуточного
    // состояния. Большие — в пуле потоков, чтобы не замораживать GUI.
    if (m_allEntries.size() < kAsyncFilterThreshold) {
        beginResetModel();
        rebuildFilteredEntries();
        endResetModel();
        m_filteredListStale = false;
        emit modelFiltered(m_filteredEntries.size());
        return;
    }
    startFilterJob();
}

void LogModel::reapplyFilterIfStale()
{
    if (m_filteredListStale)
        applyFilter();
}

void LogModel::startFilterJob()
{
    using FilterResult = QVector<std::shared_ptr<LogEntry>>;

    m_filterJobActive = true;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_filterJobCancel = cancel;

    // Снапшоты: COW-копия вектора shared_ptr держит записи живыми на время
    // работы воркера, копии настроек отвязывают его от изменений модели.
    const QVector<std::shared_ptr<LogEntry>> entries = m_allEntries;
    const QSet<LogLevel> levels = m_activeLogLevels;
    const QDateTime startTime = m_filterStartTime;
    const QDateTime endTime = m_filterEndTime;
    const FilterRuleSet rules = m_filterRules;

    QFuture<FilterResult> future = QtConcurrent::run(
        [entries, levels, startTime, endTime, rules, cancel]() -> FilterResult {
            FilterResult passing;
            passing.reserve(entries.size());
            for (qsizetype i = 0; i < entries.size(); ++i) {
                // Проверка отмены раз в 8192 записи: почти бесплатно, а
                // ненужный воркер обрывается за миллисекунды.
                if ((i & 0x1FFF) == 0 && cancel->load(std::memory_order_relaxed))
                    return FilterResult();
                if (entryPassesFilters(entries[i], levels, startTime, endTime, rules))
                    passing.append(entries[i]);
            }
            return passing;
        });
    m_filterJobFuture = future;

    // Поколение фиксируется на момент запуска: результат применяется, только
    // если к его приходу не изменились ни настройки фильтра, ни данные.
    auto* watcher = new QFutureWatcher<FilterResult>(this);
    const int generation = m_filterGeneration;
    connect(watcher, &QFutureWatcher<FilterResult>::finished, this,
            [this, watcher, generation]() {
                watcher->deleteLater();
                if (generation != m_filterGeneration)
                    return; // результат устарел (отменён или заменён новым джобом)
                m_filterJobActive = false;
                beginResetModel();
                m_filteredEntries = watcher->result();
                endResetModel();
                m_filteredListStale = false;
                emit modelFiltered(m_filteredEntries.size());
            });
    watcher->setFuture(future);
}

void LogModel::cancelPendingFilter(bool wait)
{
    // Смена поколения гарантирует, что уже посчитанный (в т.ч. пустой из-за
    // отмены) результат в полёте не будет применён.
    ++m_filterGeneration;
    // Джоб в полёте означает, что видимый список ещё не догнал настройки —
    // после отмены это надо будет довести (reapplyFilterIfStale / applyFilter).
    if (m_filterJobActive)
        m_filteredListStale = true;
    m_filterJobActive = false;
    if (m_filterJobCancel) {
        m_filterJobCancel->store(true);
        m_filterJobCancel.reset();
    }
    if (wait)
        m_filterJobFuture.waitForFinished();
}

int LogModel::rowForEntry(int logicalEntryId, const LogFile* sourceFile) const
{
    for (int i = 0; i < m_filteredEntries.size(); ++i) {
        const auto& e = m_filteredEntries[i];
        if (e->logicalEntryId == logicalEntryId && e->sourceFile.get() == sourceFile) {
            return i;
        }
    }
    return -1;
}

int LogModel::nearestVisibleRow(int logicalEntryId, const LogFile* sourceFile) const
{
    if (m_filteredEntries.isEmpty()) {
        return -1;
    }

    // m_filteredEntries — упорядоченная подпоследовательность m_allEntries,
    // поэтому идём по обоим спискам одним проходом: j — количество видимых
    // записей, расположенных до текущей позиции i в полном списке.
    int j = 0;
    for (int i = 0; i < m_allEntries.size(); ++i) {
        const auto& e = m_allEntries[i];
        const bool visible = (j < m_filteredEntries.size()
                              && m_filteredEntries[j].get() == e.get());
        if (e->logicalEntryId == logicalEntryId && e->sourceFile.get() == sourceFile) {
            if (visible) {
                return j;
            }
            // Запись скрыта: ближайшая видимая — первая после неё (j),
            // если такой нет — последняя перед ней (j - 1).
            return (j < m_filteredEntries.size()) ? j : j - 1;
        }
        if (visible) {
            ++j;
        }
    }
    return -1;
}

QColor LogModel::getColorForFile(const QString& filePath) const
{
    return m_fileColors.value(filePath, Qt::darkGray);
}

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
    if (!m_filteredEntries.isEmpty()) {
        emit dataChanged(index(0, 0),
                         index(m_filteredEntries.size() - 1, 0),
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
// Called for every painted row, so it keeps allocations minimal.
// ---------------------------------------------------------------------------

QString LogModel::formatDisplayMessage(const LogEntry& entry) const
{
    // Lines that did not match the schema at all (continuation lines, junk)
    // have no fields — show their raw text so nothing silently disappears.
    if (!m_fieldFilterEnabled || entry.fields.isEmpty())
        return entry.message;

    QString result;
    bool first = true;
    for (const int fieldIndex : m_visibleFieldIndexes) {
        const QStringView value = entry.fields.get(fieldIndex, entry.message);
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
    // blank on purpose. Falling back to the full raw message here would make
    // a partially-parsed line look as though it was not parsed at all.
    return result;
}

QModelIndex LogModel::findNextOccurrence(const QString& text, int startRow, Qt::CaseSensitivity cs, bool wrapAround) const
{
    if (text.isEmpty() || m_filteredEntries.isEmpty()) {
        return QModelIndex();
    }

    const int numRows = m_filteredEntries.size();
    const int firstRow = qBound(0, startRow + 1, numRows);

    // Search from startRow + 1 to the end
    for (int i = firstRow; i < numRows; ++i) {
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

QModelIndex LogModel::findPreviousOccurrence(const QString& text, int startRow, Qt::CaseSensitivity cs, bool wrapAround) const
{
    if (text.isEmpty() || m_filteredEntries.isEmpty()) {
        return QModelIndex();
    }

    const int numRows = m_filteredEntries.size();
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
