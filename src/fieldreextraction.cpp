#include "fieldreextraction.h"
#include "logpattern.h"

#include <QtConcurrent/QtConcurrentMap>
#include <QtConcurrent/QtConcurrentRun>

FieldReextraction::FieldReextraction(QVector<std::shared_ptr<LogEntry>> entries,
                                     std::shared_ptr<const LogPattern> pattern,
                                     QObject* parent)
    : QObject(parent)
    , m_jobs(std::make_shared<QVector<Job>>())
    , m_pattern(std::move(pattern))
{
    m_jobs->reserve(entries.size());
    for (auto& entry : entries) {
        if (entry)
            m_jobs->append({std::move(entry), LogEntryFields()});
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
    // QRegularExpression компилируется лениво — прогреваем на этом потоке,
    // чтобы воркеры не компилировали его наперегонки.
    m_pattern->extractFields(QString());
    const std::shared_ptr<const LogPattern> pattern = m_pattern;
    m_watcher.setFuture(QtConcurrent::map(*m_jobs, [pattern](Job& job) {
        job.fields = pattern->extractFields(job.entry->message());
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
