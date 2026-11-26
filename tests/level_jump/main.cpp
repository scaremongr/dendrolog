// ============================================================================
// level_jump — переход к следующей / предыдущей ошибке (F8 / Shift+F8):
//   • LogModel::findRowWithLevel против эталона «кандидаты в порядке обхода,
//     первый подходящий» на обоих бэкендах, на слитой вкладке, под фильтром и
//     на многострочных записях: цель — первая показанная строка записи, а не
//     каждая строка стек-трейса (все конфигурации фильтров — в
//     logstore_equivalence);
//   • фоновый поиск (QuickSearch::startLevel): найденная и стартовая строки
//     пересчитываются через вставки за время скана, reset — invalidated();
//   • вкладка (LogViewWidget::goToLevel), синхронный и фоновый путь, оба
//     бэкенда: переход — выбор пользователя (строка выделена, видна,
//     follow-tail снят), идёт от ВЫБРАННОЙ строки, только по показанным
//     строкам; исходы «дальше», «через край», «единственная», «нет»,
//     «прервано перестройкой модели».
// ============================================================================

#include "appsettings.h"
#include "filelegend.h"
#include "logviewwidget.h"
#include "quicksearch.h"
#include "testdocuments.h"
#include "viewidle.h"

#include <QApplication>
#include <QFuture>
#include <QSemaphore>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>
#include <cstdio>

static int failures = 0;
#define CHECK(condition, label) do { \
    if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL: %s (line %d)\n", qPrintable(QString(label)), __LINE__); } \
} while (0)

// Занимает все потоки общего пула, пока не отпустят: фоновый скан, запущенный
// за это время, гарантированно ещё НЕ начался, когда тест меняет модель. Без
// этого исход «скан успел / не успел» зависел бы от планировщика (а запись в
// список, который воркер только что дочитал, TSan принимал бы за гонку).
class PoolHold {
public:
    PoolHold()
    {
        QThreadPool* pool = QThreadPool::globalInstance();
        const int threads = pool->maxThreadCount();
        for (int i = 0; i < threads; ++i)
            m_tasks.append(QtConcurrent::run([this] { m_started.release(); m_gate.acquire(); }));
        m_started.acquire(threads);
    }
    ~PoolHold() { release(); }
    void release()
    {
        m_gate.release(int(m_tasks.size()));
        for (QFuture<void>& task : m_tasks)
            task.waitForFinished();
        m_tasks.clear();
    }

private:
    QSemaphore m_gate;
    QSemaphore m_started;
    QVector<QFuture<void>> m_tasks;
};

static const LogLevelMask kErrors = levelBit(LogLevel::Error) | levelBit(LogLevel::Fatal);
static const LogLevelMask kWarnings = levelBit(LogLevel::Warn);

static QByteArray line(int second, const char* level, const QByteArray& text)
{
    return QByteArray("2026-03-05 10:") + QByteArray::number(second / 60).rightJustified(2, '0')
         + ':' + QByteArray::number(second % 60).rightJustified(2, '0') + ".000 [w] "
         + level + " - " + text + '\n';
}

// 4000 строк, метки не убывают. ERROR — строки 100, 600, …, 3600; FATAL — 2222;
// WARN — 50, 750, 1450, 2150, 2850, 3550; остальное INFO.
static const char* levelOf(int i)
{
    if (i == 2222)
        return "FATAL";
    if (i % 500 == 100)
        return "ERROR";
    if (i % 700 == 50)
        return "WARN";
    return "INFO";
}

static QByteArray fixture(int firstSecond, const QByteArray& tag)
{
    QByteArray out;
    for (int i = 0; i < 4000; ++i)
        out += line(firstSecond + i / 20, levelOf(i), tag + " " + QByteArray::number(i));
    return out;
}

// Многострочные записи: у каждой ERROR/FATAL — три строки стек-трейса, они
// наследуют уровень записи. 1200 записей.
static QByteArray traceFixture(int firstSecond, const QByteArray& tag)
{
    QByteArray out;
    for (int i = 0; i < 1200; ++i) {
        const char* level = i % 97 == 5 ? "ERROR" : i % 301 == 7 ? "FATAL"
                          : i % 53 == 11 ? "WARN" : "INFO";
        out += line(firstSecond + i / 10, level, tag + " " + QByteArray::number(i));
        if (level[0] == 'E' || level[0] == 'F') {
            for (int frame = 0; frame < 3; ++frame)
                out += "    at com.example.Frame" + QByteArray::number(frame) + "(" + tag + ")\n";
        }
    }
    return out;
}

