// command_output — вывод команды как растущий лог (F-7), offscreen.
//
// Дочерний процесс — сам этот exe в режиме генератора строк (`--emit`,
// `--forever`), так тест одинаков на Windows и Linux и не зависит от
// утилит окружения. Проверяется:
//   • CommandSpec: разбор строки без shell и через shell, имя вкладки,
//     недавние команды (порядок, повторы, предел, ini-строки);
//   • CommandSpooler: строки stdout и stderr доходят в спул по порядку,
//     код возврата, не запустилась (нет программы, нет каталога), рост файла
//     живого процесса, stop() — вежливо и принудительно (SIGTERM игнорируется),
//     ребёнок shell (внук) не переживает stop, release() — процесс завершён,
//     спулер удалён, спул удалён, перезапуск дописывает, деструктор убивает;
//   • вкладка (оба бэкенда): строки идут во вкладку по мере вывода,
//     авто-обновление и follow-tail включены, плашка показывает состояние и
//     код возврата, строки после выхода остаются, Restart дописывает, снимок
//     вкладки (сессия, память) команду не содержит, объединение вкладок
//     переносит процесс, закрытие вкладки завершает процесс и удаляет спул.

#include "appsettings.h"
#include "commandspooler.h"
#include "commandstatusbar.h"
#include "logviewwidget.h"
#include "spooldir.h"
#include "testdocuments.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

static int g_failures = 0;
static QString g_context;

#define CHECK(cond, msg)                                                                      \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            ++g_failures;                                                                     \
            std::fprintf(stderr, "FAIL [%s] %s (line %d)\n", qPrintable(g_context),           \
                         qPrintable(QString(msg)), __LINE__);                                 \
            std::fflush(stderr);                                                              \
        }                                                                                     \
    } while (0)

// ---------------------------------------------------------------------------
// Генератор строк (дочерний процесс)
// ---------------------------------------------------------------------------

static void emitLine(FILE* out, int i, const char* level, const char* text)
{
    std::fprintf(out, "2026-11-26 10:%02d:%02d.%03d %s %s %d\n", (i / 60000) % 60, (i / 1000) % 60,
                 i % 1000, level, text, i);
    std::fflush(out);
}

static void sleepMs(int ms)
{
#ifdef Q_OS_WIN
    Sleep(DWORD(ms));
#else
    usleep(useconds_t(ms) * 1000);
#endif
}

static long currentPid()
{
#ifdef Q_OS_WIN
    return long(GetCurrentProcessId());
#else
    return long(getpid());
#endif
}

// --emit N [exit]   — N строк в stdout, одна в stderr посередине, код exit
// --forever [ignore-term] — первой строкой PID, дальше строка каждые 20 мс
static int runGenerator(int argc, char** argv)
{
    if (std::strcmp(argv[1], "--emit") == 0) {
        const int n = argc > 2 ? std::atoi(argv[2]) : 10;
        const int code = argc > 3 ? std::atoi(argv[3]) : 0;
        for (int i = 1; i <= n; ++i) {
            emitLine(stdout, i, "INFO", "emitted line");
            if (i == n / 2)
                emitLine(stderr, i, "ERROR", "stderr line");
        }
        return code;
    }
#ifndef Q_OS_WIN
    // --quiet-forever <pidfile> — молчит, глух к SIGTERM и SIGPIPE, PID — в файл
    if (std::strcmp(argv[1], "--quiet-forever") == 0 && argc > 2) {
        std::signal(SIGTERM, SIG_IGN);
        std::signal(SIGPIPE, SIG_IGN);
        if (FILE* f = std::fopen(argv[2], "w")) {
            std::fprintf(f, "PID %ld\n", currentPid());
            std::fclose(f);
        }
        for (;;)
            sleepMs(50);
    }
#endif
    if (std::strcmp(argv[1], "--forever") == 0) {
#ifndef Q_OS_WIN
        if (argc > 2 && std::strcmp(argv[2], "ignore-term") == 0)
            std::signal(SIGTERM, SIG_IGN);
#endif
        std::fprintf(stdout, "PID %ld\n", currentPid());
        std::fflush(stdout);
        for (int i = 1;; ++i) {
            emitLine(stdout, i, "INFO", "forever line");
            sleepMs(20);
        }
    }
    return 2;
}

// ---------------------------------------------------------------------------
// Помощники
// ---------------------------------------------------------------------------

