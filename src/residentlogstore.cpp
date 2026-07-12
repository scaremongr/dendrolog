#include "residentlogstore.h"
#include "logmodel.h"

#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>

// ---------------------------------------------------------------------------
// Мутации
// ---------------------------------------------------------------------------

void ResidentLogStore::appendEntries(const QVector<std::shared_ptr<LogEntry>>& entries)
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
    if (hadPrevious && visibleCount() > 0)
        emit m_model.dataChanged(m_model.index(0, 0),
                                 m_model.index(visibleCount() - 1, 0),
                                 {LogModel::IsNewRole});
}

void ResidentLogStore::mergeEntries(const QVector<std::shared_ptr<LogEntry>>& sortedBatch)
{
    if (sortedBatch.isEmpty())
        return;
    mergeSortedBatch(sortedBatch);
}

void ResidentLogStore::mergeSortedBatch(const QVector<std::shared_ptr<LogEntry>>& sortedBatch)
{
    // До инвалидации кэша: нужен для детекции перехода «один файл → несколько»
    const int oldUniqueFiles = uniqueSourceFileCount();

    for (const auto& entry : sortedBatch) {
        if (entry && entry->sourceFile())
            m_model.ensureFileColor(entry->sourceFile()->filePath);
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
            const int first = int(m_filteredEntries.size());
            m_model.beginInsertRows(QModelIndex(), first,
                                    first + int(passing.size()) - 1);
            m_filteredEntries.append(passing);
            m_model.endInsertRows();
        } else {
            insertFilteredSorted(passing);
        }
        emit m_model.modelFiltered(int(m_filteredEntries.size()));
    }

    // Появился второй файл — FileBadgeRole у ВСЕХ строк меняется с invalid на
    // плашку имени файла. Явно уведомляем view: при инкрементальном append
    // строки, закэшированные до этого батча, иначе остались бы без плашек.
    if (oldUniqueFiles <= 1 && uniqueSourceFileCount() > 1 && !m_filteredEntries.isEmpty()) {
        emit m_model.dataChanged(m_model.index(0, 0),
                                 m_model.index(int(m_filteredEntries.size()) - 1, 0),
                                 {LogModel::FileBadgeRole});
    }

    // Фоновая перефильтрация (если шла) работала со снапшотом без этого
    // батча — перезапускаем её на актуальных данных.
    if (m_filterJobActive) {
        cancelPendingFilter(false);
        startFilterJob();
    }
}

void ResidentLogStore::insertFilteredSorted(const QVector<std::shared_ptr<LogEntry>>& passing)
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
        const int pos = int(it - m_filteredEntries.constBegin());
        if (!runs.isEmpty() && runs.last().pos == pos)
            ++runs.last().count;
        else
            runs.append({pos, i, 1});
        searchFrom = pos;
    }

    // Патологическое чередование строк двух файлов: сотни вставок в середину
    // дороже одной полной перестройки (и для модели, и для view).
    if (runs.size() > 64) {
        m_model.beginResetModel();
        rebuildFilteredEntries();
        m_model.endResetModel();
        return;
    }

    int shift = 0;
    for (const Run& run : runs) {
        const int first = run.pos + shift;
        m_model.beginInsertRows(QModelIndex(), first, first + run.count - 1);
        m_filteredEntries.insert(first, run.count, std::shared_ptr<LogEntry>());
        for (int k = 0; k < run.count; ++k)
            m_filteredEntries[first + k] = passing[run.first + k];
        m_model.endInsertRows();
        shift += run.count;
    }
}

void ResidentLogStore::removeEntriesForFile(const QString& filePath)
{
    if (m_allEntries.isEmpty())
        return;

    // Полная синхронная перестройка ниже даёт консистентный список сама —
    // фоновый джоб (если шёл) больше не нужен и не должен примениться.
    cancelPendingFilter(false);

    // Чистим до удаления записей, чтобы не держать висячие указатели.
    m_newEntries.clear();

    m_model.beginResetModel();
    m_allEntries.erase(
        std::remove_if(m_allEntries.begin(), m_allEntries.end(),
            [&filePath](const std::shared_ptr<LogEntry>& e) {
                return e && e->sourceFile() && e->sourceFile()->filePath == filePath;
            }),
        m_allEntries.end());

    m_cachedUniqueSourceFileCount = -1;
    rebuildFilteredEntries();
    m_model.endResetModel();
    m_filteredListStale = false; // перестроено по текущим настройкам
    emit m_model.modelFiltered(int(m_filteredEntries.size()));
}

