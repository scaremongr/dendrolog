#include "searchresultscontroller.h"
#include "bookmarkstore.h"
#include "scrollbarmarks.h"
#include <QSet>
#include "testdocuments.h"
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>

static int failures = 0;
// Метка — строковый литерал либо QString (tag + "…"): QString в "%s" нельзя,
// это неопределённое поведение ровно в тот момент, когда проверка упала.
static void reportFailure(const char* label, int line)
{
    ++failures;
    std::fprintf(stderr, "FAIL: %s (line %d)\n", label, line);
}
static void reportFailure(const QString& label, int line)
{
    reportFailure(label.toUtf8().constData(), line);
}
#define CHECK(condition, label) do { \
    if (!(condition)) reportFailure(label, __LINE__); \
} while (0)

static FilterRuleSet query(const QString& text, const QString& field = {}, bool regex = false)
{
    FilterRuleSet rules;
    FilterRule rule;
    rule.text = text;
    rule.fieldName = field;
    rule.isRegex = regex;
    rules.rules.append(rule);
    rules.bindFields({"Timestamp", "Thread", "Level", "Message"});
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


// Кодировки (F-1, шаг 2): лог начинается ASCII, кириллица в Windows-1251
// приходит дозаписью. Индексатор решает кодировку на хвосте, поисковая база
// перенимает декодер источника и находит кириллицу без учёта регистра —
// вставками, без полного поиска. Смена кодировки пользователем перестраивает
// источник (reset) — выдача ищется заново новым декодером.
static void testEncodingLiveSearch(const QDir& dir, const QString& schema)
{
    // Решение пришло вместе с перечитанной предварительной строкой: ASCII-хвост
    // без '\n' дописан кириллицей. Порция — только dataChanged этой строки,
    // без вставок: выдача переоценивает её (refreshSearchRow) уже новым
    // декодером.
    {
        const QString path = testdocs::writeFile(dir, "provisional-cp1251.log", growingBase(0));
        auto doc = testdocs::buildIndexed({path}, schema);
        SearchResultsController c;
        c.setSource(doc.model.get());
        c.search(query(QStringLiteral("ошибка")));
        CHECK(waitResults(c, 0), "encoding provisional: nothing in the ASCII head");
        CHECK(testdocs::appendIndexedTail(doc, 0, logLine(100, "ERROR", "partial").chopped(1), schema),
              "encoding provisional: append a provisional ASCII line");
        CHECK(waitResults(c, 0), "encoding provisional: still nothing");
        CHECK(testdocs::appendIndexedTail(
                  doc, 0, testdocs::encodeText(QStringLiteral(" ОШИБКА хвоста\n"), TextEncoding::Windows1251),
                  schema),
              "encoding provisional: complete the line in Windows-1251");
        CHECK(waitResults(c, 1), "encoding provisional: the completed Cyrillic line is found");
        CHECK(c.model()->rowCount() == 1
                  && c.model()->messageAt(0).endsWith(QStringLiteral("partial ОШИБКА хвоста")),
              "encoding provisional: the result is decoded with the decided encoding");
    }

    const QString path = testdocs::writeFile(dir, "grow-cp1251.log", growingBase(0));
    auto doc = testdocs::buildIndexed({path}, schema);
    IndexedLogStore* store = doc.model->indexedOrNull();
    CHECK(store && !store->fileEncoding(doc.files.value(0)).decided(), "encoding: ASCII source undecided");
    SearchResultsController c;
    int resets = 0;
    QObject::connect(c.model(), &QAbstractItemModel::modelReset, [&] { ++resets; });
    c.setSource(doc.model.get());
    c.search(query(QStringLiteral("ошибка")));
    CHECK(waitResults(c, 0), "encoding: nothing found in the ASCII head");

    const int before = resets;
    QString tail;
    for (int i = 0; i < 3; ++i)
        tail += QString::fromUtf8(logLine(100, "ERROR", "ОШИБКА диска " + QByteArray::number(i)));
    tail += QString::fromUtf8(logLine(100, "INFO", "обычная строка"));
    CHECK(testdocs::appendIndexedTail(doc, 0, testdocs::encodeText(tail, TextEncoding::Windows1251),
                                      schema),
          "encoding: append Windows-1251 tail");
    CHECK(store->fileEncoding(doc.files.value(0)).decoder.encoding() == TextEncoding::Windows1251,
          "encoding: the tail decided Windows-1251");
    CHECK(waitResults(c, 3), "encoding: Cyrillic tail found case-insensitively");
    CHECK(resets == before, "encoding: results grow without reset after the decision");
    CHECK(c.model()->messageAt(2).endsWith(QStringLiteral("ОШИБКА диска 2")),
          "encoding: result text decoded with the source file's encoding");

    // Смена кодировки пользователем: источник — reset, выдача — заново.
    store->setFileEncoding(doc.files.value(0), TextDecoder(TextEncoding::Koi8R));
    CHECK(waitResults(c, 0), "encoding: KOI8-R reading of Windows-1251 bytes finds nothing");
    store->setFileEncoding(doc.files.value(0), TextDecoder(TextEncoding::Windows1251));
    CHECK(waitResults(c, 3), "encoding: back to Windows-1251 finds the tail again");
    CHECK(c.model()->messageAt(0).endsWith(QStringLiteral("ОШИБКА диска 0")),
          "encoding: result text after the switch back");

    // Резидентный путь (F-1, шаг 3): та же дозапись решает кодировку у
    // парсера, выдача растёт вставками и находит кириллицу без учёта регистра.
    {
        const QString residentPath = testdocs::writeFile(dir, "grow-cp1251-resident.log",
                                                         growingBase(0));
        auto res = testdocs::buildResident({residentPath}, schema);
        SearchResultsController rc;
        int residentResets = 0;
        QObject::connect(rc.model(), &QAbstractItemModel::modelReset, [&] { ++residentResets; });
        rc.setSource(res.model.get());
        rc.search(query(QStringLiteral("ошибка")));
        CHECK(waitResults(rc, 0), "resident encoding: nothing found in the ASCII head");
        const int residentBefore = residentResets;
        CHECK(testdocs::appendResidentTail(res, 0, testdocs::encodeText(tail, TextEncoding::Windows1251),
                                           schema),
              "resident encoding: append Windows-1251 tail");
        CHECK(res.encodings.value(residentPath).decoder.encoding() == TextEncoding::Windows1251,
              "resident encoding: the tail decided Windows-1251");
        CHECK(waitResults(rc, 3), "resident encoding: Cyrillic tail found case-insensitively");
        CHECK(residentResets == residentBefore, "resident encoding: results grow without reset");
        CHECK(rc.model()->rowCount() == 3
                  && rc.model()->messageAt(2).endsWith(QStringLiteral("ОШИБКА диска 2")),
              "resident encoding: result text decoded with the file's encoding");
    }
}

// Шаг 3а F-1: решение по мусорному байту в ASCII-голове — предварительное
// Windows-1251; кириллица UTF-8 приходит дозаписью и пересматривает его.
// Источник перечитывается (reset), выдача ищется заново и даёт то же, что
// на файле, открытом в UTF-8, — на обоих бэкендах.
static void testProvisionalLiveSearch(const QDir& dir, const QString& schema)
{
    QByteArray head = growingBase(0);
    head.insert(head.indexOf('\n'), " \xFF");
    QByteArray tail;
    for (int i = 0; i < 3; ++i)
        tail += logLine(100, "ERROR", "ОШИБКА диска " + QByteArray::number(i)
                                          + " при записи журнала, повтор через минуту");
    tail += logLine(100, "INFO", "обычная строка без совпадения");
    for (const bool indexed : {true, false}) {
        const QString tag = indexed ? QStringLiteral("indexed") : QStringLiteral("resident");
        const QString path = testdocs::writeFile(dir, "provisional-" + tag + ".log", head);
        auto doc = indexed ? testdocs::buildIndexed({path}, schema)
                           : testdocs::buildResident({path}, schema);
        const auto state = [&] {
            return indexed ? doc.model->indexedOrNull()->fileEncoding(doc.files.value(0))
                           : doc.encodings.value(path);
        };
        CHECK(state().decoder.encoding() == TextEncoding::Windows1251 && state().provisional,
              tag + " provisional: a lone garbage byte gives a provisional Windows-1251");
        SearchResultsController c;
        c.setSource(doc.model.get());
        c.search(query(QStringLiteral("ошибка")));
        CHECK(waitResults(c, 0), tag + " provisional: nothing in the head");
        CHECK(indexed ? testdocs::appendIndexedTail(doc, 0, tail, schema)
                      : testdocs::appendResidentTail(doc, 0, tail, schema),
              tag + " provisional: append a UTF-8 tail");
        CHECK(state().decoder.isUtf8() && state().source == FileEncoding::Source::Auto,
              tag + " provisional: the tail revises the decision to UTF-8");
        CHECK(waitResults(c, 3), tag + " provisional: the Cyrillic tail is found after the revision");
        CHECK(c.model()->messageAt(0).endsWith(QStringLiteral("ОШИБКА диска 0 при записи журнала, повтор через минуту")),
              tag + " provisional: results are decoded with the revised encoding: "
                  + c.model()->messageAt(0));
        // Эталон — тот же файл, прочитанный как UTF-8 с самого начала.
        const QString refPath = testdocs::writeFile(dir, "provisional-ref-" + tag + ".log", head + tail);
        auto ref = testdocs::buildIndexed({refPath}, schema);
        ref.model->indexedOrNull()->setFileEncoding(ref.files.value(0), TextDecoder(TextEncoding::Utf8));
        SearchResultsController rc;
        rc.setSource(ref.model.get());
        rc.search(query(QStringLiteral("ошибка")));
        CHECK(waitResults(rc, 3), tag + " provisional: reference finds the same rows");
        bool same = true;
        for (int row = 0; row < 3; ++row)
            same = same && rc.model()->messageAt(row) == c.model()->messageAt(row);
        CHECK(same, tag + " provisional: results equal the reference opened as UTF-8");
    }
}

// Метки и цвета файлов выдачи — у вкладки-источника (S-14): совпадения в
// одном файле из трёх не делают выдачу «одно-файловой» с другими метками.
static void testLabelsFromSource(const QDir& dir, const QString& schema)
{
    QStringList paths;
    for (const char* name : {"svc-alpha-node.log", "svc-beta-node.log", "svc-gamma-node.log"}) {
        QByteArray bytes;
        for (int i = 0; i < 50; ++i)
            bytes += logLine(i, "INFO", QByteArray(name).startsWith("svc-beta")
                                            && i % 10 == 0 ? "NEEDLE " + QByteArray::number(i)
                                                           : "ordinary " + QByteArray::number(i));
        paths << testdocs::writeFile(dir, QString::fromLatin1(name), bytes);
    }
    for (const bool indexed : {false, true}) {
        const QString tag = indexed ? QStringLiteral("labels indexed") : QStringLiteral("labels resident");
        auto doc = indexed ? testdocs::buildIndexed(paths, schema, testdocs::IndexedLoad::Concurrent)
                           : testdocs::buildResident(paths, schema);
        LogModel& source = *doc.model;
        CHECK(source.labeledFiles().size() == 3, tag + ": three files in the source");
        SearchResultsController c;
        int setChanges = 0;
        QObject::connect(c.model(), &LogModel::fileSetChanged, [&] { ++setChanges; });
        c.setSource(&source);
        c.search(query("NEEDLE"));
        CHECK(waitResults(c, 5), tag + ": matches in one file");
        LogModel& results = *c.model();
        CHECK(results.labeledFiles().size() == 3, tag + ": results carry the source's file set");
        for (int row = 0; row < results.rowCount(); ++row) {
            const LogFile* file = results.keyAt(row).file.get();
            const LogModel::LabeledFile* mine = results.labeledFile(file);
            const LogModel::LabeledFile* theirs = source.labeledFile(file);
            CHECK(mine && theirs && mine->label == theirs->label
                      && mine->colorOrdinal == theirs->colorOrdinal,
                  tag + ": label and colour of a result row are the source's");
            CHECK(file && results.fileColorOrdinal(file->filePath)
                              == source.fileColorOrdinal(file->filePath),
                  tag + ": colour ordinal by path is the source's");
        }
        CHECK(setChanges > 0, tag + ": the results model announces the borrowed file set");
        // Набор файлов источника сменился — выдача объявляет это как своё.
        const int before = setChanges;
        source.setFileLabelMaxChars(6);
        CHECK(setChanges > before, tag + ": source label changes are forwarded");
        results.setFileLabelSource(nullptr);
        CHECK(&results.labeledFiles() != &source.labeledFiles()
                  && results.labeledFile(results.keyAt(0).file.get())
                         != source.labeledFile(results.keyAt(0).file.get()),
              tag + ": without a label source the results use their own labels");
        c.setSource(nullptr);
    }
}


// ---------------------------------------------------------------------------
// Закладки в выдаче (U-19): отметка — закладка строки источника (его
// BookmarkStore), правка из выдачи ставит закладку в источнике, правка в
// источнике сразу видна в выдаче. Оба бэкенда, одна и слитая вкладка.
// ---------------------------------------------------------------------------

// Роль BookmarkRole каждой строки выдачи против перебора закладок источника
// по ключам.
static bool resultMarksMatchSource(LogModel& results, LogModel& source)
{
    QSet<QPair<const LogFile*, qint64>> marked;
    for (const Bookmark& b : source.bookmarks().bookmarks())
        if (source.bookmarks().visibleRow(b.id) >= 0)
            marked.insert({b.key.file.get(), b.key.line});
    for (int row = 0; row < results.rowCount(); ++row) {
        const RecordKey key = results.keyAt(row);
        const bool expected = marked.contains({key.file.get(), key.line});
        if (results.data(results.index(row), LogModel::BookmarkRole).toBool() != expected)
            return false;
    }
    return true;
}

static void testBookmarksInResults(const QDir& dir, const QString& schema)
{
    for (const bool indexed : {false, true}) {
        for (const bool mergedTab : {false, true}) {
            // Свои файлы на прогон: прежний дописан.
            const QString prefix = QStringLiteral("bm-%1-%2-").arg(int(indexed)).arg(int(mergedTab));
            const QString one = testdocs::writeFile(dir, prefix + "one.log", growingBase(0));
            const QString two = testdocs::writeFile(dir, prefix + "two.log", growingBase(10));
            const QString tag = QStringLiteral("bookmarks %1 %2")
                .arg(indexed ? "indexed" : "resident", mergedTab ? "merged" : "single");
            const QStringList paths = mergedTab ? QStringList{one, two} : QStringList{one};
            auto doc = indexed ? testdocs::buildIndexed(paths, schema, testdocs::IndexedLoad::Concurrent)
                               : testdocs::buildResident(paths, schema);
            LogModel& source = *doc.model;
            SearchResultsController c;
            // Отметки скроллбара выдачи — до setSource: источник закладок
            // меняется у уже подключённой модели.
            ScrollbarMarks marks;
            marks.setModel(c.model());
            c.setSource(&source);
            const int matches = mergedTab ? 40 : 20;
            c.search(query("NEEDLE"));
            CHECK(waitResults(c, matches), tag + ": initial search");
            LogModel& results = *c.model();

            // Закладка в источнике до поиска и после — видна в выдаче.
            const int firstSource = c.sourceRowOf(0);
            CHECK(firstSource >= 0 && source.keyAt(firstSource) == results.keyAt(0),
                  tag + ": result row maps to its source line");
            int changes = 0;
            QObject::connect(&results, &QAbstractItemModel::dataChanged,
                [&](const QModelIndex& a, const QModelIndex& b, const QList<int>& roles) {
                    if (roles.contains(LogModel::BookmarkRole) && a.row() <= 3 && b.row() >= 3)
                        ++changes;
                });
            const int src3 = c.sourceRowOf(3);
            CHECK(source.bookmarks().toggle(src3), tag + ": bookmark set in the source");
            CHECK(results.data(results.index(3), LogModel::BookmarkRole).toBool(),
                  tag + ": result row shows the source bookmark");
            CHECK(changes == 1, tag + ": the result row is repainted once");
            CHECK(resultMarksMatchSource(results, source), tag + ": marks match the source");

            // Из выдачи: поставить — закладка в источнике на своей строке.
            const auto set = c.toggleBookmark(5);
            CHECK(set && *set, tag + ": toggle from the results sets a bookmark");
            const int src5 = c.sourceRowOf(5);
            CHECK(source.bookmarks().isBookmarkedRow(src5), tag + ": bookmark lands on the source line");
            CHECK(source.bookmarks().count() == 2, tag + ": exactly one bookmark added");
            CHECK(results.data(results.index(5), LogModel::BookmarkRole).toBool(),
                  tag + ": the result row is marked");
            // Заметка из выдачи: та же закладка, не новая.
            bool created = true;
            const int id5 = c.addBookmark(5, &created);
            CHECK(id5 >= 0 && !created && source.bookmarks().count() == 2,
                  tag + ": add on a bookmarked result row returns the existing bookmark");
            created = false;
            const int id7 = c.addBookmark(7, &created);
            CHECK(id7 >= 0 && created && source.bookmarks().isBookmarkedRow(c.sourceRowOf(7)),
                  tag + ": add on an unmarked result row creates the bookmark in the source");

            // Снять в источнике — роль в выдаче гаснет.
            changes = 0;
            CHECK(!source.bookmarks().toggle(src3), tag + ": bookmark removed in the source");
            CHECK(!results.data(results.index(3), LogModel::BookmarkRole).toBool(),
                  tag + ": result row loses its mark");
            CHECK(changes == 1, tag + ": the result row is repainted on removal");
            // Снять из выдачи.
            const auto unset = c.toggleBookmark(7);
            CHECK(unset && !*unset && !source.bookmarks().find(id7), tag + ": toggle from the results removes");
            CHECK(resultMarksMatchSource(results, source), tag + ": marks match after removals");
            {
                QVector<qint64> expected;
                for (int row = 0; row < results.rowCount(); ++row)
                    if (results.data(results.index(row), LogModel::BookmarkRole).toBool())
                        expected.append(row);
                CHECK(!expected.isEmpty()
                          && marks.marks().rows[ScrollbarMarkSet::Bookmark] == expected,
                      tag + ": scrollbar marks of the results are the source bookmarks");
            }

            // Дозапись: выдача растёт вставками, отметки на месте; закладка на
            // дописанной строке видна.
            QByteArray tail;
            for (int i = 0; i < 3; ++i)
                tail += logLine(100, "INFO", "NEEDLE tail " + QByteArray::number(i));
            CHECK(indexed ? testdocs::appendIndexedTail(doc, 0, tail, schema)
                          : testdocs::appendResidentTail(doc, 0, tail, schema),
                  tag + ": append tail");
            CHECK(waitResults(c, matches + 3), tag + ": appended matches are found");
            CHECK(resultMarksMatchSource(results, source), tag + ": marks survive the append");
            const int lastResult = results.rowCount() - 1;
            CHECK(c.toggleBookmark(lastResult).value_or(false), tag + ": bookmark an appended match");
            CHECK(source.bookmarks().isBookmarkedRow(c.sourceRowOf(lastResult)),
                  tag + ": appended line bookmarked in the source");
            CHECK(resultMarksMatchSource(results, source), tag + ": marks match after the append");

            // Перестройка выдачи (фильтр источника) — отметки по-прежнему
            // закладки источника.
            source.setLogLevelFilter({LogLevel::Error});
            CHECK(waitResults(c, mergedTab ? 20 : 10), tag + ": rebuilt results");
            CHECK(resultMarksMatchSource(results, source), tag + ": marks match after the rebuild");
            source.setLogLevelFilter({});
            CHECK(waitResults(c, matches + 3), tag + ": results back");
            CHECK(resultMarksMatchSource(results, source), tag + ": marks match after the filter is reset");
            source.bookmarks().clear();
            CHECK(resultMarksMatchSource(results, source), tag + ": clear in the source clears the results");

            // Без источника — нет строк выдачи и ни одной закладки из неё.
            c.setSource(nullptr);
            CHECK(!c.toggleBookmark(0) && c.addBookmark(0) < 0, tag + ": no source - nothing to bookmark");
        }
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
    testLabelsFromSource(dir, schema);
    testBookmarksInResults(dir, schema);
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
        combined.bindFields({"Timestamp", "Thread", "Level", "Message"});
        CHECK(!combined.fieldMissing(0) && combined.fieldMissing(1), "only the orphan is missing");
        completed(results, [&] { results.searchVisible(*indexed.model, combined); });
        CHECK(results.rowCount() == 5, "missing-column rule does not search the whole row");
        // Показ полей не меняет привязку (F-17, шаг 3): колонки нет — правило
        // нейтрально при любом показе, а не поиск по всей строке.
        combined.bindFields({"Timestamp", "Thread", "Level", "Message"});
        CHECK(combined.fieldMissing(1), "missing column stays missing regardless of the display");
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
    testEncodingLiveSearch(dir, schema);
    testProvisionalLiveSearch(dir, schema);
    std::fprintf(stdout, "Search results: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