// Эталон, заданный иначе, чем реализация: кандидаты в порядке обхода, первый
// подходящий. Подходящая строка — нужного уровня и первая показанная строка
// своей записи.
static int expectedRow(const QVector<bool>& target, int startRow, bool forward, bool wrap)
{
    const int n = int(target.size());
    QVector<int> order;
    const bool inside = startRow >= 0 && startRow < n;
    if (forward) {
        for (int r = 0; r < n; ++r)
            if (r > startRow) order << r;
        if (wrap)
            for (int r = 0; r < n; ++r)
                if (r <= startRow) order << r;
    } else if (!inside) {
        for (int r = n - 1; r >= 0; --r)
            order << r;
    } else {
        for (int r = n - 1; r >= 0; --r)
            if (r < startRow) order << r;
        if (wrap)
            for (int r = n - 1; r >= 0; --r)
                if (r >= startRow) order << r;
    }
    for (const int r : std::as_const(order))
        if (target[r]) return r;
    return -1;
}

static void compareWithOracle(const QString& name, LogModel& model)
{
    const int n = model.rowCount();
    QVector<bool> startsRecord(n);
    LogModel::EntryKey previous;
    int continuations = 0;
    for (int row = 0; row < n; ++row) {
        const LogModel::EntryKey key = model.keyForRow(row);
        startsRecord[row] = row == 0 || key.logicalEntryId != previous.logicalEntryId
                            || key.sourceFile != previous.sourceFile;
        continuations += !startsRecord[row];
        previous = key;
    }
    const QVector<int> starts = {-1, 0, 1, 100, n / 2, n - 2, n - 1, n, n + 5};
    int mismatches = 0, hits = 0;
    for (const LogLevelMask mask : {kErrors, kWarnings, levelBit(LogLevel::Trace), LogLevelMask(0)}) {
        QVector<bool> target(n);
        for (int row = 0; row < n; ++row)
            target[row] = startsRecord[row] && (levelBit(model.visibleLevelAt(row)) & mask);
        for (const bool forward : {true, false}) {
            for (const bool wrap : {true, false}) {
                for (const int start : starts) {
                    const int expected = expectedRow(target, start, forward, wrap);
                    const int actual = model.findRowWithLevel(mask, start, forward, wrap);
                    hits += expected >= 0;
                    if (actual != expected && ++mismatches <= 5)
                        std::fprintf(stderr, "  %s mask=%x forward=%d wrap=%d start=%d: model %d, expected %d\n",
                                     qPrintable(name), unsigned(mask), int(forward), int(wrap), start,
                                     actual, expected);
                }
            }
        }
    }
    CHECK(mismatches == 0, name + ": findRowWithLevel matches the oracle");
    CHECK(hits > 0 && continuations > 0, name + ": the fixture has errors and continuation lines");
}

// Переход к строке файла (S-14): LogModel::findRowOfFile и снапшот против
// того же эталона — кандидаты в порядке обхода, первая строка файла.
static void compareFileOracle(const QString& name, LogModel& model,
                              const QVector<const LogFile*>& files)
{
    const int n = model.rowCount();
    QVector<const LogFile*> rowFile(n);
    for (int row = 0; row < n; ++row)
        rowFile[row] = model.keyAt(row).file.get();
    const LogScanSnapshot snapshot = model.scanSnapshot(/*filteredOnly=*/true);
    const QVector<int> starts = {-1, 0, 1, 100, n / 2, n - 2, n - 1, n, n + 5};
    int mismatches = 0;
    for (const LogFile* file : files) {
        QVector<bool> target(n);
        for (int row = 0; row < n; ++row)
            target[row] = rowFile[row] == file;
        for (const bool forward : {true, false}) {
            for (const int start : starts) {
                const int expected = expectedRow(target, start, forward, /*wrap=*/true);
                const int actual = model.findRowOfFile(file, start, forward);
                if (actual != expected && ++mismatches <= 5)
                    std::fprintf(stderr, "  %s file=%p forward=%d start=%d: model %d, expected %d\n",
                                 qPrintable(name), static_cast<const void*>(file), int(forward),
                                 start, actual, expected);
                for (const bool wrap : {true, false}) {
                    const int want = expectedRow(target, start, forward, wrap);
                    const qint64 got = snapshot.findRowOfFile(file, start, forward, wrap);
                    if (got != want && ++mismatches <= 5)
                        std::fprintf(stderr, "  %s snapshot forward=%d wrap=%d start=%d: %lld, expected %d\n",
                                     qPrintable(name), int(forward), int(wrap), start,
                                     static_cast<long long>(got), want);
                }
            }
        }
    }
    CHECK(mismatches == 0, name + ": findRowOfFile matches the oracle");
}