static QString selfExe()
{
    return QCoreApplication::applicationFilePath();
}

// Строка команды без shell: путь exe в кавычках (пробелы в пути сборки).
static QString selfCommand(const QString& args)
{
    return QLatin1Char('"') + QDir::toNativeSeparators(selfExe()) + QStringLiteral("\" ") + args;
}

static bool processAlive(qint64 pid)
{
    if (pid <= 0)
        return false;
#ifdef Q_OS_WIN
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (!h)
        return false;
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
#else
    return ::kill(pid_t(pid), 0) == 0;
#endif
}

static QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

static int countLines(const QString& path, const QByteArray& needle)
{
    return int(readAll(path).count(needle));
}

// Первая строка «PID n» в файле.
static qint64 pidFromSpool(const QString& path)
{
    const QByteArray bytes = readAll(path);
    const int at = bytes.indexOf("PID ");
    if (at < 0)
        return 0;
    const int end = bytes.indexOf('\n', at);
    if (end < 0)
        return 0;
    return bytes.mid(at + 4, end - at - 4).trimmed().toLongLong();
}

static bool waitState(CommandSpooler& s, CommandSpooler::State state, int timeoutMs = 30000)
{
    return testdocs::waitFor([&] { return s.state() == state; }, timeoutMs);
}

struct Spool {
    std::shared_ptr<SpoolDir> dir;
    explicit Spool(const QString& root) : dir(SpoolDir::create(root)) {}
    std::shared_ptr<SpoolItem> item(const QString& name) { return dir->createStreamFile(name); }
};

// ---------------------------------------------------------------------------
// CommandSpec и недавние команды
// ---------------------------------------------------------------------------

static void checkSpec()
{
    g_context = QStringLiteral("spec");
    CommandSpec spec;
    spec.commandLine = QStringLiteral("  kubectl logs -f \"deploy/my api\"  ");
    CommandSpec::Launch launch;
    CHECK(spec.resolve(&launch), "plain command resolves");
    CHECK(launch.program == QStringLiteral("kubectl"), "program is the first word");
    CHECK(launch.arguments == QStringList({"logs", "-f", "deploy/my api"}), "double quotes group arguments");
    CHECK(spec.shortLabel() == QStringLiteral("kubectl"), "tab label is the program name");

    spec.commandLine = QStringLiteral("\"C:\\Program Files\\Android\\adb.exe\" logcat");
    CHECK(spec.shortLabel() == QStringLiteral("adb"), "label drops the folder and .exe");

    QString error;
    spec.commandLine = QStringLiteral("grep \"unclosed");
    CHECK(!spec.resolve(&launch, &error) && !error.isEmpty(), "unclosed quote without the shell is refused");
    spec.useShell = true;
    CHECK(spec.resolve(&launch), "the shell takes the line as is");
#ifdef Q_OS_WIN
    CHECK(launch.nativeArguments.contains(QStringLiteral("grep \"unclosed")), "cmd.exe gets the line");
#else
    CHECK(launch.program == QStringLiteral("/bin/sh")
              && launch.arguments == QStringList({"-c", "grep \"unclosed"}),
          "sh -c gets the line");
#endif
    spec.commandLine = QStringLiteral("journalctl -f | grep sshd");
    CHECK(spec.shortLabel() == QStringLiteral("journalctl"), "shell label is the first word");

    // Три кавычки подряд — буквальная кавычка (правила splitCommand).
    spec.useShell = false;
    spec.commandLine = QStringLiteral("prog \"say \"\"\"hi\"\"\" now\"");
    CHECK(spec.resolve(&launch) && launch.arguments == QStringList({"say \"hi\" now"}),
          "tripled quote is a literal quote, not an unclosed one");
    spec.commandLine = QStringLiteral("   ");
    CHECK(!spec.resolve(&launch, &error) && !error.isEmpty(), "empty command is refused");

    // Недавние.
    QList<CommandSpec> history;
    CommandSpec a{QStringLiteral("a --x"), QStringLiteral("/tmp"), false};
    CommandSpec b{QStringLiteral("b | c"), QString(), true};
    history = CommandHistory::pushed(history, a);
    history = CommandHistory::pushed(history, b);
    history = CommandHistory::pushed(history, a);
    CHECK(history.size() == 2 && history[0] == a && history[1] == b, "repeat moves to the top, no duplicate");
    history = CommandHistory::pushed(history, CommandSpec{});
    CHECK(history.size() == 2, "empty command is not remembered");
    CommandSpec aShell = a;
    aShell.useShell = true;
    history = CommandHistory::pushed(history, aShell);
    CHECK(history.size() == 3, "same line through the shell is a different command");
    for (int i = 0; i < 30; ++i)
        history = CommandHistory::pushed(history, CommandSpec{QStringLiteral("cmd %1").arg(i), {}, false});
    CHECK(history.size() == CommandHistory::kMax, "history is capped");
    CHECK(history.first().commandLine == QStringLiteral("cmd 29"), "newest first");
    const QStringList stored = CommandHistory::toStrings(history);
    QStringList withJunk = stored;
    withJunk.insert(1, QStringLiteral("not json"));
    CHECK(CommandHistory::fromStrings(withJunk) == history, "round trip through ini strings, junk skipped");
}

