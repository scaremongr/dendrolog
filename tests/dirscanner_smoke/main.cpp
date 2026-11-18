// ============================================================================
// dirscanner_smoke — смоук-тест панели Directory Scanner.
//
// Проверяет:
//   • естественный порядок имён без учёта регистра (scannerCompareNames);
//   • полный скан временного дерева: файлы попадают в дерево, счётчики
//     записей и уровней совпадают с содержимым;
//   • фильтр по содержимому: литерал, регистр, регексп, совпадение НА СТЫКЕ
//     блоков чтения (256 КиБ) и корректный прогресс в байтах;
//   • инкрементальное обновление rescan(): добавленный файл появляется,
//     удалённый исчезает, а дописанный в хвост пересчитывается «по дельте»
//     и даёт РОВНО те же цифры, что полный повторный скан.
// ============================================================================

#include "directoryscanner.h"
#include "directoryscannerpanel.h"
#include "../common/testdocuments.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QToolButton>
#include <QSet>
#include <QStringEncoder>
#include <QTreeWidget>

#include <cstdio>

using namespace Qt::StringLiterals;

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); \
        }                                                                    \
    } while (0)

// Крутит цикл событий, пока предикат не станет истинным (или не выйдет время).
// Скан асинхронный: листинг каталога, воркеры статистики и дебаунсы живут на
// таймерах, поэтому «подождать» — единственный способ дождаться результата.
template <typename Predicate>
static bool waitFor(Predicate ready, int timeoutMs = 30000)
{
    QElapsedTimer clock;
    clock.start();
    while (!ready()) {
        if (clock.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

static void writeFile(const QString& path, const QByteArray& content)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        ++g_failures;
        std::fprintf(stderr, "FAIL cannot write %s\n", qPrintable(path));
        return;
    }
    f.write(content);
}

static void appendFile(const QString& path, const QByteArray& content)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) {
        ++g_failures;
        std::fprintf(stderr, "FAIL cannot append %s\n", qPrintable(path));
        return;
    }
    f.write(content);
}

// Ищет элемент файла по имени в любом уровне дерева.
static QTreeWidgetItem* findItem(QTreeWidgetItem* parent, const QString& name)
{
    for (int i = 0; i < parent->childCount(); ++i) {
        QTreeWidgetItem* child = parent->child(i);
        if (child->text(ScanCol::Name) == name)
            return child;
        if (QTreeWidgetItem* deep = findItem(child, name))
            return deep;
    }
    return nullptr;
}

static QString entriesOf(QTreeWidget& tree, const QString& name)
{
    QTreeWidgetItem* item = findItem(tree.invisibleRootItem(), name);
    return item ? item->text(ScanCol::Entries) : QStringLiteral("<missing>");
}

// Ждём, пока в дереве не останется файлов со статусом «Queued…»/«Scanning…».
static bool allScanned(QTreeWidgetItem* parent)
{
    for (int i = 0; i < parent->childCount(); ++i) {
        QTreeWidgetItem* child = parent->child(i);
        if (child->data(ScanCol::Name, ScanRole::IsFile).toBool()) {
            const QString entries = child->text(ScanCol::Entries);
            if (entries.isEmpty() || !entries.at(0).isDigit())
                return false;
        }
        if (!allScanned(child))
            return false;
    }
    return true;
}

// Текст колонки W/E/F, как его показывает дерево (пустое место — ноль).
static QString formatAlertsForTest(const LogParser::FileStats& stats)
{
    if (stats.warnCount == 0 && stats.errorCount == 0 && stats.fatalCount == 0)
        return u"—"_s;
    auto part = [](int n) { return n > 0 ? QString::number(n) : QString(); };
    return part(stats.warnCount) + u'/' + part(stats.errorCount) + u'/' + part(stats.fatalCount);
}

