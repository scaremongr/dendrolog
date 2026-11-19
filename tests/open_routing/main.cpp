// ============================================================================
// open_routing — «что из брошенного / переданного куда идёт» (Q-6, Q-9):
//   • OpenRouting::plan: пути из drag&drop, командной строки и от второго
//     экземпляра делятся на файлы (открыть) и каталог (Directory Scanner);
//     несколько каталогов — сканируется первый, остальные названы; файлы
//     вперемешку с каталогами открываются как раньше; несуществующее названо;
//   • план с каталогом действительно запускает скан в панели сканера;
//   • RecentFiles: спул stdin (`program | DendroLog -`) в недавние не
//     попадает — ни при открытии, ни из ini прежних версий; порядок, отсев
//     повторов и предел в 10 записей — как было в MainWindow;
//   • командная строка (F-1, шаг 4): --encoding <имя> и --encoding=<имя>,
//     синонимы, неизвестное и пропущенное имя — понятная ошибка, а не тихий
//     UTF-8; что второй экземпляр передаёт первому и как тот это разбирает.
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
#include <QUrl>
#include <cstdio>
#include <initializer_list>
#include <utility>

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

static void testCommandLine(const QTemporaryDir& tmp)
{
    using OpenRouting::parseCommandLine;
    const QString a = QDir(tmp.path()).filePath("cmd-a.log");

    {
        const auto cl = parseCommandLine({ "--encoding", "cp1251", a, "-" });
        CHECK(cl.error.isEmpty() && cl.encoding == TextEncoding::Windows1251,
              "--encoding cp1251 is Windows-1251");
        CHECK(cl.paths == QStringList({ a }) && cl.readStdin && !cl.newInstance,
              "paths and stdin around --encoding");
        const FileEncoding initial = OpenRouting::initialEncoding(cl);
        CHECK(initial.source == FileEncoding::Source::User
                  && initial.decoder.encoding() == TextEncoding::Windows1251
                  && !initial.automatic().decided(),
              "--encoding is a user choice; automatic detection is still undecided");
    }
    for (const auto& [name, expected] : std::initializer_list<std::pair<const char*, TextEncoding>>{
             { "utf-8", TextEncoding::Utf8 }, { "UTF8", TextEncoding::Utf8 },
             { "cp866", TextEncoding::Ibm866 }, { "IBM866", TextEncoding::Ibm866 },
             { "koi8-r", TextEncoding::Koi8R }, { "koi8r", TextEncoding::Koi8R },
             { "windows-1252", TextEncoding::Windows1252 }, { "Windows-1251", TextEncoding::Windows1251 } }) {
        const auto cl = parseCommandLine({ QStringLiteral("--encoding=") + QLatin1String(name), a });
        CHECK(cl.error.isEmpty() && cl.encoding == expected && cl.paths == QStringList({ a }),
              QStringLiteral("--encoding=%1").arg(QLatin1String(name)));
    }
    {
        const auto cl = parseCommandLine({ "--encoding", "latin-9", a });
        CHECK(!cl.encoding && cl.error.contains("latin-9") && cl.error.contains("Windows-1251"),
              "unknown name: a message naming it and the known ones: " + cl.error);
        CHECK(cl.paths == QStringList({ a }), "unknown name: files still open");
        CHECK(!OpenRouting::initialEncoding(cl).decided(),
              "unknown name: files open with automatic detection, not silently as UTF-8");
    }
    {
        const auto cl = parseCommandLine({ a, "--encoding" });
        CHECK(!cl.encoding && !cl.error.isEmpty() && cl.paths == QStringList({ a }),
              "missing name: a message");
    }
    // Имя забыли: следующий флаг или существующий файл не съедается.
    {
        QFile touch(a);
        CHECK(touch.open(QIODevice::WriteOnly), "write cmd-a.log");
        touch.close();
        const auto stdinCl = parseCommandLine({ "--encoding", "-" });
        CHECK(stdinCl.readStdin && !stdinCl.encoding && !stdinCl.error.isEmpty(),
              "--encoding - : stdin is still read, the missing name is reported");
        const auto fileCl = parseCommandLine({ "--encoding", a });
        CHECK(fileCl.paths == QStringList({ a }) && !fileCl.error.isEmpty(),
              "--encoding <existing file>: the file is opened, the missing name is reported");
        const auto flagCl = parseCommandLine({ "--encoding", "--new-instance", a });
        CHECK(flagCl.newInstance && flagCl.paths == QStringList({ a }), "--encoding --new-instance");
    }
    {
        const auto cl = parseCommandLine({ "--new-instance", a });
        CHECK(cl.newInstance && cl.error.isEmpty() && !cl.encoding
                  && !OpenRouting::initialEncoding(cl).decided(),
              "no --encoding: automatic detection");
    }
    // Второй экземпляр: имя как дано (ошибку покажет окно первого), пути —
    // абсолютные; первый разбирает список тем же разбором.
    {
        const QString cwd = QDir::currentPath();
        QDir::setCurrent(tmp.path());
        const auto cl = parseCommandLine({ "--encoding", "koi8r", "cmd-a.log" });
        const QStringList forwarded = OpenRouting::forwardedArguments(cl);
        QDir::setCurrent(cwd);
        const auto received = parseCommandLine(forwarded);
        CHECK(received.encoding == TextEncoding::Koi8R && received.error.isEmpty()
                  && received.paths == QStringList({ QDir::cleanPath(a) }),
              "forwarded to the first instance: " + forwarded.join(' '));
        const auto bad = parseCommandLine(OpenRouting::forwardedArguments(
            parseCommandLine({ "--encoding=nope", a })));
        CHECK(!bad.encoding && bad.error.contains("nope"),
              "an unknown name reaches the first instance and is reported there");
        const auto plain = OpenRouting::forwardedArguments(parseCommandLine({ a }));
        CHECK(plain == QStringList({ QDir::cleanPath(a) }), "no --encoding — only paths");
    }
}

