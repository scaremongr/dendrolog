#include "fieldsource.h"
#include "logfile.h"

#include <QFutureWatcher>
#include <QGlobalStatic>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

// ---------------------------------------------------------------------------
// FieldScanner
// ---------------------------------------------------------------------------

bool FieldScanner::hasSchemas() const
{
    if (m_schemas.fallback && m_schemas.fallback->isValid())
        return true;
    for (const FileSchemaPtr& schema : m_schemas.byPath)
        if (schema && schema->isValid())
            return true;
    return false;
}

FieldExtractor::Outcome FieldScanner::extract(const FileSchema* schema, QStringView line,
                                              LogEntryFields* out, int upToColumn) const
{
    if (!schema) {
        out->spans.clear();
        return FieldExtractor::Outcome::Rejected;
    }
    if (m_counters)
        m_counters->scanExtracts.fetch_add(1, std::memory_order_relaxed);
    return schema->extract(line, out, upToColumn);
}

// ---------------------------------------------------------------------------
// FieldRecordCursor
// ---------------------------------------------------------------------------

const QString& FieldRecordCursor::headText()
{
    if (!m_headKnown) {
        m_head = m_fetch ? m_fetch(m_context, m_file, m_line) : QString();
        m_headKnown = true;
    }
    return m_head;
}

const LogEntryFields* FieldRecordCursor::fieldsUpTo(int column)
{
    const FileSchema* fileSchema = schema();
    if (!fileSchema || !fileSchema->has(column))
        return nullptr;
    const int need = column < fileSchema->fieldOf.size() ? fileSchema->fieldOf[column] : -1;
    // Уже разобрано до этого поля схемы (или целиком); строка не принята —
    // ответ не зависит от того, докуда разбирать.
    if (m_extractedUpTo != kNotExtracted
        && (m_extractedUpTo == -1 || m_extractedUpTo >= need || m_fields.spans.isEmpty()))
        return &m_fields;
    m_scanner->extract(fileSchema, headText(), &m_fields, column);
    m_extractedUpTo = need;
    return &m_fields;
}

// ---------------------------------------------------------------------------
// FieldSource
// ---------------------------------------------------------------------------

namespace {

// Общий пул фоновых заданий полей (шаг 6): потоки пониженного приоритета,
// не больше половины ядер — показ и загрузка главнее. Q_GLOBAL_STATIC:
// при выходе пул дожидается своих заданий (они короткие — пачка строк; у
// закрытой вкладки задание обрывается флагом отмены между строками).
struct BackgroundFieldPool {
    QThreadPool pool;
    BackgroundFieldPool()
    {
        pool.setMaxThreadCount(qMax(2, QThread::idealThreadCount() / 2));
        pool.setThreadPriority(QThread::LowPriority);
    }
};
Q_GLOBAL_STATIC(BackgroundFieldPool, g_backgroundFieldPool)

bool hasBackgroundExtractor(const FileSchemaSet& schemas)
{
    const auto slow = [](const FileSchemaPtr& s) {
        return s && s->isValid() && s->extractor->cost() == FieldExtractor::Cost::Background;
    };
    if (slow(schemas.fallback))
        return true;
    for (const FileSchemaPtr& s : schemas.byPath)
        if (slow(s))
            return true;
    return false;
}

} // namespace

QThreadPool* FieldSource::backgroundPool()
{
    return &g_backgroundFieldPool()->pool;
}

FieldSource::FieldSource(QObject* parent)
    : QObject(parent)
    , m_counters(std::make_shared<FieldSourceCounters>())
{
    m_readyTimer = new QTimer(this);
    m_readyTimer->setSingleShot(true);
    m_readyTimer->setInterval(kReadyIntervalMs);
    connect(m_readyTimer, &QTimer::timeout, this, &FieldSource::flushReady);
}

FieldSource::~FieldSource()
{
    // Задание в полёте держит свои копии (текст, схема, счётчики) и модель
    // не трогает: ждать его не нужно, флаг обрывает его между строками.
    if (m_cancel)
        m_cancel->store(true);
}

void FieldSource::setSchemas(const FileSchemaSet& schemas)
{
    if (m_schemas == schemas)
        return;
    m_schemas = schemas;
    m_hasBackground = hasBackgroundExtractor(m_schemas);
    ++m_generation;
    m_recent.clear();
    m_old.clear();
    dropBackground();
    emit schemasChanged();
}

void FieldSource::insertCache(const CacheKey& key, const QString& text, const FieldLookup& lookup)
{
    // Вытеснение — до вставки (Qt 6 QHash: erase/rehash инвалидирует
    // итераторы; здесь их уже нет, но порядок тот же, что у кэшей чанков).
    if (m_recent.size() >= kCacheLines) {
        m_old = std::move(m_recent);
        m_recent = QHash<CacheKey, CacheItem>();
    }
    m_old.remove(key);
    m_recent.insert(key, CacheItem{ text, lookup });
}