// ---------------------------------------------------------------------------
static void testNameOrdering()
{
    CHECK(scannerCompareNames(u"Alpha", u"beta") < 0, "case-insensitive: Alpha < beta");
    CHECK(scannerCompareNames(u"BETA", u"alpha") > 0, "case-insensitive: BETA > alpha");
    CHECK(scannerCompareNames(u"log2.txt", u"log10.txt") < 0, "natural: log2 < log10");
    CHECK(scannerCompareNames(u"log007", u"log8") < 0, "leading zeros carry no value");
    CHECK(scannerCompareNames(u"a", u"a") == 0, "identical names compare equal");
    CHECK(scannerCompareNames(u"App.log", u"app.log") != 0, "case still breaks exact ties");
    // Порядок должен быть строгим и согласованным (антисимметричным).
    const QStringList names{ u"App.log"_s, u"app.log"_s, u"b.log"_s, u"B2.log"_s, u"B10.log"_s };
    for (const QString& a : names)
        for (const QString& b : names)
            CHECK((scannerCompareNames(a, b) < 0) == (scannerCompareNames(b, a) > 0),
                  "compare is antisymmetric");
}

// ---------------------------------------------------------------------------
// Примитив, на котором стоит «умное» обновление: разбор хвоста с байтового
// смещения. Сумма «голова + хвост» обязана совпасть с разбором целого файла —
// иначе дописанный лог получит завышенные счётчики.
static void testTailAnalysis(const QString& dir)
{
    const QByteArray head =
        "2026-03-01 10:00:00 INFO  one\n"
        "2026-03-01 10:00:01 WARN  two\n";
    const QByteArray tail =
        "2026-03-01 10:00:02 ERROR three\n"
        "2026-03-01 10:00:03 FATAL four\n";

    const QString path = dir + "/tail.log";
    writeFile(path, head + tail);

    LogParser parser;
    const LogParser::FileStats whole = parser.analyzeFileForStats(path).stats;
    const LogParser::FileStats onlyTail = parser.analyzeFileForStats(
        path, LogParser::StatsResume{ head.size(), QStringConverter::Utf8, true }).stats;

    writeFile(path, head);
    const LogParser::FileStats onlyHead = parser.analyzeFileForStats(path).stats;

    CHECK(whole.parseSuccess && onlyHead.parseSuccess && onlyTail.parseSuccess,
          "all three analyses succeed");
    CHECK(onlyHead.totalEntries + onlyTail.totalEntries == whole.totalEntries,
          "head + tail entry counts add up to the whole file");
    CHECK(onlyHead.warnCount + onlyTail.warnCount == whole.warnCount, "warn counts add up");
    CHECK(onlyHead.errorCount + onlyTail.errorCount == whole.errorCount, "error counts add up");
    CHECK(onlyHead.fatalCount + onlyTail.fatalCount == whole.fatalCount, "fatal counts add up");
    CHECK(whole.warnCount == 1 && whole.errorCount == 1 && whole.fatalCount == 1,
          "one entry of each level");

    // Счётчики идут по записям: слово уровня в строке без метки времени
    // (продолжение записи) не считается, слова расширенного словаря — считаются.
    writeFile(path,
              "2026-03-01 10:00:00 INFO  one\n"
              "    caused by: fatal error in critical section\n"
              "    err: connection reset, warning ignored\n"
              "2026-03-01 10:00:01 [crit] two\n"
              "2026-03-01 10:00:02 SEVERE three\n"
              "2026-03-01 10:00:03 [ERR] four\n"
              "2026-03-01 10:00:04 [WRN] five\n"
              "2026-03-01 10:00:05 NOTICE six\n");
    const LogParser::FileStats words = parser.analyzeFileForStats(path).stats;
    CHECK(words.fatalCount == 2 && words.errorCount == 1 && words.warnCount == 1,
          "level words count once per entry, continuation lines do not count");
    CHECK(onlyHead.firstEntryTimestamp == whole.firstEntryTimestamp,
          "the head keeps the first timestamp");
    CHECK(onlyTail.lastEntryTimestamp == whole.lastEntryTimestamp,
          "the tail carries the last timestamp");

    QFile::remove(path);
}

// ---------------------------------------------------------------------------
// Q-15. Счётчик записей сканера обязан совпадать с числом логических записей
// вкладки, открывшей тот же файл (без схемы): запись начинает строка с меткой
// времени и уровнем, остальные строки — её продолжение.
struct TabCounts {
    int records = 0;
    int warn = 0;
    int error = 0;
    int fatal = 0;
};

static TabCounts countTabRecords(const testdocs::Document& doc)
{
    TabCounts counts;
    QSet<int> seen;
    const LogModel& model = *doc.model;
    for (int row = 0; row < model.rowCount(); ++row) {
        const int id = model.keyForRow(row).logicalEntryId;
        if (seen.contains(id))
            continue;
        seen.insert(id);
        ++counts.records;
        const LogLevel level = model.visibleLevelAt(row);
        if (level == LogLevel::Warn) ++counts.warn;
        if (level == LogLevel::Error) ++counts.error;
        if (level == LogLevel::Fatal) ++counts.fatal;
    }
    return counts;
}

