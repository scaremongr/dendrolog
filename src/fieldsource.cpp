#include "fieldsource.h"
#include "logfile.h"

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

FieldSource::FieldSource(QObject* parent)
    : QObject(parent)
    , m_counters(std::make_shared<FieldSourceCounters>())
{
}

void FieldSource::setSchemas(const FileSchemaSet& schemas)
{
    if (m_schemas == schemas)
        return;
    m_schemas = schemas;
    ++m_generation;
    m_recent.clear();
    m_old.clear();
    emit schemasChanged();
}

FieldLookup FieldSource::fieldsFor(const RecordKey& key, QStringView text)
{
    return fieldsFor(key.file.get(), key.line, text);
}

FieldLookup FieldSource::fieldsFor(const LogFile* file, qint64 line, QStringView text)
{
    const FileSchema* schema = m_schemas.forFile(file);
    if (!schema || !schema->isValid())
        return FieldLookup();

    const CacheKey key{ file, line };
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

    // Шаг 6: Background-извлекатель — Pending и очередь фонового расчёта.
    m_counters->guiExtracts.fetch_add(1, std::memory_order_relaxed);
    FieldLookup lookup;
    if (FieldExtractor::accepted(schema->extract(text, &lookup.fields)))
        lookup.state = FieldLookup::State::Ready;
    else
        lookup.fields = LogEntryFields();

    // Вытеснение — до вставки (Qt 6 QHash: erase/rehash инвалидирует
    // итераторы; здесь их уже нет, но порядок тот же, что у кэшей чанков).
    if (m_recent.size() >= kCacheLines) {
        m_old = std::move(m_recent);
        m_recent = QHash<CacheKey, CacheItem>();
    }
    m_recent.insert(key, CacheItem{ text.toString(), lookup });
    return lookup;
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
}

FieldSource::Stats FieldSource::stats() const
{
    Stats s;
    s.guiExtracts = m_counters->guiExtracts.load(std::memory_order_relaxed);
    s.scanExtracts = m_counters->scanExtracts.load(std::memory_order_relaxed);
    s.cacheHits = m_counters->cacheHits.load(std::memory_order_relaxed);
    return s;
}

void FieldSource::resetStats()
{
    m_counters->guiExtracts.store(0);
    m_counters->scanExtracts.store(0);
    m_counters->cacheHits.store(0);
}