// ---------------------------------------------------------------------------
// CommandSpooler
// ---------------------------------------------------------------------------

static void checkSpoolNames(Spool& spool)
{
    g_context = QStringLiteral("spool names");
    const auto stream = spool.item("kubectl");
    CHECK(SpoolDir::isStreamFile(stream->path()), "stream file is recognised: " + stream->path());
    // Копия архива `stream-api.log.gz` — не поток (память файла, сессии).
    const auto copy = spool.dir->createFile(QStringLiteral("stream-api.log"));
    CHECK(!SpoolDir::isStreamFile(copy->path()), "unpacked copy named stream-… is not a stream: " + copy->path());
}

static void checkEmitAndExit(Spool& spool)
{
    g_context = QStringLiteral("emit");
    CommandSpooler s(CommandSpec{selfCommand(QStringLiteral("--emit 200 3")), {}, false}, spool.item("emit"));
    qint64 lastNotified = -1;
    QObject::connect(&s, &CommandSpooler::bytesAppended, [&](qint64 total) { lastNotified = total; });
    CHECK(s.start(), "started");
    CHECK(s.isRunning(), "running right after start");
    CHECK(!s.start(), "second start while running is refused");
    CHECK(waitState(s, CommandSpooler::State::Finished), "finished");
    const QString path = s.spoolFilePath();
    CHECK(countLines(path, "emitted line") == 200, "all stdout lines are in the spool");
    CHECK(countLines(path, "stderr line") == 1, "stderr is merged into the same spool");
    const QByteArray bytes = readAll(path);
    CHECK(bytes.indexOf("emitted line 100\n") < bytes.indexOf("stderr line"), "order is kept");
    CHECK(s.exitCode() == 3 && !s.crashed() && !s.stoppedByUser(), "exit code");
    CHECK(s.statusText().contains(QStringLiteral("3")), "status names the exit code: " + s.statusText());
    CHECK(lastNotified == QFileInfo(path).size() && s.bytesWritten() == lastNotified,
          "last notification carries the whole file");

    // Перезапуск — дописывает.
    CHECK(s.start(), "restart");
    CHECK(waitState(s, CommandSpooler::State::Finished), "restart finished");
    CHECK(countLines(path, "emitted line") == 400 && s.runCount() == 2, "restart appends to the same spool");
}

static void checkFailures(Spool& spool)
{
    g_context = QStringLiteral("failures");
    {
        CommandSpooler s(CommandSpec{QStringLiteral("dendrolog-no-such-program-f7 --x"), {}, false},
                         spool.item("missing"));
        s.start();
        CHECK(waitState(s, CommandSpooler::State::FailedToStart), "missing program: FailedToStart");
        CHECK(s.statusText().startsWith(QStringLiteral("Could not start")), "status: " + s.statusText());
        CHECK(!s.isRunning(), "not running");
    }
    {
        CommandSpooler s(CommandSpec{selfCommand(QStringLiteral("--emit 1")),
                                     QStringLiteral("/no/such/dir/f7"), false},
                         spool.item("nodir"));
        CHECK(!s.start() && s.state() == CommandSpooler::State::FailedToStart, "missing working dir is refused");
    }
    {
        CommandSpooler s(CommandSpec{QStringLiteral("x \"y"), {}, false}, spool.item("quote"));
        CHECK(!s.start() && !s.errorString().isEmpty(), "unclosed quote is refused before start");
    }
}