// UTF-16 индексный бэкенд не читает (вкладка отправляет его в резидентный
// путь), поэтому для него — только резидентная вкладка.
static void checkAgainstTab(const QString& path, const char* what, bool indexedToo = true)
{
    LogParser parser;
    const LogParser::FileStats stats = parser.analyzeFileForStats(path).stats;
    const testdocs::Document resident = testdocs::buildResident({ path }, QString());
    CHECK(resident.errors.isEmpty(), "the resident tab loads the file");
    const TabCounts tab = countTabRecords(resident);
    TabCounts tabIndexed = tab;
    if (indexedToo) {
        const testdocs::Document indexed = testdocs::buildIndexed({ path }, QString());
        CHECK(indexed.errors.isEmpty(), "the indexed tab loads the file");
        tabIndexed = countTabRecords(indexed);
    }
    if (tab.records != tabIndexed.records || stats.totalEntries != tab.records
        || stats.warnCount != tab.warn || stats.errorCount != tab.error
        || stats.fatalCount != tab.fatal) {
        ++g_failures;
        std::fprintf(stderr,
                     "FAIL %s: scanner %d entries (W%d E%d F%d), resident tab %d (W%d E%d F%d), "
                     "indexed tab %d\n",
                     what, stats.totalEntries, stats.warnCount, stats.errorCount,
                     stats.fatalCount, tab.records, tab.warn, tab.error, tab.fatal,
                     tabIndexed.records);
    }
}

static const QByteArray kMultilineLog =
    "service banner, no timestamp\n"                       // запись сама по себе
    "second banner line\n"                                 // её продолжение
    "2026-03-01 10:00:00 INFO  start\n"
    "2026-03-01 10:00:01 ERROR request failed\n"
    "java.lang.IllegalStateException: fatal error\n"
    "    at com.example.Main.run(Main.java:42)\n"
    "    at com.example.Main.main(Main.java:7)\n"
    "\n"
    "2026-03-01 10:00:02 no level on this line\n"           // продолжение: уровня нет
    "2026-03-01 10:00:03 WARN  slow response\n"
    "    warning: retry 1\n"
    "2026-03-01 10:00:04 FATAL giving up\n";

static void testRecordCounts(const QString& dir)
{
    const QString path = dir + "/records.log";

    writeFile(path, kMultilineLog);
    checkAgainstTab(path, "multi-line records");

    QByteArray crlf = kMultilineLog;
    crlf.replace("\n", "\r\n");
    writeFile(path, crlf);
    checkAgainstTab(path, "multi-line records, CRLF");

    writeFile(path, kMultilineLog.chopped(1));
    checkAgainstTab(path, "last line without a newline");

    writeFile(path, "no timestamps here\nat all\n\n");
    checkAgainstTab(path, "a file without a single primary line");

    writeFile(path, QByteArray());
    checkAgainstTab(path, "an empty file");

    QFile::remove(path);
}

// Дозапись через сам сканер: продолжение записи, пришедшее отдельной порцией,
// новой записью не считается; оборванная последняя строка после дозаписи не
// считается дважды. Эталон — полный разбор того же файла заново.
static void testScannerAppend(const QByteArray& head, const QList<QByteArray>& portions,
                              const char* what, bool indexedToo = true)
{
    QTemporaryDir tmp;
    const QString path = tmp.path() + "/grow.log";
    writeFile(path, head);

    QTreeWidget tree;
    DirectoryScanner scanner(&tree);
    scanner.setRefreshMode(DirectoryScanner::RefreshMode::Off);
    scanner.scan(tmp.path());
    CHECK(waitFor([&] { return tree.topLevelItemCount() == 1
                            && allScanned(tree.invisibleRootItem()); }), what);

    int rescans = 0;
    QObject::connect(&scanner, &DirectoryScanner::rescanFinished, &tree,
                     [&](int, int, int) { ++rescans; });
    for (const QByteArray& portion : portions) {
        appendFile(path, portion);
        const int before = rescans;
        scanner.rescan();
        CHECK(waitFor([&] { return rescans > before && allScanned(tree.invisibleRootItem()); }),
              what);

        LogParser parser;
        const LogParser::FileStats whole = parser.analyzeFileForStats(path).stats;
        QTreeWidgetItem* item = findItem(tree.invisibleRootItem(), u"grow.log"_s);
        const QString shown = item ? item->text(ScanCol::Entries) : QString();
        const QString alerts = item ? item->text(ScanCol::Alerts) : QString();
        if (shown != QString::number(whole.totalEntries)
            || alerts != formatAlertsForTest(whole)) {
            ++g_failures;
            std::fprintf(stderr, "FAIL %s: after appending the scanner shows %s entries [%s], "
                                 "a full analysis gives %d [%s]\n",
                         what, qPrintable(shown), qPrintable(alerts), whole.totalEntries,
                         qPrintable(formatAlertsForTest(whole)));
        }
    }
    checkAgainstTab(path, what, indexedToo);
}