// R-5: файл сессии (*.dlsession) открывается как сессия, а не как лог —
// одно правило на командную строку, второй экземпляр и drag&drop.
static void testSessions(const QTemporaryDir& tmp)
{
    const QString root = QDir(tmp.path()).absolutePath();
    const QString log = writeFile(root + "/s/app.log", "2026-03-05 10:00:00.000 INFO a\n");
    const QString session = writeFile(root + "/s/incident.dlsession", "{}");
    const QString upper = writeFile(root + "/s/OTHER.DLSESSION", "{}");
    const QString dir = root + "/s";

    {   // Командная строка этого процесса.
        const auto cl = OpenRouting::parseCommandLine({ session, log });
        const auto plan = OpenRouting::plan(cl.paths);
        CHECK(plan.sessions == QStringList({ session }), "command line: .dlsession is a session");
        CHECK(plan.files == QStringList({ log }), "command line: the log still opens as a file");
        CHECK(!plan.isEmpty(), "command line: session-only plan is not empty");
        CHECK(OpenRouting::plan({ upper }).sessions == QStringList({ upper }),
              "suffix is case-insensitive");
        CHECK(!OpenRouting::plan({ session }).isEmpty() && OpenRouting::plan({ session }).files.isEmpty(),
              "a session alone: not opened as a log");
    }
    {   // Второй экземпляр: относительный путь разрешён от ЕГО каталога,
        // первый разбирает переданное тем же правилом.
        const QString saved = QDir::currentPath();
        QDir::setCurrent(dir);
        const auto secondary = OpenRouting::parseCommandLine({ "incident.dlsession", "app.log" });
        const QStringList forwarded = OpenRouting::forwardedArguments(secondary);
        QDir::setCurrent(root);
        const auto primary = OpenRouting::parseCommandLine(forwarded);
        const auto plan = OpenRouting::plan(primary.paths);
        CHECK(plan.sessions == QStringList({ session }), "second instance: the session reaches the first one");
        CHECK(plan.files == QStringList({ log }), "second instance: the log reaches the first one");
        QDir::setCurrent(saved);
    }
    {   // Брошенное в окно: локальные URL → пути → то же правило; повтор убран.
        QStringList dropped;
        for (const QUrl& url : { QUrl::fromLocalFile(session), QUrl::fromLocalFile(log),
                                 QUrl::fromLocalFile(session) })
            dropped << url.toLocalFile();
        const auto plan = OpenRouting::plan(dropped);
        CHECK(plan.sessions == QStringList({ session }) && plan.files == QStringList({ log }),
              "drop: session opens as a session, log as a file");
    }
    {   // Пропавший файл сессии — назван, не открывается.
        const auto plan = OpenRouting::plan({ root + "/gone.dlsession" });
        CHECK(plan.isEmpty() && plan.missing.size() == 1, "missing session is reported");
    }
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
    testCommandLine(tmp);
    testSessions(tmp);

    if (failures == 0)
        std::printf("open_routing: OK\n");
    return failures == 0 ? 0 : 1;
}
