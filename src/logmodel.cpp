#include "logmodel.h"
#include "filelabels.h"
#include "bookmarkstore.h"
#include "navhistory.h"
#include "recordlocator.h"
#include "indexedlogstore.h"
#include "residentlogstore.h"

#include <QDebug>

#include <algorithm>

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
    // Локатор — раньше закладок: они подписываются на его ответы.
    m_locator = new RecordLocator(*this);
    m_bookmarks = new BookmarkStore(*this);
    m_fieldSource = new FieldSource(this);
    connect(m_fieldSource, &FieldSource::fieldsReady, this, &LogModel::onFieldsReady);
    // Метки и число строк файлов (U-23) — на события модели, O(числа файлов).
    // Подписка раньше любого view: на rowsInserted view уже видит набор.
    connect(this, &QAbstractItemModel::rowsInserted, this, &LogModel::refreshFileSet);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &LogModel::refreshFileSet);
    connect(this, &QAbstractItemModel::modelReset, this, &LogModel::refreshFileSet);
    connect(this, &LogModel::modelFiltered, this, &LogModel::refreshFileSet);
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
    m_locator->allReset();
    m_fieldSource->allReset();
    m_bookmarks->invalidateAll();
    if (m_navHistory)
        m_navHistory->invalidateAll();
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
    if (auto* r = residentOrNull()) {
        r->mergeEntries(sortedBatch);
        // Порция, все строки которой скрыл фильтр, не даёт ни rowsInserted,
        // ни modelFiltered — число строк файла (легенда) обновляется здесь.
        // O(числа файлов).
        refreshFileSet();
    }
}

NavHistory& LogModel::navHistory() const
{
    // Лениво: у модели выдачи Search переходов нет, история ей не нужна.
    if (!m_navHistory)
        m_navHistory = new NavHistory(*const_cast<LogModel*>(this));
    return *m_navHistory;
}

void LogModel::invalidateFileRows(const QString& filePath)
{
    m_locator->fileReset(filePath);
    m_fieldSource->fileReset(filePath);
    m_bookmarks->invalidateFile(filePath);
    if (m_navHistory)
        m_navHistory->invalidateFile(filePath);
}

void LogModel::removeEntriesForFile(const QString& filePath)
{
    invalidateFileRows(filePath);
    if (auto* r = residentOrNull())
        r->removeEntriesForFile(filePath);
}

void LogModel::replaceEntriesForFile(const QString& filePath,
                                     const QVector<std::shared_ptr<LogEntry>>& sortedEntries)
{
    if (auto* r = residentOrNull())
        r->replaceEntriesForFile(filePath, sortedEntries);
}

void LogModel::removeEntry(const std::shared_ptr<LogEntry>& entry)
{
    if (auto* r = residentOrNull())
        r->removeEntry(entry);
}

// ---------------------------------------------------------------------------
// Цвета файлов (презентация)
// ---------------------------------------------------------------------------

void LogModel::ensureFileColor(const QString& filePath)
{
    if (m_fileOrdinals.contains(filePath))
        return;
    m_fileOrdinals.insert(filePath, m_nextColorIndex++);
}

void LogModel::resetFileColors()
{
    m_fileOrdinals.clear();
    m_nextColorIndex = 0;
}

void LogModel::setFileLabelMaxChars(int maxChars)
{
    if (maxChars == m_fileLabelMaxChars)
        return;
    m_fileLabelMaxChars = maxChars;
    m_labeledFiles.clear();   // метки пересчитать
    refreshFileSet();
}

const LogModel::LabeledFile* LogModel::labeledFile(const LogFile* file) const
{
    if (m_labelSource)
        return m_labelSource->labeledFile(file);
    const auto it = m_labeledIndex.constFind(file);
    return it == m_labeledIndex.constEnd() ? nullptr : &m_labeledFiles[it.value()];
}