static QByteArray encodeUtf16(const QByteArray& utf8, QStringConverter::Encoding encoding,
                              bool withBom)
{
    QStringEncoder encoder(encoding, withBom ? QStringConverter::Flag::WriteBom
                                             : QStringConverter::Flag::Default);
    return encoder.encode(QString::fromUtf8(utf8));
}

static void testAppends()
{
    const QByteArray head =
        "2026-03-01 10:00:00 INFO  start\n"
        "2026-03-01 10:00:01 ERROR request failed\n";
    const QByteArray trace =
        "java.lang.IllegalStateException: fatal error\n"
        "    at com.example.Main.run(Main.java:42)\n";
    const QByteArray more =
        "2026-03-01 10:00:03 WARN  slow response\n"
        "2026-03-01 10:00:04 FATAL giving";            // строка оборвана
    const QByteArray rest =
        " up\n"
        "2026-03-01 10:00:05 ERROR again\n";

    testScannerAppend(head, { trace, more, rest }, "append: continuation and a torn line");
    testScannerAppend("banner without a timestamp\n", { "more banner\n", head },
                      "append: file that starts without a record");

    for (const auto encoding : { QStringConverter::Utf16LE, QStringConverter::Utf16BE }) {
        const char* what = encoding == QStringConverter::Utf16LE ? "append: UTF-16LE"
                                                                 : "append: UTF-16BE";
        testScannerAppend(encodeUtf16(head, encoding, true),
                          { encodeUtf16(trace, encoding, false),
                            encodeUtf16(more, encoding, false),
                            encodeUtf16(rest, encoding, false) },
                          what, /*indexedToo=*/false);
    }
}

// ---------------------------------------------------------------------------
// Q-10. Ротированные логи без сжатия: расширение из настроек — компонент
// имени, за которым идёт только «хвост ротации».
static void testRotatedNames()
{
    QTemporaryDir tmp;
    const QString dir = tmp.path();
    const QByteArray line = "2026-01-02 03:04:05 INFO  rotated\n";
    const QStringList wanted{
        u"app-2026-01-01.log"_s, u"app.log"_s, u"app.log-20260101"_s, u"app.log.1"_s,
        u"app.log.2"_s, u"app.log.10"_s, u"app.log.2026-01-01"_s, u"notes.TXT.3"_s,
    };
    const QStringList unwanted{
        u"app.log.bak"_s, u"app.log.swp"_s, u"app.log.gz"_s, u"app.log.1.gz"_s,
        u"app.log.old"_s, u"app.log~"_s, u"app.logs.1"_s, u"catalog.1"_s, u"app.log."_s,
        u"app.log.1a"_s, u"data.2026"_s,
    };
    for (const QString& name : wanted + unwanted)
        writeFile(dir + u'/' + name, line);

    QTreeWidget tree;
    DirectoryScanner scanner(&tree);
    scanner.setRefreshMode(DirectoryScanner::RefreshMode::Off);
    scanner.setFileExtensions({ u"log"_s, u"txt"_s });
    scanner.scan(dir);
    CHECK(waitFor([&] { return tree.topLevelItemCount() >= 1
                            && allScanned(tree.invisibleRootItem()); }),
          "rotated names: scan completes");

    QStringList listed;
    for (int i = 0; i < tree.topLevelItemCount(); ++i)
        listed << tree.topLevelItem(i)->text(ScanCol::Name);
    if (listed != wanted) {
        ++g_failures;
        std::fprintf(stderr, "FAIL rotated names: listed [%s], expected [%s]\n",
                     qPrintable(listed.join(u", "_s)), qPrintable(wanted.join(u", "_s)));
    }
    CHECK(entriesOf(tree, u"app.log.1"_s) == u"1"_s, "a rotated file is analysed like any other");
}

