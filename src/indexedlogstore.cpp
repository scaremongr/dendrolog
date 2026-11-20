#include "indexedlogstore.h"
#include "logmodel.h"
#include "sequentiallinereader.h"

#include <QFutureWatcher>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <limits>
#include <vector>

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

// Идут ли первые shownLines строк файла в порядке lessRef, т.е. можно ли взять
// порядок файла как есть. Строки одной записи лежат в файле подряд, а
// logicalId по строкам не убывает, поэтому достаточно проверить ЗАПИСИ:
// метки не убывают, и за записью без метки не следует запись с меткой
// (невалидные — «больше» любых валидных). O(записей) по снапшоту, без
// мьютекса и без lessRef на строку: на 20-ГБ логе доли секунды.
bool linesInTimeOrder(const LineIndex& index, qint64 shownLines)
{
    if (shownLines <= 1)
        return true;
    const LineIndexSnapshot snap = index.snapshot();
    const quint32 firstId = snap.logicalId(0);
    const quint32 lastId = snap.logicalId(shownLines - 1);
    qint64 prev = snap.timestampMs(firstId);
    bool prevValid = hasTimestampMs(prev);
    for (quint32 id = firstId + 1; id <= lastId; ++id) {
        const qint64 ts = snap.timestampMs(id);
        if (!hasTimestampMs(ts)) {
            prevValid = false;
            continue;
        }
        if (!prevValid || ts < prev)
            return false;
        prev = ts;
    }
    return true;
}

} // namespace

IndexedLogStore::IndexedLogStore(LogModel& model)
    : LogStore(model)
    , m_filterProgress(std::make_shared<std::atomic<int>>(0))
{
    // Фильтры модели переживают конверсию вкладки (рост файла за порог,
    // сохранённое состояние, поставленное до данных): строки индексатора
    // идут через фильтр с первой порции, а не «видно всё» до переприменения.
    m_identityVisible = !hasActiveFilterSettings();
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
    delete m_progressTimer;
}

IndexedLogStore::IndexedLogStore(LogModel& model, const IndexedLogStore& source)
    : IndexedLogStore(model)
{
    m_fieldPattern = source.m_fieldPattern;
    m_extractionEnabled = source.m_extractionEnabled;
    m_shownAllCount = source.m_shownAllCount;
    m_explicitBase = source.m_explicitBase || !source.m_identityVisible;
    m_allRefs = source.m_identityVisible ? source.m_allRefs : source.m_visibleRefs;
    m_identityVisible = false;
    // Own cache (GUI-only), but the user's budget rather than the default.
    m_textCache.setBudgetBytes(source.m_textCache.budgetBytes());
    for (const auto& file : source.m_files) {
        IndexedFile copy = file;
        copy.cacheFileId = m_textCache.addFile(file.logFile->filePath, file.encoding.decoder);
        m_files.append(std::move(copy));
    }
}

bool IndexedLogStore::appendSearchRows(const IndexedLogStore& source, int first, int last)
{
    if (first > last)
        return true;
    if (first < 0 || last >= source.visibleCount())
        return false;
    // Файлы только добавляются, номера у базы и источника совпадают. Переход
    // «один файл → несколько» меняет порядок строк, а заменённый индекс
    // (файл переписан) — сами строки: и то и другое — полный поиск.
    if (source.m_files.size() < m_files.size()
        || (m_files.size() <= 1) != (source.m_files.size() <= 1))
        return false;
    for (int i = 0; i < m_files.size(); ++i) {
        if (m_files[i].index != source.m_files[i].index)
            return false;
    }
    syncEncodingsFrom(source);
    for (int i = int(m_files.size()); i < source.m_files.size(); ++i) {
        IndexedFile copy = source.m_files[i];
        copy.cacheFileId = m_textCache.addFile(copy.logFile->filePath, copy.encoding.decoder);
        m_files.append(std::move(copy));
    }

    QVector<RowRef> refs;
    refs.reserve(last - first + 1);
    for (int row = first; row <= last; ++row)
        refs.append(source.rowToRef(row));

    if (identityAll()) {
        // Тождественная база (полный один файл) растёт только с конца.
        const qint64 firstLine = refLine(refs.first());
        if (firstLine != m_shownAllCount
            || refLine(refs.last()) != firstLine + refs.size() - 1)
            return false;
        m_shownAllCount += refs.size();
        m_pendingRanges.append({0, firstLine, qint64(refs.size())});
    } else {
        // Строки источника идут в порядке его вкладки — это и порядок базы.
        const auto less = [this](RowRef a, RowRef b) { return rowLess(a, b); };
        const bool needMerge = !m_allRefs.isEmpty() && less(refs.first(), m_allRefs.last());
        const int oldSize = int(m_allRefs.size());
        m_allRefs += refs;
        if (needMerge)
            std::inplace_merge(m_allRefs.begin(), m_allRefs.begin() + oldSize,
                               m_allRefs.end(), less);
        PendingRange range;
        range.refs = std::move(refs);
        m_pendingRanges.append(std::move(range));
    }
    // Поиск ещё идёт — новые строки проверит следующий джоб очереди.
    if (!m_filterJobActive)
        startNextPendingRange();
    return true;
}

