#include "fileshowncounter.h"

#include "logmodel.h"
#include "logscan.h"

#include <QFutureWatcher>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

namespace {
constexpr int kDebounceMs = 150;
}

FileShownCounter::FileShownCounter(QObject* parent)
    : QObject(parent)
{
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(kDebounceMs);
    connect(m_debounce, &QTimer::timeout, this, &FileShownCounter::start);
}

FileShownCounter::~FileShownCounter()
{
    if (m_cancel)
        m_cancel->store(true);
}

void FileShownCounter::setModel(LogModel* model)
{
    for (const QMetaObject::Connection& c : std::as_const(m_connections))
        disconnect(c);
    m_connections.clear();
    m_model = model;
    m_counts.clear();
    if (model) {
        m_connections << connect(model, &QAbstractItemModel::rowsInserted, this, [this]() { markDirty(false); });
        m_connections << connect(model, &QAbstractItemModel::rowsRemoved, this, [this]() { markDirty(false); });
        m_connections << connect(model, &QAbstractItemModel::modelReset, this, [this]() { markDirty(true); });
        m_connections << connect(model, &LogModel::modelFiltered, this, [this]() { markDirty(false); });
    }
    markDirty(true);
}

void FileShownCounter::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    if (active && m_dirty && !m_running && !m_debounce->isActive())
        m_debounce->start(0);
}

bool FileShownCounter::isUpToDate() const
{
    return !m_dirty && !m_running && !m_debounce->isActive();
}

void FileShownCounter::markDirty(bool reset)
{
    m_dirty = true;
    if (reset) {
        // Набор строк другой: идущий расчёт по старому снапшоту не нужен.
        ++m_generation;
        if (m_cancel)
            m_cancel->store(true);
        m_running = false;
    }
    if (m_active && !m_running)
        m_debounce->start();
}

void FileShownCounter::start()
{
    if (!m_model || !m_active || m_running || !m_dirty)
        return;
    m_dirty = false;
    m_running = true;
    ++m_launches;
    const LogScanSnapshot snapshot = m_model->scanSnapshot(/*filteredOnly=*/true);
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_cancel = cancel;
    const int generation = m_generation;

    using Counts = QHash<const LogFile*, qint64>;
    auto* watcher = new QFutureWatcher<Counts>(this);
    connect(watcher, &QFutureWatcher<Counts>::finished, this, [this, watcher, generation]() {
        watcher->deleteLater();
        if (generation != m_generation)
            return; // reset посреди расчёта — результат устарел
        m_running = false;
        m_counts = watcher->result();
        if (m_dirty && m_active)
            m_debounce->start();
        emit countsChanged();   // и при тех же числах: расчёт закончен
    });
    watcher->setFuture(QtConcurrent::run([snapshot, cancel]() -> Counts {
        Counts counts;
        const LogFile* last = nullptr;
        qint64* lastCount = nullptr;
        snapshot.forEachMeta(0, [&](qint64 row, const LogEntryMeta& meta) {
            if ((row & 0x3FFF) == 0 && cancel->load(std::memory_order_relaxed))
                return false;
            if (meta.sourceFile != last || !lastCount) {
                last = meta.sourceFile;
                lastCount = &counts[last];
            }
            ++*lastCount;
            return true;
        });
        if (cancel->load(std::memory_order_relaxed))
            return Counts();
        return counts;
    }));
}
