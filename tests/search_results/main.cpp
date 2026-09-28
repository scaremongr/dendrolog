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
    std::fprintf(stdout, "Search results: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
