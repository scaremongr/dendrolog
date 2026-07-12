#include "indexedlogstore.h"
#include "logmodel.h"
#include "sequentiallinereader.h"

#include <QFutureWatcher>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>

namespace {

// Метаданные строки прямо из индекса (без материализации LogEntry).
inline LogEntryMeta metaFromIndex(const LineIndex& index, qint64 line,
                                  const LogFile* file)
{
    LogEntryMeta m;
    const quint32 logical = index.logicalId(line);
    m.timestampMs = index.timestampMs(logical);
    m.level = index.level(line);
    m.logicalEntryId = int(logical);
    m.sourceFile = file;
    m.isPlainText = index.isPlainTextLine(line);
    return m;
}

} // namespace

IndexedLogStore::IndexedLogStore(LogModel& model)
    : LogStore(model)
    , m_filterProgress(std::make_shared<std::atomic<int>>(0))
{
    m_progressTimer = new QTimer(&model);
    m_progressTimer->setInterval(200);
    std::weak_ptr<int> alive = m_aliveGuard;
    QObject::connect(m_progressTimer, &QTimer::timeout, &model, [this, alive]() {
        if (alive.expired())
            return;
        emit m_model.filterProgress(m_filterProgress->load(std::memory_order_relaxed));
    });
}

IndexedLogStore::~IndexedLogStore()
{
    cancelPendingFilter(false);
}

// ---------------------------------------------------------------------------
// Подключение файлов и приём батчей индексатора
// ---------------------------------------------------------------------------

int IndexedLogStore::attachFile(const LogFilePtr& logFile, std::shared_ptr<LineIndex> index)
{
    // Переход «один файл → несколько»: тождество больше не работает,
    // материализуем общий порядок из уже показанных строк.
    if (m_files.size() == 1 && identityAll())
        materializeAllRefs();

    IndexedFile f;
    f.logFile = logFile;
    f.index = std::move(index);
    f.cacheFileId = m_textCache.addFile(logFile->filePath);
    m_files.append(std::move(f));
    return int(m_files.size()) - 1;
}

void IndexedLogStore::materializeAllRefs()
{
    m_allRefs.clear();
    if (m_files.isEmpty())
        return;
    // Только строки, уже показанные модели (не «сырые» из индекса).
    m_allRefs.reserve(int(m_shownAllCount));
    for (qint64 l = 0; l < m_shownAllCount; ++l)
        m_allRefs.append(makeRef(0, l));
}

