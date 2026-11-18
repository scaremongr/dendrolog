#include "mainwindow.h"
#include "appsettings.h"
#include "colorschemecontroller.h"
#include "compactstyle.h"
#include "singleinstance.h"
#include <QApplication>
#include "openrouting.h"
#include <QElapsedTimer>
#include <QIcon>
#include <QStyle>
#include <QStyleFactory>
#include <QThread>

#include <cstdio>

// ---------------------------------------------------------------------------
// Env-gated атрибуция фризов GUI: DENDRO_SLOW_EVENTS=<файл> — каждая доставка
// события на GUI-потоке дольше 100 мс пишется строкой «ms;класс;тип события».
// Меряется только внешняя доставка (вложенные sendEvent входят во время
// родителя) — атрибуция указывает на конкретного получателя-блокировщика.
// В обычных запусках недостижимо (env не задан — нулевые накладные расходы).
// ---------------------------------------------------------------------------
class TracingApplication : public QApplication {
public:
    using QApplication::QApplication;
    FILE* slowEventLog = nullptr;

    bool notify(QObject* receiver, QEvent* event) override
    {
        if (!slowEventLog || QThread::currentThread() != thread())
            return QApplication::notify(receiver, event);
        static thread_local int depth = 0;
        if (depth > 0)
            return QApplication::notify(receiver, event);
        ++depth;
        QElapsedTimer t;
        t.start();
        const bool res = QApplication::notify(receiver, event);
        const qint64 ms = t.elapsed();
        --depth;
        if (ms >= 100) {
            std::fprintf(slowEventLog, "%lld;%s;%d\n", static_cast<long long>(ms),
                         receiver->metaObject()->className(), int(event->type()));
            std::fflush(slowEventLog);
        }
        return res;
    }
};

int main(int argc, char *argv[])
{
    TracingApplication a(argc, argv);
    if (const QByteArray slowLog = qgetenv("DENDRO_SLOW_EVENTS"); !slowLog.isEmpty())
        a.slowEventLog = std::fopen(slowLog.constData(), "w");

    QApplication::setOrganizationName(QStringLiteral("DendroLog"));
    QApplication::setApplicationName(QStringLiteral("DendroLog"));
    QApplication::setApplicationVersion(QStringLiteral(DENDROLOG_VERSION));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/dendrolog.svg")));

    // Разбираем аргументы: файлы логов + служебный флаг --new-instance,
    // позволяющий принудительно открыть отдельное окно, и «-»/«--stdin» —
    // чтение потокового ввода (program | DendroLog -).
    QStringList cliFiles = a.arguments().mid(1);
    const bool forceNewInstance = cliFiles.removeAll(QStringLiteral("--new-instance")) > 0;
    const bool readStdin = (cliFiles.removeAll(QStringLiteral("-"))
                            + cliFiles.removeAll(QStringLiteral("--stdin"))) > 0;

    // Один экземпляр на пользователя: повторный запуск (например, двойной клик
    // по ассоциированному .log) передаёт файлы в уже открытое окно и выходит.
    // Пайп stdin передать нельзя — при «-» всегда открываем своё окно.
    SingleInstance guard(QStringLiteral("DendroLog"));
    if (!forceNewInstance && !readStdin && guard.isSecondary()) {
        // Пути разрешаем здесь: у первичного экземпляра другой текущий каталог.
        if (guard.sendToPrimary(OpenRouting::absolutePaths(cliFiles)))
            return 0;
        // Первичный экземпляр не ответил (завис/умер) — продолжаем как обычно.
    }

    // Fusion honours the QPalette consistently on every platform, unlike the
    // native Windows styles which (on Windows 10) ignore the dark colour scheme.
    // CompactStyle keeps Fusion's look but with one dense set of margins and
    // spacings for the whole window (see compactstyle.h).
    QApplication::setStyle(new CompactStyle(QStyleFactory::create(QStringLiteral("Fusion"))));

    // Тема окна: «как в системе» (следит за сменой темы ОС) либо выбранная в
    // Settings → Appearance. Настройки читаются здесь, раньше окна: иначе оно
    // успело бы показаться не в той теме.
    AppSettings::instance().load();
    ColorSchemeController colorScheme;

    MainWindow w;
    w.show();

    // Файлы от вторичных экземпляров: открыть и поднять окно поверх остальных.
    QObject::connect(&guard, &SingleInstance::filesReceived,
                     &w, [&w](const QStringList& files) {
        if (!files.isEmpty())
            w.openFilesFromCommandLine(files);
        w.setWindowState((w.windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
        w.raise();
        w.activateWindow();
    });

    // Пути из командной строки: файлы открываются как обычные, папка — в
    // Directory Scanner.
    if (!cliFiles.isEmpty())
        w.openFilesFromCommandLine(cliFiles);

    // Потоковый ввод: stdin спулится во временный файл и открывается
    // вкладкой-растущим логом (auto-reload + follow-tail).
    if (readStdin)
        w.openStdinStream();

    return a.exec();
}