FieldLookup FieldSource::fieldsFor(const RecordKey& recordKey, QStringView text)
{
    const LogFile* file = recordKey.file.get();
    const FileSchema* schema = m_schemas.forFile(file);
    if (!schema || !schema->isValid())
        return FieldLookup();

    const CacheKey key{ file, recordKey.line };
    auto it = m_recent.find(key);
    if (it != m_recent.end() && QStringView(it->text) == text) {
        m_counters->cacheHits.fetch_add(1, std::memory_order_relaxed);
        return it->lookup;
    }
    it = m_old.find(key);
    if (it != m_old.end() && QStringView(it->text) == text) {
        m_counters->cacheHits.fetch_add(1, std::memory_order_relaxed);
        CacheItem item = std::move(it.value());
        m_old.erase(it);
        const FieldLookup lookup = item.lookup;
        m_recent.insert(key, std::move(item));
        return lookup;
    }

    // Background-извлекатель на GUI-потоке не зовётся (шаг 6): строка — в
    // очередь, показ — сырой текст, по готовности — fieldsReady.
    if (schema->extractor->cost() == FieldExtractor::Cost::Background) {
        FieldLookup pending;
        pending.state = FieldLookup::State::Pending;
        const auto flying = m_inFlightIndex.constFind(key);
        if (flying != m_inFlightIndex.constEnd()
            && QStringView(m_inFlight[flying.value()].text) == text)
            return pending;   // уже считается
        FileSchemaPtr schemaPtr = file ? m_schemas.forPath(file->filePath) : m_schemas.fallback;
        if (!schemaPtr || schemaPtr.get() != schema)
            return pending;   // не может быть: forFile — тот же поиск по пути
        enqueue(key, recordKey, text, schemaPtr);
        return pending;
    }

    m_counters->guiExtracts.fetch_add(1, std::memory_order_relaxed);
    FieldLookup lookup;
    if (FieldExtractor::accepted(schema->extract(text, &lookup.fields)))
        lookup.state = FieldLookup::State::Ready;
    else
        lookup.fields = LogEntryFields();
    insertCache(key, text.toString(), lookup);
    return lookup;
}

void FieldSource::enqueue(const CacheKey& key, const RecordKey& recordKey, QStringView text,
                          const FileSchemaPtr& schema)
{
    auto it = m_queue.find(key);
    if (it != m_queue.end()) {
        // Запрошена снова — свежая; текст мог смениться (дописан хвост).
        it->stamp = ++m_requestClock;
        if (QStringView(it->text) != text)
            it->text = text.toString();
        it->key = recordKey;
    } else {
        m_queue.insert(key, QueueItem{ recordKey, text.toString(), schema, ++m_requestClock });
        ++m_queued;
        if (m_queue.size() > kQueueLines)
            pruneQueue();
    }
    if (!m_startPosted && !m_watcher) {
        // Не сразу: строки одной отрисовки соберутся в одну пачку.
        m_startPosted = true;
        QMetaObject::invokeMethod(this, &FieldSource::startBatch, Qt::QueuedConnection);
    }
}

void FieldSource::pruneQueue()
{
    // Отсев: остаются kQueueLines * 3/4 запрошенных последними (экран и
    // соседние), прокрученные мимо выбрасываются. Порог с запасом — чтобы
    // не сортировать очередь на каждую новую строку.
    QVector<quint64> stamps;
    stamps.reserve(m_queue.size());
    for (const QueueItem& item : std::as_const(m_queue))
        stamps.append(item.stamp);
    const int keep = kQueueLines * 3 / 4;
    std::nth_element(stamps.begin(), stamps.begin() + (stamps.size() - keep), stamps.end());
    const quint64 threshold = stamps[stamps.size() - keep];
    for (auto it = m_queue.begin(); it != m_queue.end();) {
        if (it->stamp < threshold) {
            it = m_queue.erase(it);
            ++m_dropped;
        } else {
            ++it;
        }
    }
}