static bool settled(LogViewWidget& tab)
{
    return testdocs::waitFor([&] { return !tab.isLoading(); }, 60000)
        && testdocs::settle(*tab.model());
}

// Вкладка: переходы и их исходы. syncRows — порог синхронного пути
// (0 — всегда фон).
static void checkTab(const QString& name, const QString& path, const QString& schema,
                     int syncRows)
{
    using Outcome = LogViewWidget::LevelJumpOutcome;
    LogViewWidget tab;
    tab.resize(900, 400);
    tab.setParserPattern(schema);
    tab.setExtractionEnabled(true);
    tab.addLogFile(path);
    tab.show();
    CHECK(settled(tab), name + ": load settles");
    testdocs::pump(300); // отложенный пересчёт view после show/resize
    tab.setLevelJumpSyncRows(syncRows);
    LogModel& model = *tab.model();
    LogListView& view = *tab.view();
    CHECK(model.rowCount() == 4000, name + ": fixture loaded");

    int finished = 0;
    Outcome outcome = Outcome::NotFound;
    LogLevelMask reported = 0;
    QObject::connect(&tab, &LogViewWidget::levelJumpFinished,
                     [&](LogLevelMask levels, Outcome o) { ++finished; outcome = o; reported = levels; });
    // Переход и ожидание итога; возвращает выбранную строку (-1 — не выбрана).
    const auto jump = [&](LogLevelMask levels, bool forward) {
        const int before = finished;
        tab.goToLevel(levels, forward);
        CHECK(testdocs::waitFor([&] { return finished > before; }, 20000), name + ": jump finishes");
        CHECK(finished == before + 1 && reported == levels, name + ": one result per jump");
        const QModelIndex cur = view.currentIndex();
        return cur.isValid() && view.selectionModel()->isRowSelected(cur.row(), QModelIndex())
            ? cur.row() : -1;
    };
    const auto shown = [&]() {
        const QRect r = view.visualRect(view.currentIndex());
        return r.isValid() && view.viewport()->rect().intersects(r);
    };

    // Ничего не выбрано: вперёд — с начала, назад — с конца.
    CHECK(jump(kErrors, true) == 100 && outcome == Outcome::Moved, name + ": first error from the top");
    CHECK(shown(), name + ": the error is scrolled into view");
    CHECK(jump(kErrors, true) == 600 && outcome == Outcome::Moved, name + ": next error");
    CHECK(jump(kErrors, false) == 100 && outcome == Outcome::Moved, name + ": previous error");
    // Раньше ошибок нет — через край, к последней.
    CHECK(jump(kErrors, false) == 3600 && outcome == Outcome::Wrapped, name + ": wraps back to the last error");
    CHECK(shown(), name + ": the wrapped target is scrolled into view");
    CHECK(jump(kErrors, true) == 100 && outcome == Outcome::Wrapped, name + ": wraps forward to the first error");
    // Fatal — тоже ошибка: между 2100 и 2600.
    view.goToRow(2100);
    CHECK(jump(kErrors, true) == 2222 && outcome == Outcome::Moved, name + ": Fatal counts as an error");
    CHECK(jump(kErrors, true) == 2600, name + ": error after the Fatal");
    // Предупреждения — отдельная команда.
    CHECK(jump(kWarnings, true) == 2850 && outcome == Outcome::Moved, name + ": next warning");
    CHECK(jump(kWarnings, false) == 2150 && outcome == Outcome::Moved, name + ": previous warning");

    // Уровня нет вовсе: строка остаётся на месте.
    CHECK(jump(levelBit(LogLevel::Trace), true) == 2150 && outcome == Outcome::NotFound,
          name + ": no such level leaves the selection alone");

    // Follow-tail вернул бы вид к хвосту на первой же дозаписи — переход снимает.
    view.setFollowTail(true);
    CHECK(jump(kErrors, true) == 2222 && !view.followTail(), name + ": jump turns follow-tail off");

    // Текущую строку без выделения назначил не пользователь (Qt по фокусу) —
    // опорой она не служит: поиск идёт с начала.
    view.selectionModel()->clearSelection();
    view.selectionModel()->setCurrentIndex(model.index(3000, 0), QItemSelectionModel::NoUpdate);
    CHECK(jump(kErrors, true) == 100 && outcome == Outcome::Moved,
          name + ": an unselected current row is not the starting point");

    // Только показанные строки: под фильтром по тексту ошибки вне его не видны.
    FilterRuleSet rules;
    FilterRule only;
    only.action = FilterRule::Action::Include;
    only.text = QStringLiteral(" 2222");
    rules.rules.append(only);
    model.setFilterRules(rules);
    CHECK(testdocs::settle(model), name + ": text filter settles");
    testdocs::pump(300);
    CHECK(model.rowCount() == 1, name + ": one row passes the filter");
    view.selectionModel()->clearSelection();
    CHECK(jump(kErrors, true) == 0 && outcome == Outcome::Moved, name + ": the only shown error is found");
    CHECK(jump(kErrors, true) == 0 && outcome == Outcome::OnlyCurrent, name + ": the only error is the current one");
    CHECK(jump(kErrors, false) == 0 && outcome == Outcome::OnlyCurrent, name + ": the same backwards");
    CHECK(jump(kWarnings, true) == 0 && outcome == Outcome::NotFound, name + ": no warnings among the shown rows");

    // Фильтр по уровню: показаны одни INFO — ошибок среди показанных нет.
    model.setFilterRules(FilterRuleSet());
    model.setLogLevelFilter({LogLevel::Info});
    CHECK(testdocs::settle(model), name + ": level filter settles");
    testdocs::pump(300);
    jump(kErrors, true);
    CHECK(outcome == Outcome::NotFound, name + ": hidden errors are not targets");

    // Модель перестраивается, пока переход ещё идёт (пул придержан — скан не
    // начался). Синхронный переход уже сделан (1100). Фоновый на резидентной
    // вкладке прерван перестройкой: номер из снапшота больше ничего не значит,
    // вкладка сообщает об этом и выделение не трогает. На индексной фильтр сам
    // идёт в пуле — скан может успеть до его итога (тогда тоже 1100). Итог в
    // любом случае один.
    model.setLogLevelFilter({});
    CHECK(testdocs::settle(model), name + ": reset settles");
    testdocs::pump(300);
    view.goToRow(700);
    const int before = finished;
    {
        PoolHold hold;
        tab.goToLevel(kErrors, true);
        model.setLogLevelFilter({LogLevel::Error, LogLevel::Info});
    }
    CHECK(testdocs::waitFor([&] { return finished > before; }, 20000) && testdocs::settle(model),
          name + ": jump across a filter change finishes");
    testdocs::pump(300);
    const QModelIndex cur = view.currentIndex();
    const QString landed = cur.isValid() ? model.messageAt(cur.row()) : QString();
    CHECK(finished == before + 1, name + ": one result across a filter change");
    if (syncRows > 0) {
        CHECK(outcome == Outcome::Moved && landed.endsWith(" 1100"), name + ": synchronous jump is done before the filter");
    } else if (!model.isIndexedBackend()) {
        CHECK(outcome == Outcome::Interrupted && landed.endsWith(" 700"),
              name + ": a rebuilt model interrupts the background jump");
    } else {
        CHECK((outcome == Outcome::Moved && landed.endsWith(" 1100"))
                  || (outcome == Outcome::Interrupted && landed.endsWith(" 700")),
              name + ": background jump either lands or reports the interruption");
    }
}

