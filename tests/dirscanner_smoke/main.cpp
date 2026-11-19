// ============================================================================
// dirscanner_smoke — смоук-тест панели Directory Scanner.
//
// Проверяет:
//   • естественный порядок имён без учёта регистра (scannerCompareNames);
//   • полный скан временного дерева: файлы попадают в дерево, счётчики
//     записей и уровней совпадают с содержимым;
//   • фильтр по содержимому: литерал, регистр, регексп, совпадение НА СТЫКЕ
//     блоков чтения (256 КиБ) и корректный прогресс в байтах;
//   • фильтр по содержимому не зависит от кодировки (F-1, шаг 5): cp1251,
//     cp866, KOI8-R, UTF-8 (с BOM и без), UTF-16 LE/BE; граница блока посреди
//     символа и запроса; предварительное решение и пересмотр; запасная
//     кодировка; сверка «сканер нашёл файл ⇔ вкладка нашла строку»; отмена
//     посреди файла; статистика от кодировки не зависит; дозапись, новый файл
//     и ротация проверяются кодировкой файла;
//   • инкрементальное обновление rescan(): добавленный файл появляется,
//     удалённый исчезает, а дописанный в хвост пересчитывается «по дельте»
//     и даёт РОВНО те же цифры, что полный повторный скан.
// ============================================================================

#include "directoryscanner.h"
#include "directoryscannerpanel.h"
#include "appsettings.h"
#include "filecontentsearch.h"
#include "filterruleset.h"
#include "../common/testdocuments.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QToolButton>
#include <QSet>
#include <QStringEncoder>
#include <QTreeWidget>
#include <QThread>
#include <QtConcurrent>

#include <atomic>

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
    // F-10: From/To по меткам syslog (год — по mtime файла, через Новый
    // год), nginx error и access-лога.
    writeFile(path,
              "Dec 31 23:59:58 web01 sshd[1]: error: first\n"
              "2026/12/31 23:59:59 [warn] 1#1: nginx\n"
              "1.2.3.4 - - [31/Dec/2026:22:00:01 -0200] \"GET / HTTP/1.1\" 500 0 ERROR\n"
              "Jan  1 00:00:02 web01 app[2]: INFO last\n");
    {
        QFile f(path);
        CHECK(f.open(QIODevice::ReadWrite)
                  && f.setFileTime(QDateTime(QDate(2027, 1, 1), QTime(0, 0, 5)),
                                   QFileDevice::FileModificationTime),
              "set mtime");
    }
    const LogParser::FileStats forms = parser.analyzeFileForStats(path).stats;
    CHECK(forms.totalEntries == 4, "four records with the new timestamp forms");
    CHECK(forms.firstEntryTimestamp == QDateTime(QDate(2026, 12, 31), QTime(23, 59, 58)),
          "From: syslog stamp gets the year before the file's New Year mtime");
    CHECK(forms.lastEntryTimestamp == QDateTime(QDate(2027, 1, 1), QTime(0, 0, 2)),
          "To: syslog stamp after New Year gets the mtime year");

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
    "2026-03-01 10:00:02 no level on this line\n"           // запись: ведущая метка (F-12)
    "  \"at\": \"2026-03-01 10:00:02\"\n"                     // продолжение: метка под отступом
    "2026-03-01 10:00:03 WARN  slow response\n"
    "    warning: retry 1\n"
    "2026-03-01 10:00:04 FATAL giving up\n";