void FieldSource::startBatch()
{
    m_startPosted = false;
    if (m_watcher || m_queue.isEmpty())
        return;

    // Последние запрошенные первыми.
    QVector<QueueItem> items;
    items.reserve(m_queue.size());
    for (auto it = m_queue.cbegin(); it != m_queue.cend(); ++it)
        items.append(it.value());
    std::sort(items.begin(), items.end(),
              [](const QueueItem& a, const QueueItem& b) { return a.stamp > b.stamp; });
    if (items.size() > kBatchLines)
        items.resize(kBatchLines);

    QVector<BatchLine> lines;
    lines.reserve(items.size());
    m_inFlightIndex.clear();
    for (int i = 0; i < items.size(); ++i) {
        const CacheKey key{ items[i].key.file.get(), items[i].key.line };
        m_queue.remove(key);
        m_inFlightIndex.insert(key, i);
        lines.append(BatchLine{ items[i].text, items[i].schema });
    }
    m_inFlight = std::move(items);
    m_inFlightGeneration = m_generation;
    ++m_batches;

    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_cancel = cancel;
    const std::shared_ptr<FieldSourceCounters> counters = m_counters;
    QFuture<BatchResult> future = QtConcurrent::run(
        backgroundPool(), [lines, cancel, counters]() -> BatchResult {
            BatchResult result;
            result.lookups.reserve(lines.size());
            for (const BatchLine& line : lines) {
                if (cancel->load(std::memory_order_relaxed))
                    break;
                counters->backgroundExtracts.fetch_add(1, std::memory_order_relaxed);
                FieldLookup lookup;
                if (FieldExtractor::accepted(line.schema->extract(line.text, &lookup.fields)))
                    lookup.state = FieldLookup::State::Ready;
                else
                    lookup.fields = LogEntryFields();
                result.lookups.append(std::move(lookup));
            }
            return result;
        });
    m_watcher = new QFutureWatcher<BatchResult>(this);
    connect(m_watcher, &QFutureWatcher<BatchResult>::finished, this, &FieldSource::finishBatch);
    m_watcher->setFuture(future);
}

void FieldSource::finishBatch()
{
    QFutureWatcher<BatchResult>* watcher = m_watcher;
    m_watcher = nullptr;
    if (!watcher)
        return;
    watcher->deleteLater();
    const BatchResult result = watcher->result();
    const QVector<QueueItem> items = std::move(m_inFlight);
    m_inFlight.clear();
    m_inFlightIndex.clear();

    if (m_inFlightGeneration != m_generation) {
        ++m_discarded;      // схема сменилась или строки сброшены посреди расчёта
    } else {
        const int n = int(qMin(result.lookups.size(), items.size()));
        for (int i = 0; i < n; ++i) {
            const QueueItem& item = items[i];
            insertCache(CacheKey{ item.key.file.get(), item.key.line }, item.text, result.lookups[i]);
            m_readyKeys.append(item.key);
        }
        if (!m_readyKeys.isEmpty() && !m_readyTimer->isActive())
            m_readyTimer->start();
    }
    if (!m_queue.isEmpty())
        startBatch();
}

void FieldSource::flushReady()
{
    if (m_readyKeys.isEmpty())
        return;
    const QVector<RecordKey> keys = std::move(m_readyKeys);
    m_readyKeys.clear();
    ++m_readySignals;
    emit fieldsReady(keys);
}

void FieldSource::dropBackground()
{
    m_queue.clear();
    m_readyKeys.clear();
    m_readyTimer->stop();
    // Строки задания в полёте больше не «считаются»: новый запрос той же
    // строки встаёт в очередь заново (иначе ответ отброшенного задания
    // оставил бы её сырой до следующей отрисовки).
    m_inFlightIndex.clear();
    // Задание в полёте доработает впустую: итог отбросит поколение
    // (m_inFlightGeneration != m_generation), флаг обрывает его раньше.
    if (m_cancel)
        m_cancel->store(true);
}

bool FieldSource::hasPendingWork() const
{
    return !m_queue.isEmpty() || m_watcher || !m_readyKeys.isEmpty() || m_startPosted;
}

void FieldSource::fileReset(const QString& filePath)
{
    Q_UNUSED(filePath);
    allReset();
}

void FieldSource::allReset()
{
    m_recent.clear();
    m_old.clear();
    if (m_hasBackground || hasPendingWork()) {
        ++m_generation;
        dropBackground();
        emit backgroundDropped();
    }
}

FieldSource::Stats FieldSource::stats() const
{
    Stats s;
    s.guiExtracts = m_counters->guiExtracts.load(std::memory_order_relaxed);
    s.scanExtracts = m_counters->scanExtracts.load(std::memory_order_relaxed);
    s.cacheHits = m_counters->cacheHits.load(std::memory_order_relaxed);
    s.backgroundExtracts = m_counters->backgroundExtracts.load(std::memory_order_relaxed);
    s.queued = m_queued;
    s.dropped = m_dropped;
    s.batches = m_batches;
    s.discarded = m_discarded;
    s.readySignals = m_readySignals;
    return s;
}

void FieldSource::resetStats()
{
    m_counters->guiExtracts.store(0);
    m_counters->scanExtracts.store(0);
    m_counters->cacheHits.store(0);
    m_counters->backgroundExtracts.store(0);
    m_queued = m_dropped = m_batches = m_discarded = m_readySignals = 0;
}