void ResidentLogStore::setEntries(const QVector<std::shared_ptr<LogEntry>>& entries)
{
    // Полная замена данных: фоновый джоб (если шёл) устарел, а старые
    // указатели в m_newEntries недействительны.
    cancelPendingFilter(false);
    m_newEntries.clear();

    m_model.beginResetModel();
    m_allEntries = entries;
    m_cachedUniqueSourceFileCount = -1;

    // Полное обновление: цвета файлов назначаются заново в порядке появления.
    m_model.resetFileColors();
    for (const auto& entry : m_allEntries) {
        if (entry && entry->sourceFile())
            m_model.ensureFileColor(entry->sourceFile()->filePath);
    }

    rebuildFilteredEntries();
    m_model.endResetModel();
    m_filteredListStale = false; // перестроено по текущим настройкам
    emit m_model.modelFiltered(int(m_filteredEntries.size()));
}

// ---------------------------------------------------------------------------
// Чтение
// ---------------------------------------------------------------------------

int ResidentLogStore::uniqueSourceFileCount() const
{
    if (m_cachedUniqueSourceFileCount != -1)
        return m_cachedUniqueSourceFileCount;

    QSet<LogFilePtr> uniqueFiles;
    for (const auto& entry : m_allEntries) {
        if (entry && entry->sourceFile())
            uniqueFiles.insert(entry->sourceFile());
    }
    m_cachedUniqueSourceFileCount = uniqueFiles.size();
    return m_cachedUniqueSourceFileCount;
}

std::shared_ptr<LogEntry> ResidentLogStore::entryAt(int visibleRow) const
{
    if (visibleRow < 0 || visibleRow >= m_filteredEntries.size())
        return {};
    return m_filteredEntries.at(visibleRow);
}

LogEntryMeta ResidentLogStore::visibleMetaAt(int row) const
{
    if (row < 0 || row >= m_filteredEntries.size() || !m_filteredEntries.at(row))
        return LogEntryMeta{};
    return LogScanSnapshot::metaFor(*m_filteredEntries.at(row));
}

QDateTime ResidentLogStore::visibleTimestampAt(int row) const
{
    if (row < 0 || row >= m_filteredEntries.size() || !m_filteredEntries.at(row))
        return QDateTime();
    return m_filteredEntries.at(row)->timestamp();
}

LogLevel ResidentLogStore::visibleLevelAt(int row) const
{
    if (row < 0 || row >= m_filteredEntries.size() || !m_filteredEntries.at(row))
        return LogLevel::Unknown;
    return m_filteredEntries.at(row)->level();
}

QString ResidentLogStore::messageAt(int visibleRow) const
{
    if (visibleRow < 0 || visibleRow >= m_filteredEntries.size()
        || !m_filteredEntries.at(visibleRow))
        return QString();
    return m_filteredEntries.at(visibleRow)->message();
}

int ResidentLogStore::rawTextLengthAt(int visibleRow) const
{
    if (visibleRow < 0 || visibleRow >= m_filteredEntries.size()
        || !m_filteredEntries.at(visibleRow))
        return 0;
    return int(m_filteredEntries.at(visibleRow)->message().length());
}

bool ResidentLogStore::isNewAt(int visibleRow) const
{
    if (visibleRow < 0 || visibleRow >= m_filteredEntries.size())
        return false;
    return m_newEntries.contains(m_filteredEntries.at(visibleRow).get());
}

int ResidentLogStore::firstVisibleRowAtOrAfter(const QDateTime& t) const
{
    const auto it = std::lower_bound(
        m_filteredEntries.constBegin(), m_filteredEntries.constEnd(), t,
        [](const std::shared_ptr<LogEntry>& e, const QDateTime& time) {
            return e && e->timestamp().isValid() && e->timestamp() < time;
        });
    return int(it - m_filteredEntries.constBegin());
}