static void checkForeverAndStop(Spool& spool, bool ignoreTerm)
{
    g_context = ignoreTerm ? QStringLiteral("stop: SIGTERM ignored") : QStringLiteral("stop");
    CommandSpooler s(CommandSpec{selfCommand(ignoreTerm ? QStringLiteral("--forever ignore-term")
                                                        : QStringLiteral("--forever")),
                                 {}, false},
                     spool.item("forever"));
    s.setGracePeriodMs(ignoreTerm ? 400 : 5000);
    CHECK(s.start(), "started");
    const QString path = s.spoolFilePath();
    CHECK(testdocs::waitFor([&] { return countLines(path, "forever line") >= 5; }), "lines arrive");
    const qint64 size1 = QFileInfo(path).size();
    CHECK(testdocs::waitFor([&] { return QFileInfo(path).size() > size1; }), "spool grows");
    const qint64 childPid = pidFromSpool(path);
    CHECK(childPid > 0 && childPid == s.processId(), "child PID");
    s.stop();
    CHECK(s.state() == CommandSpooler::State::Stopping, "stopping");
    CHECK(waitState(s, CommandSpooler::State::Finished), "stopped");
    CHECK(s.stoppedByUser() && s.statusText() == QStringLiteral("Stopped"), "status: " + s.statusText());
    CHECK(testdocs::waitFor([&] { return !processAlive(childPid); }, 10000), "process is gone");
    // Файл закрыт на выходе: всё, что спулер записал, — в файле, и больше
    // туда никто не пишет.
    CHECK(s.bytesWritten() == QFileInfo(path).size(), "spool ends where the spooler stopped writing");
}

static void checkShellGrandchild(Spool& spool, bool ignoreTerm)
{
    g_context = ignoreTerm ? QStringLiteral("shell grandchild, SIGTERM ignored") : QStringLiteral("shell grandchild");
    // `; echo` не даёт shell заменить себя программой (exec): внук настоящий.
    // Внук, глухой к SIGTERM, переживает лидера: его добивает SIGKILL группе.
    const QString args = ignoreTerm ? QStringLiteral("--forever ignore-term") : QStringLiteral("--forever");
#ifdef Q_OS_WIN
    const QString line = selfCommand(args) + QStringLiteral(" & echo done");
#else
    const QString line = selfCommand(args) + QStringLiteral("; echo done");
#endif
    CommandSpooler s(CommandSpec{line, {}, true}, spool.item("shell"));
    s.setGracePeriodMs(2000);
    CHECK(s.start(), "started");
    const QString path = s.spoolFilePath();
    CHECK(testdocs::waitFor([&] { return pidFromSpool(path) > 0; }), "grandchild prints its PID");
    const qint64 grandchild = pidFromSpool(path);
    CHECK(grandchild != s.processId(), "a real grandchild behind the shell");
    s.stop();
    CHECK(waitState(s, CommandSpooler::State::Finished), "stopped");
    CHECK(testdocs::waitFor([&] { return !processAlive(grandchild); }, 10000), "grandchild is gone too");
}

#ifndef Q_OS_WIN
// Лидер (shell) вышел сам, а его фоновый ребёнок жив и глух к SIGTERM:
// закрытие вкладки (release) добивает группу и только потом удаляет спулер.
static void checkReleaseLeftoverGroup(Spool& spool)
{
    g_context = QStringLiteral("release: group outlives the leader");
    // Фоновый ребёнок молчит (пишущего добил бы SIGPIPE закрытого канала):
    // PID — через файл.
    const QString pidFile = spool.dir->path() + QStringLiteral("/leftover.pid");
    const QString line = selfCommand(QStringLiteral("--quiet-forever \"%1\"").arg(pidFile))
        + QStringLiteral(" & echo started");
    auto* s = new CommandSpooler(CommandSpec{line, {}, true}, spool.item("leftover"));
    s->setGracePeriodMs(300);
    QPointer<CommandSpooler> guard(s);
    CHECK(s->start(), "started");
    CHECK(testdocs::waitFor([&] { return pidFromSpool(pidFile) > 0; }), "background child writes its PID");
    const qint64 child = pidFromSpool(pidFile);
    CHECK(waitState(*s, CommandSpooler::State::Finished), "the shell itself exits");
    CHECK(processAlive(child), "its background child lives on");
    s->release();
    CHECK(testdocs::waitFor([&] { return !processAlive(child); }, 10000), "release kills the rest of the group");
    CHECK(testdocs::waitFor([&] {
              QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
              return guard.isNull();
          }),
          "spooler goes after the group");
}
#endif