// Многострочные записи во вкладке: F8 идёт по записям, а не по строкам трейса.
static void checkTraces(const QString& name, const QString& path, const QString& schema,
                        int syncRows)
{
    LogViewWidget tab;
    tab.resize(900, 400);
    tab.setParserPattern(schema);
    tab.setExtractionEnabled(true);
    tab.addLogFile(path);
    tab.show();
    CHECK(settled(tab), name + ": load settles");
    testdocs::pump(300);
    tab.setLevelJumpSyncRows(syncRows);
    LogModel& model = *tab.model();
    LogListView& view = *tab.view();
    int finished = 0;
    QObject::connect(&tab, &LogViewWidget::levelJumpFinished,
                     [&](LogLevelMask, LogViewWidget::LevelJumpOutcome) { ++finished; });
    const auto jump = [&](bool forward) {
        const int before = finished;
        tab.goToLevel(kErrors, forward);
        CHECK(testdocs::waitFor([&] { return finished > before; }, 20000), name + ": jump finishes");
        return view.currentIndex().isValid() ? model.messageAt(view.currentIndex().row()) : QString();
    };
    // Ошибки — записи 5, 7 (FATAL), 102, …; у каждой три строки трейса.
    CHECK(jump(true).endsWith("ERROR - trace 5"), name + ": first error record");
    CHECK(jump(true).endsWith("FATAL - trace 7"), name + ": next press skips the stack trace");
    CHECK(jump(true).endsWith("ERROR - trace 102"), name + ": and goes on by records");
    CHECK(jump(false).endsWith("FATAL - trace 7"), name + ": back by records");
    // Из середины трейса назад — к заголовку своей записи.
    view.goToRow(view.currentIndex().row() + 2);
    CHECK(model.messageAt(view.currentIndex().row()).contains("Frame1"), name + ": standing inside a trace");
    CHECK(jump(false).endsWith("FATAL - trace 7"), name + ": from inside a trace back to its header");
    view.goToRow(view.currentIndex().row() + 2);
    CHECK(jump(true).endsWith("ERROR - trace 102"), name + ": from inside a trace on to the next record");
}

