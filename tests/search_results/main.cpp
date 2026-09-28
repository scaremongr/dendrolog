#include "searchresultscontroller.h"
#include "testdocuments.h"
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>

static int failures = 0;
#define CHECK(condition, label) do { \
    if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

static FilterRuleSet query(const QString& text, const QString& field = {}, bool regex = false)
{
    FilterRuleSet rules;
    FilterRule rule;
    rule.text = text;
    rule.fieldName = field;
    rule.isRegex = regex;
    rules.rules.append(rule);
    rules.bindFields({"Timestamp", "Thread", "Level", "Message"}, !field.isEmpty());
    return rules;
}

// Wait for actual publication rather than a quiet-period heuristic: a slow
// disk may stay quiet while a worker is still searching.
static void completed(LogModel& model, const std::function<void()>& start)
{
    bool done = false;
    QObject relay;
    QObject::connect(&model, &LogModel::modelFiltered, &relay, [&](int) { done = true; });
    start();
    CHECK(testdocs::waitFor([&] { return done; }), "operation publishes its result");
}

// ---------------------------------------------------------------------------
// Живая выдача: SearchResultsController поверх растущего источника.
// ---------------------------------------------------------------------------

// Строка фикстуры: секунда задаёт место во времени (порядок слитой вкладки).
static QByteArray logLine(int second, const char* level, const QByteArray& text)
{
    return QByteArray("2026-03-05 10:") + QByteArray::number(second / 60).rightJustified(2, '0')
         + ':' + QByteArray::number(second % 60).rightJustified(2, '0') + ".000 [worker-3] "
         + level + " - " + text + '\n';
}

// 1000 строк: уровни чередуются INFO/ERROR, в каждой сотне по NEEDLE на
// INFO- и на ERROR-строке — 20 совпадений, 10 из них ERROR.
static QByteArray growingBase(int firstSecond)
{
    QByteArray out;
    for (int i = 0; i < 1000; ++i) {
        const bool needle = i % 100 < 2;
        out += logLine(firstSecond + i / 50, i % 2 ? "ERROR" : "INFO",
                       (needle ? "NEEDLE base " : "ordinary ") + QByteArray::number(i));
    }
    return out;
}

// Выдача пришла в покой с заданным числом строк; pump — пауза сверх
// дебаунса контроллера, чтобы отложенный полный поиск успел бы проявиться.
static bool waitResults(SearchResultsController& c, int rows)
{
    const bool reached = testdocs::waitFor(
        [&] { return !c.model()->isFiltering() && c.model()->rowCount() == rows; }, 20000);
    testdocs::pump(SearchResultsController::kRefreshDelayMs + 150);
    return reached && !c.model()->isFiltering() && c.model()->rowCount() == rows;
}