void LogModel::setFileLabelSource(LogModel* source)
{
    if (source == this)
        source = nullptr;
    if (source == m_labelSource)
        return;
    for (const QMetaObject::Connection& c : std::as_const(m_labelSourceConnections))
        disconnect(c);
    m_labelSourceConnections.clear();
    m_labelSource = source;
    if (source) {
        m_labelSourceConnections << connect(source, &LogModel::fileSetChanged, this,
                                            &LogModel::fileSetChanged);
        m_labelSourceConnections << connect(source, &LogModel::fileLinesChanged, this,
                                            &LogModel::fileLinesChanged);
        // Источник ушёл — свои метки (QPointer уже пуст).
        m_labelSourceConnections << connect(source, &QObject::destroyed, this,
                                            &LogModel::fileSetChanged);
    }
    emit fileSetChanged();
    emit fileLinesChanged();
}

int LogModel::findRowOfFile(const LogFile* file, int from, bool forward) const
{
    const int n = m_store->visibleCount();
    if (!file || n == 0)
        return -1;
    if (from < 0 || from >= n)
        from = forward ? -1 : n;
    return int(scanSnapshot(/*filteredOnly=*/true).findRowOfFile(file, from, forward,
                                                                 /*wrapAround=*/true));
}

void LogModel::refreshFileSet()
{
    QVector<LogStore::FileLines> files = m_store->fileLineCounts();
    // Файл в наборе, пока подключён к вкладке (у резидентной — с первой
    // порции), даже без строк: полная перезагрузка сбрасывает его строки, и
    // колонка с легендой не должны мигать на каждую ротацию.
    files.erase(std::remove_if(files.begin(), files.end(),
                               [](const LogStore::FileLines& f) { return !f.file; }),
                files.end());
    for (const LogStore::FileLines& f : std::as_const(files))
        ensureFileColor(f.file->filePath);
    std::stable_sort(files.begin(), files.end(),
                     [this](const LogStore::FileLines& a, const LogStore::FileLines& b) {
                         return m_fileOrdinals.value(a.file->filePath) < m_fileOrdinals.value(b.file->filePath);
                     });
    bool sameSet = files.size() == m_labeledFiles.size();
    for (int i = 0; sameSet && i < files.size(); ++i)
        sameSet = files[i].file == m_labeledFiles[i].file
            && m_fileOrdinals.value(files[i].file->filePath) == m_labeledFiles[i].colorOrdinal;
    if (sameSet) {
        bool linesChanged = false;
        for (int i = 0; i < files.size(); ++i) {
            if (m_labeledFiles[i].lines != files[i].lines || m_labeledFiles[i].maxLine != files[i].maxLine) {
                m_labeledFiles[i].lines = files[i].lines;
                m_labeledFiles[i].maxLine = files[i].maxLine;
                linesChanged = true;
            }
        }
        if (linesChanged && !m_labelSource)
            emit fileLinesChanged();
        return;
    }
    QStringList names;
    for (const LogStore::FileLines& f : std::as_const(files))
        names << f.file->shortName();
    const QStringList labels = FileLabels::build(names, m_fileLabelMaxChars);
    m_labeledFiles.clear();
    m_labeledIndex.clear();
    for (int i = 0; i < files.size(); ++i) {
        m_labeledFiles.append({files[i].file, labels.value(i),
                               m_fileOrdinals.value(files[i].file->filePath), files[i].lines,
                               files[i].maxLine});
        m_labeledIndex.insert(files[i].file.get(), i);
    }
    // Метки у источника (выдача Search): view читает его набор, свой
    // объявлять незачем — источник сообщает о своём сам.
    if (m_labelSource)
        return;
    emit fileSetChanged();
    emit fileLinesChanged();
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
    case SourceFileRole:
        return QVariant::fromValue(quintptr(m_store->visibleMetaAt(index.row()).sourceFile));
    case IsNewRole:
        return m_store->isNewAt(index.row());
    case RowMarkerColorRole: {
        if (m_rowMarkers.isEmpty())
            return QVariant();
        const QColor color = m_rowMarkers.firstMatchColor(m_store->messageAt(index.row()));
        return color.isValid() ? QVariant(color) : QVariant();
    }
    case BookmarkRole:
        return m_bookmarks->isBookmarkedRow(index.row());
    case AnsiStyleRole: {
        AnsiStyles styles = m_store->ansiStylesAt(index.row());
        if (styles.isEmpty())
            return QVariant();
        // Показан не сырой текст строки (колонки полей) — позиции не те.
        if (m_fieldFilterEnabled) {
            const auto entry = m_store->entryAt(index.row());
            if (!entry || formatDisplayMessage(*entry) != entry->message())
                return QVariant();
        }
        return QVariant::fromValue(std::move(styles));
    }
    default:
        return QVariant();
    }
}