QPair<QDateTime, QDateTime> ResidentLogStore::fullTimeRange() const
{
    // Список отсортирован по времени, записи без валидной метки — в конце:
    // min — первая запись, max — последняя валидная с конца.
    int end = int(m_allEntries.size());
    while (end > 0) {
        const auto& e = m_allEntries[end - 1];
        if (e && e->timestamp().isValid())
            break;
        --end;
    }
    if (end <= 0 || !m_allEntries.first()
        || !m_allEntries.first()->timestamp().isValid())
        return {};
    return { m_allEntries.first()->timestamp(),
             m_allEntries[end - 1]->timestamp() };
}

QStringList ResidentLogStore::sampleMessages(int maxCount) const
{
    QStringList samples;
    for (const auto& entry : m_allEntries) {
        if (!entry || entry->message().trimmed().isEmpty())
            continue;
        samples.append(entry->message());
        if (samples.size() >= maxCount)
            break;
    }
    return samples;
}

QVector<std::shared_ptr<LogEntry>> ResidentLogStore::logicalRecordLines(
    const std::shared_ptr<LogEntry>& line, int maxLines) const
{
    if (!line)
        return {};

    // Свободный текст (не-лог файл или преамбула до первой настоящей записи):
    // «логическая запись» тут номинальна — весь файл слипся в запись #0, и
    // панель деталей печатала бы его целиком. Единица показа — сама строка.
    if (line->isPlainText())
        return { line };

    // m_allEntries отсортирован logEntryPtrLess, а строки одной логической
    // записи делят timestamp/sourceFile/logicalEntryId — значит, лежат в
    // векторе подряд: lower_bound + расширение в обе стороны.
    const auto sameLogicalEntry = [&line](const std::shared_ptr<LogEntry>& e) {
        return e && e->logicalEntryId() == line->logicalEntryId()
            && e->sourceFile() == line->sourceFile();
    };

    auto it = std::lower_bound(m_allEntries.constBegin(), m_allEntries.constEnd(),
                               line, logEntryPtrLess);
    if (it == m_allEntries.constEnd() || !sameLogicalEntry(*it))
        return { line }; // страховка: запись не нашлась на ожидаемом месте

    while (it != m_allEntries.constBegin() && sameLogicalEntry(*(it - 1)))
        --it;

    QVector<std::shared_ptr<LogEntry>> lines;
    for (; it != m_allEntries.constEnd() && sameLogicalEntry(*it)
           && lines.size() < maxLines; ++it)
        lines.append(*it);
    return lines;
}

int ResidentLogStore::rowForEntry(int logicalEntryId, const LogFile* sourceFile) const
{
    for (int i = 0; i < m_filteredEntries.size(); ++i) {
        const auto& e = m_filteredEntries[i];
        if (e->logicalEntryId() == logicalEntryId && e->sourceFile().get() == sourceFile) {
            return i;
        }
    }
    return -1;
}

