#include "scrollbarmarks.h"

#include "bookmarkstore.h"
#include "logmodel.h"
#include "searchresultscontroller.h"

#include <QFutureWatcher>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <cmath>
#include <limits>

namespace {

// Прореживание: в каждой из kFineBuckets корзин остаются первая и последняя
// отмеченные строки. Строки приходят по возрастанию.
class Thinner {
public:
    Thinner(QVector<qint64>& out, qint64 rowCount)
        : m_out(out)
        , m_rowCount(rowCount)
    {
    }

    void add(qint64 row)
    {
        const qint64 bucket = m_rowCount <= ScrollbarMarks::kFineBuckets
            ? row
            : row * ScrollbarMarks::kFineBuckets / m_rowCount;
        if (bucket != m_bucket) {
            m_out.append(row);
            m_bucket = bucket;
            m_inBucket = 1;
        } else if (m_inBucket == 1) {
            m_out.append(row);
            m_inBucket = 2;
        } else {
            m_out.last() = row;
        }
    }

private:
    QVector<qint64>& m_out;
    qint64 m_rowCount;
    qint64 m_bucket = -1;
    int m_inBucket = 0;
};

bool isCancelled(const std::atomic_bool* cancel)
{
    return cancel && cancel->load(std::memory_order_relaxed);
}

} // namespace

ScrollbarMarks::ScrollbarMarks(QObject* parent)
    : QObject(parent)
    , m_debounce(new QTimer(this))
{
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(kDebounceMs);
    connect(m_debounce, &QTimer::timeout, this, &ScrollbarMarks::launch);
}

ScrollbarMarks::~ScrollbarMarks()
{
    // Воркер держит только снапшоты и свой флаг — ждать его не нужно.
    if (m_cancel)
        m_cancel->store(true);
}

LogModel* ScrollbarMarks::model() const
{
    return m_model;
}

void ScrollbarMarks::setModel(LogModel* model)
{
    if (model == m_model)
        return;
    disconnectModel();
    if (m_running) {
        ++m_generation;
        m_cancel->store(true);
        m_running = false;
    }
    m_model = model;
    m_marks = ScrollbarMarkSet();
    m_searchCarry = false;
    ++m_serial;
    emit marksChanged();
    if (m_model) {
        connectModel();
        markStale();
    } else {
        m_stale = false;
        m_debounce->stop();
    }
}

void ScrollbarMarks::connectModel()
{
    LogModel* model = m_model;
    m_modelConnections << connect(model, &QAbstractItemModel::rowsInserted, this,
                                  [this]() { markStale(); });
    m_modelConnections << connect(model, &LogModel::modelFiltered, this,
                                  [this]() { markStale(); });
    // Без ролей — текст строки изменился (дописан хвост без '\n'): уровень мог
    // смениться. С ролями — подсветка, маркеры, закладки: отметок не меняют.
    m_modelConnections << connect(model, &QAbstractItemModel::dataChanged, this,
        [this](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
            if (roles.isEmpty())
                markStale();
        });
    m_modelConnections << connect(model, &QAbstractItemModel::modelReset, this,
                                  [this]() { invalidate(); });
    m_modelConnections << connect(model, &QAbstractItemModel::rowsRemoved, this,
                                  [this]() { invalidate(); });
    m_modelConnections << connect(model, &QAbstractItemModel::layoutChanged, this,
                                  [this]() { invalidate(); });
    m_modelConnections << connect(model, &QAbstractItemModel::rowsMoved, this,
                                  [this]() { invalidate(); });
    m_modelConnections << connect(&model->bookmarks(), &BookmarkStore::changed, this,
                                  [this]() {
                                      refreshBookmarks();
                                      ++m_serial;
                                      emit marksChanged();
                                  });
}

void ScrollbarMarks::disconnectModel()
{
    for (const QMetaObject::Connection& c : std::as_const(m_modelConnections))
        disconnect(c);
    m_modelConnections.clear();
}

void ScrollbarMarks::setSearch(SearchResultsController* search)
{
    if (search == m_search)
        return;
    for (const QMetaObject::Connection& c : std::as_const(m_searchConnections))
        disconnect(c);
    m_searchConnections.clear();
    m_search = search;
    m_searchCarry = false;
    if (search) {
        // Изменения выдачи касаются этой вкладки, только если она источник
        // (или отметки поиска уже нарисованы и их надо снять).
        const auto touched = [this]() {
            return searchApplies() || !m_marks.rows[ScrollbarMarkSet::Search].isEmpty();
        };
        const auto stale = [this, touched]() {
            // Идущий расчёт взял выдачу, а поиск здесь больше не действует
            // (снят, источник сменился): его результат принёс бы отметки
            // поиска, которые уже никто не снимет, — отбросить.
            if (m_running && m_runningSearch && !searchApplies())
                invalidate();
            else if (touched())
                markStale();
        };
        // Выдача перестроена: прежние отметки поиска переносить нельзя, а
        // идущий расчёт отбросить, только если он её сопоставлял, — расчёт
        // одних уровней от выдачи не зависит.
        const auto rebuilt = [this, touched]() {
            m_searchCarry = false;
            if (m_running && m_runningSearch)
                invalidate();
            else if (touched())
                markStale();
        };
        LogModel* results = search->model();
        m_searchConnections << connect(search, &SearchResultsController::statusChanged,
                                       this, stale);
        m_searchConnections << connect(results, &QAbstractItemModel::rowsInserted, this, stale);
        m_searchConnections << connect(results, &LogModel::modelFiltered, this, stale);
        m_searchConnections << connect(results, &QAbstractItemModel::modelReset, this, rebuilt);
        m_searchConnections << connect(results, &QAbstractItemModel::rowsRemoved, this, rebuilt);
    }
    if (m_model)
        markStale();
}