// Легенда слитой вкладки (S-14): «Go to Next / Previous Line of This File»
// до порога — сразу, выше — в пуле; переход — goToRow (выбор пользователя),
// действие пользователя раньше итога главнее, перестройка модели —
// сообщение «ещё раз».
static void checkLegend(const QString& name, const QStringList& paths, const QString& schema,
                        bool background)
{
    LogViewWidget tab;
    tab.resize(900, 400);
    tab.setParserPattern(schema);
    tab.setExtractionEnabled(true);
    for (const QString& path : paths)
        tab.addLogFile(path);
    tab.show();
    CHECK(settled(tab) && testdocs::settleViews(*tab.model()), name + ": load settles");
    LogModel& model = *tab.model();
    LogListView& view = *tab.view();
    FileLegend& legend = *tab.fileLegend();
    legend.setFileJumpSyncRows(background ? 0 : 1000000);
    QStringList messages;
    QObject::connect(&tab, &LogViewWidget::statusMessage, [&](const QString& text) { messages << text; });
    int small = -1;
    for (int i = 0; i < legend.chipCount(); ++i)
        if (legend.chipFile(i) && legend.chipFile(i)->filePath == paths.last())
            small = i;
    CHECK(legend.chipCount() == paths.size() && small >= 0, name + ": a chip per file");
    if (small < 0)
        return;
    const LogFile* smallFile = legend.chipFile(small);
    const auto selected = [&]() {
        const QModelIndex cur = view.currentIndex();
        return cur.isValid() && view.selectionModel()->isRowSelected(cur.row(), QModelIndex())
            ? cur.row() : -1;
    };
    const auto go = [&](bool forward) {
        const bool started = legend.goToFileLine(small, forward);
        CHECK(started, name + ": the jump starts");
        CHECK(legend.isFileJumpRunning() == background,
              name + (background ? ": a big tab searches in the pool" : ": a small tab jumps at once"));
        CHECK(testdocs::waitFor([&] { return !legend.isFileJumpRunning(); }, 20000),
              name + ": the jump finishes");
        return selected();
    };
    // Эталон — перебор видимых строк.
    const auto oracle = [&](int from, bool forward) {
        const int n = model.rowCount();
        for (int step = 1; step <= n; ++step) {
            const int row = (((forward ? from + step : from - step) % n) + n) % n;
            if (model.keyAt(row).file.get() == smallFile)
                return row;
        }
        return -1;
    };
    view.goToRow(10);
    int expected = oracle(10, true);
    CHECK(expected >= 0 && go(true) == expected, name + ": next line of the small file");
    const int first = expected;
    expected = oracle(first, true);
    CHECK(go(true) == expected, name + ": and the next one");
    CHECK(go(false) == first, name + ": previous line of the small file");
    CHECK(model.keyAt(selected()).file.get() == smallFile, name + ": the selected row is of that file");
    // Через край.
    view.goToRow(model.rowCount() - 1);
    expected = oracle(model.rowCount() - 1, true);
    CHECK(go(true) == expected, name + ": wraps past the end");

    // Файл скрыт фильтром по файлу — строк нет, выбор на месте, сообщение.
    model.setHiddenFiles({paths.last()});
    CHECK(testdocs::settleViews(model), name + ": file filter settles");
        view.goToRow(5);
    messages.clear();
    legend.goToFileLine(small, true);
    CHECK(testdocs::waitFor([&] { return !legend.isFileJumpRunning(); }, 20000)
              && selected() == 5 && !messages.isEmpty(),
          name + ": a hidden file has no line to go to");
    model.setHiddenFiles({});
    CHECK(testdocs::settleViews(model), name + ": file filter reset settles");
    
    if (!background)
        return;
    // Пользователь выбрал строку раньше итога — итог отброшен.
    view.goToRow(20);
    {
        PoolHold hold;
        legend.goToFileLine(small, true);
        view.goToRow(30);
    }
    CHECK(testdocs::waitFor([&] { return !legend.isFileJumpRunning(); }, 20000) && selected() == 30,
          name + ": a user action before the result wins");
    // Пользователь успел действие, а потом модель перестроилась — молча.
    view.goToRow(25);
    const RecordKey picked = model.keyAt(35);
    {
        PoolHold hold;
        legend.goToFileLine(small, true);
        view.goToRow(35);
        model.setLogLevelFilter({LogLevel::Info, LogLevel::Error, LogLevel::Warn, LogLevel::Fatal});
    }
    CHECK(testdocs::waitFor([&] { return !legend.isFileJumpRunning(); }, 20000)
              && testdocs::settleViews(model),
          name + ": jump after a user action finishes");
    const RecordKey now = selected() >= 0 ? model.keyAt(selected()) : RecordKey();
    CHECK(now.file == picked.file && now.line == picked.line, name + ": the user's own row stays");
    model.setLogLevelFilter({});
    CHECK(testdocs::settleViews(model), name + ": level filter reset settles");
    // Модель перестроилась посреди поиска, пользователь ничего не делал —
    // поиск идёт заново от того же выбора и доходит до строки файла.
    view.goToRow(40);
    const int expectedAfter = oracle(40, true);
    {
        PoolHold hold;
        legend.goToFileLine(small, true);
        model.setLogLevelFilter({LogLevel::Info, LogLevel::Error, LogLevel::Warn, LogLevel::Fatal});
    }
    CHECK(testdocs::waitFor([&] { return !legend.isFileJumpRunning() && !model.isFiltering(); }, 20000)
              && testdocs::settleViews(model, [&] { return legend.isFileJumpRunning(); }),
          name + ": jump across a filter change finishes");
    CHECK(selected() >= 0 && model.keyAt(selected()).file.get() == smallFile
              && selected() == expectedAfter,
          name + ": a rebuilt model restarts the jump, which lands on the file");
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    if (!temporary.isValid())
        return 1;
    const QDir dir(temporary.path());
    const QString schema = testdocs::buildSchema();
    const QString a = testdocs::writeFile(dir, "a.log", fixture(0, "alpha"));
    const QString b = testdocs::writeFile(dir, "b.log", fixture(30, "beta"));

    const QString t1 = testdocs::writeFile(dir, "t1.log", traceFixture(0, "trace"));
    const QString t2 = testdocs::writeFile(dir, "t2.log", traceFixture(17, "other"));

    // --- findRowWithLevel против эталона ---------------------------------------
    {
        auto resident = testdocs::buildResident({t1}, schema);
        auto indexed = testdocs::buildIndexed({t1}, schema);
        auto residentMerged = testdocs::buildResident({t1, t2}, schema);
        auto indexedMerged = testdocs::buildIndexed({t1, t2}, schema, testdocs::IndexedLoad::Concurrent);
        struct { const char* name; LogModel* model; } models[] = {
            {"resident", resident.model.get()},
            {"indexed", indexed.model.get()},
            {"resident merged", residentMerged.model.get()},
            {"indexed merged", indexedMerged.model.get()},
        };
        for (const auto& m : models) {
            compareWithOracle(QString::fromLatin1(m.name), *m.model);
            // Видимое подмножество: поиск идёт только по нему. «Frame1» прячет
            // заголовки записей и оставляет по строке трейса — целью становится
            // первая ПОКАЗАННАЯ строка записи.
            FilterRuleSet rules;
            FilterRule frames;
            frames.text = QStringLiteral("Frame");
            rules.rules.append(frames);
            FilterRule notFirst;
            notFirst.action = FilterRule::Action::Exclude;
            notFirst.text = QStringLiteral("Frame0");
            rules.rules.append(notFirst);
            m.model->setFilterRules(rules);
            CHECK(testdocs::settle(*m.model), "filter settles");
            CHECK(m.model->rowCount() > 0 && m.model->messageAt(0).contains("Frame1"),
                  QString::fromLatin1(m.name) + ": only trace lines 1 and 2 are shown");
            compareWithOracle(QString::fromLatin1(m.name) + " filtered", *m.model);
            m.model->setFilterRules(FilterRuleSet());
            CHECK(testdocs::settle(*m.model), "filter reset settles");
        }
    }

    // --- Асинхронно: результат, сдвиг через вставки, перестройка --------------
    {
        auto doc = testdocs::buildResident({a}, schema);
        LogModel& model = *doc.model;
        QuickSearch search;
        int found = -2, invalidated = 0;
        QObject::connect(&search, &QuickSearch::finished, [&](int row) { found = row; });
        QObject::connect(&search, &QuickSearch::invalidated, [&] { ++invalidated; });

        search.startLevel(&model, kErrors, true, 700);
        CHECK(search.isRunning(), "level search runs");
        CHECK(testdocs::waitFor([&] { return found != -2; }), "level search finishes");
        CHECK(found == 1100 && search.startRowNow() == 700, "forward from row 700 finds row 1100");

        // Строки, вставленные перед найденной и перед стартовой, пока идёт скан.
        auto earlier = testdocs::buildResident({b}, schema);
        QVector<std::shared_ptr<LogEntry>> batch;
        const QDateTime limit = model.visibleTimestampAt(1000); // b.log начинается позже a.log
        for (const auto& entry : earlier.model->residentEntriesForFieldMutation()) {
            if (entry->timestamp() < limit)
                batch.append(entry);
        }
        std::sort(batch.begin(), batch.end(), logEntryPtrLess);
        found = -2;
        const int before = model.rowCount();
        {
            PoolHold hold;
            search.startLevel(&model, kErrors, true, 700);
            model.mergeEntries(batch);
        }
        const int inserted = model.rowCount() - before;
        CHECK(inserted > 0, "entries merged during the level search");
        CHECK(testdocs::waitFor([&] { return found != -2; }), "shifted level search finishes");
        CHECK(found >= 0 && model.messageAt(found).endsWith("alpha 1100"),
              "found row is mapped through insertions to the current model");
        CHECK(search.startRowNow() > 700
                  && model.messageAt(search.startRowNow()).endsWith("alpha 700"),
              "the start row is mapped through the same insertions");
        CHECK(invalidated == 0, "insertions do not invalidate");

        found = -2;
        {
            PoolHold hold;
            search.startLevel(&model, kErrors, false, -1);
            model.setLogLevelFilter({LogLevel::Error}); // < 100k записей — синхронный reset
        }
        CHECK(testdocs::waitFor([&] { return invalidated == 1; }), "reset invalidates the level search");
        testdocs::pump(50);
        CHECK(found == -2, "invalidated level search reports no row");
    }

    // --- Строка файла (S-14): ядро против эталона ------------------------------
    // c.log — 30 строк в середине и в конце времени a/b.
    QByteArray smallBytes;
    for (int i = 0; i < 30; ++i)
        smallBytes += line(i < 15 ? 100 + i : 400 + i, "INFO", "gamma " + QByteArray::number(i));
    const QString c = testdocs::writeFile(dir, "c.log", smallBytes);
    {
        auto resident = testdocs::buildResident({a}, schema);
        auto indexed = testdocs::buildIndexed({a}, schema);
        auto residentMerged = testdocs::buildResident({a, b, c}, schema);
        auto indexedMerged = testdocs::buildIndexed({a, b, c}, schema, testdocs::IndexedLoad::Concurrent);
        auto stranger = testdocs::buildResident({t1}, schema);
        struct { const char* name; testdocs::Document* doc; } docs[] = {
            {"resident", &resident},
            {"indexed", &indexed},
            {"resident merged", &residentMerged},
            {"indexed merged", &indexedMerged},
        };
        for (const auto& d : docs) {
            LogModel& model = *d.doc->model;
            const QString name = QString::fromLatin1(d.name);
            QVector<const LogFile*> files;
            for (const LogFilePtr& f : d.doc->files)
                files << f.get();
            CHECK(!files.isEmpty(), name + ": files known");
            compareFileOracle(name, model, files);
            // Файла во вкладке нет — ничего.
            const LogFile* absent = stranger.files.value(0).get();
            CHECK(model.findRowOfFile(absent, 10, true) == -1
                      && model.findRowOfFile(nullptr, 10, true) == -1,
                  name + ": a file not in the tab has no rows");
            // Под фильтром — только видимые строки.
            model.setLogLevelFilter({LogLevel::Error, LogLevel::Warn});
            CHECK(testdocs::settle(model), name + ": level filter settles");
            compareFileOracle(name + " filtered", model, files);
            model.setLogLevelFilter({});
            CHECK(testdocs::settle(model), name + ": level filter reset settles");
            if (files.size() > 1) {
                // Файл скрыт фильтром по файлу — у него строк нет.
                model.setHiddenFiles({files.last()->filePath});
                CHECK(testdocs::settle(model), name + ": file filter settles");
                CHECK(model.findRowOfFile(files.last(), 0, true) == -1
                          && model.findRowOfFile(files.last(), -1, false) == -1,
                      name + ": a hidden file has no rows");
                compareFileOracle(name + " file hidden", model, files);
                model.setHiddenFiles({});
                CHECK(testdocs::settle(model), name + ": file filter reset settles");
            }
        }
    }

    // --- Строка файла в фоне: сдвиг через вставки, перестройка -----------------
    {
        auto doc = testdocs::buildResident({a}, schema);
        LogModel& model = *doc.model;
        const LogFile* alpha = doc.files.value(0).get();
        QuickSearch search;
        int found = -2, invalidated = 0;
        QObject::connect(&search, &QuickSearch::finished, [&](int row) { found = row; });
        QObject::connect(&search, &QuickSearch::invalidated, [&] { ++invalidated; });
        search.startFile(&model, alpha, true, 700);
        CHECK(testdocs::waitFor([&] { return found != -2; }), "file search finishes");
        CHECK(found == 701, "next row of the file from row 700");

        auto earlier = testdocs::buildResident({b}, schema);
        QVector<std::shared_ptr<LogEntry>> batch;
        const QDateTime limit = model.visibleTimestampAt(1000);
        for (const auto& entry : earlier.model->residentEntriesForFieldMutation()) {
            if (entry->timestamp() < limit)
                batch.append(entry);
        }
        std::sort(batch.begin(), batch.end(), logEntryPtrLess);
        found = -2;
        {
            PoolHold hold;
            search.startFile(&model, alpha, true, 700);
            model.mergeEntries(batch);
        }
        CHECK(testdocs::waitFor([&] { return found != -2; }), "shifted file search finishes");
        CHECK(found >= 0 && model.messageAt(found).endsWith("alpha 701"),
              "found file row is mapped through insertions");
        CHECK(invalidated == 0, "insertions do not invalidate the file search");
        found = -2;
        {
            PoolHold hold;
            search.startFile(&model, alpha, false, -1);
            model.setLogLevelFilter({LogLevel::Error});
        }
        CHECK(testdocs::waitFor([&] { return invalidated == 1; }), "reset invalidates the file search");
        testdocs::pump(50);
        CHECK(found == -2, "invalidated file search reports no row");
    }

    // --- Вкладка: резидентная и индексная, синхронный путь и фон --------------
    AppSettings& settings = AppSettings::instance();
    settings.setIndexedThresholdMB(512);
    checkTab(QStringLiteral("resident sync"), a, schema, 1000000);
    checkTab(QStringLiteral("resident background"), a, schema, 0);
    checkTraces(QStringLiteral("resident traces"), t1, schema, 1000000);
    settings.setIndexedThresholdMB(0);
    checkTab(QStringLiteral("indexed sync"), a, schema, 1000000);
    checkTab(QStringLiteral("indexed background"), a, schema, 0);
    checkTraces(QStringLiteral("indexed traces"), t1, schema, 0);
    settings.setIndexedThresholdMB(512);
    checkLegend(QStringLiteral("legend resident sync"), {a, b, c}, schema, false);
    checkLegend(QStringLiteral("legend resident background"), {a, b, c}, schema, true);
    settings.setIndexedThresholdMB(0);
    checkLegend(QStringLiteral("legend indexed sync"), {a, b, c}, schema, false);
    checkLegend(QStringLiteral("legend indexed background"), {a, b, c}, schema, true);

    std::fprintf(stdout, "Level jump: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