int ResidentLogStore::nearestVisibleRow(int logicalEntryId, const LogFile* sourceFile) const
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
        if (e->logicalEntryId() == logicalEntryId && e->sourceFile().get() == sourceFile) {
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

int ResidentLogStore::findNextOccurrence(const QString& text, int startRow,
                                         Qt::CaseSensitivity cs, bool wrapAround) const
{
    if (text.isEmpty() || m_filteredEntries.isEmpty())
        return -1;

    const int numRows = int(m_filteredEntries.size());
    const int firstRow = qBound(0, startRow + 1, numRows);

    // Search from startRow + 1 to the end
    for (int i = firstRow; i < numRows; ++i) {
        if (m_filteredEntries[i] && m_filteredEntries[i]->message().contains(text, cs))
            return i;
    }

    // If wrapAround is true and not found, search from the beginning to startRow
    if (wrapAround) {
        for (int i = 0; i <= startRow && i < numRows; ++i) {
            if (m_filteredEntries[i] && m_filteredEntries[i]->message().contains(text, cs))
                return i;
        }
    }
    return -1;
}

int ResidentLogStore::findPreviousOccurrence(const QString& text, int startRow,
                                             Qt::CaseSensitivity cs, bool wrapAround) const
{
    if (text.isEmpty() || m_filteredEntries.isEmpty())
        return -1;

    const int numRows = int(m_filteredEntries.size());
    int currentRow = startRow - 1;
    if (startRow < 0 || startRow >= numRows) { // If startRow is out of bounds, start from the end
        currentRow = numRows - 1;
    }

    // Search from startRow - 1 to the beginning
    for (int i = currentRow; i >= 0; --i) {
        if (m_filteredEntries[i] && m_filteredEntries[i]->message().contains(text, cs))
            return i;
    }

    // If wrapAround is true and not found, search from the end to startRow
    if (wrapAround) {
        for (int i = numRows - 1; i >= startRow && i >= 0; --i) {
            if (m_filteredEntries[i] && m_filteredEntries[i]->message().contains(text, cs))
                return i;
        }
    }
    return -1;
}

LogScanSnapshot ResidentLogStore::scanSnapshot(bool filteredOnly) const
{
    return LogScanSnapshot::fromEntries(filteredOnly ? m_filteredEntries
                                                     : m_allEntries);
}

// ---------------------------------------------------------------------------
// Фильтрация
// ---------------------------------------------------------------------------

// Общая проверка фильтров. Свободная функция, потому что фоновый воркер
// работает со СНАПШОТАМИ настроек и не имеет права читать члены модели.
static bool entryPassesFilters(const std::shared_ptr<LogEntry>& entry,
                               const QSet<LogLevel>& levels,
                               const QDateTime& startTime,
                               const QDateTime& endTime,
                               const FilterRuleSet& rules)
{
    if (!entry) {
        return false;
    }

    if (!levels.isEmpty() && !levels.contains(entry->level())) {
        return false;
    }

    if (startTime.isValid() && entry->timestamp() < startTime) {
        return false;
    }
    if (endTime.isValid() && entry->timestamp() > endTime) {
        return false;
    }

    if (rules.isActive() && !rules.matches(*entry)) {
        return false;
    }

    return true;
}

bool ResidentLogStore::passesFilters(const std::shared_ptr<LogEntry>& entry) const
{
    return entryPassesFilters(entry, m_model.logLevelFilter(),
                              m_model.startTimeFilter(), m_model.endTimeFilter(),
                              m_model.filterRules());
}

void ResidentLogStore::rebuildFilteredEntries()
{
    m_filteredEntries.clear();
    m_filteredEntries.reserve(m_allEntries.size());

    for (const auto& entry : m_allEntries) {
        if (passesFilters(entry)) {
            m_filteredEntries.push_back(entry);
        }
    }
}

void ResidentLogStore::applyFilter()
{
    cancelPendingFilter(false);

    // Небольшие данные фильтруем синхронно: мгновенно и без промежуточного
    // состояния. Большие — в пуле потоков, чтобы не замораживать GUI.
    if (m_allEntries.size() < kAsyncFilterThreshold) {
        m_model.beginResetModel();
        rebuildFilteredEntries();
        m_model.endResetModel();
        m_filteredListStale = false;
        emit m_model.modelFiltered(int(m_filteredEntries.size()));
        return;
    }
    startFilterJob();
}

void ResidentLogStore::reapplyFilterIfStale()
{
    if (m_filteredListStale)
        applyFilter();
}

void ResidentLogStore::startFilterJob()
{
    using FilterResult = QVector<std::shared_ptr<LogEntry>>;

    m_filterJobActive = true;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_filterJobCancel = cancel;

    // Снапшоты: COW-копия вектора shared_ptr держит записи живыми на время
    // работы воркера, копии настроек отвязывают его от изменений модели.
    const QVector<std::shared_ptr<LogEntry>> entries = m_allEntries;
    const QSet<LogLevel> levels = m_model.logLevelFilter();
    const QDateTime startTime = m_model.startTimeFilter();
    const QDateTime endTime = m_model.endTimeFilter();
    const FilterRuleSet rules = m_model.filterRules();

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
    auto* watcher = new QFutureWatcher<FilterResult>(&m_model);
    const int generation = m_filterGeneration;
    std::weak_ptr<int> alive = m_aliveGuard;
    QObject::connect(watcher, &QFutureWatcher<FilterResult>::finished, &m_model,
            [this, watcher, generation, alive]() {
                watcher->deleteLater();
                if (alive.expired())
                    return; // store уже заменён другим бэкендом
                if (generation != m_filterGeneration)
                    return; // результат устарел (отменён или заменён новым джобом)
                m_filterJobActive = false;
                m_model.beginResetModel();
                m_filteredEntries = watcher->result();
                m_model.endResetModel();
                m_filteredListStale = false;
                emit m_model.modelFiltered(int(m_filteredEntries.size()));
            });
    watcher->setFuture(future);
}

void ResidentLogStore::cancelPendingFilter(bool wait)
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
