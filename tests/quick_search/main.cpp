// ============================================================================
// quick_search — фоновый быстрый поиск (F3 / Shift+F3):
//   • скан по снапшоту даёт ту же строку, что синхронный LogModel::findNext/
//     PreviousOccurrence, на обоих бэкендах, при любом старте, в обе стороны,
//     с заворачиванием и без;
//   • найденная строка пересчитывается через вставки, случившиеся за время
//     скана; reset/удаление строк — invalidated() вместо устаревшего номера;
//   • отменённый поиск ничего не присылает;
//   • строка старта (startRowFor): выбранная строка, а если её скрыл фильтр —
//     её место среди видимых строк, сверенное с перебором и с синхронным
//     поиском по полной модели.
// ============================================================================

#include "quicksearch.h"
#include "testdocuments.h"

#include <QHash>
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

// Строка старта против перебора. order — ключи всех строк модели БЕЗ фильтра
// (её порядок), модель сейчас отфильтрована. Для каждой строки-выбора место
// среди видимых считается перебором: p — число видимых строк, стоящих раньше;
// matches — есть ли в строке полной модели NEEDLE.
// Старт: строка видна — она сама; скрыта — вперёд p - 1 (первой проверяется
// строка p), назад p (первой проверяется p - 1), в том числе когда скрытая
// запись стоит после ВСЕХ видимых строк (p == n: вперёд — сразу через край).
static void checkStartRows(const QString& name, LogModel& model,
                           const QVector<LogModel::EntryKey>& order,
                           const QVector<bool>& matches)
{
    const int n = model.rowCount();
    QHash<QPair<int, const LogFile*>, int> visibleRow;
    for (int row = 0; row < n; ++row) {
        const LogModel::EntryKey key = model.keyForRow(row);
        visibleRow.insert({key.logicalEntryId, key.sourceFile}, row);
    }
    CHECK(visibleRow.size() == n && n > 0 && n < order.size(), name + ": fixture rows are unique records");
    const int total = order.size();
    QVector<int> rowOf(total, -1); // позиция полной модели → видимая строка
    for (int i = 0; i < total; ++i)
        rowOf[i] = visibleRow.value({order[i].logicalEntryId, order[i].sourceFile}, -1);
    // Первое видимое совпадение от позиции i полной модели, не включая её.
    const auto expected = [&](int i, int step) {
        for (int k = 1; k <= total; ++k) {
            const int j = ((i + step * k) % total + total) % total;
            if (rowOf[j] >= 0 && matches[j])
                return rowOf[j];
        }
        return -1;
    };

    const LogScanSnapshot snapshot = model.scanSnapshot(true);
    const QString needle = QStringLiteral("NEEDLE");
    int mismatches = 0, hiddenChecked = 0, beyondLast = 0, searchMismatches = 0;
    int p = 0; // видимых строк раньше позиции i
    for (int i = 0; i < order.size(); ++i) {
        const LogModel::EntryKey key = order[i];
        const int row = rowOf[i];
        int wantForward, wantBackward;
        if (row >= 0) {
            wantForward = wantBackward = row;
        } else {
            wantForward = p - 1;
            wantBackward = p;
            ++hiddenChecked;
            if (p == n)
                ++beyondLast;
        }
        // Выбранная строка скрыта: view отдаёт -1 и якорь-запись.
        const int forward = QuickSearch::startRowFor(model, -1, key.logicalEntryId, key.sourceFile, true);
        const int backward = QuickSearch::startRowFor(model, -1, key.logicalEntryId, key.sourceFile, false);
        if (forward != wantForward || backward != wantBackward) {
            if (++mismatches <= 5)
                std::fprintf(stderr, "  %s origin %d: forward %d/%d backward %d/%d\n", qPrintable(name),
                             i, forward, wantForward, backward, wantBackward);
        }
        // Поиск от скрытой строки находит ближайшее видимое совпадение в
        // порядке ПОЛНОЙ модели — то, к которому пользователь пришёл бы от
        // выбранной строки, пролистав скрытые.
        // (Каждая 17-я скрытая строка и все за последней видимой: скан читает
        // текст, а под санитайзером это дорого.)
        if (row < 0 && (p == n || hiddenChecked % 17 == 0)) {
            const qint64 next = QuickSearch::scan(snapshot, needle, Qt::CaseSensitive, true, forward, true);
            const qint64 previous = QuickSearch::scan(snapshot, needle, Qt::CaseSensitive, false, backward, true);
            if (next != expected(i, 1) || previous != expected(i, -1))
                ++searchMismatches;
        }
        if (row >= 0)
            ++p;
    }
    CHECK(mismatches == 0, name + ": start row matches the brute-force position");
    CHECK(searchMismatches == 0,
          name + ": search from a hidden row finds the nearest visible match in tab order");
    CHECK(hiddenChecked > 0 && beyondLast > 0, name + ": hidden rows inside and past the visible ones are covered");

    // Выбранная видимая строка важнее якоря; без выбора — с начала или с конца.
    CHECK(QuickSearch::startRowFor(model, 7, order[0].logicalEntryId, order[0].sourceFile, true) == 7
              && QuickSearch::startRowFor(model, 7, -1, nullptr, false) == 7,
          name + ": the selected row wins");
    CHECK(QuickSearch::startRowFor(model, -1, -1, nullptr, true) == -1
              && QuickSearch::startRowFor(model, -1, -1, nullptr, false) == n,
          name + ": no selection starts from the top or from the end");
    CHECK(QuickSearch::startRowFor(model, n + 3, -1, nullptr, true) == -1,
          name + ": a row past the end is not a selection");
    CHECK(QuickSearch::startRowFor(model, -1, 1 << 30, order[0].sourceFile, true) == -1
              && QuickSearch::startRowFor(model, -1, 1 << 30, order[0].sourceFile, false) == n,
          name + ": a record that is gone is not a selection");
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
            // Порядок полной модели и строки с совпадением в ней — эталон для
            // строки старта под фильтром.
            QVector<LogModel::EntryKey> order;
            QVector<bool> matches;
            for (int row = 0; row < m.model->rowCount(); ++row) {
                order.append(m.model->keyForRow(row));
                matches.append(m.model->messageAt(row).contains(QLatin1String("NEEDLE")));
            }
            // Видимое подмножество: поиск идёт только по нему.
            m.model->setLogLevelFilter({LogLevel::Error});
            CHECK(testdocs::settle(*m.model), "filter settles");
            compareWithModel(QString::fromLatin1(m.name) + " filtered", *m.model);
            checkStartRows(QString::fromLatin1(m.name) + " filtered", *m.model, order, matches);
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