void ScrollbarMarks::setEnabled(bool enabled)
{
    if (enabled == m_enabled)
        return;
    m_enabled = enabled;
    if (!enabled) {
        m_debounce->stop();
        return;
    }
    if (m_stale && !m_running)
        launch();
}

bool ScrollbarMarks::isUpToDate() const
{
    return !m_stale && !m_running && !m_debounce->isActive();
}

void ScrollbarMarks::flush()
{
    if (m_stale && !m_running) {
        m_debounce->stop();
        launch();
    }
}

void ScrollbarMarks::markStale()
{
    // O(1): на батч индексатора здесь только флаг и таймер (инвариант 3).
    m_stale = true;
    if (m_enabled && !m_running && !m_debounce->isActive())
        m_debounce->start();
}

void ScrollbarMarks::invalidate()
{
    // Номера строк идущего расчёта больше ничего не значат — как и строки
    // опубликованных отметок поиска.
    m_searchCarry = false;
    if (m_running) {
        ++m_generation;
        m_cancel->store(true);
        m_running = false;
        ++m_discarded;
    }
    markStale();
}

bool ScrollbarMarks::searchApplies() const
{
    return m_search && m_model && m_search->source() == m_model && m_search->isActive();
}

LogModel* ScrollbarMarks::searchModel() const
{
    return searchApplies() ? m_search->model() : nullptr;
}

void ScrollbarMarks::refreshBookmarks()
{
    QVector<qint64>& out = m_marks.rows[ScrollbarMarkSet::Bookmark];
    out.clear();
    if (!m_model)
        return;
    const QVector<int> rows = m_model->bookmarks().visibleRows();
    out.reserve(rows.size());
    for (int row : rows)
        out.append(row);
}

void ScrollbarMarks::launch()
{
    m_debounce->stop();
    if (!m_enabled || !m_model || m_running)
        return;
    m_stale = false;

    const LogScanSnapshot snapshot = m_model->scanSnapshot(/*filteredOnly=*/true);
    // Выдача, которая ещё считается, неполна: отметок поиска нет, пока она не
    // готова (её modelFiltered/reset снова пометят набор устаревшим).
    // Отставшая выдача (полный поиск отложен) — не подпоследовательность
    // видимых строк вкладки: сопоставлять её нельзя, отметок поиска нет.
    LogModel* results = searchModel();
    const bool stale = results && m_search->isStale();
    const LogScanSnapshot search = results && !stale && !results->isFiltering()
        ? results->scanSnapshot(/*filteredOnly=*/true) : LogScanSnapshot();
    // Выдача дописывается (джоб по новым строкам источника), а не
    // перестраивается: уже найденные строки на месте — их отметки переносятся
    // в новый набор (см. m_searchCarry).
    m_runningSearchSkipped = results && !stale && results->isFiltering();

    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_cancel = cancel;
    m_running = true;
    m_runningSearch = search.rowCount() > 0;
    ++m_launches;
    const int generation = ++m_generation;

    struct Result {
        ScrollbarMarkSet marks;
        bool cancelled = false;
    };
    auto* watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, generation]() {
        watcher->deleteLater();
        if (generation != m_generation)
            return; // отменён: reset, удаление строк, смена модели
        m_running = false;
        Result result = watcher->result();
        if (!result.cancelled) {
            QVector<qint64> bookmarks = std::move(m_marks.rows[ScrollbarMarkSet::Bookmark]);
            const bool carry = m_runningSearchSkipped && m_searchCarry && searchApplies();
            QVector<qint64> carried;
            if (carry)
                carried = std::move(m_marks.rows[ScrollbarMarkSet::Search]);
            m_marks = std::move(result.marks);
            m_marks.rows[ScrollbarMarkSet::Bookmark] = std::move(bookmarks);
            if (carry)
                m_marks.rows[ScrollbarMarkSet::Search] = std::move(carried);
            else
                m_searchCarry = m_runningSearch;
            refreshBookmarks();
            ++m_serial;
            emit marksChanged();
        }
        // За время расчёта пришли вставки — следующий через дебаунс.
        if (m_stale && m_enabled)
            m_debounce->start();
    });
    watcher->setFuture(QtConcurrent::run([snapshot, search, cancel]() {
        Result r;
        r.marks = compute(snapshot, search, cancel.get(), &r.cancelled);
        return r;
    }));
}

