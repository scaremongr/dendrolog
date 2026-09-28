#include "quicksearch.h"
#include "logmodel.h"

#include <QFutureWatcher>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

QuickSearch::QuickSearch(QObject* parent)
    : QObject(parent)
    , m_progressTimer(new QTimer(this))
{
    m_progressTimer->setInterval(200);
    connect(m_progressTimer, &QTimer::timeout, this, [this]() {
        if (m_running && m_progress)
            emit progress(m_progress->load(std::memory_order_relaxed));
    });
}

QuickSearch::~QuickSearch()
{
    // Воркер держит только снапшот и свои флаги — ждать его не нужно.
    cancel();
}

void QuickSearch::start(LogModel* model, const QString& text, Qt::CaseSensitivity cs,
                        bool forward, int startRow, bool wrapAround)
{
    cancel();
    if (!model)
        return;
    m_model = model;
    m_insertions.clear();
    m_valid = true;
    // Всё, что меняет номера строк, пока идёт скан: вставки пересчитываются,
    // остальное делает результат неприменимым.
    m_connections << connect(model, &QAbstractItemModel::rowsInserted, this,
        [this](const QModelIndex&, int first, int last) {
            m_insertions.append({first, qint64(last) - first + 1});
        });
    const auto invalidate = [this]() { m_valid = false; };
    m_connections << connect(model, &QAbstractItemModel::modelReset, this, invalidate);
    m_connections << connect(model, &QAbstractItemModel::layoutChanged, this, invalidate);
    m_connections << connect(model, &QAbstractItemModel::rowsRemoved, this, invalidate);
    m_connections << connect(model, &QAbstractItemModel::rowsMoved, this, invalidate);

    const LogScanSnapshot snapshot = model->scanSnapshot(/*filteredOnly=*/true);
    auto cancelFlag = std::make_shared<std::atomic_bool>(false);
    auto progressValue = std::make_shared<std::atomic<int>>(0);
    m_cancel = cancelFlag;
    m_progress = progressValue;
    m_running = true;
    m_progressTimer->start();

    const int generation = ++m_generation;
    auto* watcher = new QFutureWatcher<qint64>(this);
    connect(watcher, &QFutureWatcher<qint64>::finished, this, [this, watcher, generation]() {
        watcher->deleteLater();
        if (generation != m_generation)
            return; // отменён или заменён новым поиском
        const qint64 row = watcher->result();
        const bool valid = m_valid && m_model;
        const QVector<Insertion> insertions = m_insertions;
        m_running = false;
        m_progressTimer->stop();
        detachModel();
        if (!valid) {
            emit invalidated();
            return;
        }
        emit finished(row < 0 ? -1 : int(mapRow(row, insertions)));
    });
    watcher->setFuture(QtConcurrent::run(
        [snapshot, text, cs, forward, startRow, wrapAround, cancelFlag, progressValue]() {
            return scan(snapshot, text, cs, forward, startRow, wrapAround,
                        cancelFlag.get(), progressValue.get());
        }));
}

void QuickSearch::cancel()
{
    ++m_generation;
    if (m_cancel)
        m_cancel->store(true);
    m_cancel.reset();
    m_running = false;
    m_progressTimer->stop();
    detachModel();
}

void QuickSearch::detachModel()
{
    for (const QMetaObject::Connection& connection : std::as_const(m_connections))
        disconnect(connection);
    m_connections.clear();
    m_model = nullptr;
}

qint64 QuickSearch::scan(const LogScanSnapshot& snapshot, const QString& text,
                         Qt::CaseSensitivity cs, bool forward, qint64 startRow,
                         bool wrapAround, const std::atomic_bool* cancel,
                         std::atomic<int>* progress)
{
    const qint64 n = snapshot.rowCount();
    if (text.isEmpty() || n == 0)
        return -1;

    qint64 found = -1;
    qint64 scanned = 0;
    bool cancelled = false;
    // Прогресс — доля просмотренных строк от всех: второй проход при
    // заворачивании продолжает первый.
    const auto check = [&](qint64 row, QStringView line) {
        if ((++scanned & 0xFFF) == 0) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                cancelled = true;
                return false;
            }
            if (progress)
                progress->store(int(qMin<qint64>(99, scanned * 100 / n)),
                                std::memory_order_relaxed);
        }
        if (line.contains(text, cs)) {
            found = row;
            return false;
        }
        return true;
    };

    // Та же семантика, что у LogStore::findNext/PreviousOccurrence.
    if (forward) {
        const qint64 first = qBound<qint64>(0, startRow + 1, n);
        snapshot.forEachLine(first, [&](qint64 row, const LogEntryMeta&, QStringView line) {
            return check(row, line);
        });
        if (found < 0 && !cancelled && wrapAround) {
            snapshot.forEachLine(0, [&](qint64 row, const LogEntryMeta&, QStringView line) {
                if (row > startRow)
                    return false;
                return check(row, line);
            });
        }
    } else {
        const qint64 current = (startRow < 0 || startRow >= n) ? n - 1 : startRow - 1;
        snapshot.forEachLineBackward(current, [&](qint64 row, const LogEntryMeta&, QStringView line) {
            return check(row, line);
        });
        if (found < 0 && !cancelled && wrapAround) {
            snapshot.forEachLineBackward(n - 1, [&](qint64 row, const LogEntryMeta&, QStringView line) {
                if (row < startRow)
                    return false;
                return check(row, line);
            });
        }
    }
    if (progress)
        progress->store(100, std::memory_order_relaxed);
    return cancelled ? -1 : found;
}

qint64 QuickSearch::mapRow(qint64 row, const QVector<Insertion>& insertions)
{
    for (const Insertion& insertion : insertions) {
        if (insertion.first <= row)
            row += insertion.count;
    }
    return row;
}
