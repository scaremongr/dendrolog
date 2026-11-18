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
#include "logviewwidget.h"
#include "quicksearch.h"
#include "testdocuments.h"

#include <QApplication>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cstdio>

static int failures = 0;
#define CHECK(condition, label) do { \
    if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL: %s (line %d)\n", qPrintable(QString(label)), __LINE__); } \
} while (0)

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

    // Модель перестраивается сразу после запуска. Синхронный переход уже
    // сделан (1100). Фоновый либо успел до перестройки (тогда тоже 1100), либо
    // прерван ею: номер из снапшота больше ничего не значит, вкладка сообщает
    // об этом и выделение не трогает. Итог в любом случае один.
    model.setLogLevelFilter({});
    CHECK(testdocs::settle(model), name + ": reset settles");
    testdocs::pump(300);
    view.goToRow(700);
    const int before = finished;
    tab.goToLevel(kErrors, true);
    model.setLogLevelFilter({LogLevel::Error, LogLevel::Info});
    CHECK(testdocs::waitFor([&] { return finished > before; }, 20000) && testdocs::settle(model),
          name + ": jump across a filter change finishes");
    testdocs::pump(300);
    const QModelIndex cur = view.currentIndex();
    const QString landed = cur.isValid() ? model.messageAt(cur.row()) : QString();
    CHECK(finished == before + 1, name + ": one result across a filter change");
    if (syncRows > 0) {
        CHECK(outcome == Outcome::Moved && landed.endsWith(" 1100"), name + ": synchronous jump is done before the filter");
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
        search.startLevel(&model, kErrors, true, 700);
        const int before = model.rowCount();
        model.mergeEntries(batch);
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
        search.startLevel(&model, kErrors, false, -1);
        model.setLogLevelFilter({LogLevel::Error}); // < 100k записей — синхронный reset
        CHECK(testdocs::waitFor([&] { return invalidated == 1; }), "reset invalidates the level search");
        testdocs::pump(50);
        CHECK(found == -2, "invalidated level search reports no row");
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

    std::fprintf(stdout, "Level jump: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