void IndexedLogStore::appendIndexedRows(const LogFilePtr& logFile, qint64 firstLine,
                                        qint64 count, bool markNew)
{
    if (!logFile || count <= 0)
        return;
    int fileId = -1;
    for (int i = 0; i < m_files.size(); ++i) {
        if (m_files[i].logFile == logFile) { fileId = i; break; }
    }
    if (fileId < 0)
        return;

    // Хвост файла дописан после переиндексации «предварительной» строки:
    // первая строка диапазона уже показана — её содержимое изменилось.
    const bool tailReindexed = identityAll()
        ? (m_files.size() == 1 && firstLine < allCount()
           && fileId == 0)
        : (!m_allRefs.isEmpty() && refFile(m_allRefs.last()) == fileId
           && refLine(m_allRefs.last()) >= firstLine);

    qint64 freshFirst = firstLine;
    if (tailReindexed) {
        // Обновляем существующую строку (текст мог дорасти).
        m_textCache.invalidateFile(m_files[fileId].cacheFileId);
        freshFirst = firstLine + 1;

        if (m_identityVisible) {
            const int row = identityAll()
                ? int(firstLine)
                : int(std::lower_bound(m_allRefs.constBegin(), m_allRefs.constEnd(),
                                       makeRef(fileId, firstLine))
                      - m_allRefs.constBegin());
            if (row >= 0 && row < visibleCount())
                emit m_model.dataChanged(m_model.index(row, 0), m_model.index(row, 0));
        } else {
            // Точечная переоценка одной изменившейся строки текущим фильтром.
            const RowRef ref = makeRef(fileId, firstLine);
            const auto it = std::lower_bound(m_visibleRefs.begin(), m_visibleRefs.end(),
                                             ref,
                                             [this](RowRef a, RowRef b) { return lessRef(a, b); });
            const bool wasVisible = (it != m_visibleRefs.end() && *it == ref);
            const bool nowPasses = refPassesFiltersNow(ref);
            if (wasVisible && nowPasses) {
                const int row = int(it - m_visibleRefs.begin());
                emit m_model.dataChanged(m_model.index(row, 0), m_model.index(row, 0));
            } else if (wasVisible && !nowPasses) {
                const int row = int(it - m_visibleRefs.begin());
                m_model.beginRemoveRows(QModelIndex(), row, row);
                m_visibleRefs.erase(it);
                m_model.endRemoveRows();
                emit m_model.modelFiltered(visibleCount());
            } else if (!wasVisible && nowPasses) {
                const int row = int(it - m_visibleRefs.begin());
                m_model.beginInsertRows(QModelIndex(), row, row);
                m_visibleRefs.insert(it, ref);
                m_model.endInsertRows();
                emit m_model.modelFiltered(visibleCount());
            }
        }
    }

    const qint64 freshCount = firstLine + count - freshFirst;
    if (freshCount <= 0)
        return;

    // Общий порядок (мульти-файл): дописываем и, при необходимости, сливаем.
    if (!identityAll()) {
        QVector<RowRef> refs;
        refs.reserve(int(freshCount));
        for (qint64 l = freshFirst; l < firstLine + count; ++l)
            refs.append(makeRef(fileId, l));
        const bool needMerge = !m_allRefs.isEmpty()
            && lessRef(refs.first(), m_allRefs.last());
        const int oldSize = int(m_allRefs.size());
        m_allRefs += refs;
        if (needMerge) {
            std::inplace_merge(m_allRefs.begin(), m_allRefs.begin() + oldSize,
                               m_allRefs.end(),
                               [this](RowRef a, RowRef b) { return lessRef(a, b); });
        }
    }

    if (markNew) {
        // Подсветка «новых» строк переходит на этот батч.
        const bool hadPrevious = !m_newRefs.isEmpty();
        m_newRefs.clear();
        for (qint64 l = freshFirst; l < firstLine + count; ++l)
            m_newRefs.insert(makeRef(fileId, l));
        if (hadPrevious && visibleCount() > 0)
            emit m_model.dataChanged(m_model.index(0, 0),
                                     m_model.index(visibleCount() - 1, 0),
                                     {LogModel::IsNewRole});
    }

    if (m_identityVisible) {
        // Без фильтра видимые строки = все строки. Для одного файла вставка
        // всегда в конец; для слитых файлов новые строки могли встать в
        // середину — тогда честный reset (редкий случай).
        if (identityAll()) {
            const int first = int(m_shownAllCount);
            m_model.beginInsertRows(QModelIndex(), first, int(firstLine + count) - 1);
            m_shownAllCount = firstLine + count;
            m_model.endInsertRows();
        } else {
            m_model.beginResetModel();
            m_shownAllCount = qMax(m_shownAllCount, firstLine + count);
            m_model.endResetModel();
        }
        emit m_model.modelFiltered(visibleCount());
        return;
    }

    // Активный фильтр: новые строки прогоняются инкрементальным диапазонным
    // джобом (FIFO), чтобы не пересканировать весь файл на каждый батч.
    // Данные модели известны сразу (allCount растёт), видимость — по джобу.
    if (identityAll())
        m_shownAllCount = qMax(m_shownAllCount, firstLine + count);
    m_pendingRanges.append({fileId, freshFirst, freshCount});
    if (!m_filterJobActive)
        startNextPendingRange();
}

void IndexedLogStore::resetFileIndex(const LogFilePtr& logFile,
                                     std::shared_ptr<LineIndex> fresh)
{
    cancelPendingFilter(false);
    m_pendingRanges.clear();
    m_newRefs.clear();

    m_model.beginResetModel();
    for (int i = 0; i < m_files.size(); ++i) {
        if (m_files[i].logFile != logFile)
            continue;
        m_files[i].index = std::move(fresh);
        m_textCache.invalidateFile(m_files[i].cacheFileId);
        if (!identityAll()) {
            const int fileId = i;
            m_allRefs.erase(std::remove_if(m_allRefs.begin(), m_allRefs.end(),
                                           [fileId](RowRef r) {
                                               return refFile(r) == fileId;
                                           }),
                            m_allRefs.end());
        } else {
            m_shownAllCount = 0; // единственный файл начинает с нуля
        }
        break;
    }
    if (!m_identityVisible)
        m_visibleRefs.clear();
    m_model.endResetModel();
    m_filteredListStale = hasActiveFilterSettings();
    emit m_model.modelFiltered(visibleCount());
    if (m_filteredListStale)
        applyFilter();
}

std::shared_ptr<LineIndex> IndexedLogStore::indexForFile(const QString& filePath) const
{
    for (const auto& f : m_files) {
        if (f.logFile && f.logFile->filePath == filePath)
            return f.index;
    }
    return nullptr;
}

void IndexedLogStore::setFieldPattern(const QString& patternString, bool extractionEnabled)
{
    m_fieldPattern.setPattern(patternString);
    m_extractionEnabled = extractionEnabled;
}

void IndexedLogStore::setTextCacheBudget(qint64 bytes)
{
    m_textCache.setBudgetBytes(bytes);
}

// ---------------------------------------------------------------------------
// Отображение row → ref и сравнение глобального порядка
// ---------------------------------------------------------------------------

qint64 IndexedLogStore::allCount() const
{
    if (!identityAll())
        return m_allRefs.size();
    // НЕ живой index->lineCount(): воркер публикует строки раньше, чем GUI
    // получит батч, а rowCount() модели обязан меняться строго между
    // begin/endInsertRows.
    return m_files.size() == 1 ? m_shownAllCount : 0;
}