static void checkReleaseAndDestructor(Spool& spool)
{
    g_context = QStringLiteral("release");
    {
        auto* owner = new QObject;
        auto* s = new CommandSpooler(CommandSpec{selfCommand(QStringLiteral("--forever")), {}, false},
                                     spool.item("release"), owner);
        QPointer<CommandSpooler> guard(s);
        CHECK(s->start(), "started");
        const QString path = s->spoolFilePath();
        CHECK(testdocs::waitFor([&] { return pidFromSpool(path) > 0; }), "running");
        const qint64 pid = pidFromSpool(path);
        s->release();
        delete owner; // владелец ушёл — спулер живёт до выхода процесса
        CHECK(testdocs::waitFor([&] { return guard.isNull(); }), "spooler deletes itself after the process exits");
        CHECK(testdocs::waitFor([&] { return !processAlive(pid); }, 10000), "process is gone");
        CHECK(testdocs::waitFor([&] { return !QFileInfo::exists(path); }), "spool is removed");
    }
    g_context = QStringLiteral("destructor");
    {
        auto* s = new CommandSpooler(CommandSpec{selfCommand(QStringLiteral("--forever ignore-term")), {}, false},
                                     spool.item("dtor"));
        s->setGracePeriodMs(200);
        CHECK(s->start(), "started");
        const QString path = s->spoolFilePath();
        CHECK(testdocs::waitFor([&] { return pidFromSpool(path) > 0; }), "running");
        const qint64 pid = pidFromSpool(path);
        delete s;
        CHECK(testdocs::waitFor([&] { return !processAlive(pid); }, 5000), "destructor kills the process");
    }
}

// ---------------------------------------------------------------------------
// Вкладка
// ---------------------------------------------------------------------------

static CommandStatusBar* barOf(LogViewWidget& tab)
{
    return tab.findChild<CommandStatusBar*>(QStringLiteral("commandStatusBar"));
}

static bool tabIdle(LogViewWidget& tab)
{
    return testdocs::waitIdle([&] { return tab.isLoading() || tab.model()->isFiltering(); });
}

