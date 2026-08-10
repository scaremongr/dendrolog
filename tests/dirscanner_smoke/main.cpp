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

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QToolButton>
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
    const LogParser::FileStats whole = parser.analyzeFileForStats(path);
    const LogParser::FileStats onlyTail = parser.analyzeFileForStats(path, head.size());

    writeFile(path, head);
    const LogParser::FileStats onlyHead = parser.analyzeFileForStats(path);

    CHECK(whole.parseSuccess && onlyHead.parseSuccess && onlyTail.parseSuccess,
          "all three analyses succeed");
    CHECK(onlyHead.totalEntries + onlyTail.totalEntries == whole.totalEntries,
          "head + tail entry counts add up to the whole file");
    CHECK(onlyHead.warnCount + onlyTail.warnCount == whole.warnCount, "warn counts add up");
    CHECK(onlyHead.errorCount + onlyTail.errorCount == whole.errorCount, "error counts add up");
    CHECK(onlyHead.fatalCount + onlyTail.fatalCount == whole.fatalCount, "fatal counts add up");
    CHECK(onlyHead.firstEntryTimestamp == whole.firstEntryTimestamp,
          "the head keeps the first timestamp");
    CHECK(onlyTail.lastEntryTimestamp == whole.lastEntryTimestamp,
          "the tail carries the last timestamp");

    QFile::remove(path);
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
