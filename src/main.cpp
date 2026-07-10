#include "mainwindow.h"
#include "singleinstance.h"
#include <QApplication>
#include <QIcon>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QPalette>
#include <QColor>

// ---------------------------------------------------------------------------
// Windows (especially Windows 10) does not propagate the system dark theme to
// the classic native widget style, so the application window stayed light even
// when the OS was set to dark mode. To make the appearance follow the OS on all
// Windows versions we switch to the Fusion style — which fully honours the
// QPalette — and feed it a light or dark palette derived from the system's
// reported colour scheme (Qt::ColorScheme, available since Qt 6.5).
// ---------------------------------------------------------------------------
static QPalette buildDarkPalette()
{
    QPalette p;
    const QColor window(0x2d, 0x2d, 0x2d);
    const QColor base(0x1e, 0x1e, 0x1e);
    const QColor alternateBase(0x2a, 0x2a, 0x2a);
    const QColor text(0xdd, 0xdd, 0xdd);
    const QColor button(0x35, 0x35, 0x35);
    const QColor highlight(0x2a, 0x7a, 0xda);
    const QColor disabled(0x7f, 0x7f, 0x7f);

    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, alternateBase);
    p.setColor(QPalette::ToolTipBase, window);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, button);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::red);
    p.setColor(QPalette::Link, highlight);
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::PlaceholderText, disabled);

    p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    p.setColor(QPalette::Disabled, QPalette::Text, disabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(0x45, 0x45, 0x45));
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);
    return p;
}

static void applyColorScheme(Qt::ColorScheme scheme)
{
    if (scheme == Qt::ColorScheme::Dark)
        qApp->setPalette(buildDarkPalette());
    else if (QStyle* style = qApp->style())
        qApp->setPalette(style->standardPalette());
}

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    QApplication::setOrganizationName(QStringLiteral("DendroLog"));
    QApplication::setApplicationName(QStringLiteral("DendroLog"));
    QApplication::setApplicationVersion(QStringLiteral(DENDROLOG_VERSION));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/dendrolog.svg")));

    // Разбираем аргументы: файлы логов + служебный флаг --new-instance,
    // позволяющий принудительно открыть отдельное окно.
    QStringList cliFiles = a.arguments().mid(1);
    const bool forceNewInstance = cliFiles.removeAll(QStringLiteral("--new-instance")) > 0;

    // Один экземпляр на пользователя: повторный запуск (например, двойной клик
    // по ассоциированному .log) передаёт файлы в уже открытое окно и выходит.
    SingleInstance guard(QStringLiteral("DendroLog"));
    if (!forceNewInstance && guard.isSecondary()) {
        if (guard.sendToPrimary(cliFiles))
            return 0;
        // Первичный экземпляр не ответил (завис/умер) — продолжаем как обычно.
    }

    // Fusion honours the QPalette consistently on every platform, unlike the
    // native Windows styles which (on Windows 10) ignore the dark colour scheme.
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    applyColorScheme(QGuiApplication::styleHints()->colorScheme());

    // Follow the OS theme live if the user toggles dark/light mode while the
    // application is running.
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                     &a, [](Qt::ColorScheme scheme) { applyColorScheme(scheme); });

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

    // Файлы, переданные аргументами командной строки, открываются как обычные.
    if (!cliFiles.isEmpty())
        w.openFilesFromCommandLine(cliFiles);

    return a.exec();
}