int IndexedLogStore::visibleCount() const
{
    return m_identityVisible ? int(allCount()) : int(m_visibleRefs.size());
}

IndexedLogStore::RowRef IndexedLogStore::rowToRef(int visibleRow) const
{
    if (!m_identityVisible)
        return m_visibleRefs.at(visibleRow);
    return identityAll() ? makeRef(0, visibleRow) : m_allRefs.at(visibleRow);
}

bool IndexedLogStore::lessRef(RowRef a, RowRef b) const
{
    const int fa = refFile(a), fb = refFile(b);
    const qint64 la = refLine(a), lb = refLine(b);
    const LineIndex& ia = *m_files[fa].index;
    const LineIndex& ib = *m_files[fb].index;

    // 1. Время логической записи (невалидные — «больше» любых валидных).
    const qint64 ta = ia.timestampMs(ia.logicalId(la));
    const qint64 tb = ib.timestampMs(ib.logicalId(lb));
    if (ta >= 0 && tb >= 0) {
        if (ta != tb)
            return ta < tb;
    } else if (ta >= 0) {
        return true;
    } else if (tb >= 0) {
        return false;
    }

    // 2. Файл-источник (детерминированный порядок по пути).
    if (fa != fb) {
        const QString& pa = m_files[fa].logFile->filePath;
        const QString& pb = m_files[fb].logFile->filePath;
        if (pa != pb)
            return pa < pb;
    }

    // 3–4. Логическая запись, затем номер строки.
    const quint32 ida = ia.logicalId(la);
    const quint32 idb = ib.logicalId(lb);
    if (fa == fb && ida != idb)
        return ida < idb;
    return fa == fb ? la < lb : fa < fb;
}

bool IndexedLogStore::hasActiveFilterSettings() const
{
    return !m_model.logLevelFilter().isEmpty()
        || m_model.startTimeFilter().isValid()
        || m_model.endTimeFilter().isValid()
        || m_model.filterRules().isActive();
}

// ---------------------------------------------------------------------------
// Точечный доступ
// ---------------------------------------------------------------------------

std::shared_ptr<LogEntry> IndexedLogStore::materializeEntry(RowRef ref) const
{
    const int fileId = refFile(ref);
    const qint64 line = refLine(ref);
    const IndexedFile& f = m_files[fileId];
    const LineIndex& index = *f.index;

    const quint32 logical = index.logicalId(line);
    QDateTime ts;
    const qint64 ms = index.timestampMs(logical);
    if (ms >= 0)
        ts = QDateTime::fromMSecsSinceEpoch(ms);

    const QString text = m_textCache.lineText(f.cacheFileId,
                                              index.lineStartOffset(line),
                                              index.lineByteLength(line));
    auto entry = std::make_shared<LogEntry>(int(logical), int(line + 1), ts,
                                            index.level(line), text, f.logFile);
    // Поля извлекаются по требованию — в индексе спаны не хранятся. Только
    // primary-строки: у continuation полей нет и в резидентном пути.
    if (m_extractionEnabled && m_fieldPattern.isValid() && index.isPrimary(line))
        entry->setFields(m_fieldPattern.extractFields(entry->message()));
    return entry;
}

std::shared_ptr<LogEntry> IndexedLogStore::entryAt(int visibleRow) const
{
    if (visibleRow < 0 || visibleRow >= visibleCount())
        return {};
    return materializeEntry(rowToRef(visibleRow));
}

LogEntryMeta IndexedLogStore::visibleMetaAt(int row) const
{
    if (row < 0 || row >= visibleCount())
        return LogEntryMeta{};
    const RowRef ref = rowToRef(row);
    const IndexedFile& f = m_files[refFile(ref)];
    return metaFromIndex(*f.index, refLine(ref), f.logFile.get());
}

QDateTime IndexedLogStore::visibleTimestampAt(int row) const
{
    const LogEntryMeta m = visibleMetaAt(row);
    return m.timestampMs >= 0 ? QDateTime::fromMSecsSinceEpoch(m.timestampMs)
                              : QDateTime();
}

LogLevel IndexedLogStore::visibleLevelAt(int row) const
{
    return visibleMetaAt(row).level;
}

QString IndexedLogStore::messageAt(int visibleRow) const
{
    if (visibleRow < 0 || visibleRow >= visibleCount())
        return QString();
    const RowRef ref = rowToRef(visibleRow);
    const IndexedFile& f = m_files[refFile(ref)];
    const qint64 line = refLine(ref);
    return m_textCache.lineText(f.cacheFileId, f.index->lineStartOffset(line),
                                f.index->lineByteLength(line));
}

int IndexedLogStore::rawTextLengthAt(int visibleRow) const
{
    if (visibleRow < 0 || visibleRow >= visibleCount())
        return 0;
    const RowRef ref = rowToRef(visibleRow);
    const IndexedFile& f = m_files[refFile(ref)];
    // Оценка: длина в байтах UTF-8 >= длины в QChar; для ASCII совпадает.
    // View уточняет высоты лениво при отрисовке.
    return int(f.index->lineByteLength(refLine(ref)));
}