static void checkTab(const QString& root, const QString& schema, const QString& name)
{
    g_context = QStringLiteral("tab ") + name;
    std::fprintf(stderr, "-- %s\n", qPrintable(g_context));
    std::shared_ptr<SpoolDir> dir = SpoolDir::shared(root);
    CHECK(dir != nullptr, "spool dir");

    // ---- Конечная команда: строки, код возврата, строки остаются ---------------
    {
        auto tab = std::make_unique<LogViewWidget>();
        tab->setParserPattern(schema);
        auto* s = new CommandSpooler(CommandSpec{selfCommand(QStringLiteral("--emit 300 2")), {}, false},
                                     dir->createStreamFile(QStringLiteral("emit")));
        tab->addCommandOutput(s);
        CHECK(tab->commands() == QList<CommandSpooler*>({s}) && s->parent() == tab.get(), "tab owns the spooler");
        CHECK(tab->autoReload() && tab->view()->followTail(), "auto-reload and follow-tail are on");
        LogFilePtr lf = tab->loadedFiles().value(0);
        CHECK(lf && lf->source().kind == LogSource::Kind::Stream, "stream source");
        CHECK(lf && lf->displayPath() == s->spec().commandLine, "shown as the command line");
        CHECK(lf && lf->shortName() == QFileInfo(selfExe()).completeBaseName(), "tab name is the program");
        CHECK(waitState(*s, CommandSpooler::State::Finished), "finished");
        CHECK(testdocs::waitFor([&] { return tab->model()->rowCount() == 301; }),
              QStringLiteral("all lines are in the tab: %1").arg(tab->model()->rowCount()));
        CHECK(barOf(*tab) && barOf(*tab)->text().contains(QStringLiteral("code 2")),
              "bar shows the exit code: " + (barOf(*tab) ? barOf(*tab)->text() : QString()));
        auto* restart = tab->findChild<QPushButton*>(QStringLiteral("commandRestartButton"));
        CHECK(restart && restart->isVisibleTo(tab.get()) && restart->isEnabled(), "Restart offered after exit");
        // Снимок вкладки (сессия, память, Ctrl+Shift+T) команды не содержит:
        // без подтверждения пользователя ничего не запускается.
        const TabState state = tab->captureState();
        CHECK(state.sources.isEmpty(), "snapshot holds no command source");
        // Restart: строки дописываются.
        if (restart)
            restart->click();
        CHECK(s->runCount() == 2, "restarted");
        CHECK(waitState(*s, CommandSpooler::State::Finished), "restart finished");
        CHECK(testdocs::waitFor([&] { return tab->model()->rowCount() == 602; }),
              QStringLiteral("restart appends: %1").arg(tab->model()->rowCount()));
        const QString path = s->spoolFilePath();
        const QPointer<CommandSpooler> guard(s);
        lf.reset();
        tab.reset();
        // deleteLater снаружи цикла событий (закрытие вкладки в тесте) —
        // доставить явно; в программе вкладку закрывают из цикла.
        CHECK(testdocs::waitFor([&] {
                  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                  return guard.isNull();
              }),
              "spooler of an exited process goes with the tab");
        CHECK(testdocs::waitFor([&] { return !QFileInfo::exists(path); }), "spool removed with the tab");
    }

    // ---- Живая команда: рост, объединение, закрытие убивает --------------------
    {
        auto tab = std::make_unique<LogViewWidget>();
        tab->setParserPattern(schema);
        auto* s = new CommandSpooler(CommandSpec{selfCommand(QStringLiteral("--forever")), {}, false},
                                     dir->createStreamFile(QStringLiteral("forever")));
        tab->addCommandOutput(s);
        CHECK(testdocs::waitFor([&] { return tab->model()->rowCount() >= 10; }), "lines arrive while running");
        const int rows = tab->model()->rowCount();
        CHECK(testdocs::waitFor([&] { return tab->model()->rowCount() > rows + 5; }), "tab grows");
        CHECK(barOf(*tab) && barOf(*tab)->text().contains(QStringLiteral("Running")), "bar: running");
        const qint64 pid = s->processId();
        CHECK(processAlive(pid), "alive");

        // Объединение: процесс переходит в другую вкладку и переживает закрытие
        // прежней.
        auto other = std::make_unique<LogViewWidget>();
        other->setParserPattern(schema);
        other->addFilesFrom(*tab);
        other->adoptCommandsFrom(*tab);
        CHECK(tab->commands().isEmpty() && other->commands() == QList<CommandSpooler*>({s}),
              "merge moves the spooler");
        const QString path = s->spoolFilePath();
        tab.reset();
        QCoreApplication::processEvents();
        CHECK(processAlive(pid) && s->isRunning(), "process survives the merged-away tab");
        CHECK(barOf(*other) != nullptr, "bar moves with the process");
        CHECK(tabIdle(*other), "merged tab loads");
        const int merged = other->model()->rowCount();
        CHECK(testdocs::waitFor([&] { return other->model()->rowCount() > merged; }), "merged tab keeps growing");

        other.reset();
        CHECK(testdocs::waitFor([&] { return !processAlive(pid); }, 10000), "closing the tab stops the process");
        CHECK(testdocs::waitFor([&] { return !QFileInfo::exists(path); }), "spool removed");
    }
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::strncmp(argv[1], "--", 2) == 0 && std::strcmp(argv[1], "--bench") != 0)
        return runGenerator(argc, argv);

    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    if (!temporary.isValid())
        return 1;
    const QString root = QDir(temporary.path()).filePath(QStringLiteral("spool"));

    checkSpec();
    {
        Spool spool(root);
        CHECK(spool.dir != nullptr, "spool dir");
        if (spool.dir) {
            checkSpoolNames(spool);
            checkEmitAndExit(spool);
            checkFailures(spool);
            checkForeverAndStop(spool, false);
            checkForeverAndStop(spool, true);
            checkShellGrandchild(spool, false);
            checkShellGrandchild(spool, true);
#ifndef Q_OS_WIN
            checkReleaseLeftoverGroup(spool);
#endif
            checkReleaseAndDestructor(spool);
        }
    }

    const QString schema = testdocs::buildSchema();
    AppSettings& settings = AppSettings::instance();
    checkTab(root, schema, QStringLiteral("resident"));
    settings.setIndexedThresholdMB(0);
    checkTab(root, schema, QStringLiteral("indexed"));
    settings.setIndexedThresholdMB(512);
    // Удаление спулов — в глобальном пуле (SpoolItem): дождаться до уборки
    // временного каталога.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QThreadPool::globalInstance()->waitForDone();

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("command_output: all checks passed\n");
    return 0;
}
