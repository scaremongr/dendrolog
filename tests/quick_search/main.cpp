// ============================================================================
// quick_search — фоновый быстрый поиск (F3 / Shift+F3):
//   • скан по снапшоту даёт ту же строку, что синхронный LogModel::findNext/
//     PreviousOccurrence, на обоих бэкендах, при любом старте, в обе стороны,
//     с заворачиванием и без;
//   • найденная строка пересчитывается через вставки, случившиеся за время
//     скана; reset/удаление строк — invalidated() вместо устаревшего номера;
//   • отменённый поиск ничего не присылает.
// ============================================================================

#include "quicksearch.h"
#include "testdocuments.h"

#include <QTemporaryDir>
#include <cstdio>

static int failures = 0;
#define CHECK(condition, label) do { \
    if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL: %s (line %d)\n", qPrintable(QString(label)), __LINE__); } \
} while (0)

static QByteArray line(int second, const char* level, const QByteArray& text)
{
    return QByteArray("2026-03-05 10:") + QByteArray::number(second / 60).rightJustified(2, '0')
         + ':' + QByteArray::number(second % 60).rightJustified(2, '0') + ".000 [w] "
         + level + " - " + text + '\n';
}

// 3000 строк, NEEDLE (в разном регистре) на нескольких строках.
static QByteArray fixture(int firstSecond)
{
    QByteArray out;
    for (int i = 0; i < 3000; ++i) {
        QByteArray text = "ordinary " + QByteArray::number(i);
        if (i % 700 == 13)
            text = "NEEDLE " + QByteArray::number(i);
        else if (i % 900 == 5)
            text = "needle lower " + QByteArray::number(i);
        out += line(firstSecond + i / 20, i % 3 ? "INFO" : "ERROR", text);
    }
    return out;
}