bool IndexedLogStore::isNewAt(int visibleRow) const
{
    if (m_newRefs.isEmpty() || visibleRow < 0 || visibleRow >= visibleCount())
        return false;
    return m_newRefs.contains(rowToRef(visibleRow));
}

// ---------------------------------------------------------------------------
// Навигация
// ---------------------------------------------------------------------------

int IndexedLogStore::firstVisibleRowAtOrAfter(const QDateTime& t) const
{
    const qint64 wantMs = t.isValid() ? t.toMSecsSinceEpoch()
                                      : std::numeric_limits<qint64>::min();
    // Видимые строки упорядочены глобальным порядком (время → файл → запись),
    // невалидные метки в конце — двоичный поиск по «ts < want».
    int lo = 0, hi = visibleCount();
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        const LogEntryMeta m = visibleMetaAt(mid);
        const bool less = (m.timestampMs >= 0) && m.timestampMs < wantMs;
        if (less)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

QPair<QDateTime, QDateTime> IndexedLogStore::fullTimeRange() const
{
    const qint64 total = allCount();
    if (total <= 0)
        return {};

    // Снапшоты один раз — обратный проход по невалидному хвосту может быть
    // длинным, мьютекс на строку недопустим.
    QVector<LineIndexSnapshot> snaps;
    snaps.reserve(m_files.size());
    for (const auto& f : m_files)
        snaps.append(f.index->snapshot());

    const auto tsOfAll = [this, &snaps](qint64 i) -> qint64 {
        const RowRef ref = identityAll() ? makeRef(0, i) : m_allRefs.at(int(i));
        const LineIndexSnapshot& s = snaps[refFile(ref)];
        return s.timestampMs(s.logicalId(refLine(ref)));
    };
    qint64 end = total;
    while (end > 0 && tsOfAll(end - 1) < 0)
        --end;
    const qint64 firstMs = tsOfAll(0);
    if (end <= 0 || firstMs < 0)
        return {};
    return { QDateTime::fromMSecsSinceEpoch(firstMs),
             QDateTime::fromMSecsSinceEpoch(tsOfAll(end - 1)) };
}

QStringList IndexedLogStore::sampleMessages(int maxCount) const
{
    QStringList samples;
    const qint64 total = allCount();
    // Кап на просмотр: сэмплы нужны из головы файла, не сканировать весь лог.
    const qint64 scanCap = qMin<qint64>(total, 10000);
    for (qint64 i = 0; i < scanCap && samples.size() < maxCount; ++i) {
        const RowRef ref = identityAll() ? makeRef(0, i) : m_allRefs.at(int(i));
        const IndexedFile& f = m_files[refFile(ref)];
        const qint64 line = refLine(ref);
        const QString text = m_textCache.lineText(f.cacheFileId,
                                                  f.index->lineStartOffset(line),
                                                  f.index->lineByteLength(line));
        if (!text.trimmed().isEmpty())
            samples.append(text);
    }
    return samples;
}

QVector<std::shared_ptr<LogEntry>> IndexedLogStore::logicalRecordLines(
    const std::shared_ptr<LogEntry>& line, int maxLines) const
{
    if (!line)
        return {};
    if (line->isPlainText())
        return { line };

    int fileId = -1;
    for (int i = 0; i < m_files.size(); ++i) {
        if (m_files[i].logFile == line->sourceFile()) { fileId = i; break; }
    }
    if (fileId < 0)
        return { line };

    const LineIndex& index = *m_files[fileId].index;
    const qint64 total = index.lineCount();
    qint64 l = qint64(line->originalLineNumber()) - 1;
    if (l < 0 || l >= total)
        return { line };
    const quint32 id = quint32(line->logicalEntryId());
    if (index.logicalId(l) != id)
        return { line }; // страховка: строка не на ожидаемом месте

    // Строки логической записи в файле идут подряд.
    qint64 first = l;
    while (first > 0 && index.logicalId(first - 1) == id)
        --first;

    QVector<std::shared_ptr<LogEntry>> lines;
    for (qint64 i = first; i < total && index.logicalId(i) == id
                           && lines.size() < maxLines; ++i)
        lines.append(materializeEntry(makeRef(fileId, i)));
    return lines;
}

int IndexedLogStore::rowForEntry(int logicalEntryId, const LogFile* sourceFile) const
{
    int fileId = -1;
    for (int i = 0; i < m_files.size(); ++i) {
        if (m_files[i].logFile.get() == sourceFile) { fileId = i; break; }
    }
    if (fileId < 0)
        return -1;
    const LineIndex& index = *m_files[fileId].index;
    const qint64 total = index.lineCount();

    // logicalId по строкам файла не убывает — двоичный поиск первой строки.
    const quint32 id = quint32(logicalEntryId);
    qint64 lo = 0, hi = total;
    while (lo < hi) {
        const qint64 mid = lo + (hi - lo) / 2;
        if (index.logicalId(mid) < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= total || index.logicalId(lo) != id)
        return -1;

    // Первая ВИДИМАЯ строка этой записи.
    for (qint64 l = lo; l < total && index.logicalId(l) == id; ++l) {
        const RowRef ref = makeRef(fileId, l);
        if (m_identityVisible) {
            if (identityAll())
                return int(l);
            const auto it = std::lower_bound(m_allRefs.constBegin(), m_allRefs.constEnd(),
                                             ref,
                                             [this](RowRef a, RowRef b) { return lessRef(a, b); });
            if (it != m_allRefs.constEnd() && *it == ref)
                return int(it - m_allRefs.constBegin());
        } else {
            const auto it = std::lower_bound(m_visibleRefs.constBegin(),
                                             m_visibleRefs.constEnd(), ref,
                                             [this](RowRef a, RowRef b) { return lessRef(a, b); });
            if (it != m_visibleRefs.constEnd() && *it == ref)
                return int(it - m_visibleRefs.constBegin());
        }
    }
    return -1;
}

int IndexedLogStore::nearestVisibleRow(int logicalEntryId, const LogFile* sourceFile) const
{
    if (visibleCount() == 0)
        return -1;
    const int exact = rowForEntry(logicalEntryId, sourceFile);
    if (exact >= 0)
        return exact;

    int fileId = -1;
    for (int i = 0; i < m_files.size(); ++i) {
        if (m_files[i].logFile.get() == sourceFile) { fileId = i; break; }
    }
    if (fileId < 0)
        return -1;
    const LineIndex& index = *m_files[fileId].index;
    const qint64 total = index.lineCount();
    const quint32 id = quint32(logicalEntryId);
    qint64 lo = 0, hi = total;
    while (lo < hi) {
        const qint64 mid = lo + (hi - lo) / 2;
        if (index.logicalId(mid) < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= total || index.logicalId(lo) != id)
        return -1; // записи нет в данных вовсе

    // Запись скрыта фильтром: ближайшая видимая — первая после неё, иначе
    // последняя перед ней (семантика резидентного nearestVisibleRow).
    if (m_identityVisible)
        return -1; // тождество: скрытых записей не бывает — сюда не попадаем

    const RowRef ref = makeRef(fileId, lo);
    const auto it = std::lower_bound(m_visibleRefs.constBegin(), m_visibleRefs.constEnd(),
                                     ref,
                                     [this](RowRef a, RowRef b) { return lessRef(a, b); });
    const int j = int(it - m_visibleRefs.constBegin());
    return j < int(m_visibleRefs.size()) ? j : j - 1;
}

int IndexedLogStore::findNextOccurrence(const QString& text, int startRow,
                                        Qt::CaseSensitivity cs, bool wrapAround) const
{
    // ВНИМАНИЕ: синхронный дисковый скан. Обычно совпадение близко и чтение
    // последовательное; на гигантских файлах без совпадений возможна пауза —
    // асинхронный вариант заведён в план (2.9).
    const int numRows = visibleCount();
    if (text.isEmpty() || numRows == 0)
        return -1;

    std::vector<std::unique_ptr<SequentialLineReader>> readers(size_t(m_files.size()));
    const auto lineText = [&](int row) -> const QString& {
        const RowRef ref = rowToRef(row);
        const int fileId = refFile(ref);
        if (!readers[size_t(fileId)]) {
            readers[size_t(fileId)] = std::make_unique<SequentialLineReader>(
                m_files[fileId].logFile->filePath);
            readers[size_t(fileId)]->open();
        }
        const LineIndex& index = *m_files[fileId].index;
        const qint64 line = refLine(ref);
        return readers[size_t(fileId)]->lineAt(index.lineStartOffset(line),
                                       index.lineByteLength(line));
    };

    const int firstRow = qBound(0, startRow + 1, numRows);
    for (int i = firstRow; i < numRows; ++i) {
        if (lineText(i).contains(text, cs))
            return i;
    }
    if (wrapAround) {
        for (int i = 0; i <= startRow && i < numRows; ++i) {
            if (lineText(i).contains(text, cs))
                return i;
        }
    }
    return -1;
}

int IndexedLogStore::findPreviousOccurrence(const QString& text, int startRow,
                                            Qt::CaseSensitivity cs, bool wrapAround) const
{
    const int numRows = visibleCount();
    if (text.isEmpty() || numRows == 0)
        return -1;

    std::vector<std::unique_ptr<SequentialLineReader>> readers(size_t(m_files.size()));
    const auto lineText = [&](int row) -> const QString& {
        const RowRef ref = rowToRef(row);
        const int fileId = refFile(ref);
        if (!readers[size_t(fileId)]) {
            readers[size_t(fileId)] = std::make_unique<SequentialLineReader>(
                m_files[fileId].logFile->filePath);
            readers[size_t(fileId)]->open();
        }
        const LineIndex& index = *m_files[fileId].index;
        const qint64 line = refLine(ref);
        return readers[size_t(fileId)]->lineAt(index.lineStartOffset(line),
                                       index.lineByteLength(line));
    };

    int currentRow = startRow - 1;
    if (startRow < 0 || startRow >= numRows)
        currentRow = numRows - 1;
    for (int i = currentRow; i >= 0; --i) {
        if (lineText(i).contains(text, cs))
            return i;
    }
    if (wrapAround) {
        for (int i = numRows - 1; i >= startRow && i >= 0; --i) {
            if (lineText(i).contains(text, cs))
                return i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Сканы
// ---------------------------------------------------------------------------

namespace {

// Снапшот индексного хранилища для воркеров: самодостаточный (свои ридеры).
class IndexedScanSnapshotImpl final : public LogScanSnapshot::Impl {
public:
    struct FileSnap {
        LineIndexSnapshot index;
        QString path;
        LogFilePtr logFile;
    };

    QVector<FileSnap> files;
    QVector<IndexedLogStore::RowRef> refs; // пуст = тождество по файлу 0
    qint64 identityCount = 0;              // счётчик строк при тождестве

    qint64 rowCount() const override
    {
        return refs.isEmpty() ? identityCount : refs.size();
    }

    LogEntryMeta metaAt(qint64 row) const override
    {
        if (row < 0 || row >= rowCount())
            return LogEntryMeta{};
        int fileId; qint64 line;
        resolve(row, fileId, line);
        const FileSnap& f = files[fileId];
        LogEntryMeta m;
        const quint32 logical = f.index.logicalId(line);
        m.timestampMs = f.index.timestampMs(logical);
        m.level = f.index.level(line);
        m.logicalEntryId = int(logical);
        m.sourceFile = f.logFile.get();
        m.isPlainText = f.index.isPlainTextLine(line);
        return m;
    }

    void forEachMeta(qint64 fromRow,
                     const std::function<bool(qint64, const LogEntryMeta&)>& visit)
        const override
    {
        const qint64 n = rowCount();
        for (qint64 i = qMax<qint64>(0, fromRow); i < n; ++i) {
            if (!visit(i, metaAt(i)))
                return;
        }
    }

    void forEachLine(qint64 fromRow,
                     const std::function<bool(qint64, const LogEntryMeta&,
                                              QStringView)>& visit) const override
    {
        std::vector<std::unique_ptr<SequentialLineReader>> readers(size_t(files.size()));
        const qint64 n = rowCount();
        for (qint64 i = qMax<qint64>(0, fromRow); i < n; ++i) {
            int fileId; qint64 line;
            resolve(i, fileId, line);
            if (!readers[size_t(fileId)]) {
                readers[size_t(fileId)] =
                    std::make_unique<SequentialLineReader>(files[fileId].path);
                readers[size_t(fileId)]->open();
            }
            const FileSnap& f = files[fileId];
            const QString& text = readers[size_t(fileId)]->lineAt(
                f.index.lineStartOffset(line), f.index.lineByteLength(line));
            if (!visit(i, metaAt(i), QStringView(text)))
                return;
        }
    }

    QString textAt(qint64 row) const override
    {
        if (row < 0 || row >= rowCount())
            return QString();
        int fileId; qint64 line;
        resolve(row, fileId, line);
        SequentialLineReader reader(files[fileId].path);
        if (!reader.open())
            return QString();
        const FileSnap& f = files[fileId];
        return reader.lineAt(f.index.lineStartOffset(line),
                             f.index.lineByteLength(line));
    }

    LogFilePtr sourceFilePtrAt(qint64 row) const override
    {
        if (row < 0 || row >= rowCount())
            return LogFilePtr();
        int fileId; qint64 line;
        resolve(row, fileId, line);
        return files[fileId].logFile;
    }

private:
    void resolve(qint64 row, int& fileId, qint64& line) const
    {
        if (refs.isEmpty()) {
            fileId = 0;
            line = row;
        } else {
            const IndexedLogStore::RowRef ref = refs.at(int(row));
            fileId = int(ref >> IndexedLogStore::kFileBits);
            line = qint64(ref & ((quint64(1) << IndexedLogStore::kFileBits) - 1));
        }
    }
};

} // namespace

LogScanSnapshot IndexedLogStore::scanSnapshot(bool filteredOnly) const
{
    auto impl = std::make_shared<IndexedScanSnapshotImpl>();
    impl->files.reserve(m_files.size());
    for (const auto& f : m_files)
        impl->files.append({f.index->snapshot(), f.logFile->filePath, f.logFile});

    if (filteredOnly && !m_identityVisible) {
        impl->refs = m_visibleRefs;
    } else if (!identityAll()) {
        impl->refs = m_allRefs;
    } else {
        impl->identityCount = allCount();
    }
    return LogScanSnapshot(std::move(impl));
}

// ---------------------------------------------------------------------------
// Фильтрация — всегда асинхронно (текстовые правила читают диск)
// ---------------------------------------------------------------------------

bool IndexedLogStore::refPassesFiltersNow(RowRef ref) const
{
    // Точечная проверка ОДНОЙ строки на GUI-потоке (переиндексированный
    // хвост): максимум одна строка текста через чанк-кэш — ограниченно.
    const int fileId = refFile(ref);
    const qint64 line = refLine(ref);
    const IndexedFile& f = m_files[fileId];
    const LineIndex& index = *f.index;

    const QSet<LogLevel> levels = m_model.logLevelFilter();
    if (!levels.isEmpty() && !levels.contains(index.level(line)))
        return false;

    const qint64 tsMs = index.timestampMs(index.logicalId(line));
    const QDateTime startTime = m_model.startTimeFilter();
    const QDateTime endTime = m_model.endTimeFilter();
    if (startTime.isValid()
        && (tsMs < 0 || tsMs < startTime.toMSecsSinceEpoch()))
        return false;
    if (endTime.isValid() && tsMs >= 0 && tsMs > endTime.toMSecsSinceEpoch())
        return false;

    const FilterRuleSet& rules = m_model.filterRules();
    if (rules.isActive()) {
        const QString text = m_textCache.lineText(f.cacheFileId,
                                                  index.lineStartOffset(line),
                                                  index.lineByteLength(line));
        LogEntryFields fields;
        if (m_extractionEnabled && m_fieldPattern.isValid() && index.isPrimary(line))
            fields = m_fieldPattern.extractFields(text);
        if (!rules.matchesLine(QStringView(text), fields))
            return false;
    }
    return true;
}

void IndexedLogStore::applyFilter()
{
    cancelPendingFilter(false);
    m_pendingRanges.clear();

    if (!hasActiveFilterSettings()) {
        // Тождество: видно всё, отдельный список не нужен.
        m_model.beginResetModel();
        m_identityVisible = true;
        m_visibleRefs.clear();
        m_model.endResetModel();
        m_filteredListStale = false;
        emit m_model.modelFiltered(visibleCount());
        return;
    }
    startFilterJob(/*fullRescan=*/true);
}

void IndexedLogStore::reapplyFilterIfStale()
{
    if (m_filteredListStale)
        applyFilter();
}

void IndexedLogStore::startNextPendingRange()
{
    if (m_pendingRanges.isEmpty())
        return;
    const PendingRange r = m_pendingRanges.takeFirst();
    startFilterJob(/*fullRescan=*/false, r.first, r.count, r.fileId);
}

void IndexedLogStore::startFilterJob(bool fullRescan, qint64 rangeFirst,
                                     qint64 rangeCount, int rangeFileId)
{
    m_filterJobActive = true;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_filterJobCancel = cancel;
    m_filterProgress->store(0);
    m_progressTimer->start();

    // Снапшоты входных данных для воркера.
    struct FileInput {
        LineIndexSnapshot index;
        QString path;
    };
    auto filesIn = std::make_shared<QVector<FileInput>>();
    for (const auto& f : m_files)
        filesIn->append({f.index->snapshot(), f.logFile->filePath});

    // Диапазон работы: весь документ или свежий батч одного файла.
    QVector<RowRef> workRefs;
    qint64 identityCount = 0;
    if (fullRescan) {
        if (identityAll())
            identityCount = allCount();
        else
            workRefs = m_allRefs;
    } else {
        workRefs.reserve(int(rangeCount));
        for (qint64 l = rangeFirst; l < rangeFirst + rangeCount; ++l)
            workRefs.append(makeRef(rangeFileId, l));
    }

    const QSet<LogLevel> levels = m_model.logLevelFilter();
    const QDateTime startTime = m_model.startTimeFilter();
    const QDateTime endTime = m_model.endTimeFilter();
    const qint64 startMs = startTime.isValid() ? startTime.toMSecsSinceEpoch()
                                               : std::numeric_limits<qint64>::min();
    const bool startValid = startTime.isValid();
    const qint64 endMs = endTime.isValid() ? endTime.toMSecsSinceEpoch()
                                           : std::numeric_limits<qint64>::max();
    const bool endValid = endTime.isValid();
    const FilterRuleSet rules = m_model.filterRules();
    const LogPattern pattern = m_fieldPattern;
    const bool extraction = m_extractionEnabled;
    auto progress = m_filterProgress;

    QFuture<QVector<RowRef>> future = QtConcurrent::run(
        [filesIn, workRefs, identityCount, levels, startValid, startMs, endValid,
         endMs, rules, pattern, extraction, cancel, progress]() -> QVector<RowRef> {
            QVector<RowRef> passing;
            const qint64 n = workRefs.isEmpty() ? identityCount : workRefs.size();
            passing.reserve(int(qMin<qint64>(n, 4096)));

            std::vector<std::unique_ptr<SequentialLineReader>> readers(size_t(filesIn->size()));
            const bool needText = rules.isActive();

            for (qint64 i = 0; i < n; ++i) {
                if ((i & 0x1FFF) == 0) {
                    if (cancel->load(std::memory_order_relaxed))
                        return QVector<RowRef>();
                    progress->store(int(i * 100 / qMax<qint64>(1, n)),
                                    std::memory_order_relaxed);
                }
                const RowRef ref = workRefs.isEmpty()
                    ? ((quint64(0) << kFileBits) | quint64(i))
                    : workRefs.at(int(i));
                const int fileId = int(ref >> kFileBits);
                const qint64 line = qint64(ref & ((quint64(1) << kFileBits) - 1));
                const LineIndexSnapshot& index = (*filesIn)[fileId].index;

                // Дешёвый отсев по метаданным — без чтения диска.
                const quint32 logical = index.logicalId(line);
                const qint64 tsMs = index.timestampMs(logical);
                if (!levels.isEmpty() && !levels.contains(index.level(line)))
                    continue;
                if (startValid && (tsMs < 0 || tsMs < startMs))
                    continue;
                if (endValid && tsMs >= 0 && tsMs > endMs)
                    continue;

                if (needText) {
                    if (!readers[size_t(fileId)]) {
                        readers[size_t(fileId)] = std::make_unique<SequentialLineReader>(
                            (*filesIn)[fileId].path);
                        readers[size_t(fileId)]->open();
                    }
                    const QString& text = readers[size_t(fileId)]->lineAt(
                        index.lineStartOffset(line), index.lineByteLength(line));
                    LogEntryFields fields;
                    if (extraction && pattern.isValid() && index.isPrimary(line))
                        fields = pattern.extractFields(text);
                    if (!rules.matchesLine(QStringView(text), fields))
                        continue;
                }
                passing.append(ref);
            }
            progress->store(100, std::memory_order_relaxed);
            return passing;
        });
    m_filterJobFuture = future;

    auto* watcher = new QFutureWatcher<QVector<RowRef>>(&m_model);
    const int generation = m_filterGeneration;
    std::weak_ptr<int> alive = m_aliveGuard;
    QObject::connect(watcher, &QFutureWatcher<QVector<RowRef>>::finished, &m_model,
            [this, watcher, generation, alive, fullRescan]() {
                watcher->deleteLater();
                if (alive.expired())
                    return; // store уже заменён другим бэкендом
                if (generation != m_filterGeneration)
                    return; // результат устарел
                m_filterJobActive = false;
                m_progressTimer->stop();
                emit m_model.filterProgress(100);

                if (fullRescan) {
                    m_model.beginResetModel();
                    m_identityVisible = false;
                    m_visibleRefs = watcher->result();
                    m_model.endResetModel();
                    m_filteredListStale = false;
                    emit m_model.modelFiltered(visibleCount());
                } else {
                    insertVisibleSorted(watcher->result());
                    emit m_model.modelFiltered(visibleCount());
                }
                startNextPendingRange();
            });
    watcher->setFuture(future);
}

void IndexedLogStore::insertVisibleSorted(const QVector<RowRef>& passing)
{
    if (passing.isEmpty())
        return;
    // Прошедшие строки отсортированы порядком своего файла; в видимом списке
    // они обычно встают в конец (tail-догрузка). Общий случай — серии вставок,
    // как в резидентном insertFilteredSorted.
    struct Run { int pos; int first; int count; };
    QVector<Run> runs;
    int searchFrom = 0;
    for (int i = 0; i < passing.size(); ++i) {
        const auto it = std::upper_bound(m_visibleRefs.constBegin() + searchFrom,
                                         m_visibleRefs.constEnd(), passing[i],
                                         [this](RowRef a, RowRef b) { return lessRef(a, b); });
        const int pos = int(it - m_visibleRefs.constBegin());
        if (!runs.isEmpty() && runs.last().pos == pos)
            ++runs.last().count;
        else
            runs.append({pos, i, 1});
        searchFrom = pos;
    }

    if (runs.size() > 64) {
        m_model.beginResetModel();
        // Полная пересборка честнее сотен вставок в середину — но заново
        // фильтровать не нужно: сливаем два отсортированных списка.
        QVector<RowRef> merged;
        merged.reserve(m_visibleRefs.size() + passing.size());
        std::merge(m_visibleRefs.constBegin(), m_visibleRefs.constEnd(),
                   passing.constBegin(), passing.constEnd(),
                   std::back_inserter(merged),
                   [this](RowRef a, RowRef b) { return lessRef(a, b); });
        m_visibleRefs = std::move(merged);
        m_model.endResetModel();
        return;
    }

    int shift = 0;
    for (const Run& run : runs) {
        const int first = run.pos + shift;
        m_model.beginInsertRows(QModelIndex(), first, first + run.count - 1);
        m_visibleRefs.insert(first, run.count, RowRef(0));
        for (int k = 0; k < run.count; ++k)
            m_visibleRefs[first + k] = passing[run.first + k];
        m_model.endInsertRows();
        shift += run.count;
    }
}

void IndexedLogStore::cancelPendingFilter(bool wait)
{
    ++m_filterGeneration;
    if (m_filterJobActive)
        m_filteredListStale = true;
    m_filterJobActive = false;
    if (m_progressTimer)
        m_progressTimer->stop();
    if (m_filterJobCancel) {
        m_filterJobCancel->store(true);
        m_filterJobCancel.reset();
    }
    if (wait)
        m_filterJobFuture.waitForFinished();
}