bool IndexedLogStore::refreshSearchRow(const IndexedLogStore& source, int row)
{
    if (row < 0 || row >= source.visibleCount())
        return false;
    const RowRef ref = source.rowToRef(row);
    const int fileId = refFile(ref);
    if (fileId >= m_files.size() || m_files[fileId].index != source.m_files[fileId].index)
        return false;
    // Перечитанная предварительная строка могла и решить кодировку файла.
    syncEncodingsFrom(source);
    m_textCache.invalidateFile(m_files[fileId].cacheFileId);
    // Если строка ещё в очереди поиска, джоб проверит её сам; вставку,
    // сделанную здесь раньше него, insertSortedRefs не задублирует.
    reevaluateVisibleRef(ref);
    return true;
}

void IndexedLogStore::syncEncodingsFrom(const IndexedLogStore& source)
{
    // Кодировка у источника могла решиться с последней порции (индексатор
    // дошёл до первой не-ASCII строки). Строки базы, проверенные прежде, —
    // ASCII (кроме предварительной, которую вызывающий переоценивает), их
    // текст от этого не меняется; новые строки читаются уже новым декодером.
    // Смена пользователем идёт через reset источника — полный поиск.
    const int common = int(qMin(m_files.size(), source.m_files.size()));
    for (int i = 0; i < common; ++i) {
        if (m_files[i].encoding.decoder != source.m_files[i].encoding.decoder)
            m_textCache.setFileDecoder(m_files[i].cacheFileId,
                                       source.m_files[i].encoding.decoder);
        m_files[i].encoding = source.m_files[i].encoding;
    }
}

void IndexedLogStore::reevaluateVisibleRef(RowRef ref)
{
    // Точечная переоценка одной изменившейся строки текущим фильтром.
    const auto it = std::lower_bound(m_visibleRefs.begin(), m_visibleRefs.end(), ref,
                                     [this](RowRef a, RowRef b) { return rowLess(a, b); });
    const bool wasVisible = (it != m_visibleRefs.end() && *it == ref);
    const bool nowPasses = refPassesFiltersNow(ref);
    const int row = int(it - m_visibleRefs.begin());
    if (wasVisible && nowPasses) {
        emit m_model.dataChanged(m_model.index(row, 0), m_model.index(row, 0));
    } else if (wasVisible) {
        m_model.beginRemoveRows(QModelIndex(), row, row);
        m_visibleRefs.erase(it);
        m_model.endRemoveRows();
        emit m_model.modelFiltered(visibleCount());
    } else if (nowPasses) {
        m_model.beginInsertRows(QModelIndex(), row, row);
        m_visibleRefs.insert(it, ref);
        m_model.endInsertRows();
        emit m_model.modelFiltered(visibleCount());
    }
}

// ---------------------------------------------------------------------------
// Подключение файлов и приём батчей индексатора
// ---------------------------------------------------------------------------

QVector<LogStore::FileLines> IndexedLogStore::fileLineCounts() const
{
    QVector<FileLines> out;
    out.reserve(m_files.size());
    for (const IndexedFile& f : m_files)
        out.append({f.logFile, f.shownLines, f.shownLines});
    return out;
}

int IndexedLogStore::attachFile(const LogFilePtr& logFile, std::shared_ptr<LineIndex> index)
{
    // Переход «один файл → несколько»: тождество больше не работает,
    // материализуем общий порядок из уже показанных строк.
    const bool leavingIdentity = (m_files.size() == 1);
    bool reorder = false;
    bool refilter = false;
    if (leavingIdentity) {
        // Фильтр, запущенный в тождественном режиме, вернёт строки в порядке
        // файла — после перехода это уже не порядок вкладки. Отменяем и
        // перезапускаем полным пересчётом, когда переход завершён.
        refilter = m_filterJobActive;
        if (refilter)
            cancelPendingFilter(false);
        // Если сам файл не упорядочен по времени, строки вкладки переставятся —
        // честный reset на весь переход (до него view видит тождество, после —
        // слитый порядок; промежуточных состояний он не увидит).
        reorder = !linesInTimeOrder(*m_files[0].index, m_shownAllCount);
        if (reorder)
            m_model.beginResetModel();
        materializeAllRefs(reorder);
    }

    // Цвет файла — в порядке подключения (U-23: колонка меток, легенда).
    m_model.ensureFileColor(logFile->filePath);
    IndexedFile f;
    f.logFile = logFile;
    f.index = std::move(index);
    f.cacheFileId = m_textCache.addFile(logFile->filePath, f.encoding.decoder);
    m_files.append(std::move(f));

    if (reorder)
        m_model.endResetModel();
    if (refilter)
        applyFilter();
    return int(m_files.size()) - 1;
}

