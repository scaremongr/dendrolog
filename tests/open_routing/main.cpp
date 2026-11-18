// ============================================================================
// open_routing — «что из брошенного / переданного куда идёт» (Q-6, Q-9):
//   • OpenRouting::plan: пути из drag&drop, командной строки и от второго
//     экземпляра делятся на файлы (открыть) и каталог (Directory Scanner);
//     несколько каталогов — сканируется первый, остальные названы; файлы
//     вперемешку с каталогами открываются как раньше; несуществующее названо;
//   • план с каталогом действительно запускает скан в панели сканера;
//   • RecentFiles: спул stdin (`program | DendroLog -`) в недавние не
//     попадает — ни при открытии, ни из ini прежних версий; порядок, отсев
//     повторов и предел в 10 записей — как было в MainWindow.
// ============================================================================

#include "directoryscanner.h"
#include "directoryscannerpanel.h"
#include "openrouting.h"
#include "stdinspooler.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <cstdio>

static int failures = 0;
#define CHECK(condition, label) do { \
    if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL: %s (line %d)\n", qPrintable(QString(label)), __LINE__); } \
} while (0)

template <typename Predicate>
static bool waitFor(Predicate ready, int timeoutMs = 30000)
{
    QElapsedTimer timer;
    timer.start();
    while (!ready()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

static QString writeFile(const QString& path, const QByteArray& content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        std::fprintf(stderr, "cannot write %s\n", qPrintable(path));
    f.write(content);
    return path;
}

static void testPlan(const QTemporaryDir& tmp)
{
    const QString root = QDir(tmp.path()).absolutePath();
    const QString a = writeFile(root + "/a.log", "2026-03-05 10:00:00.000 INFO a\n");
    const QString b = writeFile(root + "/b.log", "2026-03-05 10:00:01.000 INFO b\n");
    const QString dir1 = root + "/logs1";
    const QString dir2 = root + "/logs2";
    writeFile(dir1 + "/one.log", "2026-03-05 10:00:02.000 ERROR one\n");
    writeFile(dir2 + "/two.log", "2026-03-05 10:00:03.000 WARN two\n");
    const QString missing = root + "/nope.log";

    {   // Только файлы — как раньше: все открываются, скана нет.
        const auto plan = OpenRouting::plan({a, b});
        CHECK(plan.files == QStringList({a, b}), "files only: both files open, order kept");
        CHECK(plan.scanDirectory.isEmpty(), "files only: nothing to scan");
        CHECK(plan.skippedDirectories.isEmpty() && plan.missing.isEmpty(), "files only: nothing left over");
        CHECK(!plan.isEmpty(), "files only: plan is not empty");
    }
    {   // Q-6: один каталог — в сканер.
        const auto plan = OpenRouting::plan({dir1});
        CHECK(plan.files.isEmpty(), "folder: no files to open");
        CHECK(plan.scanDirectory == dir1, "folder: goes to the scanner");
        CHECK(!plan.isEmpty(), "folder: plan is not empty");
    }
    {   // Каталоги вперемешку с файлами: файлы открываются, сканируется
        // ПЕРВЫЙ каталог, второй назван пропущенным.
        const auto plan = OpenRouting::plan({a, dir1, b, dir2});
        CHECK(plan.files == QStringList({a, b}), "mixed: files open as before");
        CHECK(plan.scanDirectory == dir1, "mixed: the first folder is scanned");
        CHECK(plan.skippedDirectories == QStringList({dir2}), "mixed: the second folder is reported as skipped");
    }
    {   // Завершающий разделитель и повторы не дают второго каталога / файла.
        const auto plan = OpenRouting::plan({dir1 + "/", dir1, a, a});
        CHECK(plan.scanDirectory == dir1, "trailing separator: same folder, clean path");
        CHECK(plan.skippedDirectories.isEmpty(), "the same folder twice is not a skipped folder");
        CHECK(plan.files == QStringList({a}), "the same file twice opens once");
    }
    {   // Несуществующее не открывается и не сканируется, но названо.
        const auto plan = OpenRouting::plan({missing, QString(), a});
        CHECK(plan.files == QStringList({a}), "missing path: existing file still opens");
        CHECK(plan.missing == QStringList({missing}), "missing path is reported, empty string is dropped");
        CHECK(OpenRouting::plan({missing}).files.isEmpty(), "missing only: no files");
        CHECK(OpenRouting::plan({}).isEmpty(), "no paths: empty plan");
    }
    {   // Относительные пути (командная строка) разрешаются от текущего каталога.
        const QString saved = QDir::currentPath();
        QDir::setCurrent(root);
        const auto plan = OpenRouting::plan({"a.log", "logs2"});
        CHECK(plan.files == QStringList({a}), "relative file path becomes absolute");
        CHECK(plan.scanDirectory == dir2, "relative folder path becomes absolute");
        const QStringList abs = OpenRouting::absolutePaths({"a.log", "logs2", missing});
        CHECK(abs == QStringList({a, dir2, missing}),
              "absolutePaths: what a second instance hands over does not depend on its cwd");
        QDir::setCurrent(saved);
    }
}

// План с каталогом → панель сканера: скан запускается и находит файлы.
static void testFolderStartsScan(const QTemporaryDir& tmp)
{
    const QString dir = QDir(tmp.path()).absoluteFilePath("scanme");
    writeFile(dir + "/svc.log", "2026-03-05 10:00:00.000 ERROR boom\n");
    writeFile(dir + "/sub/other.log", "2026-03-05 10:00:01.000 INFO fine\n");

    const auto plan = OpenRouting::plan({dir});
    DirectoryScannerPanel panel;
    panel.scanner()->setFileExtensions({QStringLiteral("log")});
    panel.scanDirectory(plan.scanDirectory);
    CHECK(panel.scanner()->rootPath() == dir, "scanner is pointed at the dropped folder");
    auto* tree = panel.findChild<QTreeWidget*>();
    CHECK(tree != nullptr, "scanner panel has a tree");
    if (!tree)
        return;
    CHECK(waitFor([&] { return tree->topLevelItemCount() >= 2; }),
          "scan of the dropped folder lists its files");
}

static void testRecentFiles(const QTemporaryDir& tmp)
{
    const QString root = QDir(tmp.path()).absolutePath();
    const QString spool = StdinSpooler::spoolFilePathFor(4242);
    const QString a = root + "/a.log";
    const QString b = root + "/b.log";

    CHECK(StdinSpooler::isSpoolFilePath(spool), "spool path is recognised");
    CHECK(StdinSpooler::isSpoolFilePath(QDir::toNativeSeparators(spool)), "spool path with native separators is recognised");
    CHECK(!StdinSpooler::isSpoolFilePath(a), "ordinary log is not a spool");
    CHECK(!StdinSpooler::isSpoolFilePath(root + "/DendroLog-stdin-4242.log"),
          "a file of the same name outside the temp folder is the user's own file");
    CHECK(!StdinSpooler::isSpoolFilePath(QDir::temp().filePath("DendroLog-stdin-x.log")),
          "name without a pid is not a spool");

    RecentFiles recent;
    CHECK(recent.add(a), "ordinary file is added");
    // Q-9: то, что делал MainWindow::openStdinStream → openRecentFile.
    CHECK(!recent.add(spool), "stdin spool is not added to recent files");
    CHECK(recent.paths() == QStringList({a}), "recent files hold no spool entry");

    CHECK(recent.add(b), "second file is added");
    CHECK(recent.paths() == QStringList({b, a}), "newest first");
    recent.add(a);
    CHECK(recent.paths() == QStringList({a, b}), "re-opened file moves to the top, no duplicate");

    for (int i = 0; i < 15; ++i)
        recent.add(root + QStringLiteral("/f%1.log").arg(i));
    CHECK(recent.paths().size() == RecentFiles::MaxCount, "list is capped");
    CHECK(recent.paths().first() == root + "/f14.log", "cap drops the oldest");

    // ini прежних версий: мёртвая запись спула вычищается при загрузке.
    recent.setPaths({a, spool, b, a});
    CHECK(recent.paths() == QStringList({a, b}), "loading drops spool entries and duplicates");

    CHECK(recent.remove(a) && !recent.remove(a), "remove reports whether anything changed");
    recent.clear();
    CHECK(recent.paths().isEmpty(), "clear empties the list");
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "cannot create temp dir\n");
        return 2;
    }

    testPlan(tmp);
    testFolderStartsScan(tmp);
    testRecentFiles(tmp);

    if (failures == 0)
        std::printf("open_routing: OK\n");
    return failures == 0 ? 0 : 1;
}