// ---------------------------------------------------------------------------
// Панель целиком: кнопка ⟳ включается только когда есть что обновлять, режим
// «вручную» подсвечивает её при изменениях на диске, а режим «автоматически»
// подтягивает их сам (по событию файловой системы, с дебаунсом).
static void testPanelRefresh()
{
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        ++g_failures;
        std::fprintf(stderr, "FAIL cannot create temp dir for the panel test\n");
        return;
    }
    const QString dir = tmp.path();
    writeFile(dir + "/one.log", "2026-04-01 10:00:00 INFO  one\n");

    DirectoryScannerPanel panel;
    DirectoryScanner* scanner = panel.scanner();
    auto* button = panel.findChild<QToolButton*>(u"scannerRefreshButton"_s);
    CHECK(button != nullptr, "panel exposes its refresh button");
    if (!button)
        return;

    CHECK(!button->isEnabled(), "refresh is disabled until a directory is scanned");

    scanner->setRefreshMode(DirectoryScanner::RefreshMode::Manual);
    panel.scanDirectory(dir);
    QTreeWidget* tree = panel.findChild<QTreeWidget*>();
    CHECK(tree != nullptr, "panel owns a results tree");
    if (!tree)
        return;
    CHECK(waitFor([&] { return tree->topLevelItemCount() == 1
                            && allScanned(tree->invisibleRootItem()); }),
          "panel scan completes");
    CHECK(button->isEnabled(), "refresh is enabled once a directory is scanned");
    CHECK(!scanner->hasPendingChanges(), "a fresh scan has nothing pending");

    // Manual: изменение на диске только помечается, дерево не трогается.
    writeFile(dir + "/two.log", "2026-04-01 11:00:00 WARN  two\n");
    CHECK(waitFor([&] { return scanner->hasPendingChanges(); }, 15000),
          "the watcher notices a new file in manual mode");
    CHECK(tree->topLevelItemCount() == 1, "manual mode leaves the tree alone");
    CHECK(button->toolTip().contains(u"changed on disk"_s),
          "the button explains that changes are waiting");

    button->click();
    CHECK(waitFor([&] { return tree->topLevelItemCount() == 2
                            && allScanned(tree->invisibleRootItem()); }),
          "clicking refresh pulls the new file in");
    CHECK(!scanner->hasPendingChanges(), "refreshing clears the pending flag");

    // Auto: то же изменение приезжает само, после дебаунса событий ФС.
    scanner->setRefreshMode(DirectoryScanner::RefreshMode::Auto);
    writeFile(dir + "/three.log", "2026-04-01 12:00:00 ERROR three\n");
    CHECK(waitFor([&] { return tree->topLevelItemCount() == 3
                            && allScanned(tree->invisibleRootItem()); }, 20000),
          "automatic mode refreshes on its own");

    // Off: ничего не отслеживается, дерево остаётся как есть.
    scanner->setRefreshMode(DirectoryScanner::RefreshMode::Off);
    writeFile(dir + "/four.log", "2026-04-01 13:00:00 INFO  four\n");
    const bool refreshedAnyway = waitFor([&] { return tree->topLevelItemCount() > 3; }, 5000);
    CHECK(!refreshedAnyway, "off mode does not refresh by itself");
    CHECK(!scanner->hasPendingChanges(), "off mode does not even flag changes");
    button->click();
    CHECK(waitFor([&] { return tree->topLevelItemCount() == 4; }),
          "the button still refreshes on demand while off");
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    testNameOrdering();

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "FAIL cannot create temp dir\n");
        return 1;
    }
    const QString rootPath = tmp.path();
    QDir(rootPath).mkdir(QStringLiteral("sub"));

    testTailAnalysis(rootPath);
    testRecordCounts(rootPath);
    testAppends();
    testRotatedNames();

    // Три записи, из них одна ERROR и одна WARN.
    const QByteArray alpha =
        "2026-01-02 03:04:05 INFO  starting up\n"
        "2026-01-02 03:04:06 WARN  disk almost full\n"
        "2026-01-02 03:04:07 ERROR write failed\n";
    writeFile(rootPath + "/Alpha.log", alpha);
    writeFile(rootPath + "/beta.log",
              "2026-01-02 04:00:00 INFO  hello needle world\n");
    writeFile(rootPath + "/sub/gamma.log",
              "2026-01-02 05:00:00 INFO  nested\n");
    writeFile(rootPath + "/ignored.dat", "not a log\n");
    // Последняя строка БЕЗ перевода строки: дочитывать такой файл с хвоста
    // нельзя (граница пришлась бы на середину строки), должен идти полный
    // повторный разбор.
    writeFile(rootPath + "/partial.log", "2026-01-02 07:00:00 INFO  no newline yet");

    // Файл, где искомое слово лежит РОВНО на стыке блоков чтения по 256 КиБ.
    QByteArray straddle(262140, 'a');
    straddle += "NEEDLE\n";
    writeFile(rootPath + "/big.log", straddle);

    QTreeWidget tree;
    DirectoryScanner scanner(&tree);
    scanner.setRefreshMode(DirectoryScanner::RefreshMode::Off);  // тикать сами

    scanner.scan(rootPath);
    CHECK(waitFor([&] { return tree.topLevelItemCount() >= 5
                            && allScanned(tree.invisibleRootItem()); }),
          "initial scan completes");

    CHECK(findItem(tree.invisibleRootItem(), u"Alpha.log"_s) != nullptr, "Alpha.log listed");
    CHECK(findItem(tree.invisibleRootItem(), u"ignored.dat"_s) == nullptr,
          "non-matching extension skipped");
    CHECK(entriesOf(tree, u"Alpha.log"_s) == u"3"_s, "Alpha.log has 3 entries");
    if (QTreeWidgetItem* item = findItem(tree.invisibleRootItem(), u"Alpha.log"_s))
        CHECK(item->text(ScanCol::Alerts) == u"1/1/"_s, "Alpha.log alerts are 1 warn / 1 error");

    // Каталоги идут первыми, дальше имена — без учёта регистра.
    CHECK(tree.topLevelItem(0)->text(ScanCol::Name) == u"sub"_s, "directories sort first");
    QStringList topFiles;
    for (int i = 1; i < tree.topLevelItemCount(); ++i)
        topFiles << tree.topLevelItem(i)->text(ScanCol::Name);
    CHECK(topFiles == (QStringList{ u"Alpha.log"_s, u"beta.log"_s,
                                    u"big.log"_s, u"partial.log"_s }),
          "file names ordered case-insensitively");

    // Вложенный каталог наполняется лениво — раскрываем, чтобы gamma.log попал
    // и в дерево, и в область действия фильтров.
    tree.expandItem(tree.topLevelItem(0));
    CHECK(waitFor([&] { return findItem(tree.invisibleRootItem(), u"gamma.log"_s) != nullptr
                            && allScanned(tree.invisibleRootItem()); }),
          "lazy expand populates the subdirectory");

    // ── Фильтр по содержимому ────────────────────────────────────────────────
    int matched = -1, total = -1;
    qint64 lastBytesDone = 0, lastBytesTotal = 0;
    QObject::connect(&scanner, &DirectoryScanner::contentFilterFinished,
                     [&](int m, int t) { matched = m; total = t; });
    QObject::connect(&scanner, &DirectoryScanner::contentFilterProgress,
                     [&](qint64 done, qint64 all, int, int) {
        lastBytesDone = done;
        lastBytesTotal = all;
    });

    auto runContentFilter = [&](const QString& text, bool regex, bool caseSensitive) {
        matched = -1;
        scanner.setContentFilter(text, regex, caseSensitive);
        scanner.applyFilters();
        CHECK(waitFor([&] { return matched >= 0; }), "content search finishes");
    };

    runContentFilter(u"needle"_s, false, false);
    CHECK(matched == 2, "case-insensitive 'needle' matches beta.log and big.log");
    CHECK(total == 5, "every listed log file was a candidate");
    CHECK(lastBytesTotal > 0, "progress reports a byte budget");

    runContentFilter(u"NEEDLE"_s, false, true);
    CHECK(matched == 1, "case-sensitive 'NEEDLE' matches only big.log (across a chunk edge)");

    runContentFilter(u"^2026-01-02 04:.*hello"_s, true, false);
    CHECK(matched == 1, "regex anchored at line start matches beta.log");

    runContentFilter(u"zzz-nothing-here"_s, false, false);
    CHECK(matched == 0, "absent text matches nothing");

    // Быстрая правка запроса подряд: каждый Apply отменяет предыдущий поиск,
    // который ещё крутится на пуле. Отменённая задача продолжает читать свою
    // последовательность файлов — та не должна уехать из-под неё.
    matched = -1;
    for (const QString& probe : { u"nee"_s, u"need"_s, u"needl"_s, u"needle"_s }) {
        scanner.setContentFilter(probe, false, false);
        scanner.applyFilters();   // без прокрутки цикла событий — отмена «на лету»
    }
    CHECK(waitFor([&] { return matched >= 0; }), "rapid re-apply settles");
    CHECK(matched == 2, "the last query wins after a burst of cancellations");

    scanner.clearFilters();

    // ── Инкрементальное обновление ───────────────────────────────────────────
    int rescans = 0, lastAdded = 0, lastRemoved = 0, lastUpdated = 0;
    QObject::connect(&scanner, &DirectoryScanner::rescanFinished,
                     [&](int a, int r, int u) {
        ++rescans;
        lastAdded = a; lastRemoved = r; lastUpdated = u;
    });

    // Ничего не менялось — обновление не должно трогать дерево.
    scanner.rescan();
    CHECK(waitFor([&] { return rescans == 1; }), "no-op rescan finishes");
    CHECK(lastAdded == 0 && lastRemoved == 0 && lastUpdated == 0,
          "unchanged tree reports no differences");

    // Добавили файл, удалили файл, дописали хвост существующему.
    writeFile(rootPath + "/delta.log", "2026-01-02 06:00:00 INFO  fresh\n");
    QFile::remove(rootPath + "/beta.log");
    appendFile(rootPath + "/Alpha.log",
               "2026-01-02 03:04:08 ERROR second failure\n"
               "2026-01-02 03:04:09 INFO  recovered\n");
    // Дописываем прямо в незавершённую строку: наивное «дочитывание с прошлого
    // размера» посчитало бы её второй раз.
    appendFile(rootPath + "/partial.log",
               " and more\n2026-01-02 07:00:01 WARN  second line\n");

    scanner.rescan();
    CHECK(waitFor([&] { return rescans == 2 && allScanned(tree.invisibleRootItem()); }),
          "incremental rescan finishes");
    CHECK(lastAdded == 1, "delta.log detected as added");
    CHECK(lastRemoved == 1, "beta.log detected as removed");
    CHECK(lastUpdated == 2, "Alpha.log and partial.log detected as modified");
    CHECK(findItem(tree.invisibleRootItem(), u"beta.log"_s) == nullptr, "beta.log dropped");
    CHECK(findItem(tree.invisibleRootItem(), u"delta.log"_s) != nullptr, "delta.log added");
    CHECK(entriesOf(tree, u"partial.log"_s) == u"2"_s,
          "a file that did not end on a newline is re-read in full, not resumed");

    const QString incrementalEntries = entriesOf(tree, u"Alpha.log"_s);
    QString incrementalAlerts;
    if (QTreeWidgetItem* item = findItem(tree.invisibleRootItem(), u"Alpha.log"_s))
        incrementalAlerts = item->text(ScanCol::Alerts);
    CHECK(incrementalEntries == u"5"_s, "appended entries folded into the old count");

    // Золотое сравнение: полный скан с нуля должен дать те же цифры, что
    // «дочитанный хвост».
    scanner.scan(rootPath);
    CHECK(waitFor([&] { return tree.topLevelItemCount() >= 5
                            && allScanned(tree.invisibleRootItem()); }),
          "control full scan completes");
    CHECK(entriesOf(tree, u"Alpha.log"_s) == incrementalEntries,
          "incremental entry count matches a full re-read");
    CHECK(entriesOf(tree, u"partial.log"_s) == u"2"_s,
          "partial.log count matches a full re-read too");
    if (QTreeWidgetItem* item = findItem(tree.invisibleRootItem(), u"Alpha.log"_s))
        CHECK(item->text(ScanCol::Alerts) == incrementalAlerts,
              "incremental alert counts match a full re-read");

    testPanelRefresh();

    if (g_failures == 0)
        std::printf("dirscanner_smoke: OK\n");
    else
        std::fprintf(stderr, "dirscanner_smoke: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