void LogModel::notifyBookmarkRow(int row)
{
    if (row < 0 || row >= rowCount())
        return;
    const QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx, { int(BookmarkRole) });
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

LogEntryMeta LogModel::visibleMetaAt(int row) const
{
    return m_store->visibleMetaAt(row);
}

bool LogModel::setTimeZero(int row)
{
    if (row < 0 || row >= rowCount())
        return false;
    const RecordKey key = m_store->keyAt(row);
    const LogEntryMeta meta = m_store->visibleMetaAt(row);
    if (!key.isValid() || !hasTimestampMs(meta.timestampMs))
        return false;
    TimeZero zero;
    zero.ms = meta.timestampMs;
    zero.filePath = key.file->orderKey();
    zero.line = key.line;
    if (zero.ms == m_timeZero.ms && zero.line == m_timeZero.line
        && zero.filePath == m_timeZero.filePath)
        return true;
    m_timeZero = zero;
    emit timeZeroChanged();
    return true;
}

void LogModel::clearTimeZero()
{
    if (!m_timeZero.isSet())
        return;
    m_timeZero = TimeZero{};
    emit timeZeroChanged();
}

bool LogModel::toggleTimeZero(int row)
{
    if (isTimeZeroRow(row)) {
        clearTimeZero();
        return true;
    }
    return setTimeZero(row);
}