// Скан снапшота обязан совпасть с синхронным поиском модели во всех режимах.
static void compareWithModel(const QString& name, LogModel& model)
{
    const LogScanSnapshot snapshot = model.scanSnapshot(true);
    const int n = model.rowCount();
    CHECK(snapshot.rowCount() == n, name + ": snapshot covers the visible rows");
    const QVector<int> starts = {-1, 0, 1, n / 2, n - 2, n - 1, n, n + 5};
    const QStringList texts = {QStringLiteral("NEEDLE"), QStringLiteral("absent text"),
                               QStringLiteral("ordinary 2999")};
    int mismatches = 0;
    for (const QString& text : texts) {
        for (const Qt::CaseSensitivity cs : {Qt::CaseSensitive, Qt::CaseInsensitive}) {
            for (const bool wrap : {true, false}) {
                for (const int start : starts) {
                    const QModelIndex next = model.findNextOccurrence(text, start, cs, wrap);
                    const qint64 scanNext = QuickSearch::scan(snapshot, text, cs, true, start, wrap);
                    const QModelIndex prev = model.findPreviousOccurrence(text, start, cs, wrap);
                    const qint64 scanPrev = QuickSearch::scan(snapshot, text, cs, false, start, wrap);
                    if (scanNext != (next.isValid() ? next.row() : -1)
                        || scanPrev != (prev.isValid() ? prev.row() : -1)) {
                        if (++mismatches <= 5)
                            std::fprintf(stderr, "  %s '%s' cs=%d wrap=%d start=%d: next %lld/%d prev %lld/%d\n",
                                         qPrintable(name), qPrintable(text), int(cs), int(wrap), start,
                                         (long long)scanNext, next.isValid() ? next.row() : -1,
                                         (long long)scanPrev, prev.isValid() ? prev.row() : -1);
                    }
                }
            }
        }
    }
    CHECK(mismatches == 0, name + ": background scan matches the synchronous search");
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    if (!temporary.isValid())
        return 1;
    const QDir dir(temporary.path());
    const QString schema = testdocs::buildSchema();
    const QString a = testdocs::writeFile(dir, "a.log", fixture(0));
    const QString b = testdocs::writeFile(dir, "b.log", fixture(30));

    // --- Эквивалентность синхронному поиску -----------------------------------
    {
        auto resident = testdocs::buildResident({a}, schema);
        auto indexed = testdocs::buildIndexed({a}, schema);
        auto residentMerged = testdocs::buildResident({a, b}, schema);
        auto indexedMerged = testdocs::buildIndexed({a, b}, schema, testdocs::IndexedLoad::Concurrent);
        struct { const char* name; LogModel* model; } models[] = {
            {"resident", resident.model.get()},
            {"indexed", indexed.model.get()},
            {"resident merged", residentMerged.model.get()},
            {"indexed merged", indexedMerged.model.get()},
        };
        for (const auto& m : models) {
            compareWithModel(QString::fromLatin1(m.name), *m.model);
            // Видимое подмножество: поиск идёт только по нему.
            m.model->setLogLevelFilter({LogLevel::Error});
            CHECK(testdocs::settle(*m.model), "filter settles");
            compareWithModel(QString::fromLatin1(m.name) + " filtered", *m.model);
            m.model->setLogLevelFilter({});
            CHECK(testdocs::settle(*m.model), "filter reset settles");
        }
    }

    // --- Пересчёт номера через вставки ---------------------------------------
    {
        using I = QuickSearch::Insertion;
        CHECK(QuickSearch::mapRow(10, {}) == 10, "no insertions");
        CHECK(QuickSearch::mapRow(10, {I{11, 5}}) == 10, "insertion after the row");
        CHECK(QuickSearch::mapRow(10, {I{10, 5}}) == 15, "insertion at the row shifts it");
        CHECK(QuickSearch::mapRow(10, {I{0, 2}, I{12, 3}}) == 15,
              "second insertion counts in updated coordinates");
        CHECK(QuickSearch::mapRow(10, {I{0, 2}, I{13, 3}}) == 12, "insertion after the shifted row");
    }

    // --- Асинхронно: результат, сдвиг, перестройка, отмена --------------------
    {
        auto doc = testdocs::buildResident({a}, schema);
        LogModel& model = *doc.model;
        QuickSearch search;
        int found = -2, invalidated = 0;
        QObject::connect(&search, &QuickSearch::finished, [&](int row) { found = row; });
        QObject::connect(&search, &QuickSearch::invalidated, [&] { ++invalidated; });

        search.start(&model, "NEEDLE", Qt::CaseSensitive, true, 100);
        CHECK(search.isRunning(), "search runs");
        CHECK(testdocs::waitFor([&] { return found != -2; }), "search finishes");
        CHECK(found == 713, "forward from row 100 finds row 713");
        CHECK(!search.isRunning(), "search is done");

        // Строки, вставленные перед найденной, пока идёт скан: номер в текущей
        // модели сдвигается. Слияние — синхронно, до того как результат мог
        // прийти (он доставляется циклом событий).
        auto earlier = testdocs::buildResident({b}, schema);
        QVector<std::shared_ptr<LogEntry>> batch;
        for (const auto& entry : earlier.model->residentEntriesForFieldMutation()) {
            if (entry->timestamp().toString("mm") == "00") // 10:00:xx — раньше, чем 10:01 у row 1400
                batch.append(entry);
        }
        std::sort(batch.begin(), batch.end(), logEntryPtrLess);
        found = -2;
        search.start(&model, "NEEDLE 1413", Qt::CaseSensitive, true, 0);
        const int before = model.rowCount();
        model.mergeEntries(batch);
        const int inserted = model.rowCount() - before;
        CHECK(inserted > 0, "entries merged during the search");
        CHECK(testdocs::waitFor([&] { return found != -2; }), "shifted search finishes");
        CHECK(found >= 0 && model.messageAt(found).endsWith("NEEDLE 1413"),
              "found row is mapped through insertions to the current model");
        CHECK(invalidated == 0, "insertions do not invalidate");

        // Перестройка модели посреди скана — invalidated, а не номер.
        found = -2;
        search.start(&model, "NEEDLE", Qt::CaseSensitive, false, -1);
        model.setLogLevelFilter({LogLevel::Error}); // < 100k записей — синхронный reset
        CHECK(testdocs::waitFor([&] { return invalidated == 1; }), "reset invalidates the search");
        testdocs::pump(50);
        CHECK(found == -2, "invalidated search reports no row");

        // Отмена: ни результата, ни invalidated.
        search.start(&model, "NEEDLE", Qt::CaseSensitive, true, -1);
        search.cancel();
        CHECK(!search.isRunning(), "cancelled search is not running");
        testdocs::pump(200);
        CHECK(found == -2 && invalidated == 1, "cancelled search stays silent");

        // Не найдено.
        search.start(&model, "absent text", Qt::CaseSensitive, true, -1);
        CHECK(testdocs::waitFor([&] { return found != -2; }), "search without matches finishes");
        CHECK(found == -1, "no match reports -1");
    }

    std::fprintf(stdout, "Quick search: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