void IndexedLogStore::materializeAllRefs(bool sortByTime)
{
    m_allRefs.clear();
    if (m_files.isEmpty())
        return;
    // Только строки, уже показанные модели (не «сырые» из индекса).
    m_allRefs.reserve(int(m_shownAllCount));
    if (!sortByTime || m_shownAllCount == 0) {
        for (qint64 l = 0; l < m_shownAllCount; ++l)
            m_allRefs.append(makeRef(0, l));
        return;
    }

    // База слияния обязана быть отсортирована lessRef: строки следующих файлов
    // вставляются в неё бинарным поиском. Раньше здесь оставался порядок
    // файла, и на файле с преамбулой или метками не по порядку слияние тихо
    // расставляло строки куда попало.
    //
    // Внутри одного файла lessRef — это время записи (без метки — в конец),
    // затем id записи, затем строка. Строки записи лежат подряд, а id по
    // строкам не убывает (и у одного файла идут подряд), поэтому достаточно
    // УСТОЙЧИВО отсортировать id записей по времени и развернуть записи в
    // строки. Прежде сортировались строки через lessRef — с мьютексом индекса
    // в каждом сравнении, минуты на GUI-потоке для десятков миллионов строк;
    // здесь ключи читаются из снапшота без блокировок, а сортируется по
    // 4 байта на запись.
    const LineIndexSnapshot snap = m_files[0].index->snapshot();
    const quint32 firstId = snap.logicalId(0);
    const quint32 lastId = snap.logicalId(m_shownAllCount - 1);
    QVector<quint32> ids;
    ids.reserve(int(lastId - firstId + 1));
    for (quint32 id = firstId; id <= lastId; ++id)
        ids.append(id);
    const auto key = [&snap](quint32 id) {
        const qint64 ts = snap.timestampMs(id);
        return hasTimestampMs(ts) ? ts : std::numeric_limits<qint64>::max();
    };
    std::stable_sort(ids.begin(), ids.end(),
                     [&key](quint32 a, quint32 b) { return key(a) < key(b); });

    // Первая строка записи — двоичный поиск по неубывающим id строк.
    const auto firstLineOf = [&snap, this](quint32 id) {
        qint64 lo = 0, hi = m_shownAllCount;
        while (lo < hi) {
            const qint64 mid = lo + (hi - lo) / 2;
            if (snap.logicalId(mid) < id)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo;
    };
    for (const quint32 id : std::as_const(ids)) {
        for (qint64 l = firstLineOf(id); l < m_shownAllCount && snap.logicalId(l) == id; ++l)
            m_allRefs.append(makeRef(0, l));
    }

    // Видимые строки — в том же порядке: отбираем их из m_allRefs по
    // битовой карте строк, O(N) вместо ещё одной сортировки.
    if (!m_identityVisible) {
        std::vector<bool> visible(size_t(m_shownAllCount), false);
        for (const RowRef ref : std::as_const(m_visibleRefs)) {
            const qint64 l = refLine(ref);
            if (l < m_shownAllCount)
                visible[size_t(l)] = true;
        }
        QVector<RowRef> ordered;
        ordered.reserve(m_visibleRefs.size());
        for (const RowRef ref : std::as_const(m_allRefs)) {
            if (visible[size_t(refLine(ref))])
                ordered.append(ref);
        }
        m_visibleRefs = std::move(ordered);
    }
}

void IndexedLogStore::appendIndexedRows(const LogFilePtr& logFile, qint64 firstLine,
                                        qint64 count, bool markNew)
{
    if (!logFile || count <= 0)
        return;
    const int fileId = fileIdOf(logFile.get());
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
            // m_allRefs отсортирован lessRef, а не численно по RowRef —
            // сравнение без компаратора искало строку не там.
            const int row = identityAll()
                ? int(firstLine)
                : int(std::lower_bound(m_allRefs.constBegin(), m_allRefs.constEnd(),
                                       makeRef(fileId, firstLine),
                                       [this](RowRef a, RowRef b) { return rowLess(a, b); })
                      - m_allRefs.constBegin());
            if (row >= 0 && row < visibleCount())
                emit m_model.dataChanged(m_model.index(row, 0), m_model.index(row, 0));
        } else {
            reevaluateVisibleRef(makeRef(fileId, firstLine));
        }
    }

    const qint64 freshCount = firstLine + count - freshFirst;
    if (freshCount <= 0)
        return;
    // До вставки: модель на rowsInserted уже видит число строк файла.
    m_files[fileId].shownLines = qMax(m_files[fileId].shownLines, firstLine + count);

    // Общий порядок (мульти-файл): батч приходит в порядке файла, а слияние
    // требует порядка lessRef (как резидентный путь сортирует батч перед
    // mergeEntries). На упорядоченном логе — одна проверка O(B).
    QVector<RowRef> refs;
    if (!identityAll()) {
        refs.reserve(int(freshCount));
        for (qint64 l = freshFirst; l < firstLine + count; ++l)
            refs.append(makeRef(fileId, l));
        const auto less = [this](RowRef a, RowRef b) { return lessRef(a, b); };
        if (!std::is_sorted(refs.begin(), refs.end(), less))
            std::sort(refs.begin(), refs.end(), less);
        if (!m_identityVisible) {
            // m_allRefs сейчас не видим — сливаем молча.
            const bool needMerge = !m_allRefs.isEmpty()
                && lessRef(refs.first(), m_allRefs.last());
            const int oldSize = int(m_allRefs.size());
            m_allRefs += refs;
            if (needMerge)
                std::inplace_merge(m_allRefs.begin(), m_allRefs.begin() + oldSize,
                                   m_allRefs.end(), less);
        }
    }

    // Подсветка «новых» строк переходит на этот батч. Пометка ставится ДО
    // вставки: view на rowsInserted уже видит строки новыми (по ним считается
    // «↓ N new lines»); перерисовка прежних — когда view знает о вставке.
    // При активном фильтре строки прежних батчей ещё могут ждать своей
    // очереди в диапазонном джобе: их пометку не снимаем, иначе джоб вставит
    // их уже «старыми» (без маркера и мимо счёта новых строк).
    bool hadPreviousNew = false;
    if (markNew) {
        hadPreviousNew = !m_newRefs.isEmpty();
        const bool earlierBatchPending = !m_identityVisible
            && (m_filterJobActive || !m_pendingRanges.isEmpty());
        if (!earlierBatchPending)
            m_newRefs.clear();
        for (qint64 l = freshFirst; l < firstLine + count; ++l)
            m_newRefs.insert(makeRef(fileId, l));
    }
    const auto markBatchNew = [&]() {
        if (hadPreviousNew && visibleCount() > 0)
            emit m_model.dataChanged(m_model.index(0, 0),
                                     m_model.index(visibleCount() - 1, 0),
                                     {LogModel::IsNewRole});
    };

    if (m_identityVisible) {
        // Без фильтра видимые строки = все строки. Для одного файла вставка
        // всегда в конец; у слитых файлов — сериями по месту во времени:
        // дозапись хвоста — одна серия в конце, без reset и с сохранением
        // выделения. Reset — только при сотнях серий (батчи файлов вперемешку).
        if (identityAll()) {
            const int first = int(m_shownAllCount);
            m_model.beginInsertRows(QModelIndex(), first, int(firstLine + count) - 1);
            m_shownAllCount = firstLine + count;
            m_model.endInsertRows();
        } else {
            m_shownAllCount = qMax(m_shownAllCount, firstLine + count);
            insertSortedRefs(m_allRefs, refs);
        }
        markBatchNew();
        emit m_model.modelFiltered(visibleCount());
        return;
    }
    markBatchNew();

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
    if (const int fileId = fileIdOf(logFile.get()); fileId >= 0) {
        m_files[fileId].index = std::move(fresh);
        m_files[fileId].shownLines = 0;
        m_textCache.invalidateFile(m_files[fileId].cacheFileId);
        // Переписанный файл — новый текст: автоматика решает заново, выбор
        // пользователя остаётся.
        if (m_files[fileId].encoding.source != FileEncoding::Source::User) {
            m_files[fileId].encoding = FileEncoding();
            m_textCache.setFileDecoder(m_files[fileId].cacheFileId, TextDecoder());
        } else {
            m_files[fileId].encoding =
                FileEncoding().withUserChoice(m_files[fileId].encoding.decoder);
        }
        if (!identityAll()) {
            m_allRefs.erase(std::remove_if(m_allRefs.begin(), m_allRefs.end(),
                                           [fileId](RowRef r) {
                                               return refFile(r) == fileId;
                                           }),
                            m_allRefs.end());
        } else {
            m_shownAllCount = 0; // единственный файл начинает с нуля
        }
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

FileEncoding IndexedLogStore::fileEncoding(const LogFilePtr& logFile) const
{
    const int fileId = fileIdOf(logFile.get());
    return fileId >= 0 ? m_files[fileId].encoding : FileEncoding();
}

void IndexedLogStore::adoptDetectedEncoding(const LogFilePtr& logFile,
                                            const FileEncoding& encoding)
{
    const int fileId = fileIdOf(logFile.get());
    if (fileId < 0)
        return;
    IndexedFile& f = m_files[fileId];
    // Пересмотр предварительного решения (шаг 3а): строки файла уже
    // показаны прежним декодером — как ручная смена, только источник
    // остаётся «авто». Первое решение reset не требует (показанные строки —
    // ASCII); выбор пользователя остаётся (FileEncoding::adopt).
    const bool revised = f.encoding.adopt(encoding);
    m_textCache.setFileDecoder(f.cacheFileId, f.encoding.decoder);
    if (revised)
        rereadWithNewDecoder();
}

void IndexedLogStore::setFileEncoding(const LogFilePtr& logFile, TextDecoder decoder)
{
    const int fileId = fileIdOf(logFile.get());
    if (fileId < 0)
        return;
    IndexedFile& f = m_files[fileId];
    const bool changed = f.encoding.decoder != decoder;
    f.encoding = f.encoding.withUserChoice(decoder);
    if (!changed)
        return;
    m_textCache.setFileDecoder(f.cacheFileId, decoder);
    rereadWithNewDecoder();
}

void IndexedLogStore::setFileEncodingState(const LogFilePtr& logFile,
                                           const FileEncoding& encoding)
{
    const int fileId = fileIdOf(logFile.get());
    if (fileId < 0)
        return;
    IndexedFile& f = m_files[fileId];
    const bool changed = f.encoding.decoder != encoding.decoder;
    f.encoding = encoding;
    if (!changed)
        return;
    m_textCache.setFileDecoder(f.cacheFileId, encoding.decoder);
    if (f.index && f.index->lineCount() > 0)
        rereadWithNewDecoder();
}

void IndexedLogStore::rereadWithNewDecoder()
{
    // Видимый набор зависит от текста только через текстовые правила. Без
    // них уровни и время те же, m_visibleRefs остаётся; с ними — пересчёт
    // заново (applyFilter отменяет джоб со старым декодером поколением и
    // сбрасывает очередь диапазонов: полный пересчёт покрывает и их).
    const bool refilter = m_model.filterRules().isActive();
    if (refilter)
        cancelPendingFilter(false);
    // Reset: строки те же, текст другой — view сбрасывает кэш отрисовки и
    // высот и возвращается к якорю-записи; подписчики (быстрый поиск, Search
    // Results, статистика, таймлайн) перезапускаются.
    m_model.beginResetModel();
    m_model.endResetModel();
    // С перефильтрацией итог сообщит джоб (modelFiltered по завершении):
    // число строк старого видимого набора — не результат.
    if (refilter)
        applyFilter();
    else
        emit m_model.modelFiltered(visibleCount());
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

    // 1. Время логической записи (без метки — «больше» любой метки, в том
    // числе отрицательной: до 1970 года).
    const qint64 ta = ia.timestampMs(ia.logicalId(la));
    const qint64 tb = ib.timestampMs(ib.logicalId(lb));
    const bool hasA = hasTimestampMs(ta), hasB = hasTimestampMs(tb);
    if (hasA && hasB) {
        if (ta != tb)
            return ta < tb;
    } else if (hasA) {
        return true;
    } else if (hasB) {
        return false;
    }

    // 2. Файл-источник: детерминированный порядок по ключу источника
    // (LogFile::orderKey, как у LogEntry::operator<).
    if (fa != fb) {
        const QString& pa = m_files[fa].logFile->orderKey();
        const QString& pb = m_files[fb].logFile->orderKey();
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

int IndexedLogStore::fileIdOf(const LogFile* file) const
{
    for (int i = 0; i < m_files.size(); ++i) {
        if (m_files[i].logFile.get() == file)
            return i;
    }
    return -1;
}

qint64 IndexedLogStore::firstLineOfRecord(const LineIndex& index, quint32 id)
{
    const qint64 total = index.lineCount();
    qint64 lo = 0, hi = total;
    while (lo < hi) {
        const qint64 mid = lo + (hi - lo) / 2;
        if (index.logicalId(mid) < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (lo < total && index.logicalId(lo) == id) ? lo : -1;
}

QStringList IndexedLogStore::filePaths() const
{
    QStringList paths;
    paths.reserve(m_files.size());
    for (const auto& f : m_files)
        paths.append(f.logFile->filePath);
    return paths;
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
    if (hasTimestampMs(ms))
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
    return hasTimestampMs(m.timestampMs) ? QDateTime::fromMSecsSinceEpoch(m.timestampMs)
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
    // Видимые строки упорядочены глобальным порядком (время → файл → запись),
    // строки без метки в конце — двоичный поиск по «ts < want». Невалидный
    // момент — «раньше всего»: ни одна метка не меньше его (kNoTimestampMs —
    // минимум qint64, а у строки без метки «меньше» всегда ложно).
    const qint64 wantMs = t.isValid() ? t.toMSecsSinceEpoch() : kNoTimestampMs;
    int lo = 0, hi = visibleCount();
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        const LogEntryMeta m = visibleMetaAt(mid);
        const bool less = hasTimestampMs(m.timestampMs) && m.timestampMs < wantMs;
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
    while (end > 0 && !hasTimestampMs(tsOfAll(end - 1)))
        --end;
    const qint64 firstMs = tsOfAll(0);
    if (end <= 0 || !hasTimestampMs(firstMs))
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

    const int fileId = fileIdOf(line->sourceFile().get());
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
    const int fileId = fileIdOf(sourceFile);
    if (fileId < 0)
        return -1;
    const LineIndex& index = *m_files[fileId].index;
    const quint32 id = quint32(logicalEntryId);
    const qint64 first = firstLineOfRecord(index, id);
    if (first < 0)
        return -1;

    // Первая ВИДИМАЯ строка этой записи.
    const qint64 total = index.lineCount();
    for (qint64 l = first; l < total && index.logicalId(l) == id; ++l) {
        const RowRef ref = makeRef(fileId, l);
        if (m_identityVisible) {
            if (identityAll())
                return int(l);
            const auto it = std::lower_bound(m_allRefs.constBegin(), m_allRefs.constEnd(),
                                             ref,
                                             [this](RowRef a, RowRef b) { return rowLess(a, b); });
            if (it != m_allRefs.constEnd() && *it == ref)
                return int(it - m_allRefs.constBegin());
        } else {
            const auto it = std::lower_bound(m_visibleRefs.constBegin(),
                                             m_visibleRefs.constEnd(), ref,
                                             [this](RowRef a, RowRef b) { return rowLess(a, b); });
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

    const int fileId = fileIdOf(sourceFile);
    if (fileId < 0)
        return -1;
    const qint64 first = firstLineOfRecord(*m_files[fileId].index, quint32(logicalEntryId));
    if (first < 0)
        return -1; // записи нет в данных вовсе

    // Запись скрыта фильтром: ближайшая видимая — первая после неё, иначе
    // последняя перед ней (семантика резидентного nearestVisibleRow).
    if (m_identityVisible)
        return -1; // тождество: скрытых записей не бывает — сюда не попадаем

    const RowRef ref = makeRef(fileId, first);
    const auto it = std::lower_bound(m_visibleRefs.constBegin(), m_visibleRefs.constEnd(),
                                     ref,
                                     [this](RowRef a, RowRef b) { return rowLess(a, b); });
    const int j = int(it - m_visibleRefs.constBegin());
    return j < int(m_visibleRefs.size()) ? j : j - 1;
}

int IndexedLogStore::placeAmongVisible(int logicalEntryId, const LogFile* sourceFile,
                                       bool* visible) const
{
    const int exact = rowForEntry(logicalEntryId, sourceFile);
    if (visible)
        *visible = exact >= 0;
    if (exact >= 0)
        return exact;

    const int fileId = fileIdOf(sourceFile);
    if (fileId < 0)
        return -1;
    const qint64 first = firstLineOfRecord(*m_files[fileId].index, quint32(logicalEntryId));
    if (first < 0 || m_identityVisible)
        return -1; // записи нет в данных (при тождестве скрытых не бывает)

    // Запись скрыта: её место — место её первой строки среди видимых, как в
    // nearestVisibleRow, но без прижатия к последней строке.
    return visiblePlace(makeRef(fileId, first));
}

int IndexedLogStore::nearestVisibleRowForLine(qint64 lineNumber,
                                              const LogFile* sourceFile) const
{
    const int visible = visibleCount();
    const int fileId = fileIdOf(sourceFile);
    if (visible == 0 || fileId < 0)
        return -1;

    // Индекс растёт в воркере раньше, чем строки показаны модели: одно-файловая
    // вкладка ограничена показанным, у слитой место ещё не показанной строки
    // всё равно находится двоичным поиском.
    qint64 total = m_files[fileId].index->lineCount();
    if (identityAll())
        total = qMin(total, m_shownAllCount);
    if (total <= 0)
        return -1;
    const qint64 line = qBound(qint64(0), lineNumber - 1, total - 1);
    return qMin(visiblePlace(makeRef(fileId, line)), visible - 1);
}

// ---------------------------------------------------------------------------
// Ключ строки (R-1)
// ---------------------------------------------------------------------------

bool IndexedLogStore::refShown(RowRef ref) const
{
    if (identityAll())
        return refLine(ref) < m_shownAllCount;
    const auto it = std::lower_bound(m_allRefs.constBegin(), m_allRefs.constEnd(), ref,
                                     [this](RowRef a, RowRef b) { return rowLess(a, b); });
    return it != m_allRefs.constEnd() && *it == ref;
}

int IndexedLogStore::visiblePlace(RowRef ref, bool* visible) const
{
    if (m_identityVisible && identityAll()) {
        // Тождество: строка видна, если показана; место — её номер.
        const qint64 line = refLine(ref);
        if (visible)
            *visible = line < m_shownAllCount;
        return int(qMin(line, m_shownAllCount));
    }
    const QVector<RowRef>& list = m_identityVisible ? m_allRefs : m_visibleRefs;
    const auto it = std::lower_bound(list.constBegin(), list.constEnd(), ref,
                                     [this](RowRef a, RowRef b) { return rowLess(a, b); });
    if (visible)
        *visible = it != list.constEnd() && *it == ref;
    return int(it - list.constBegin());
}

bool IndexedLogStore::refForKey(const RecordKey& key, RowRef* ref) const
{
    if (!key.isValid())
        return false;
    const int fileId = fileIdOf(key.file.get());
    if (fileId < 0)
        return false;
    const LineIndex& index = *m_files[fileId].index;
    const qint64 line = key.line - 1;
    if (line >= index.lineCount())
        return false;
    // Ключ совпадает целиком, как у резидентного бэкенда (его поиск идёт по
    // id записи и метке): строка того же номера из другой записи — не она.
    const quint32 logical = index.logicalId(line);
    if (key.logicalEntryId < 0 || logical != quint32(key.logicalEntryId))
        return false;
    if (index.timestampMs(logical) != key.timestampMs)
        return false;
    *ref = makeRef(fileId, line);
    return true;
}

RecordKey IndexedLogStore::keyForLine(const LogFile* file, qint64 line, qint64* offset,
                                      quint32* length) const
{
    const int fileId = fileIdOf(file);
    if (fileId < 0 || line < 1)
        return {};
    const IndexedFile& f = m_files[fileId];
    const qint64 l = line - 1;
    if (l >= f.index->lineCount() || !refShown(makeRef(fileId, l)))
        return {};
    const quint32 logical = f.index->logicalId(l);
    RecordKey key;
    key.file = f.logFile;
    key.line = line;
    key.logicalEntryId = int(logical);
    key.timestampMs = f.index->timestampMs(logical);   // kNoTimestampMs — метки нет
    if (offset)
        *offset = f.index->lineStartOffset(l);
    if (length)
        *length = f.index->lineByteLength(l);
    return key;
}

RecordKey IndexedLogStore::keyAt(int visibleRow) const
{
    if (visibleRow < 0 || visibleRow >= visibleCount())
        return {};
    const RowRef ref = rowToRef(visibleRow);
    const IndexedFile& f = m_files[refFile(ref)];
    const qint64 line = refLine(ref);
    const quint32 logical = f.index->logicalId(line);
    RecordKey key;
    key.file = f.logFile;
    key.line = line + 1;
    key.logicalEntryId = int(logical);
    key.timestampMs = f.index->timestampMs(logical);   // kNoTimestampMs — метки нет
    return key;
}

int IndexedLogStore::rowForKey(const RecordKey& key) const
{
    RowRef ref = 0;
    if (!refForKey(key, &ref))
        return -1;
    bool visible = false;
    const int place = visiblePlace(ref, &visible);
    return visible ? place : -1;
}

int IndexedLogStore::nearestRowForKey(const RecordKey& key) const
{
    const int count = visibleCount();
    RowRef ref = 0;
    if (count == 0 || !refForKey(key, &ref))
        return -1;
    bool visible = false;
    const int place = visiblePlace(ref, &visible);
    if (visible)
        return place;
    // Скрытая строка: есть ли она среди показанных. Без фильтра видимые и
    // есть все показанные — второй двоичный поиск не нужен.
    if (m_identityVisible || !refShown(ref))
        return -1;
    return qMin(place, count - 1);
}

namespace {

// Снапшот индексного хранилища для воркеров: самодостаточный (свои ридеры).
class IndexedScanSnapshotImpl final : public LogScanSnapshot::Impl {
public:
    struct FileSnap {
        LineIndexSnapshot index;
        QString path;
        LogFilePtr logFile;
        TextDecoder decoder; // кодировка на момент снапшота (F-1)
    };

    QStringList paths() const
    {
        QStringList out;
        for (const FileSnap& f : files)
            out.append(f.path);
        return out;
    }
    QVector<TextDecoder> decoders() const
    {
        QVector<TextDecoder> out;
        for (const FileSnap& f : files)
            out.append(f.decoder);
        return out;
    }

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

    LogLevel levelAt(qint64 row) const override
    {
        if (row < 0 || row >= rowCount())
            return LogLevel::Unknown;
        int fileId; qint64 line;
        resolve(row, fileId, line);
        return files[fileId].index.level(line);
    }

    qint64 lineAt(qint64 row, const LogFile** file) const override
    {
        if (row < 0 || row >= rowCount()) {
            *file = nullptr;
            return 0;
        }
        int fileId; qint64 line;
        resolve(row, fileId, line);
        *file = files[fileId].logFile.get();
        return line + 1;
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
        SequentialLineReaders readers(paths(), decoders());
        const qint64 n = rowCount();
        for (qint64 i = qMax<qint64>(0, fromRow); i < n; ++i) {
            int fileId; qint64 line;
            resolve(i, fileId, line);
            const FileSnap& f = files[fileId];
            const QString& text = readers.lineAt(
                fileId, f.index.lineStartOffset(line), f.index.lineByteLength(line));
            if (!visit(i, metaAt(i), QStringView(text)))
                return;
        }
    }

    void forEachLineBackward(qint64 fromRow,
                             const std::function<bool(qint64, const LogEntryMeta&,
                                                      QStringView)>& visit) const override
    {
        // SequentialLineReader сам ставит окно по направлению скана — обход
        // назад не перечитывает окно на каждой строке.
        SequentialLineReaders readers(paths(), decoders());
        for (qint64 i = qMin<qint64>(fromRow, rowCount() - 1); i >= 0; --i) {
            int fileId; qint64 line;
            resolve(i, fileId, line);
            const FileSnap& f = files[fileId];
            const QString& text = readers.lineAt(
                fileId, f.index.lineStartOffset(line), f.index.lineByteLength(line));
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
        SequentialLineReader reader(files[fileId].path, files[fileId].decoder);
        if (!reader.open())
            return QString();
        const FileSnap& f = files[fileId];
        return reader.lineAt(f.index.lineStartOffset(line),
                             f.index.lineByteLength(line));
    }

    std::unique_ptr<LogScanSnapshot::LineCursor> lineCursor() const override
    {
        // Свои ридеры на курсор: SequentialLineReader ставит окно по
        // направлению скана, обход назад не перечитывает окно на строку.
        class Cursor final : public LogScanSnapshot::LineCursor {
        public:
            explicit Cursor(const IndexedScanSnapshotImpl& snapshot)
                : m_snapshot(snapshot), m_readers(snapshot.paths(), snapshot.decoders()) {}
            QStringView lineAt(qint64 row) override
            {
                if (row < 0 || row >= m_snapshot.rowCount())
                    return {};
                int fileId; qint64 line;
                m_snapshot.resolve(row, fileId, line);
                const FileSnap& f = m_snapshot.files[fileId];
                return QStringView(m_readers.lineAt(fileId, f.index.lineStartOffset(line),
                                                    f.index.lineByteLength(line)));
            }
        private:
            const IndexedScanSnapshotImpl& m_snapshot;
            SequentialLineReaders m_readers;
        };
        return std::make_unique<Cursor>(*this);
    }

    LogFilePtr sourceFilePtrAt(qint64 row) const override
    {
        if (row < 0 || row >= rowCount())
            return LogFilePtr();
        int fileId; qint64 line;
        resolve(row, fileId, line);
        return files[fileId].logFile;
    }

    LogEntryFields fieldsAt(qint64 row, const QString& text) const override
    {
        // Как materializeEntry: поля — только у первичных строк.
        if (!extraction || !pattern.isValid() || row < 0 || row >= rowCount())
            return LogEntryFields();
        int fileId; qint64 line;
        resolve(row, fileId, line);
        return files[fileId].index.isPrimary(line) ? pattern.extractFields(text)
                                                   : LogEntryFields();
    }

    // Схема полей на момент снапшота (копия: воркер не делит её со store).
    LogPattern pattern;
    bool extraction = false;

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
        impl->files.append({f.index->snapshot(), f.logFile->filePath, f.logFile,
                            f.encoding.decoder});
    impl->pattern = m_fieldPattern;
    impl->extraction = m_extractionEnabled;

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
        && (!hasTimestampMs(tsMs) || tsMs < startTime.toMSecsSinceEpoch()))
        return false;
    if (endTime.isValid() && hasTimestampMs(tsMs) && tsMs > endTime.toMSecsSinceEpoch())
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
    startFilterJob(/*fullRescan=*/false, &r);
}

void IndexedLogStore::startFilterJob(bool fullRescan, const PendingRange* range)
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
        TextDecoder decoder; // копия: смена кодировки отменяет джоб поколением
    };
    auto filesIn = std::make_shared<QVector<FileInput>>();
    for (const auto& f : m_files)
        filesIn->append({f.index->snapshot(), f.logFile->filePath, f.encoding.decoder});

    // Диапазон работы: весь документ, свежий батч одного файла или явный
    // список новых строк поисковой базы.
    QVector<RowRef> workRefs;
    qint64 identityCount = 0;
    if (fullRescan) {
        if (identityAll())
            identityCount = allCount();
        else
            workRefs = m_allRefs;
    } else if (!range->refs.isEmpty()) {
        workRefs = range->refs;
    } else {
        workRefs.reserve(int(range->count));
        for (qint64 l = range->first; l < range->first + range->count; ++l)
            workRefs.append(makeRef(range->fileId, l));
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

            QStringList paths;
            QVector<TextDecoder> decoders;
            for (const FileInput& f : *filesIn) {
                paths.append(f.path);
                decoders.append(f.decoder);
            }
            SequentialLineReaders readers(std::move(paths), std::move(decoders));
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
                if (startValid && (!hasTimestampMs(tsMs) || tsMs < startMs))
                    continue;
                if (endValid && hasTimestampMs(tsMs) && tsMs > endMs)
                    continue;

                if (needText) {
                    const QString& text = readers.lineAt(
                        fileId, index.lineStartOffset(line), index.lineByteLength(line));
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
                    insertSortedRefs(m_visibleRefs, watcher->result());
                    emit m_model.modelFiltered(visibleCount());
                }
                startNextPendingRange();
            });
    watcher->setFuture(future);
}

void IndexedLogStore::insertSortedRefs(QVector<RowRef>& list, const QVector<RowRef>& refs)
{
    if (refs.isEmpty())
        return;
    // Прошедшие строки приходят в порядке своего файла. Серии вставок ниже
    // (upper_bound от searchFrom, std::merge) требуют порядка ВКЛАДКИ — на
    // слитой вкладке с файлом «не по порядку» это разные порядки.
    const auto less = [this](RowRef a, RowRef b) { return rowLess(a, b); };
    QVector<RowRef> sorted;
    const QVector<RowRef>* source = &refs;
    if (!std::is_sorted(refs.constBegin(), refs.constEnd(), less)) {
        sorted = refs;
        std::sort(sorted.begin(), sorted.end(), less);
        source = &sorted;
    }

    // В списке строки обычно встают в конец (tail-догрузка). Общий случай —
    // серии вставок, как в резидентном insertFilteredSorted. Строку, которая
    // уже есть (переоценённый хвост поисковой базы успели вставить раньше
    // джоба), не дублируем: равные по порядку соседствуют, upper_bound
    // указывает сразу за ними.
    struct Run { int pos; int first; int count; };
    QVector<Run> runs;
    QVector<RowRef> fresh;
    fresh.reserve(source->size());
    int searchFrom = 0;
    for (const RowRef ref : *source) {
        const auto it = std::upper_bound(list.constBegin() + searchFrom, list.constEnd(),
                                         ref, less);
        const int pos = int(it - list.constBegin());
        searchFrom = pos;
        if (pos > 0 && list.at(pos - 1) == ref)
            continue;
        if (!runs.isEmpty() && runs.last().pos == pos)
            ++runs.last().count;
        else
            runs.append({pos, int(fresh.size()), 1});
        fresh.append(ref);
    }
    if (fresh.isEmpty())
        return;

    if (runs.size() > 64) {
        m_model.beginResetModel();
        // Полная пересборка честнее сотен вставок в середину — но заново
        // фильтровать не нужно: сливаем два отсортированных списка.
        QVector<RowRef> merged;
        merged.reserve(list.size() + fresh.size());
        std::merge(list.constBegin(), list.constEnd(), fresh.constBegin(), fresh.constEnd(),
                   std::back_inserter(merged), less);
        list = std::move(merged);
        m_model.endResetModel();
        return;
    }

    int shift = 0;
    for (const Run& run : runs) {
        const int first = run.pos + shift;
        m_model.beginInsertRows(QModelIndex(), first, first + run.count - 1);
        list.insert(first, run.count, RowRef(0));
        for (int k = 0; k < run.count; ++k)
            list[first + k] = fresh[run.first + k];
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