bool LogModel::isTimeZeroRow(int row) const
{
    if (!m_timeZero.isSet() || row < 0 || row >= rowCount())
        return false;
    // Момент тоже: в переписанном файле под тем же номером — другая строка,
    // и «= 0» на ней сбивал бы с толку (дельты считаются от прежнего момента).
    const RecordKey key = m_store->keyAt(row);
    return key.isValid() && key.line == m_timeZero.line
        && key.timestampMs == m_timeZero.ms
        && key.file->orderKey() == m_timeZero.filePath;
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

qint64 LogModel::totalLineCount() const
{
    return m_store->allCount();
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

void LogModel::searchVisible(const LogModel& source, const FilterRuleSet& rules)
{
    Q_ASSERT(this != &source);
    if (this == &source)
        return;
    if (rules.usableRuleCount() == 0) {
        clear();
        return;
    }

    // Resident workers share entries with the source. Finish cancellation
    // before dropping the old request.
    cancelPendingFilter(!isIndexedBackend());
    beginResetModel();
    // Схемы файлов — как у источника (поля — тем же путём, FieldSource).
    m_fileSchemas = source.m_fileSchemas;
    m_fieldSource->setSchemas(m_fileSchemas);
    if (auto* indexed = source.indexedOrNull())
        m_store = std::make_unique<IndexedLogStore>(*this, *indexed);
    else
        m_store = std::make_unique<ResidentLogStore>(*this, source.resident()->visibleEntries());
    m_filterRules = rules;
    m_activeLogLevels.clear();
    m_filterStartTime = {};
    m_filterEndTime = {};
    m_availableFieldNames = source.m_availableFieldNames;
    m_visibleFieldIndexes = source.m_visibleFieldIndexes;
    m_fieldFilterEnabled = source.m_fieldFilterEnabled;
    m_fileOrdinals = source.m_fileOrdinals;
    m_nextColorIndex = source.m_nextColorIndex;
    m_fileLabelMaxChars = source.m_fileLabelMaxChars;
    endResetModel();
    applyFilter();
}

void LogModel::clear()
{
    cancelPendingFilter(!isIndexedBackend());
    m_locator->allReset();
    m_fieldSource->allReset();
    m_bookmarks->invalidateAll();
    if (m_navHistory)
        m_navHistory->invalidateAll();
    beginResetModel();
    m_store = std::make_unique<ResidentLogStore>(*this);
    m_filterRules = {};
    m_activeLogLevels.clear();
    m_hiddenFiles.clear();
    ++m_hiddenFilesGeneration;
    m_filterStartTime = {};
    m_filterEndTime = {};
    resetFileColors();
    endResetModel();
    emit modelFiltered(0);
}

bool LogModel::appendSearchRows(const LogModel& source, int first, int last)
{
    // Без запроса пустой набор правил пропустил бы в результаты всё подряд.
    if (this == &source || !m_filterRules.isActive())
        return false;
    IndexedLogStore* indexed = indexedOrNull();
    IndexedLogStore* sourceIndexed = source.indexedOrNull();
    // Схемы файлов — как у источника.
    if (!(m_fileSchemas == source.m_fileSchemas)) {
        m_fileSchemas = source.m_fileSchemas;
        m_fieldSource->setSchemas(m_fileSchemas);
    }
    if (indexed && sourceIndexed)
        return indexed->appendSearchRows(*sourceIndexed, first, last);
    if (!indexed && !sourceIndexed)
        return resident()->appendSearchRows(*source.resident(), first, last);
    return false; // источник сменил бэкенд
}

bool LogModel::refreshSearchRow(const LogModel& source, int row)
{
    if (this == &source || !m_filterRules.isActive())
        return false;
    IndexedLogStore* indexed = indexedOrNull();
    IndexedLogStore* sourceIndexed = source.indexedOrNull();
    // Резидентные строки текст не меняют; такой сигнал — повод искать заново.
    return indexed && sourceIndexed && indexed->refreshSearchRow(*sourceIndexed, row);
}

LogScanSnapshot LogModel::scanSnapshot(bool filteredOnly) const
{
    return m_store->scanSnapshot(filteredOnly);
}

QVector<std::shared_ptr<LogEntry>> LogModel::residentEntries() const
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
    // Строки перечитает индексатор: закладки сверятся, когда покажутся снова.
    m_locator->allReset();
    m_bookmarks->invalidateAll();
    if (m_navHistory)
        m_navHistory->invalidateAll();
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
    m_locator->allReset();
    m_bookmarks->invalidateAll();
    if (m_navHistory)
        m_navHistory->invalidateAll();
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

int LogModel::placeAmongVisible(int logicalEntryId, const LogFile* sourceFile,
                                bool* visible) const
{
    return m_store->placeAmongVisible(logicalEntryId, sourceFile, visible);
}

int LogModel::nearestVisibleRowForLine(qint64 lineNumber, const LogFile* sourceFile) const
{
    return m_store->nearestVisibleRowForLine(lineNumber, sourceFile);
}

RecordKey LogModel::keyAt(int visibleRow) const
{
    return m_store->keyAt(visibleRow);
}

int LogModel::rowForKey(const RecordKey& key) const
{
    return m_store->rowForKey(key);
}

int LogModel::nearestRowForKey(const RecordKey& key) const
{
    return m_store->nearestRowForKey(key);
}

int LogModel::findRowWithLevel(LogLevelMask levels, int startRow, bool forward,
                               bool wrapAround) const
{
    return m_store->findRowWithLevel(levels, startRow, forward, wrapAround);
}

QModelIndex LogModel::findOccurrence(const TextQuery& query, int startRow, bool forward,
                                     bool wrapAround) const
{
    const int row = m_store->findOccurrence(query, startRow, forward, wrapAround);
    return row >= 0 ? index(row, 0) : QModelIndex();
}

QModelIndex LogModel::findNextOccurrence(const QString& text, int startRow,
                                         Qt::CaseSensitivity cs, bool wrapAround) const
{
    return findOccurrence(TextQuery(text, cs), startRow, /*forward=*/true, wrapAround);
}

QModelIndex LogModel::findPreviousOccurrence(const QString& text, int startRow,
                                             Qt::CaseSensitivity cs, bool wrapAround) const
{
    return findOccurrence(TextQuery(text, cs), startRow, /*forward=*/false, wrapAround);
}

// ---------------------------------------------------------------------------
// Фильтры (настройки — здесь, исполнение — в хранилище)
// ---------------------------------------------------------------------------

void LogModel::setLogLevelFilter(const QSet<LogLevel>& levels)
{
    m_activeLogLevels = levels;
    emit filterSettingsChanged();
    applyFilter();
}

void LogModel::setHiddenFiles(const QSet<QString>& filePaths)
{
    if (filePaths == m_hiddenFiles)
        return;
    m_hiddenFiles = filePaths;
    ++m_hiddenFilesGeneration;
    emit filterSettingsChanged();
    applyFilter();
}

void LogModel::setTimeRangeFilter(const QDateTime& start, const QDateTime& end)
{
    m_filterStartTime = start;
    m_filterEndTime = end;
    emit filterSettingsChanged();
    applyFilter();
}

void LogModel::setFilterRules(const FilterRuleSet& rules)
{
    m_filterRules = rules;
    emit filterSettingsChanged();
    applyFilter();
}

void LogModel::setFilters(const QSet<LogLevel>& levels, const QDateTime& start,
                          const QDateTime& end, const FilterRuleSet& rules)
{
    m_activeLogLevels = levels;
    m_filterStartTime = start;
    m_filterEndTime = end;
    m_filterRules = rules;
    emit filterSettingsChanged();
    applyFilter();
}

void LogModel::setFilters(const QSet<LogLevel>& levels, const QDateTime& start,
                          const QDateTime& end, const FilterRuleSet& rules,
                          const QSet<QString>& hiddenFiles)
{
    if (hiddenFiles != m_hiddenFiles) {
        m_hiddenFiles = hiddenFiles;
        ++m_hiddenFilesGeneration;
    }
    setFilters(levels, start, end, rules);
}

bool LogModel::hasFilters() const
{
    // Правила, из которых ни одно не действует (невалидный регекс, колонки
    // нет в схеме), строк не скрывают.
    return !m_activeLogLevels.isEmpty() || m_filterStartTime.isValid()
        || m_filterEndTime.isValid() || m_filterRules.usableRuleCount() > 0
        || !m_hiddenFiles.isEmpty();
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

bool LogModel::isFiltering() const
{
    return m_store->isFiltering();
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

void LogModel::setFileSchemas(const FileSchemaSet& schemas)
{
    if (m_fileSchemas == schemas)
        return;
    m_fileSchemas = schemas;
    // O(1): записи не трогаются, кэш полей пуст (F-17, шаг 3); показ полей
    // — перерисовать видимое.
    m_fieldSource->setSchemas(m_fileSchemas);
    if (m_fieldFilterEnabled)
        notifyDisplayChanged();
}

void LogModel::setFileSchema(const QString& filePath, FileSchemaPtr schema)
{
    FileSchemaSet schemas = m_fileSchemas;
    if (schema)
        schemas.byPath.insert(filePath, std::move(schema));
    else
        schemas.byPath.remove(filePath);
    setFileSchemas(schemas);
}

FieldLookup LogModel::fieldLookupOf(const LogEntry& entry) const
{
    RecordKey key;
    key.file = entry.sourceFile();
    key.line = entry.originalLineNumber();
    key.logicalEntryId = entry.logicalEntryId();
    if (entry.timestamp().isValid())
        key.timestampMs = entry.timestamp().toMSecsSinceEpoch();
    return m_fieldSource->fieldsFor(key, entry.message());
}

LogEntryFields LogModel::fieldsOf(const LogEntry& entry) const
{
    FieldLookup lookup = fieldLookupOf(entry);
    return lookup.isReady() ? std::move(lookup.fields) : LogEntryFields();
}

bool LogModel::fieldRulesInBackground() const
{
    return m_fieldSource->hasBackgroundSchema() && m_filterRules.needsFields();
}

void LogModel::onFieldsReady(const QVector<RecordKey>& keys)
{
    // Ключ → строка (O(log N)); строки, ушедшие из видимых, пропускаются.
    // Сортированные строки — слитыми диапазонами: соседние (разрыв до
    // kGap) — в один. Строки приходят с экрана и двух соседних, поэтому
    // обычно диапазон один и короткий (у view — точечная инвалидация).
    QVector<int> rows;
    rows.reserve(keys.size());
    for (const RecordKey& key : keys) {
        const int row = m_store->rowForKey(key);
        if (row >= 0)
            rows.append(row);
    }
    if (rows.isEmpty())
        return;
    std::sort(rows.begin(), rows.end());
    constexpr int kGap = 64;
    const QList<int> roles{ Qt::DisplayRole, int(AnsiStyleRole), int(FieldsReadyRole) };
    int first = rows.first();
    int last = first;
    for (int i = 1; i <= rows.size(); ++i) {
        if (i < rows.size() && rows[i] - last <= kGap) {
            last = rows[i];
            continue;
        }
        emit dataChanged(index(first, 0), index(last, 0), roles);
        if (i < rows.size())
            first = last = rows[i];
    }
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
    if (!m_fieldFilterEnabled)
        return entry.message();
    const LogEntryFields fields = fieldsOf(entry);
    if (fields.isEmpty())
        return entry.message();
    return formatDisplayText(entry.message(), fields, m_fieldFilterEnabled,
                             m_visibleFieldIndexes, m_fileSchemas.forFile(entry.sourceFile().get()));
}

QString LogModel::formatDisplayText(const QString& message, const LogEntryFields& fields,
                                    bool fieldsShown, const QVector<int>& visibleIndexes,
                                    const FileSchema* schema)
{
    // Lines that did not match the schema at all (continuation lines, junk)
    // have no fields — show their raw text so nothing silently disappears.
    if (!fieldsShown || fields.isEmpty())
        return message;
    // Слитая вкладка (F-5): у схемы файла нет ни одной выбранной колонки —
    // строка сырым текстом, а не пустой. Пустой выбор (сняты все галочки) —
    // пустая строка, как прежде.
    if (schema && !visibleIndexes.isEmpty() && !schema->hasAnyOf(visibleIndexes))
        return message;

    QString result;
    bool first = true;
    for (const int fieldIndex : visibleIndexes) {
        // Экранированное значение (F-4) — раскодированным, но однострочным:
        // \n, \t, \r остаются последовательностями (строка view — одна линия).
        const bool escaped = fields.isEscaped(fieldIndex);
        const QString decoded = escaped ? fields.value(fieldIndex, message, true) : QString();
        const QStringView value = escaped ? QStringView(decoded) : fields.get(fieldIndex, message);
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

// Длина текста строки для оценки высот view. Без показа полей — точная
// (сырой текст). С показом полей — ОЦЕНКА по сырому тексту на обоих
// бэкендах (F-17, шаг 3): поля не хранятся, а считать их здесь значило бы
// разбирать каждую вставленную строку (инвариант 3: на вставку поля не
// считаются); view уточняет высоты лениво при отрисовке.
int LogModel::displayTextLength(int row) const
{
    if (row < 0 || row >= m_store->visibleCount())
        return 0;
    return m_store->rawTextLengthAt(row);
}