static void testLiveSearch(const QDir& dir, const QString& schema, const QString& largePath)
{
    const FilterRuleSet needle = query("NEEDLE");

    // --- Индексная вкладка, один файл, без фильтра -------------------------
    {
        const QString path = testdocs::writeFile(dir, "grow.log", growingBase(0));
        auto doc = testdocs::buildIndexed({path}, schema);
        SearchResultsController c;
        int resets = 0;
        QObject::connect(c.model(), &QAbstractItemModel::modelReset, [&] { ++resets; });
        c.setSource(doc.model.get());
        c.search(needle);
        CHECK(waitResults(c, 20), "indexed: initial search");
        CHECK(c.statusText().startsWith("20 match"), "indexed: status shows the count");

        const int before = resets;
        // Текст конца файла уже прочитан и лежит в кэше чанков (как у view,
        // который показывает хвост): короткий последний чанк после дозаписи
        // обязан перечитаться, а не отдавать новые строки пустыми.
        CHECK(!doc.model->messageAt(doc.model->rowCount() - 1).isEmpty(), "indexed: tail text cached");
        CHECK(!c.model()->messageAt(19).isEmpty(), "indexed: last result text cached");
        QByteArray tail;
        for (int i = 0; i < 3; ++i)
            tail += logLine(100, "INFO", "NEEDLE tail " + QByteArray::number(i));
        tail += logLine(100, "INFO", "ordinary tail");
        CHECK(testdocs::appendIndexedTail(doc, 0, tail, schema), "indexed: append tail");
        CHECK(waitResults(c, 23), "indexed: appended matches are found");
        CHECK(resets == before, "indexed: results grow without reset or a new full search");
        CHECK(c.model()->messageAt(22).endsWith("NEEDLE tail 2"), "indexed: appended match at the end");
        CHECK(doc.model->messageAt(doc.model->rowCount() - 1).endsWith("ordinary tail"),
              "indexed: appended source line is readable despite the cached tail chunk");
        CHECK(c.statusText().startsWith("23 match"), "indexed: status follows appends");

        // Хвост без '\n': строка «предварительная» и пока не совпадает; когда
        // её допишут, та же строка переиндексируется и должна попасть в выдачу.
        CHECK(testdocs::appendIndexedTail(doc, 0, logLine(101, "INFO", "partial").chopped(1), schema),
              "indexed: append provisional line");
        CHECK(waitResults(c, 23), "indexed: provisional line does not match yet");
        CHECK(testdocs::appendIndexedTail(doc, 0, " NEEDLE completed\n", schema),
              "indexed: complete provisional line");
        CHECK(waitResults(c, 24), "indexed: completed line is re-evaluated");
        CHECK(c.model()->messageAt(23).endsWith("partial NEEDLE completed"),
              "indexed: completed line shows its full text");
        CHECK(resets == before, "indexed: provisional tail handled without reset");

        // Фильтр источника по уровню: полный поиск заново, затем дописанные
        // строки проверяются, только если источник их показывает.
        doc.model->setLogLevelFilter({LogLevel::Error});
        CHECK(waitResults(c, 10), "indexed filtered: new full search after the source filter");
        const int filteredResets = resets;
        QByteArray mixed = logLine(102, "ERROR", "NEEDLE err") + logLine(102, "INFO", "NEEDLE info")
                         + logLine(102, "ERROR", "ordinary err");
        CHECK(testdocs::appendIndexedTail(doc, 0, mixed, schema), "indexed filtered: append tail");
        CHECK(waitResults(c, 11), "indexed filtered: only lines visible in the source are added");
        CHECK(c.model()->messageAt(10).endsWith("NEEDLE err"), "indexed filtered: visible match added");
        CHECK(resets == filteredResets, "indexed filtered: appended without reset");

        // Не живой режим: изменения, требующие полного поиска, откладываются.
        c.setLive(false);
        doc.model->setLogLevelFilter({});
        testdocs::pump(SearchResultsController::kRefreshDelayMs + 300);
        CHECK(c.model()->rowCount() == 11, "not live: full search is deferred");
        c.setLive(true);
        CHECK(waitResults(c, 26), "live again: the deferred search runs");

        // Выбор отображаемых полей источника зеркалится без поиска.
        const int displayResets = resets;
        doc.model->setAvailableFields({"Timestamp", "Thread", "Level", "Message"});
        doc.model->setFieldDisplaySelection(true, {3});
        CHECK(c.model()->data(c.model()->index(0), Qt::DisplayRole).toString() == "NEEDLE base 0",
              "results mirror the source field selection");
        CHECK(resets == displayResets, "field selection mirrored without a search");

        // Источник закрыт — выдача пуста.
        doc.model.reset();
        CHECK(c.model()->rowCount() == 0 && !c.isActive(), "closing the source clears results");
        CHECK(c.statusText() == "No search active.", "closing the source resets the status");
    }

    // --- Слитая индексная вкладка без фильтра --------------------------------
    {
        const QString a = testdocs::writeFile(dir, "merge-a.log", growingBase(0));
        const QString b = testdocs::writeFile(dir, "merge-b.log", growingBase(10));
        auto doc = testdocs::buildIndexed({a, b}, schema, testdocs::IndexedLoad::Concurrent);
        SearchResultsController c;
        c.setSource(doc.model.get());
        c.search(needle);
        CHECK(waitResults(c, 40), "merged: initial search");
        int resets = 0, sourceResets = 0, sourceInserts = 0;
        QObject::connect(c.model(), &QAbstractItemModel::modelReset, [&] { ++resets; });
        QObject::connect(doc.model.get(), &QAbstractItemModel::modelReset, [&] { ++sourceResets; });
        QObject::connect(doc.model.get(), &QAbstractItemModel::rowsInserted,
                         [&](const QModelIndex&, int, int) { ++sourceInserts; });
        const int sourceRows = doc.model->rowCount();
        CHECK(testdocs::appendIndexedTail(doc, 0, logLine(200, "WARN", "NEEDLE merged tail"), schema),
              "merged: append tail");
        CHECK(waitResults(c, 41), "merged: appended match found");
        CHECK(doc.model->rowCount() == sourceRows + 1, "merged: source grew by the tail");
        CHECK(sourceResets == 0 && sourceInserts > 0,
              "merged tab inserts an appended tail instead of resetting the view");
        CHECK(resets == 0, "merged: results grow without reset");
        CHECK(c.model()->messageAt(40).endsWith("NEEDLE merged tail"), "merged: tail match is last");
    }

    // --- Резидентная вкладка ---------------------------------------------------
    {
        const QString path = testdocs::writeFile(dir, "grow-resident.log", growingBase(0));
        auto doc = testdocs::buildResident({path}, schema);
        SearchResultsController c;
        c.setSource(doc.model.get());
        c.search(needle);
        CHECK(waitResults(c, 20), "resident: initial search");
        int resets = 0;
        QObject::connect(c.model(), &QAbstractItemModel::modelReset, [&] { ++resets; });
        CHECK(testdocs::appendResidentTail(doc, 0, logLine(100, "INFO", "NEEDLE resident tail")
                                                      + logLine(100, "INFO", "ordinary"), schema),
              "resident: append tail");
        CHECK(waitResults(c, 21), "resident: appended match found");
        CHECK(resets == 0, "resident: results grow without reset");
        CHECK(c.model()->messageAt(20).endsWith("NEEDLE resident tail"), "resident: tail match is last");
    }

    // --- Дозапись посреди долгого поиска не начинает его заново -----------------
    {
        QFile::remove(dir.filePath("large-growing.log"));
        CHECK(QFile::copy(largePath, dir.filePath("large-growing.log")), "copy large fixture");
        auto doc = testdocs::buildIndexed({dir.filePath("large-growing.log")}, schema);
        SearchResultsController c;
        int resets = 0;
        c.setSource(doc.model.get());
        QObject::connect(c.model(), &QAbstractItemModel::modelReset, [&] { ++resets; });
        c.search(query("LATE_NEEDLE"));
        const int afterStart = resets; // сброс выдачи на старте поиска
        const bool stillSearching = c.model()->isFiltering();
        CHECK(testdocs::appendIndexedTail(doc, 0,
                  logLine(200, "ERROR", "LATE_NEEDLE appended 1")
                + logLine(200, "ERROR", "LATE_NEEDLE appended 2"), schema),
              "large: append during search");
        CHECK(waitResults(c, 7), "large: search completes with appended matches");
        CHECK(resets == afterStart + 1, "large: one publication, the search was not restarted");
        if (!stillSearching)
            std::fprintf(stdout, "note: search finished before the append; restart check is weaker\n");
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 1;
    const QDir dir(temporary.path());
    const QString schema = testdocs::buildSchema();
    QByteArray bytes;
    for (int row = 0; row < 200005; ++row) {
        const bool late = row >= 200000;
        bytes += late ? "2026-03-05 10:00:01.000 [worker-1] ERROR - "
                      : "2026-03-05 10:00:00.000 [worker-2] INFO - ";
        bytes += late ? "LATE_NEEDLE " : "ordinary ";
        bytes += QByteArray::number(row) + '\n';
    }
    const QString path = testdocs::writeFile(dir, "large.log", bytes);
    auto indexed = testdocs::buildIndexed({path}, schema);
    CHECK(indexed.errors.isEmpty(), "load indexed fixture");
    CHECK(indexed.model->rowCount() == 200005, "fixture exceeds former input cap");
    indexed.model->setAvailableFields({"Timestamp", "Thread", "Level", "Message"});
    indexed.model->setFieldDisplaySelection(true, {3});

    LogModel results;
    bool eventDelivered = false;
    completed(results, [&] {
        QTimer::singleShot(0, &app, [&] { eventDelivered = true; });
        results.searchVisible(*indexed.model, query("LATE_NEEDLE"));
        CHECK(results.isIndexedBackend(), "indexed search keeps text on disk");
        CHECK(results.rowCount() == 0, "no unfiltered rows shown while searching");
    });
    CHECK(eventDelivered, "event loop runs during search");
    CHECK(results.rowCount() == 5, "all matches beyond row 200000 are found");
    CHECK(indexed.model->rowCount() == 200005, "search leaves source visible set unchanged");
    for (int row = 0; row < results.rowCount(); ++row) {
        const auto entry = results.entryAt(row);
        CHECK(entry && entry->originalLineNumber() == 200001 + row, "physical source line is preserved");
        const auto key = results.keyForRow(row);
        CHECK(indexed.model->rowForEntry(key.logicalEntryId, key.sourceFile) == 200000 + row,
              "result navigates to matching source entry");
        CHECK(results.data(results.index(row), Qt::DisplayRole).toString()
                  == QStringLiteral("LATE_NEEDLE %1").arg(200000 + row),
              "result preserves selected display fields");
    }

    completed(results, [&] { results.searchVisible(*indexed.model, query("no such message")); });
    CHECK(results.rowCount() == 0, "complete scan may legitimately find no matches");
    completed(results, [&] { results.searchVisible(*indexed.model, query("worker-1", "Thread")); });
    CHECK(results.rowCount() == 5, "field search finds late matches");
    completed(results, [&] { results.searchVisible(*indexed.model, query("worker-1", "Message")); });
    CHECK(results.rowCount() == 0, "field search never falls back to the whole row");

    // Колонки нет в схеме: правило нейтрально, а не ищет по всей строке.
    {
        const FilterRuleSet orphan = query("worker-1", "NoSuchField");
        CHECK(orphan.fieldMissing(0) && orphan.usableRuleCount() == 0,
              "rule bound to a missing column is unusable");
        CHECK(orphan.highlightPatterns().isEmpty(), "unusable rule highlights nothing");
        completed(results, [&] { results.searchVisible(*indexed.model, orphan); });
        CHECK(results.rowCount() == 0, "missing-column query finds nothing");
        // Рядом с пригодным правилом — нейтрально: AND «worker-2 по всей
        // строке» обнулил бы выдачу, нейтральное правило её не трогает.
        FilterRuleSet combined;
        FilterRule late;
        late.text = "LATE_NEEDLE";
        FilterRule missing;
        missing.text = "worker-2";
        missing.fieldName = "NoSuchField";
        combined.rules = {late, missing};
        combined.bindFields({"Timestamp", "Thread", "Level", "Message"}, true);
        CHECK(!combined.fieldMissing(0) && combined.fieldMissing(1), "only the orphan is missing");
        completed(results, [&] { results.searchVisible(*indexed.model, combined); });
        CHECK(results.rowCount() == 5, "missing-column rule does not search the whole row");
        // Log Fields выключены: колонки не проверяются, поиск по всей строке.
        combined.bindFields({"Timestamp", "Thread", "Level", "Message"}, false);
        CHECK(!combined.fieldMissing(1), "fields off: no missing-column state");
    }

    // A broad query may return more than 200000 matches as row references.
    completed(results, [&] { results.searchVisible(*indexed.model, query("ordinary|LATE_NEEDLE", {}, true)); });
    CHECK(results.rowCount() == 200005, "result count has no former input cap either");
    CHECK(results.messageAt(200004).endsWith("LATE_NEEDLE 200004"), "last broad-query result remains readable");

    completed(*indexed.model, [&] { indexed.model->setLogLevelFilter({LogLevel::Error}); });
    completed(results, [&] { results.searchVisible(*indexed.model, query("ordinary|LATE_NEEDLE", {}, true)); });
    CHECK(results.rowCount() == 5, "search respects source visibility");
    completed(*indexed.model, [&] { indexed.model->setTimeRangeFilter(
        QDateTime::fromString("2026-03-05 11:00:00", "yyyy-MM-dd HH:mm:ss"), {}); });
    completed(results, [&] { results.searchVisible(*indexed.model, query("LATE_NEEDLE")); });
    CHECK(results.rowCount() == 0, "empty source subset never expands to full file");
    completed(*indexed.model, [&] { indexed.model->setTimeRangeFilter({}, {}); });
    completed(*indexed.model, [&] { indexed.model->setLogLevelFilter({}); });

    // Replacing a request before processing events must discard old completions.
    int publications = 0;
    QObject relay;
    auto publication = QObject::connect(&results, &LogModel::modelFiltered, &relay,
        [&](int) { ++publications; });
    completed(results, [&] {
        results.searchVisible(*indexed.model, query("ordinary"));
        results.searchVisible(*indexed.model, query("LATE_NEEDLE"));
    });
    testdocs::pump(120);
    CHECK(results.rowCount() == 5 && publications == 1, "only latest request is published");
    QObject::disconnect(publication);

    results.searchVisible(*indexed.model, query("ordinary"));
    results.clear();
    testdocs::pump(120);
    CHECK(results.rowCount() == 0 && !results.isIndexedBackend(), "clear cancels pending indexed results");

    // One file with nonchronological timestamps must retain FILE order even
    // when the search base is a filtered, explicit list of line references.
    const QByteArray unsorted(
        "2026-03-05 10:00:03.000 [a] ERROR - needle last-time\n"
        "2026-03-05 10:00:01.000 [a] INFO - hidden\n"
        "2026-03-05 10:00:02.000 [a] ERROR - needle middle-time\n");
    const QString unsortedPath = testdocs::writeFile(dir, "unsorted.log", unsorted);
    auto unordered = testdocs::buildIndexed({unsortedPath}, schema);
    completed(*unordered.model, [&] { unordered.model->setLogLevelFilter({LogLevel::Error}); });
    completed(results, [&] { results.searchVisible(*unordered.model, query("needle")); });
    CHECK(results.rowCount() == 2 && results.messageAt(0).endsWith("last-time"), "filtered single-file order preserved");
    for (int row = 0; row < results.rowCount(); ++row) {
        const auto key = results.keyForRow(row);
        CHECK(results.rowForEntry(key.logicalEntryId, key.sourceFile) == row, "subset lookups use file order");
    }

    auto merged = testdocs::buildIndexed({unsortedPath, path}, schema, testdocs::IndexedLoad::Concurrent);
    completed(*merged.model, [&] { merged.model->setLogLevelFilter({LogLevel::Error}); });
    completed(results, [&] { results.searchVisible(*merged.model, query("needle")); });
    CHECK(results.rowCount() == 7, "merged search includes all sources");
    for (int row = 1; row < results.rowCount(); ++row)
        CHECK(results.visibleTimestampAt(row - 1) <= results.visibleTimestampAt(row), "merged results stay chronological");

    auto resident = testdocs::buildResident({unsortedPath}, schema);
    completed(results, [&] { results.searchVisible(*resident.model, query("needle")); });
    CHECK(!results.isIndexedBackend() && results.rowCount() == 2, "switch from indexed to resident source");
    auto largeResident = testdocs::buildResident({path}, schema);
    CHECK(largeResident.errors.isEmpty(), "load large resident fixture");
    completed(results, [&] { results.searchVisible(*largeResident.model, query("LATE_NEEDLE")); });
    CHECK(!results.isIndexedBackend() && results.rowCount() == 5, "resident asynchronous search remains complete");
    results.searchVisible(*largeResident.model, query("ordinary"));
    results.clear();
    testdocs::pump(120);
    CHECK(results.rowCount() == 0, "clear cancels resident background results");
    completed(results, [&] {
        results.searchVisible(*merged.model, query("needle"));
        merged.model.reset();
    });
    CHECK(results.rowCount() == 7, "source lifetime is independent of background request");
    results.searchVisible(*resident.model, query("[", {}, true));
    CHECK(results.rowCount() == 0, "invalid-only query does not return every row");
    results.searchVisible(*resident.model, {});
    CHECK(results.rowCount() == 0, "empty query clears results");
    {
        LogModel ephemeral;
        ephemeral.searchVisible(*indexed.model, query("ordinary"));
    }
    testdocs::pump(120);
    CHECK(indexed.model->rowCount() == 200005, "closing results leaves source intact");

    testLiveSearch(dir, schema, path);
    std::fprintf(stdout, "Search results: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
