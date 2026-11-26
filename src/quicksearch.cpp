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

void QuickSearch::start(LogModel* model, const TextQuery& query, bool forward,
                        int startRow, bool wrapAround)
{
    launch(model, startRow,
           [query, forward, startRow, wrapAround](const LogScanSnapshot& snapshot,
                                                  const std::atomic_bool* cancel,
                                                  std::atomic<int>* progress) {
               return scan(snapshot, query, forward, startRow, wrapAround, cancel, progress);
           });
}

void QuickSearch::startLevel(LogModel* model, LogLevelMask levels, bool forward,
                             int startRow, bool wrapAround)
{
    launch(model, startRow,
           [levels, forward, startRow, wrapAround](const LogScanSnapshot& snapshot,
                                                   const std::atomic_bool* cancel,
                                                   std::atomic<int>* progress) {
               return snapshot.findRowWithLevel(levels, startRow, forward, wrapAround,
                                                cancel, progress);
           });
}

void QuickSearch::startFile(LogModel* model, const LogFile* file, bool forward,
                            int startRow, bool wrapAround)
{
    launch(model, startRow,
           [file, forward, startRow, wrapAround](const LogScanSnapshot& snapshot,
                                                 const std::atomic_bool* cancel,
                                                 std::atomic<int>* progress) {
               return snapshot.findRowOfFile(file, startRow, forward, wrapAround,
                                             cancel, progress);
           });
}

void QuickSearch::launch(LogModel* model, int startRow, Scan scan)
{
    cancel();
    if (!model)
        return;
    m_model = model;
    m_startRow = startRow;
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
        m_startRowNow = m_startRow < 0 ? m_startRow : int(mapRow(m_startRow, insertions));
        emit finished(row < 0 ? -1 : int(mapRow(row, insertions)));
    });
    watcher->setFuture(QtConcurrent::run(
        [snapshot, scan = std::move(scan), cancelFlag, progressValue]() {
            return scan(snapshot, cancelFlag.get(), progressValue.get());
        }));
}

int QuickSearch::startRowFor(const LogModel& model, int selectedRow, int anchorEntryId,
                             const LogFile* anchorFile, bool forward)
{
    const int rows = model.rowCount();
    if (selectedRow >= 0 && selectedRow < rows)
        return selectedRow;
    if (anchorEntryId >= 0) {
        bool visible = false;
        const int place = model.placeAmongVisible(anchorEntryId, anchorFile, &visible);
        if (place >= 0 && visible)
            return place;
        // Запись скрыта: place — число видимых строк перед ней, то есть номер
        // первой видимой строки за её местом. Старт в поиск не включается,
        // поэтому вперёд отступаем на строку назад (place проверится первой),
        // а назад стартуем с place (первой проверится place - 1).
        if (place >= 0)
            return forward ? place - 1 : place;
    }
    return forward ? -1 : rows;
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

qint64 QuickSearch::scan(const LogScanSnapshot& snapshot, const TextQuery& query,
                         bool forward, qint64 startRow, bool wrapAround,
                         const std::atomic_bool* cancel, std::atomic<int>* progress)
{
    return snapshot.findRowWithText(query, startRow, forward, wrapAround, cancel, progress);
}

qint64 QuickSearch::mapRow(qint64 row, const QVector<Insertion>& insertions)
{
    for (const Insertion& insertion : insertions) {
        if (insertion.first <= row)
            row += insertion.count;
    }
    return row;
}