ScrollbarMarkSet ScrollbarMarks::compute(const LogScanSnapshot& snapshot,
                                         const LogScanSnapshot& search,
                                         const std::atomic_bool* cancel,
                                         bool* cancelled)
{
    ScrollbarMarkSet out;
    const qint64 n = snapshot.rowCount();
    out.rowCount = n;
    if (cancelled)
        *cancelled = false;
    const auto stop = [&]() {
        if (cancelled)
            *cancelled = true;
        return ScrollbarMarkSet();
    };

    // ---- Уровни: первая показанная строка записи (как цель F8) -------------
    Thinner errors(out.rows[ScrollbarMarkSet::Error], n);
    Thinner warnings(out.rows[ScrollbarMarkSet::Warning], n);
    qint64 metaRow = -1; // строка, чья мета лежит в meta (соседние совпадения)
    LogEntryMeta meta;
    for (qint64 row = 0; row < n; ++row) {
        if ((row & 0xFFFF) == 0 && isCancelled(cancel))
            return stop();
        const LogLevel level = snapshot.levelAt(row);
        Thinner* target = nullptr;
        if (level == LogLevel::Error || level == LogLevel::Fatal)
            target = &errors;
        else if (level == LogLevel::Warn)
            target = &warnings;
        if (!target)
            continue;
        // Продолжение записи, чья строка показана прямо перед этой? Строки
        // записи идут подряд в любом порядке вкладки.
        const LogEntryMeta current = snapshot.metaAt(row);
        if (row > 0) {
            const LogEntryMeta previous = metaRow == row - 1 ? meta : snapshot.metaAt(row - 1);
            metaRow = row;
            meta = current;
            if (previous.logicalEntryId == current.logicalEntryId
                && previous.sourceFile == current.sourceFile)
                continue;
        } else {
            metaRow = row;
            meta = current;
        }
        target->add(row);
    }

    // ---- Поиск: выдача — подпоследовательность строк вкладки ---------------
    const qint64 m = search.rowCount();
    if (m > 0 && n > 0) {
        Thinner found(out.rows[ScrollbarMarkSet::Search], n);
        // Строки выдачи, которых нет среди видимых строк вкладки, — выдача
        // разошлась с вкладкой вопреки ожиданию (отставшую launch не берёт).
        // Такая строка пропускается, следующие ищутся с прежнего места; после
        // kMaxMisses промахов (каждый — проход до конца) сопоставление
        // прекращается, следующий расчёт возьмёт свежую выдачу.
        constexpr int kMaxMisses = 8;
        int misses = 0;
        qint64 i = 0;
        qint64 steps = 0;
        for (qint64 j = 0; j < m && i < n && misses < kMaxMisses; ++j) {
            const LogFile* wantedFile = nullptr;
            const qint64 wantedLine = search.lineAt(j, &wantedFile);
            const qint64 from = i;
            bool hit = false;
            for (; i < n; ++i) {
                if ((++steps & 0xFFFF) == 0 && isCancelled(cancel))
                    return stop();
                const LogFile* file = nullptr;
                const qint64 line = snapshot.lineAt(i, &file);
                if (line == wantedLine && file == wantedFile) {
                    found.add(i);
                    ++i;
                    hit = true;
                    break;
                }
            }
            if (!hit) {
                ++misses;
                i = from;
            }
        }
    }
    return out;
}

int ScrollbarMarks::trackPosition(double value, const ScrollbarTrack& track)
{
    const int g = track.grooveLength;
    if (g <= 0)
        return -1;
    if (!(value > 0)) // и NaN
        return 0;
    const qint64 v = qint64(std::llround(value));
    double px;
    if (track.maximum <= 0) {
        px = double(v) * g / double(qMax(1, track.pageStep));
    } else {
        // QStyle::sliderPositionFromValue (min = 0, не перевёрнут) — так Qt
        // ставит ползунок; дальше максимума — та же пропорция.
        const qint64 range = track.maximum;
        const qint64 span = qMax(0, g - track.sliderLength);
        const qint64 p = qMin(v, range);
        qint64 pos;
        if (span <= 0)
            pos = 0;
        else if (range > qint64(std::numeric_limits<int>::max() / 4096))
            pos = qint64(double(p) * (double(span) / double(range)));
        else if (range > span)
            pos = (2 * p * span + range) / (2 * range);
        else
            pos = p * (span / range) + (2 * p * (span % range) + range) / (2 * range);
        px = double(pos);
        if (v > range)
            px += double(v - range) * double(span) / double(range);
    }
    return int(qBound<double>(0, std::floor(px), g - 1));
}
