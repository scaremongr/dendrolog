#include "fieldreextraction.h"
#include "logfile.h"
#include "logpattern.h"

#include <QtConcurrent/QtConcurrentMap>
#include <QtConcurrent/QtConcurrentRun>

FieldReextraction::FieldReextraction(QVector<std::shared_ptr<LogEntry>> entries,
                                     FileSchemaSet schemas, QObject* parent)
    : QObject(parent)
    , m_jobs(std::make_shared<QVector<Job>>())
    , m_schemas(std::make_shared<const FileSchemaSet>(std::move(schemas)))
{
    m_jobs->reserve(entries.size());
    // Схема записи — по её файлу; записи одного файла идут подряд не всегда,
    // поэтому последний файл запоминается (слитая вкладка — лишний поиск
    // только на смене файла).
    const LogFile* lastFile = nullptr;
    const FileSchema* lastSchema = nullptr;
    for (auto& entry : entries) {
        if (!entry)
            continue;
        const LogFile* file = entry->sourceFile().get();
        if (file != lastFile || !lastFile) {
            lastFile = file;
            lastSchema = m_schemas->forFile(file);
            if (lastSchema && !lastSchema->isValid())
                lastSchema = nullptr;
        }
        m_jobs->append({std::move(entry), lastSchema, LogEntryFields()});
    }
    connect(&m_watcher, &QFutureWatcher<void>::progressRangeChanged,
            this, &FieldReextraction::progressRangeChanged);
    connect(&m_watcher, &QFutureWatcher<void>::progressValueChanged,
            this, &FieldReextraction::progressValueChanged);
    connect(&m_watcher, &QFutureWatcher<void>::finished,
            this, &FieldReextraction::finished);
}

FieldReextraction::~FieldReextraction()
{
    cancel();
}

void FieldReextraction::start()
{
    if (m_started || !m_jobs)
        return;
    m_started = true;
    // QRegularExpression компилируется лениво — прогреваем каждую схему на
    // этом потоке, чтобы воркеры не компилировали его наперегонки.
    if (m_schemas->fallback)
        m_schemas->fallback->pattern.extractFields(QString());
    for (const FileSchemaPtr& schema : std::as_const(m_schemas->byPath))
        if (schema)
            schema->pattern.extractFields(QString());
    // Схемы заданий живут в m_schemas: деструктор и cancel() ждут воркеров.
    m_watcher.setFuture(QtConcurrent::map(*m_jobs, [](Job& job) {
        job.fields = job.schema ? job.schema->extractFields(job.entry->message())
                                : LogEntryFields();
    }));
}

void FieldReextraction::cancel()
{
    if (!m_started)
        return;
    // Сначала отвязываем сигналы: отменённое задание не должно выглядеть
    // досчитанным для владельца.
    m_watcher.disconnect(this);
    m_watcher.cancel();
    m_watcher.waitForFinished();
}

bool FieldReextraction::isReady() const
{
    return m_started && m_jobs && m_watcher.isFinished() && !m_watcher.isCanceled();
}

void FieldReextraction::apply()
{
    if (!isReady())
        return;
    for (Job& job : *m_jobs)
        job.entry->swapFields(job.fields);
    // Теперь в job.fields старые спаны. Миллионы освобождений — не на
    // GUI-потоке: буфер уходит в пул и умирает там.
    (void)QtConcurrent::run([jobs = std::move(m_jobs)]() mutable { jobs.reset(); });
}