static void testRecordCounts(const QString& dir)
{
    const QString path = dir + "/records.log";

    writeFile(path, kMultilineLog);
    checkAgainstTab(path, "multi-line records");
    {
        LogParser parser;
        const LogParser::FileStats stats = parser.analyzeFileForStats(path).stats;
        CHECK(stats.totalEntries == 6,
              "banner + five records: a stamp at the line start without a level starts one");
    }

    // F-12: access-лог и syslog без слов уровня — запись на строку, со своим
    // моментом; метка в теле сообщения записи не начинает.
    writeFile(path,
              "10.0.0.1 - - [01/Mar/2026:10:00:00 +0000] \"GET / HTTP/1.1\" 200 1\n"
              "2001:db8::1 - bob [01/Mar/2026:10:00:01 +0000] \"GET /a HTTP/1.1\" 404 0\n"
              "Mar  1 10:00:02 web01 sshd[7]: Accepted publickey\n"
              "Response: {\"ts\":\"2026-03-01T10:00:03Z\"}\n"
              "[2026-03-01 10:00:04] cache warmed\n");
    checkAgainstTab(path, "records without level words");
    {
        LogParser parser;
        const LogParser::FileStats stats = parser.analyzeFileForStats(path).stats;
        CHECK(stats.totalEntries == 4, "access, IPv6 access, syslog, bracketed: four records");
        CHECK(stats.firstEntryTimestamp
                  == QDateTime(QDate(2026, 3, 1), QTime(10, 0), QTimeZone::UTC),
              "From: the access-log stamp");
        CHECK(stats.lastEntryTimestamp == QDateTime(QDate(2026, 3, 1), QTime(10, 0, 4)),
              "To: the bracketed stamp");
    }

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
// F-1, шаг 5. Фильтр по содержимому читает файл той же кодировкой, что и
// вкладка: BOM, иначе EncodingDetector::advance по блокам (окно 64 КБ,
// предварительное решение и пересмотр) и TextDecoder. Критерий — «сканер нашёл
// файл ⇔ вкладка нашла строку» (Text Filters на резидентной и индексной
// вкладке) для каждой кодировки.

struct ContentProbe {
    QString text;
    bool regex = false;
    bool caseSensitive = false;
    int expect = -1;   // 1 — найдено, 0 — нет, -1 — только сверка с вкладкой
};

static QString describeProbe(const ContentProbe& p)
{
    return u"'%1'%2%3"_s.arg(p.text, p.regex ? u" regex"_s : QString(),
                               p.caseSensitive ? u" case"_s : QString());
}

static bool tabFinds(testdocs::Document& doc, const ContentProbe& probe)
{
    FilterRuleSet rules;
    FilterRule rule;
    rule.action = FilterRule::Action::Include;
    rule.text = probe.text;
    rule.isRegex = probe.regex;
    rule.caseSensitive = probe.caseSensitive;
    rules.rules.append(rule);
    rules.bindFields({}, false);   // как MainWindow: без привязки регекс не скомпилирован
    doc.model->setFilterRules(rules);
    testdocs::settle(*doc.model);
    const bool found = doc.model->rowCount() > 0;
    doc.model->setFilterRules(FilterRuleSet());
    testdocs::settle(*doc.model);
    return found;
}

static FileContentSearch::Result searchFile(const QString& path, const ContentProbe& probe,
                                            qint64 blockBytes = FileContentSearch::kReadBlockBytes,
                                            TextEncoding fallback = testdocs::kTestFallbackEncoding,
                                            qsizetype maxLineBytes = FileContentSearch::kMaxLineBytes)
{
    FileContentSearch::Options options;
    options.fallback = fallback;
    options.blockBytes = blockBytes;
    options.maxLineBytes = maxLineBytes;
    const ContentQuery query(probe.text, probe.regex,
                             probe.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
    return FileContentSearch::search(path, query, options);
}

// Сверка одного файла: поиск сканера (на разных размерах блока) против
// вкладки, открытой обоими бэкендами (UTF-16 — только резидентной).
static void checkFileAgainstTab(const QString& path, const QList<ContentProbe>& probes,
                                const QString& what, bool indexedToo = true,
                                const QList<qint64>& blockSizes = { FileContentSearch::kReadBlockBytes })
{
    testdocs::Document resident = testdocs::buildResident({ path }, QString());
    CHECK(resident.errors.isEmpty(), qPrintable(what + u": the resident tab loads the file: "_s
                                                 + resident.errors.join(u"; "_s)));
    testdocs::Document indexed;
    if (indexedToo) {
        indexed = testdocs::buildIndexed({ path }, QString());
        CHECK(indexed.errors.isEmpty(), qPrintable(what + u": the indexed tab loads the file"_s));
    }
    for (const ContentProbe& probe : probes) {
        const bool tab = tabFinds(resident, probe);
        if (indexedToo && tabFinds(indexed, probe) != tab) {
            ++g_failures;
            std::fprintf(stderr, "FAIL %s %s: resident and indexed tabs disagree\n",
                         qPrintable(what), qPrintable(describeProbe(probe)));
        }
        if (probe.expect >= 0 && tab != (probe.expect == 1)) {
            ++g_failures;
            std::fprintf(stderr, "FAIL %s %s: the tab %s the text, expected otherwise\n",
                         qPrintable(what), qPrintable(describeProbe(probe)),
                         tab ? "finds" : "does not find");
        }
        for (const qint64 block : blockSizes) {
            const FileContentSearch::Result result = searchFile(path, probe, block);
            if (result.matched != tab) {
                ++g_failures;
                std::fprintf(stderr,
                             "FAIL %s %s (block %lld): scanner %s, tab %s (scanner read it as %s)\n",
                             qPrintable(what), qPrintable(describeProbe(probe)), (long long)block,
                             result.matched ? "matches" : "does not match",
                             tab ? "finds the line" : "finds nothing",
                             qPrintable(TextEncodings::displayName(result.usedEncoding)));
            }
        }
    }
}

static QByteArray encodeFixture(const QString& text, const QString& kind)
{
    if (kind == u"UTF-8"_s)
        return text.toUtf8();
    if (kind == u"UTF-8 BOM"_s)
        return QByteArray("\xEF\xBB\xBF") + text.toUtf8();
    if (kind == u"UTF-16LE"_s || kind == u"UTF-16BE"_s) {
        QStringEncoder encoder(kind == u"UTF-16LE"_s ? QStringConverter::Utf16LE
                                                     : QStringConverter::Utf16BE,
                               QStringConverter::Flag::WriteBom);
        return encoder.encode(text);
    }
    return testdocs::encodeText(text, *TextEncodings::fromName(kind));
}

static const QStringList kEncodingKinds{
    u"Windows-1251"_s, u"IBM866"_s, u"KOI8-R"_s, u"UTF-8"_s, u"UTF-8 BOM"_s,
    u"UTF-16LE"_s, u"UTF-16BE"_s,
};

static QList<ContentProbe> cyrillicProbes()
{
    return {
        { u"соединения"_s, false, false, 1 },
        { u"СОЕДИНЕНИЯ"_s, false, false, 1 },
        { u"Ошибка соединения"_s, false, true, 1 },
        { u"ошибка соединения"_s, false, true, 0 },
        { u"ПРЕДУПРЕЖДЕНИЕ: диск"_s, false, true, 1 },
        { u"щёлкнул"_s, false, false, 1 },
        { u"\\[worker-0\\] ERROR - Ошибка\\s+соединения"_s, true, true, 1 },
        { u"^\\S+ \\S+ \\[worker-1\\] INFO - пользователь\\s+ИВАНОВ"_s, true, false, 1 },
        { u"Иванова"_s, true, false, 0 },
        { u"ascii preamble line 2$"_s, true, true, 1 },
        { u"зззнетничего"_s, false, false, 0 },
        // F-14: \w, \d, \b и регистр — по Юникоду, как у вкладки.
        { u"пользовател\\w+"_s, true, false, 1 },
        { u"\\bошибка\\b"_s, true, false, 1 },
        { u"\\bошибк\\b"_s, true, false, 0 },
        { u"попытка \\d+ \\w+"_s, true, false, 1 },
        { u"ОШИБКА \\d+"_s, true, false, 1 },
        { u"ОШИБКА \\d+"_s, true, true, 0 },
    };
}

static void testContentEncodings()
{
    QTemporaryDir tmp;
    const QString dir = tmp.path();
    const QString text = testdocs::cyrillicLogText(4);
    const QList<ContentProbe> probes = cyrillicProbes();

    // Каждая кодировка: сканер против вкладки, блоки разного размера
    // (граница блока проходит через разные места символов и строк).
    for (const QString& kind : kEncodingKinds) {
        const QString path = dir + u"/enc-"_s + QString(kind).replace(u' ', u'_') + u".log"_s;
        writeFile(path, encodeFixture(text, kind));
        const bool wide = kind.startsWith(u"UTF-16"_s);
        checkFileAgainstTab(path, probes, kind, !wide, { 16, 61, 4096, FileContentSearch::kReadBlockBytes });
        const FileContentSearch::Result r = searchFile(path, probes.first());
        const TextEncoding expected = kind == u"UTF-8 BOM"_s ? TextEncoding::Utf8
            : kind == u"UTF-16LE"_s                          ? TextEncoding::Utf16LE
            : kind == u"UTF-16BE"_s                          ? TextEncoding::Utf16BE
                                                             : *TextEncodings::fromName(kind);
        if (r.usedEncoding != expected) {
            ++g_failures;
            std::fprintf(stderr, "FAIL %s: the scanner read it as %s\n", qPrintable(kind),
                         qPrintable(TextEncodings::displayName(r.usedEncoding)));
        }
    }

    // То же через сам сканер: одно дерево, все кодировки сразу.
    QTreeWidget tree;
    DirectoryScanner scanner(&tree);
    scanner.setRefreshMode(DirectoryScanner::RefreshMode::Off);
    scanner.setFallbackEncoding(testdocs::kTestFallbackEncoding);
    scanner.scan(dir);
    CHECK(waitFor([&] { return tree.topLevelItemCount() == kEncodingKinds.size()
                            && allScanned(tree.invisibleRootItem()); }),
          "encodings: scan completes");
    int matched = -1;
    QObject::connect(&scanner, &DirectoryScanner::contentFilterFinished,
                     [&](int m, int) { matched = m; });
    for (const ContentProbe& probe : probes) {
        matched = -1;
        scanner.setContentFilter(probe.text, probe.regex, probe.caseSensitive);
        scanner.applyFilters();
        CHECK(waitFor([&] { return matched >= 0; }), "encodings: content search finishes");
        const int want = probe.expect == 1 ? int(kEncodingKinds.size()) : 0;
        if (matched != want) {
            QStringList shown;
            for (int i = 0; i < tree.topLevelItemCount(); ++i)
                if (!tree.topLevelItem(i)->isHidden())
                    shown << tree.topLevelItem(i)->text(ScanCol::Name);
            ++g_failures;
            std::fprintf(stderr, "FAIL scanner %s: %d of %d files match [%s]\n",
                         qPrintable(describeProbe(probe)), matched, want,
                         qPrintable(shown.join(u", "_s)));
        }
    }
}

// Совпадение, разрезанное границей блока чтения, и многобайтный символ на
// границе — на каждом сдвиге.
static void testContentBlockEdges()
{
    QTemporaryDir tmp;
    const QString dir = tmp.path();
    const QString needle = u"Ошибка соединения"_s;
    for (const QString& kind : { u"UTF-8"_s, u"Windows-1251"_s, u"KOI8-R"_s, u"UTF-16LE"_s }) {
        int misses = 0;
        for (int shift = 0; shift < 48; ++shift) {
            QString line = u"2026-03-01 10:00:00 ERROR "_s + QString(shift, u'x') + u" "_s + needle
                + u" конец\n"_s;
            const QString path = dir + u"/edge.log"_s;
            writeFile(path, encodeFixture(line + line.toUpper().replace(u"ОШИБКА"_s, u"сбой"_s), kind));
            for (const qint64 block : { 16, 17, 23, 32 }) {
                if (!searchFile(path, { needle, false, true }, block).matched)
                    ++misses;
                if (!searchFile(path, { u"ERROR x* Ошибка соединения конец$"_s, true, true }, block).matched)
                    ++misses;
            }
        }
        if (misses) {
            ++g_failures;
            std::fprintf(stderr, "FAIL block edges, %s: %d searches missed the needle\n",
                         qPrintable(kind), misses);
        }
    }

    // Строка без '\n' длиннее предела проверяется кусками с перекрытием:
    // подстрока на стыке кусков не теряется.
    int misses = 0;
    for (int shift = 0; shift < 40; ++shift) {
        const QString path = dir + u"/long.bin"_s;
        writeFile(path, QByteArray(100 + shift, 'z') + needle.toUtf8() + QByteArray(200, 'z'));
        if (!searchFile(path, { needle, false, false }, 16, TextEncoding::Windows1251, 64).matched)
            ++misses;
    }
    CHECK(misses == 0, "a needle inside an over-long line is found at every offset");

    // Регекс (кусок без перекрытия): многобайтный символ на месте разреза
    // длинной строки не теряется — разрез идёт по границе символа.
    misses = 0;
    for (int shift = 0; shift < 40; ++shift) {
        const QString path = dir + u"/long-utf8.bin"_s;
        writeFile(path, QByteArray(60 + shift, 'z') + u"ж"_s.toUtf8() + QByteArray(200, 'z')
                            + u" ещё кириллица"_s.toUtf8());
        if (!searchFile(path, { u"ж"_s, true, true }, 16, TextEncoding::Windows1251, 64).matched)
            ++misses;
    }
    CHECK(misses == 0, "a UTF-8 character at the cut of an over-long line is kept whole");

    // Через сканер, блок 256 КиБ: кириллица UTF-8 и cp1251 ровно на стыке.
    QTemporaryDir tree256;
    for (const QString& kind : { u"UTF-8"_s, u"Windows-1251"_s }) {
        QByteArray bytes(FileContentSearch::kReadBlockBytes - 5, 'a');
        bytes += '\n';
        bytes += encodeFixture(needle + u" и ещё немного русского текста\n"_s, kind);
        writeFile(tree256.path() + u"/straddle-"_s + kind + u".log"_s, bytes);
    }
    QTreeWidget tree;
    DirectoryScanner scanner(&tree);
    scanner.setRefreshMode(DirectoryScanner::RefreshMode::Off);
    scanner.setFallbackEncoding(testdocs::kTestFallbackEncoding);
    scanner.scan(tree256.path());
    CHECK(waitFor([&] { return tree.topLevelItemCount() == 2 && allScanned(tree.invisibleRootItem()); }),
          "straddle: scan completes");
    int matched = -1;
    QObject::connect(&scanner, &DirectoryScanner::contentFilterFinished,
                     [&](int m, int) { matched = m; });
    scanner.setContentFilter(u"ОШИБКА СОЕДИНЕНИЯ"_s, false, false);
    scanner.applyFilters();
    CHECK(waitFor([&] { return matched >= 0; }), "straddle: search finishes");
    CHECK(matched == 2, "cyrillic across the 256 KiB block edge is found in UTF-8 and cp1251");
}

// Предварительное решение и его пересмотр (шаг 3а): результат — как у файла,
// сразу прочитанного итоговой кодировкой, и как у вкладки.
static void testContentProvisional()
{
    QTemporaryDir tmp;
    const QString dir = tmp.path();
    const QByteArray asciiPad = [] {
        QByteArray pad;
        for (int i = 0; pad.size() < 300 * 1024; ++i)
            pad += "2026-03-01 10:00:00 INFO  padding line " + QByteArray::number(i) + "\n";
        return pad;
    }();
    // Кириллический лог (~20 не-ASCII строк): свидетельства хватает на
    // окончательное решение, но набирается оно по строкам — у резидентного
    // парсера решение до этого предварительное на каждой из них (Q-18).
    const QString cyrText = testdocs::cyrillicLogText(0);
    const QByteArray utf8Text = cyrText.toUtf8();
    const QByteArray cp1251Text = testdocs::encodeText(cyrText, TextEncoding::Windows1251);

    struct Case {
        const char* name;
        QByteArray bytes;
        QList<ContentProbe> probes;
    };
    const QList<Case> cases{
        // Мусорный байт в начале UTF-8-файла: решение — запасная, предварительно;
        // кириллица дальше 64 КБ пересматривает его в UTF-8.
        { "garbage byte, then UTF-8 beyond 64 KiB",
          QByteArray("2026-03-01 09:59:59 INFO  torn \xFF byte\n") + asciiPad + utf8Text,
          { { u"соединения"_s, false, false, 1 }, { u"пользователь иванов"_s, false, false, 1 },
            { u"torn"_s, false, false, 1 } } },
        // Строка с запросом прочитана под предварительной запасной (мусор
        // перевешивает одно слово UTF-8), пересмотр приходит следующими блоками.
        { "UTF-8 word under a provisional single-byte decision",
          QByteArray("2026-03-01 09:59:58 WARN  junk \xC0\xC1\xF5\xFF\xFE\xC0\xC1\xF5\xFF\n"
                     "2026-03-01 09:59:59 ERROR сбой\n") + asciiPad + utf8Text,
          { { u"сбой"_s, false, true, 1 }, { u"ERROR сбой$"_s, true, true, 1 } } },
        // Обратное: слово UTF-8 даёт предварительное UTF-8, дальше — cp1251;
        // итоговая кодировка — cp1251, и строка «сбой» уже не находится.
        { "provisional UTF-8 revised to cp1251",
          QByteArray("2026-03-01 09:59:59 ERROR сбой\n") + asciiPad + cp1251Text,
          { { u"сбой"_s, false, false, 0 }, { u"соединения"_s, false, false, 1 } } },
        // Одно слово cp1251 — предварительно, пересмотра нет.
        { "one cp1251 word, never revised",
          QByteArray("2026-03-01 09:59:59 ERROR ") + testdocs::encodeText(u"сбой"_s, TextEncoding::Windows1251)
              + "\n" + asciiPad,
          { { u"СБОЙ"_s, false, false, -1 }, { u"сбой"_s, true, true, -1 } } },
    };
    for (const Case& c : cases) {
        const QString path = dir + u"/provisional.log"_s;
        writeFile(path, c.bytes);
        checkFileAgainstTab(path, c.probes, QString::fromLatin1(c.name), true,
                            { 64, 4096, 64 * 1024, FileContentSearch::kReadBlockBytes });
        for (const ContentProbe& probe : c.probes) {
            const bool got = searchFile(path, probe).matched;
            if (probe.expect >= 0 && got != (probe.expect == 1)) {
                ++g_failures;
                std::fprintf(stderr, "FAIL %s %s: the scanner says %d\n", c.name,
                             qPrintable(describeProbe(probe)), int(got));
            }
            // Запомненные строки не влезли — пересмотр перечитывает файл с
            // начала; ответ тот же, прогресс не считает байты дважды.
            for (const qint64 block : { 64, 4096 }) {
                std::atomic<qint64> done{ 0 };
                FileContentSearch::Options options;
                options.fallback = testdocs::kTestFallbackEncoding;
                options.blockBytes = block;
                options.retainedLimitBytes = 1;
                options.bytesDone = &done;
                const ContentQuery query(probe.text, probe.regex,
                                         probe.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
                const bool reread = FileContentSearch::search(path, query, options).matched;
                if (reread != got || done.load() > c.bytes.size()) {
                    ++g_failures;
                    std::fprintf(stderr, "FAIL %s %s (reread, block %lld): %d vs %d, %lld bytes counted\n",
                                 c.name, qPrintable(describeProbe(probe)), (long long)block,
                                 int(reread), int(got), (long long)done.load());
                }
            }
        }
    }
}

// Запасная кодировка: значением из настроек; смена перепроверяет только
// файлы, решённые автоматикой в однобайтную кодировку.
static void testContentFallback()
{
    QTemporaryDir tmp;
    const QString dir = tmp.path();
    const QString text = testdocs::cyrillicLogText(2);
    writeFile(dir + u"/cp1251.log"_s, testdocs::encodeText(text, TextEncoding::Windows1251));
    writeFile(dir + u"/utf8.log"_s, text.toUtf8());
    writeFile(dir + u"/ascii.log"_s, "2026-03-01 10:00:00 INFO  nothing russian here\n");
    const qint64 cp1251Size = QFileInfo(dir + u"/cp1251.log"_s).size();

    QTreeWidget tree;
    DirectoryScanner scanner(&tree);
    scanner.setRefreshMode(DirectoryScanner::RefreshMode::Off);
    scanner.setFallbackEncoding(TextEncoding::Windows1252);
    scanner.scan(dir);
    CHECK(waitFor([&] { return tree.topLevelItemCount() == 3 && allScanned(tree.invisibleRootItem()); }),
          "fallback: scan completes");
    int matched = -1;
    qint64 bytesTotal = -1;
    QObject::connect(&scanner, &DirectoryScanner::contentFilterFinished,
                     [&](int m, int) { matched = m; });
    QObject::connect(&scanner, &DirectoryScanner::contentFilterProgress,
                     [&](qint64, qint64 all, int, int) { bytesTotal = all; });

    scanner.setContentFilter(u"соединения"_s, false, false);
    scanner.applyFilters();
    CHECK(waitFor([&] { return matched >= 0; }), "fallback: first search finishes");
    CHECK(matched == 1, "with a Windows-1252 fallback only the UTF-8 file has Cyrillic");

    matched = -1;
    bytesTotal = -1;
    scanner.setFallbackEncoding(TextEncoding::Windows1251);
    CHECK(waitFor([&] { return matched >= 0; }), "fallback change re-runs the content filter");
    CHECK(matched == 2, "with a Windows-1251 fallback the cp1251 file matches too");
    CHECK(bytesTotal == cp1251Size,
          "only the file decided by the fallback is read again (UTF-8 and ASCII verdicts are kept)");

    // Тот же ответ, что у вкладки с той же запасной.
    checkFileAgainstTab(dir + u"/cp1251.log"_s, { { u"соединения"_s, false, false, 1 } },
                        u"fallback cp1251"_s);

    // Панель берёт запасную из настроек и следует за её сменой.
    AppSettings& settings = AppSettings::instance();
    const std::optional<TextEncoding> before = settings.fallbackEncodingChoice();
    settings.setFallbackEncodingChoice(TextEncoding::Koi8R);
    {
        DirectoryScannerPanel panel;
        CHECK(panel.scanner()->fallbackEncoding() == TextEncoding::Koi8R,
              "the panel hands the configured fallback to its scanner");
        settings.setFallbackEncodingChoice(TextEncoding::Ibm866);
        CHECK(panel.scanner()->fallbackEncoding() == TextEncoding::Ibm866,
              "the panel follows a fallback change");
    }
    settings.setFallbackEncodingChoice(before);
}

// Отмена посреди файла: воркер выходит между блоками, совпадения не сообщает.
static void testContentCancel()
{
    QTemporaryDir tmp;
    const QString path = tmp.path() + u"/huge.log"_s;
    {
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly), "cancel: create the file");
        const QByteArray line = "2026-03-01 10:00:00 INFO  nothing to see here, move along\n";
        QByteArray chunk;
        while (chunk.size() < 1024 * 1024)
            chunk += line;
        for (int i = 0; i < 48; ++i)
            f.write(chunk);
        f.write(testdocs::encodeText(u"2026-03-01 10:00:01 ERROR сбой в самом конце\n"_s,
                                     TextEncoding::Windows1251));
    }
    const qint64 size = QFileInfo(path).size();
    const FileContentSearch::Result whole = searchFile(path, { u"сбой"_s, false, false });
    CHECK(whole.matched && whole.bytesRead == size,
          "cancel: the needle is at the end, and the whole file is accounted for");

    // Отмена ставится из потока поиска после блока, кончившегося за первым
    // мегабайтом: детерминированно «посреди файла», без гонки с таймером.
    std::atomic<bool> cancelled{ false };
    std::atomic<qint64> bytesDone{ 0 };
    FileContentSearch::Options options;
    options.fallback = TextEncoding::Windows1251;
    options.blockBytes = 64 * 1024;
    options.cancelled = &cancelled;
    options.bytesDone = &bytesDone;
    options.afterBlock = [&](qint64 end) {
        if (end >= 1024 * 1024)
            cancelled.store(true);
    };
    const ContentQuery query(u"сбой"_s, false, Qt::CaseInsensitive);
    const FileContentSearch::Result result = FileContentSearch::search(path, query, options);
    CHECK(result.cancelled, "cancel: the search reports cancellation");
    CHECK(!result.matched, "cancel: a cancelled search matches nothing");
    CHECK(result.bytesRead == bytesDone.load() && result.bytesRead < 2 * 1024 * 1024,
          "cancel: the worker stops at the next block, and reports what it read");
}

// Статистика сканера от кодировки не зависит: файлы cp1251 и UTF-16 дают те
// же цифры, что тот же текст в UTF-8, и совпадают с вкладкой.
static void testStatsAcrossEncodings()
{
    QTemporaryDir tmp;
    const QString text = testdocs::cyrillicLogText(3);
    const QString utf8Path = tmp.path() + u"/stats-utf8.log"_s;
    writeFile(utf8Path, text.toUtf8());
    LogParser parser;
    const LogParser::FileStats reference = parser.analyzeFileForStats(utf8Path).stats;
    CHECK(reference.totalEntries > 0 && reference.errorCount > 0 && reference.warnCount > 0,
          "stats: the fixture has records and alerts");
    for (const QString& kind : { u"Windows-1251"_s, u"IBM866"_s, u"KOI8-R"_s, u"UTF-16LE"_s, u"UTF-16BE"_s }) {
        const QString path = tmp.path() + u"/stats-"_s + kind + u".log"_s;
        writeFile(path, encodeFixture(text, kind));
        const LogParser::FileStats stats = parser.analyzeFileForStats(path).stats;
        if (stats.totalEntries != reference.totalEntries || stats.warnCount != reference.warnCount
            || stats.errorCount != reference.errorCount || stats.fatalCount != reference.fatalCount
            || stats.firstEntryTimestamp != reference.firstEntryTimestamp
            || stats.lastEntryTimestamp != reference.lastEntryTimestamp) {
            ++g_failures;
            std::fprintf(stderr, "FAIL stats %s: %d entries W%d E%d F%d, UTF-8 gives %d W%d E%d F%d\n",
                         qPrintable(kind), stats.totalEntries, stats.warnCount, stats.errorCount,
                         stats.fatalCount, reference.totalEntries, reference.warnCount,
                         reference.errorCount, reference.fatalCount);
        }
        checkAgainstTab(path, qPrintable(u"stats "_s + kind), !kind.startsWith(u"UTF-16"_s));
    }
}

// Инкрементальное обновление: дописанный хвост, новый файл и ротация
// проверяются кодировкой файла.
static void testContentIncremental()
{
    QTemporaryDir tmp;
    const QString dir = tmp.path();
    const QString grows = dir + u"/grows.log"_s;
    const QString rotated = dir + u"/rotated.log"_s;
    writeFile(grows, "2026-03-01 10:00:00 INFO  only ascii so far\n");
    writeFile(rotated, "2026-03-01 10:00:00 INFO  before rotation\n");

    QTreeWidget tree;
    DirectoryScanner scanner(&tree);
    scanner.setRefreshMode(DirectoryScanner::RefreshMode::Off);
    scanner.setFallbackEncoding(testdocs::kTestFallbackEncoding);
    scanner.scan(dir);
    CHECK(waitFor([&] { return tree.topLevelItemCount() == 2 && allScanned(tree.invisibleRootItem()); }),
          "incremental: scan completes");
    int matched = -1, rescans = 0;
    QObject::connect(&scanner, &DirectoryScanner::contentFilterFinished,
                     [&](int m, int) { matched = m; });
    QObject::connect(&scanner, &DirectoryScanner::rescanFinished, [&](int, int, int) { ++rescans; });
    scanner.setContentFilter(u"ОШИБКА СОЕДИНЕНИЯ"_s, false, false);
    scanner.applyFilters();
    CHECK(waitFor([&] { return matched >= 0; }), "incremental: first search finishes");
    CHECK(matched == 0, "incremental: nothing matches yet");

    const auto visibleNames = [&] {
        QStringList shown;
        for (int i = 0; i < tree.topLevelItemCount(); ++i)
            if (!tree.topLevelItem(i)->isHidden())
                shown << tree.topLevelItem(i)->text(ScanCol::Name);
        shown.sort();
        return shown;
    };
    const QString cyr = testdocs::cyrillicLogText(0);
    appendFile(grows, testdocs::encodeText(cyr, TextEncoding::Windows1251));
    writeFile(rotated, testdocs::encodeText(cyr, TextEncoding::Koi8R));
    writeFile(dir + u"/new.log"_s, testdocs::encodeText(cyr, TextEncoding::Ibm866));
    matched = -1;
    scanner.rescan();
    CHECK(waitFor([&] { return rescans == 1 && allScanned(tree.invisibleRootItem()); }),
          "incremental: rescan finishes");
    CHECK(waitFor([&] { return !scanner.isContentFilterRunning()
                            && visibleNames() == QStringList{ u"grows.log"_s, u"new.log"_s, u"rotated.log"_s }; }),
          "incremental: the cp1251 tail, the KOI8-R rotation and the new cp866 file all match");
}

// ---------------------------------------------------------------------------
// F-13. Даты до 1970 года: From/To в дереве, ключ сортировки (отрицательный
// момент, ровно эпоха — 0) и фильтр по датам. Моменты заданы заранее.
static void testPre1970Dates()
{
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        ++g_failures;
        std::fprintf(stderr, "FAIL cannot create temp dir for the pre-1970 test\n");
        return;
    }
    const QString dir = tmp.path();
    writeFile(dir + "/moon.log",
              "1969-07-20 20:17:40 INFO  landing\n"
              "1969-07-21 02:56:15 WARN  first step\n");
    writeFile(dir + "/epoch.log",
              "1969-12-31T23:59:59.999Z INFO  one ms before\n"
              "1970-01-01T00:00:00.000Z INFO  epoch\n");
    writeFile(dir + "/now.log", "2026-04-01 10:00:00 INFO  now\n");

    const QDateTime moonFrom(QDate(1969, 7, 20), QTime(20, 17, 40));
    const QDateTime moonTo(QDate(1969, 7, 21), QTime(2, 56, 15));
    const LogParser parser;
    const LogParser::FileStats moon = parser.analyzeFileForStats(dir + "/moon.log").stats;
    CHECK(moon.firstEntryTimestamp == moonFrom && moon.lastEntryTimestamp == moonTo,
          "pre-1970: analyzeFileForStats keeps moments before the epoch");
    const LogParser::FileStats epoch = parser.analyzeFileForStats(dir + "/epoch.log").stats;
    CHECK(epoch.firstEntryTimestamp.isValid() && epoch.firstEntryTimestamp.toMSecsSinceEpoch() == -1
              && epoch.lastEntryTimestamp.isValid()
              && epoch.lastEntryTimestamp.toMSecsSinceEpoch() == 0,
          "pre-1970: -1 ms and the epoch itself are timestamps");

    QTreeWidget tree;
    DirectoryScanner scanner(&tree);
    scanner.setRefreshMode(DirectoryScanner::RefreshMode::Off);
    scanner.scan(dir);
    CHECK(waitFor([&] { return tree.topLevelItemCount() == 3
                            && allScanned(tree.invisibleRootItem()); }),
          "pre-1970: scan completes");

    QTreeWidgetItem* moonItem = findItem(tree.invisibleRootItem(), u"moon.log"_s);
    QTreeWidgetItem* epochItem = findItem(tree.invisibleRootItem(), u"epoch.log"_s);
    QTreeWidgetItem* nowItem = findItem(tree.invisibleRootItem(), u"now.log"_s);
    CHECK(moonItem && epochItem && nowItem, "pre-1970: all files listed");
    if (!moonItem || !epochItem || !nowItem)
        return;
    CHECK(moonItem->text(ScanCol::From) == u"1969-07-20 20:17:40"_s
              && moonItem->text(ScanCol::To) == u"1969-07-21 02:56:15"_s,
          qPrintable(u"pre-1970: From/To shown, got "_s + moonItem->text(ScanCol::From)
                     + u" / "_s + moonItem->text(ScanCol::To)));
    CHECK(moonItem->data(ScanCol::From, ScanRole::SortKey).toLongLong()
              == moonFrom.toMSecsSinceEpoch(),
          "pre-1970: From sort key is the negative moment");
    CHECK(epochItem->data(ScanCol::From, ScanRole::SortKey).isValid()
              && epochItem->data(ScanCol::From, ScanRole::SortKey).toLongLong() == -1
              && epochItem->data(ScanCol::To, ScanRole::SortKey).isValid()
              && epochItem->data(ScanCol::To, ScanRole::SortKey).toLongLong() == 0,
          "pre-1970: -1 ms and 0 ms are sort keys, not 'no timestamp'");

    // Сортировка по From: 1969-07 < −1 мс < 2026.
    tree.sortItems(ScanCol::From, Qt::AscendingOrder);
    QStringList order;
    for (int i = 0; i < tree.topLevelItemCount(); ++i)
        order << tree.topLevelItem(i)->text(ScanCol::Name);
    CHECK(order == (QStringList{ u"moon.log"_s, u"epoch.log"_s, u"now.log"_s }),
          qPrintable(u"pre-1970: sorting by From, got "_s + order.join(u", "_s)));

    // Фильтр по датам: окно [−1 с, −1 мс] задевает только epoch.log.
    scanner.setDateFilter(true, QDateTime::fromMSecsSinceEpoch(-1000),
                          QDateTime::fromMSecsSinceEpoch(-1));
    scanner.applyFilters();
    CHECK(waitFor([&] { return moonItem->isHidden() && !epochItem->isHidden()
                            && nowItem->isHidden(); }),
          "pre-1970: date filter around the epoch keeps only epoch.log");
    scanner.setDateFilter(true, QDateTime(QDate(1969, 7, 21), QTime(0, 0)),
                          QDateTime(QDate(1969, 7, 22), QTime(0, 0)));
    scanner.applyFilters();
    CHECK(waitFor([&] { return !moonItem->isHidden() && epochItem->isHidden()
                            && nowItem->isHidden(); }),
          "pre-1970: date filter in 1969 keeps only moon.log");
}

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
    testContentEncodings();
    testContentBlockEdges();
    testContentProvisional();
    testContentFallback();
    testContentCancel();
    testStatsAcrossEncodings();
    testContentIncremental();
    testPre1970Dates();

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
