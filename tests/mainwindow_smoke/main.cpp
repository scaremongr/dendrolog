// ============================================================================
// mainwindow_smoke — окно целиком под тестом (P-8).
//
// Логика возможностей покрыта тестами контроллеров и вкладки; здесь — склейка
// окна: меню, действия, сочетания клавиш, доки, сигналы между ними. Тест
// создаёт MainWindow без дисплея (offscreen) с временным каталогом настроек и
// без сети и проходит сквозные сценарии через то, что видит пользователь:
// QAction::trigger, сочетания (QTest::keyClick в активное окно), кнопки и
// ссылки, бросок файлов, второй экземпляр через настоящий SingleInstance.
//
// Модальные диалоги закрывает DialogDriver: таймер внутри вложенного цикла
// exec() находит активный модальный виджет или всплывающее меню и либо
// выполняет заранее поставленный обработчик (ввести строку в Go to, выбрать
// файл в диалоге сессии, пункт контекстного меню вкладки), либо закрывает
// его (reject) и записывает заголовок — список печатается в конце.
//
// Сценарии с файлами — на обоих бэкендах хранилища (порог индексного через
// AppSettings::setIndexedThresholdMB). Ждём состояние (waitFor), а не время;
// пиксели не сравниваются.
// ============================================================================

#include "appsettings.h"
#include "bookmarkstore.h"
#include "compactstyle.h"
#include "directoryscanner.h"
#include "encodingindicator.h"
#include "filterpanelwidget.h"
#include "logmodel.h"
#include "logtabwidget.h"
#include "logviewwidget.h"
#include "mainwindow.h"
#include "viewexport.h"
#include "openrouting.h"
#include "schemastore.h"
#include "sessionreportdialog.h"
#include "shortcutmanager.h"
#include "singleinstance.h"
#include "testdocuments.h"
#include "updatechecker.h"

#include <QApplication>
#include <QDockWidget>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QShortcut>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyleFactory>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <QToolBar>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QTextBrowser>
#include "timelinehistogramwidget.h"

#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const QString& what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", qPrintable(what));
    }
    std::fflush(stderr);
}

QElapsedTimer g_clock;

void section(const QString& name)
{
    if (!g_clock.isValid())
        g_clock.start();
    std::fprintf(stderr, "--- [%6.1f s] %s\n", g_clock.elapsed() / 1000.0, qPrintable(name));
    std::fflush(stderr);
}

bool waitFor(const std::function<bool()>& ready, int timeoutMs = 30000)
{
    return testdocs::waitFor(ready, timeoutMs);
}

// ---------------------------------------------------------------------------
// DialogDriver: модальные диалоги и всплывающие меню внутри exec().
// ---------------------------------------------------------------------------
class DialogDriver : public QObject {
public:
    using Handler = std::function<void(QWidget*)>;

    DialogDriver()
    {
        m_timer.setInterval(10);
        connect(&m_timer, &QTimer::timeout, this, [this]() { tick(); });
        m_timer.start();
    }

    // Следующий модальный виджет или меню получит этот обработчик.
    void expect(Handler handler) { m_handlers.push_back(std::move(handler)); }
    bool idle() const { return m_handlers.empty(); }
    void dropExpectations() { m_handlers.clear(); }

    const QStringList& closedByDefault() const { return m_closed; }

private:
    // Обработчик идёт отдельным singleShot, а не внутри тика: диалог,
    // открытый из обработчика (подтверждение, ошибка), снова поймает тик —
    // Qt не входит повторно в слот того же таймера.
    void tick()
    {
        QWidget* w = QApplication::activePopupWidget();
        if (!w)
            w = QApplication::activeModalWidget();
        if (!w || !w->isVisible() || m_scheduled.contains(w))
            return;
        m_scheduled.insert(w);
        Handler h;
        if (!m_handlers.empty()) {
            h = std::move(m_handlers.front());
            m_handlers.pop_front();
        } else {
            const QString title = qobject_cast<QMenu*>(w)
                ? QStringLiteral("menu") : w->windowTitle();
            if (!m_closed.contains(title))
                m_closed << title;
            h = [](QWidget* widget) { close(widget); };
        }
        QPointer<QWidget> guard(w);
        QTimer::singleShot(0, this, [this, guard, w, h]() {
            m_scheduled.remove(w);
            if (guard)
                h(guard);
        });
    }

public:
    static void close(QWidget* w)
    {
        if (auto* dialog = qobject_cast<QDialog*>(w))
            dialog->reject();
        else
            w->close();
    }

private:
    QTimer m_timer;
    std::deque<Handler> m_handlers;
    QStringList m_closed;
    QSet<QWidget*> m_scheduled;
};

DialogDriver* g_driver = nullptr;

// ---------------------------------------------------------------------------
// Доступ к окну
// ---------------------------------------------------------------------------
LogTabWidget* tabs(MainWindow& win)
{
    return win.findChild<LogTabWidget*>(QStringLiteral("tabWidget"));
}

LogViewWidget* currentTab(MainWindow& win)
{
    return qobject_cast<LogViewWidget*>(tabs(win)->currentWidget());
}

LogViewWidget* tabAt(MainWindow& win, int i)
{
    return qobject_cast<LogViewWidget*>(tabs(win)->widget(i));
}

// Действие по имени команды ShortcutManager (свойство commandId).
QAction* command(MainWindow& win, const QString& id)
{
    for (QAction* a : win.findChildren<QAction*>())
        if (a->property("commandId").toString() == id)
            return a;
    check(false, QStringLiteral("no action for command %1").arg(id));
    return nullptr;
}

// Пункт меню окна по тексту (без «&» и многоточий).
QString plainText(const QString& text)
{
    QString t = text;
    t.remove(QLatin1Char('&'));
    t.remove(QStringLiteral("..."));
    t.remove(QChar(0x2026));
    return t.trimmed();
}

void collectMenuActions(QMenu* menu, QList<QAction*>* out)
{
    for (QAction* a : menu->actions()) {
        if (a->isSeparator())
            continue;
        if (a->menu())
            collectMenuActions(a->menu(), out);
        else
            out->append(a);
    }
}

QList<QAction*> menuActions(MainWindow& win)
{
    QList<QAction*> out;
    for (QAction* top : win.menuBar()->actions())
        if (top->menu())
            collectMenuActions(top->menu(), &out);
    return out;
}

QAction* menuAction(MainWindow& win, const QString& text)
{
    for (QAction* a : menuActions(win))
        if (plainText(a->text()) == text)
            return a;
    check(false, QStringLiteral("no menu item '%1'").arg(text));
    return nullptr;
}

QMenu* recentFilesMenu(MainWindow& win)
{
    return win.findChild<QMenu*>(QStringLiteral("menuRecentFiles"));
}

// Сочетание — в виджет с фокусом активного окна, как с клавиатуры.
void press(MainWindow& win, const QKeySequence& seq)
{
    if (seq.isEmpty()) {
        check(false, QStringLiteral("empty key sequence"));
        return;
    }
    // Сочетания окна действуют в активном окне; закрытый диалог (отчёт
    // сессии) возвращает активность не сразу.
    // (qWaitForWindowActive не годится: окно считается активным и тогда,
    // когда активен его дочерний диалог.)
    if (QApplication::activeWindow() != &win) {
        win.activateWindow();
        check(waitFor([&]() { return QApplication::activeWindow() == &win; }, 5000),
              QStringLiteral("window active for a shortcut"));
    }
    // Фокус в закрытом немодальном диалоге (offscreen не возвращает его
    // окну сам) — как щелчок в окно: фокус виду вкладки либо окну.
    if (!QApplication::focusWidget() || QApplication::focusWidget()->window() != &win) {
        auto* tab = tabs(win)->count() ? currentTab(win) : nullptr;
        if (tab)
            tab->view()->setFocus(Qt::MouseFocusReason);
        else
            win.setFocus(Qt::MouseFocusReason);
    }
    QWidget* target = QApplication::focusWidget();
    if (!target || target->window() != &win)
        target = &win;
    const QKeyCombination k = seq[0];
    QTest::keyClick(target, k.key(), k.keyboardModifiers());
}

void pressCommand(MainWindow& win, const QString& id)
{
    if (QAction* a = command(win, id))
        press(win, a->shortcut());
}

bool waitLoaded(LogViewWidget* tab, int minRows = 1)
{
    if (!tab)
        return false;
    if (!waitFor([&]() { return !tab->isLoading() && tab->model()->rowCount() >= minRows; }, 60000))
        return false;
    return testdocs::settle(*tab->model());
}

// Вкладка с пропавшим файлом (отчёт сессии) пуста — ждём только загрузку.
bool waitAllLoaded(MainWindow& win)
{
    for (int i = 0; i < tabs(win)->count(); ++i)
        if (!waitLoaded(tabAt(win, i), 0))
            return false;
    return true;
}

int tabIndexForFile(MainWindow& win, const QString& path)
{
    for (int i = 0; i < tabs(win)->count(); ++i) {
        LogViewWidget* t = tabAt(win, i);
        for (const LogFilePtr& f : t->loadedFiles())
            if (LogViewWidget::samePath(f->filePath, path))
                return i;
    }
    return -1;
}

std::shared_ptr<LogEntry> selectedEntry(LogViewWidget* tab)
{
    const int row = tab->view()->userSelectedRow();
    return row >= 0 ? tab->model()->entryAt(row) : nullptr;
}

QString describeTabs(MainWindow& win)
{
    QStringList out;
    for (int i = 0; i < tabs(win)->count(); ++i) {
        LogViewWidget* t = tabAt(win, i);
        QStringList files;
        for (const LogFilePtr& f : t->loadedFiles())
            files << QFileInfo(f->filePath).fileName();
        QStringList marks;
        for (const Bookmark& b : t->bookmarks().bookmarks())
            marks << QStringLiteral("%1:%2").arg(QFileInfo(b.filePath).fileName()).arg(b.line);
        out << QStringLiteral("[%1 | %2]").arg(files.join(','), marks.join(','));
    }
    return out.join(' ');
}

// Закладки вкладок по порядку: «файл:строка».
QVector<QSet<QString>> tabBookmarks(MainWindow& win)
{
    QVector<QSet<QString>> out;
    for (int i = 0; i < tabs(win)->count(); ++i) {
        QSet<QString> marks;
        for (const Bookmark& b : tabAt(win, i)->bookmarks().bookmarks())
            marks.insert(QStringLiteral("%1:%2").arg(QFileInfo(b.filePath).fileName()).arg(b.line));
        out << marks;
    }
    return out;
}

QString statusText(MainWindow& win)
{
    // Сообщение строки состояния либо её основная подпись.
    QString text = win.statusBar()->currentMessage();
    for (QLabel* l : win.statusBar()->findChildren<QLabel*>())
        text += QLatin1Char('|') + l->text();
    return text;
}

void dropPaths(MainWindow& win, const QStringList& paths)
{
    QMimeData mime;
    QList<QUrl> urls;
    for (const QString& p : paths)
        urls << QUrl::fromLocalFile(p);
    mime.setUrls(urls);
    const QPointF pos(win.width() / 2.0, win.height() / 2.0);
    QDragEnterEvent enter(pos.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&win, &enter);
    check(enter.isAccepted(), QStringLiteral("drag enter accepted for %1").arg(paths.join(',')));
    QDropEvent drop(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&win, &drop);
    check(drop.isAccepted(), QStringLiteral("drop accepted for %1").arg(paths.join(',')));
}

// ---------------------------------------------------------------------------
// Сочетания клавиш: два действия (или QShortcut) с одной клавишей в одном
// контексте — неоднозначность, Qt не выполнит ни одно. Ловит класс ошибок,
// который юнит-тесты ShortcutManager не видят: действия, созданные мимо
// менеджера, повторно назначенные стандартные клавиши, виджетные сочетания
// под оконными.
// ---------------------------------------------------------------------------
struct ShortcutOwner {
    QString name;
    Qt::ShortcutContext context;
    QList<QWidget*> widgets; // где действует (для Widget*-контекстов)
};

QString describeAction(QAction* a)
{
    QString name = a->property("commandId").toString();
    if (name.isEmpty())
        name = a->objectName();
    if (name.isEmpty())
        name = plainText(a->text());
    return name;
}

// Окно, в котором действует виджет сочетания (у пунктов меню — окно
// строки меню, как у Qt).
QWidget* shortcutWindow(QWidget* w)
{
    while (w) {
        if (auto* menu = qobject_cast<QMenu*>(w)) {
            const QList<QObject*> owners = menu->menuAction()->associatedObjects();
            QWidget* next = nullptr;
            for (QObject* o : owners)
                if (auto* ow = qobject_cast<QWidget*>(o); ow && ow != menu) {
                    next = ow;
                    break;
                }
            if (!next)
                return nullptr;
            w = next;
            continue;
        }
        return w->window();
    }
    return nullptr;
}

void checkShortcutAmbiguity(MainWindow& win, const QString& stage)
{
    // Ключ — сочетание; значение — владельцы.
    std::map<QString, QList<ShortcutOwner>> byKey;
    int total = 0;

    QSet<QAction*> actions;
    for (QAction* a : win.findChildren<QAction*>())
        actions.insert(a);
    for (QWidget* w : win.findChildren<QWidget*>())
        for (QAction* a : w->actions())
            actions.insert(a);
    for (QAction* a : win.actions())
        actions.insert(a);

    for (QAction* a : std::as_const(actions)) {
        if (!a->isEnabled() || a->shortcuts().isEmpty())
            continue;
        ShortcutOwner owner{describeAction(a), a->shortcutContext(), {}};
        bool inWindow = false;
        for (QObject* o : a->associatedObjects()) {
            auto* w = qobject_cast<QWidget*>(o);
            if (!w)
                continue;
            if (shortcutWindow(w) == &win) {
                inWindow = true;
                owner.widgets << w;
            }
        }
        if (!inWindow)
            continue; // действует в другом окне (диалог) либо нигде
        for (const QKeySequence& seq : a->shortcuts()) {
            byKey[seq.toString(QKeySequence::PortableText)].append(owner);
            ++total;
        }
    }
    for (QShortcut* s : win.findChildren<QShortcut*>()) {
        if (!s->isEnabled())
            continue;
        auto* parentWidget = qobject_cast<QWidget*>(s->parent());
        if (!parentWidget || parentWidget->window() != &win)
            continue;
        for (const QKeySequence& seq : s->keys()) {
            byKey[seq.toString(QKeySequence::PortableText)].append(
                {QStringLiteral("QShortcut(%1)").arg(s->objectName()), s->context(), {parentWidget}});
            ++total;
        }
    }

    // Мнемоники строки меню (&File → Alt+F) — тоже сочетания окна.
    for (QAction* top : win.menuBar()->actions()) {
        const QKeySequence mnemonic = QKeySequence::mnemonic(top->text());
        if (mnemonic.isEmpty())
            continue;
        byKey[mnemonic.toString(QKeySequence::PortableText)].append(
            {QStringLiteral("menu %1").arg(plainText(top->text())), Qt::WindowShortcut, {win.menuBar()}});
        ++total;
    }

    // Внутри окна: два оконных — конфликт всегда; виджетное с оконным —
    // конфликт, пока фокус в его виджете (Qt не отдаёт приоритет ни одному);
    // два виджетных — если делят виджет.
    const auto windowWide = [](const ShortcutOwner& o) {
        return o.context == Qt::WindowShortcut || o.context == Qt::ApplicationShortcut;
    };
    for (const auto& [key, owners] : byKey) {
        for (int i = 0; i < owners.size(); ++i) {
            for (int j = i + 1; j < owners.size(); ++j) {
                const ShortcutOwner& a = owners[i];
                const ShortcutOwner& b = owners[j];
                bool clash = windowWide(a) || windowWide(b);
                if (!clash)
                    for (QWidget* w : a.widgets)
                        clash = clash || b.widgets.contains(w);
                check(!clash, QStringLiteral("[%1] ambiguous shortcut %2: %3 and %4")
                                  .arg(stage, key, a.name, b.name));
            }
        }
    }
    check(total > 20, QStringLiteral("[%1] shortcut scan saw %2 shortcuts").arg(stage).arg(total));
}

// Каждое действие с сочетанием, заведённое в ShortcutManager, должно где-то
// действовать: действие без виджета (не в меню, не в окне) молча не
// срабатывает.
void checkShortcutsReachable(MainWindow& win)
{
    for (QAction* a : win.findChildren<QAction*>()) {
        if (a->property("commandId").toString().isEmpty() || a->shortcut().isEmpty())
            continue;
        bool reachable = false;
        for (QObject* o : a->associatedObjects())
            if (auto* w = qobject_cast<QWidget*>(o); w && shortcutWindow(w) == &win)
                reachable = true;
        check(reachable, QStringLiteral("shortcut of %1 (%2) is not attached to the window")
                             .arg(describeAction(a), a->shortcut().toString()));
    }
}

// Каждое сочетание из ShortcutManager срабатывает с клавиатуры — в том
// числе когда пользователь скрыл тулбары (контекстное меню окна): действие,
// живущее только в тулбаре, вместе с ним теряет и сочетание.
void setToolBarsVisible(MainWindow& win, bool visible);

// hideToolBars — перед каждым нажатием скрыть тулбары снова (команды поиска
// показывают скрытый тулбар Find).
void checkEveryShortcutFires(MainWindow& win, const QString& stage, bool hideToolBars = false)
{
    // Выход и закрытие вкладки меняют сценарий — их проверяют отдельно.
    const QStringList skip { QStringLiteral("quit"), QStringLiteral("closeTab") };
    int fired = 0;
    QList<QAction*> actions;
    for (QAction* a : win.findChildren<QAction*>())
        if (!a->property("commandId").toString().isEmpty() && !a->shortcut().isEmpty())
            actions << a;
    for (QAction* a : std::as_const(actions)) {
        const QString id = a->property("commandId").toString();
        if (skip.contains(id) || !a->isEnabled())
            continue;
        if (auto* tab = tabs(win)->count() ? currentTab(win) : nullptr)
            tab->view()->setFocus(Qt::MouseFocusReason);
        if (hideToolBars)
            setToolBarsVisible(win, false);
        QSignalSpy spy(a, &QAction::triggered);
        press(win, a->shortcut());
        check(waitFor([&]() { return spy.count() > 0; }, 3000),
              QStringLiteral("[%1] %2 fires from its shortcut %3").arg(stage, id, a->shortcut().toString()));
        waitFor([]() { return !QApplication::activeModalWidget() && !QApplication::activePopupWidget(); }, 5000);
        // Тогл — вернуть как было (он мог и выключиться, если сочетание
        // не дошло, — тогда не трогаем).
        if (spy.count() > 0 && a->isCheckable() && a->isEnabled())
            a->trigger();
        if (spy.count() > 0)
            ++fired;
        testdocs::pump(10);
    }
    check(fired > 20, QStringLiteral("[%1] %2 shortcuts fired").arg(stage).arg(fired));
}

void setToolBarsVisible(MainWindow& win, bool visible)
{
    for (QToolBar* tb : win.findChildren<QToolBar*>())
        if (tb->parentWidget() == &win && tb->isVisible() != visible)
            tb->toggleViewAction()->trigger(); // как пункт контекстного меню окна
}

// ---------------------------------------------------------------------------
// Фикстуры
// ---------------------------------------------------------------------------
struct Fixtures {
    QString a, b, c;      // два обычных лога и лог в Windows-1251
    QString gone;         // файл сессии, который потом исчезнет
    QString dir;          // папка для Directory Scanner
    QString sessionPath;  // куда сохраняется сессия
};

Fixtures makeFixtures(const QDir& root)
{
    Fixtures f;
    const QDateTime base = testdocs::fixtureBaseTime();
    f.a = testdocs::writeFile(root, QStringLiteral("alpha.log"),
        testdocs::makeLogBytes(600, 11, "\n", base, false, true, true));
    f.b = testdocs::writeFile(root, QStringLiteral("beta.log"),
        testdocs::makeLogBytes(400, 22, "\n", base.addMSecs(250), false, true, true));
    f.c = testdocs::writeFile(root, QStringLiteral("cyr1251.log"),
        testdocs::encodeText(testdocs::cyrillicLogText(5), TextEncoding::Windows1251));
    f.gone = testdocs::writeFile(root, QStringLiteral("gone.log"),
        testdocs::makeLogBytes(50, 33, "\n", base, false, true, true));
    root.mkpath(QStringLiteral("scan"));
    QDir scan(root.filePath(QStringLiteral("scan")));
    testdocs::writeFile(scan, QStringLiteral("one.log"),
        testdocs::makeLogBytes(20, 44, "\n", base, false, true, true));
    testdocs::writeFile(scan, QStringLiteral("two.log"),
        testdocs::makeLogBytes(20, 55, "\n", base, false, true, true));
    f.dir = scan.absolutePath();
    f.sessionPath = root.filePath(QStringLiteral("saved.dlsession"));
    return f;
}

// Диалог файла: путь — в поле имени, как набрал бы пользователь, и «OK».
// (selectFile зависит от того, успела ли модель каталога перейти в папку.)
void typeFileName(QFileDialog* dialog, const QString& path)
{
    auto* edit = dialog->findChild<QLineEdit*>(QStringLiteral("fileNameEdit"));
    check(edit, QStringLiteral("file dialog has a file name field"));
    if (edit)
        edit->setText(QDir::toNativeSeparators(path));
    QMetaObject::invokeMethod(dialog, "accept");
}

void showWindow(MainWindow& win)
{
    win.resize(1200, 800);
    win.show();
    win.activateWindow();
    check(QTest::qWaitForWindowActive(&win, 5000), QStringLiteral("window becomes active"));
}

// ---------------------------------------------------------------------------
// Сценарии
// ---------------------------------------------------------------------------

// Открытие: командная строка, второй экземпляр, бросок файла и папки.
void scenarioOpen(MainWindow& win, const Fixtures& fx, bool indexed, const QString& instanceKey)
{
    section(QStringLiteral("open: command line"));
    win.openCommandLine(OpenRouting::parseCommandLine({fx.a}));
    check(waitFor([&]() { return tabs(win)->count() == 1; }), QStringLiteral("command line opens a tab"));
    check(waitLoaded(currentTab(win)), QStringLiteral("command-line tab loads"));
    check(currentTab(win)->model()->isIndexedBackend() == indexed,
          QStringLiteral("backend of the tab is %1").arg(indexed ? "indexed" : "resident"));

    section(QStringLiteral("open: second instance"));
    {
        SingleInstance primary(instanceKey, OpenRouting::parseCommandLine({}));
        check(primary.isPrimary(), QStringLiteral("first SingleInstance is primary"));
        win.attachSingleInstance(&primary);
        const OpenRouting::CommandLine second = OpenRouting::parseCommandLine({fx.b});
        SingleInstance secondary(instanceKey, second);
        check(secondary.role() == SingleInstance::Role::Forward,
              QStringLiteral("second SingleInstance forwards"));
        const QStringList forwarded = OpenRouting::forwardedArguments(second);
        // Отправка блокирует поток — в отдельном, пока GUI-поток крутит события.
        QFuture<bool> sent = QtConcurrent::run([&secondary, forwarded]() {
            return secondary.sendToPrimary(forwarded);
        });
        check(waitFor([&]() { return sent.isFinished(); }), QStringLiteral("second instance send finishes"));
        check(sent.result(), QStringLiteral("second instance delivers its files"));
        check(waitFor([&]() { return tabIndexForFile(win, fx.b) >= 0; }),
              QStringLiteral("file from the second instance opens"));
        check(tabs(win)->count() == 2, QStringLiteral("second instance file opens in its own tab"));
    }
    check(waitAllLoaded(win), QStringLiteral("tabs load"));

    section(QStringLiteral("open: drop"));
    dropPaths(win, {fx.c});
    check(waitFor([&]() { return tabIndexForFile(win, fx.c) >= 0; }), QStringLiteral("dropped file opens"));
    check(tabs(win)->count() == 3, QStringLiteral("dropped file opens a tab"));
    check(waitAllLoaded(win), QStringLiteral("dropped tab loads"));

    auto* scannerDock = win.findChild<QDockWidget*>(QStringLiteral("directoryScannerDockWidget"));
    auto* scanner = win.findChild<DirectoryScanner*>();
    dropPaths(win, {fx.dir});
    check(waitFor([&]() { return scannerDock->isVisible(); }), QStringLiteral("dropped folder shows the scanner"));
    check(scanner && QDir(scanner->rootPath()) == QDir(fx.dir),
          QStringLiteral("dropped folder is scanned: %1").arg(scanner ? scanner->rootPath() : QString()));
    check(tabs(win)->count() == 3, QStringLiteral("dropped folder opens no tab"));
    scannerDock->toggleViewAction()->trigger();
    check(!scannerDock->isVisible(), QStringLiteral("scanner dock hides by its action"));
}

// Фильтры уровней и текста, быстрый поиск, панель Search, Go to, история,
// F8, закладки — на вкладке alpha.
void scenarioNavigate(MainWindow& win, const Fixtures& fx)
{
    tabs(win)->setCurrentIndex(tabIndexForFile(win, fx.a));
    LogViewWidget* tab = currentTab(win);
    LogModel* model = tab->model();
    const int allRows = model->rowCount();
    tab->view()->setFocus();

    section(QStringLiteral("level filter"));
    pressCommand(win, QStringLiteral("levelError"));
    check(testdocs::settle(*model), QStringLiteral("level filter settles"));
    check(model->logLevelFilter().contains(LogLevel::Error), QStringLiteral("Alt+2 sets the Error level filter"));
    check(command(win, QStringLiteral("levelError"))->isChecked(), QStringLiteral("Error level button is checked"));
    check(model->rowCount() > 0 && model->rowCount() < allRows,
          QStringLiteral("level filter hides rows (%1 of %2)").arg(model->rowCount()).arg(allRows));
    pressCommand(win, QStringLiteral("levelAll"));
    check(testdocs::settle(*model), QStringLiteral("show all settles"));
    check(model->logLevelFilter().isEmpty() && model->rowCount() == allRows, QStringLiteral("Alt+0 shows all levels"));
    check(!command(win, QStringLiteral("levelError"))->isChecked(), QStringLiteral("Error level button unchecked"));

    section(QStringLiteral("text filter"));
    auto* textDock = win.findChild<QDockWidget*>(QStringLiteral("textFilterDockWidget"));
    pressCommand(win, QStringLiteral("panelTextFilters"));
    check(waitFor([&]() { return textDock->isVisible(); }), QStringLiteral("Ctrl+F1 shows Text Filters"));
    FilterPanelWidget* filterPanel = textDock->findChild<FilterPanelWidget*>();
    FilterRuleSet rules;
    FilterRule rule;
    rule.text = QStringLiteral("timeout");
    rules.rules << rule;
    filterPanel->setRuleSet(rules);
    QAbstractButton* apply = nullptr;
    for (QAbstractButton* b : filterPanel->findChildren<QAbstractButton*>())
        if (b->text().endsWith(QStringLiteral("Apply")))
            apply = b;
    check(apply, QStringLiteral("Text Filters has Apply"));
    if (apply)
        apply->click();
    check(testdocs::settle(*model), QStringLiteral("text filter settles"));
    bool allMatch = model->rowCount() > 0;
    for (int r = 0; r < model->rowCount() && allMatch; ++r) {
        const auto entry = model->entryAt(r);
        // Продолжения записи показываются вместе с ней.
        allMatch = entry->message().contains(QStringLiteral("timeout"), Qt::CaseInsensitive)
            || !entry->timestamp().isValid() || entry->message().trimmed().isEmpty();
    }
    check(model->rowCount() < allRows && allMatch,
          QStringLiteral("Apply filters the tab by text (%1 of %2)").arg(model->rowCount()).arg(allRows));
    check(command(win, QStringLiteral("resetFilters")), QStringLiteral("reset filters command"));
    pressCommand(win, QStringLiteral("resetFilters"));
    check(testdocs::settle(*model), QStringLiteral("reset settles"));
    check(model->rowCount() == allRows, QStringLiteral("Ctrl+Shift+R resets the text filter"));
    pressCommand(win, QStringLiteral("panelTextFilters"));
    check(waitFor([&]() { return !textDock->isVisible(); }), QStringLiteral("Ctrl+F1 hides Text Filters"));

    section(QStringLiteral("quick search"));
    tab->view()->setFocus();
    pressCommand(win, QStringLiteral("focusSearch"));
    auto* field = win.findChild<QLineEdit*>(QStringLiteral("searchLineEdit"));
    check(waitFor([&]() { return field->hasFocus(); }, 5000), QStringLiteral("Ctrl+F focuses the search field"));
    field->setText(QStringLiteral("Disk"));
    pressCommand(win, QStringLiteral("searchNext"));
    check(waitFor([&]() { return !tab->isQuickSearchRunning() && selectedEntry(tab); }),
          QStringLiteral("F3 finds and selects a line"));
    auto found = selectedEntry(tab);
    check(found && found->message().contains(QStringLiteral("Disk"), Qt::CaseInsensitive),
          QStringLiteral("F3 selects a line with the text: %1").arg(found ? found->message() : QString()));
    const int firstHit = tab->view()->userSelectedRow();
    pressCommand(win, QStringLiteral("searchNext"));
    check(waitFor([&]() { return !tab->isQuickSearchRunning() && tab->view()->userSelectedRow() > firstHit; }),
          QStringLiteral("F3 again moves to the next match"));
    pressCommand(win, QStringLiteral("searchPrev"));
    check(waitFor([&]() { return !tab->isQuickSearchRunning() && tab->view()->userSelectedRow() == firstHit; }),
          QStringLiteral("Shift+F3 returns to the previous match"));

    section(QStringLiteral("search panel"));
    auto* searchDock = win.findChild<QDockWidget*>(QStringLiteral("searchResultsDockWidget"));
    pressCommand(win, QStringLiteral("findAll"));
    check(waitFor([&]() { return searchDock->isVisible(); }), QStringLiteral("Find All shows the Search panel"));
    int expected = 0;
    for (int r = 0; r < model->rowCount(); ++r)
        if (model->entryAt(r)->message().contains(QStringLiteral("Disk"), Qt::CaseInsensitive))
            ++expected;
    LogListView* resultsView = nullptr;
    for (LogListView* v : searchDock->findChildren<LogListView*>())
        resultsView = v;
    check(resultsView, QStringLiteral("Search panel has a results view"));
    if (resultsView) {
        auto* resultsModel = qobject_cast<LogModel*>(resultsView->model());
        check(waitFor([&]() { return resultsModel && !resultsModel->isFiltering()
                                     && resultsModel->rowCount() == expected; }),
              QStringLiteral("Search panel lists every match (%1 expected, %2 shown)")
                  .arg(expected).arg(resultsModel ? resultsModel->rowCount() : -1));
    }
    searchDock->toggleViewAction()->trigger();
    check(!searchDock->isVisible(), QStringLiteral("Search panel hides by its action"));
    tab->view()->setFocus();

    section(QStringLiteral("go to"));
    const int beforeGoTo = tab->view()->userSelectedRow();
    check(beforeGoTo == firstHit, QStringLiteral("selection survives Find All (%1, expected %2)").arg(beforeGoTo).arg(firstHit));
    bool goToShown = false;
    g_driver->expect([&](QWidget* w) {
        goToShown = true;
        auto* input = w->findChild<QLineEdit*>(QStringLiteral("goToInput"));
        auto* go = w->findChild<QPushButton*>(QStringLiteral("goToButton"));
        if (!input || !go) {
            DialogDriver::close(w);
            return;
        }
        input->setText(QStringLiteral("10"));
        go->click();
    });
    pressCommand(win, QStringLiteral("goTo"));
    check(waitFor([&]() { return goToShown; }, 5000), QStringLiteral("Ctrl+G opens Go to"));
    auto atLine = selectedEntry(tab);
    check(atLine && atLine->originalLineNumber() == 10,
          QStringLiteral("Go to 10 selects line 10 (got %1)").arg(atLine ? atLine->originalLineNumber() : -1));
    const int lineTenRow = tab->view()->userSelectedRow();

    section(QStringLiteral("navigation history"));
    check(command(win, QStringLiteral("navigateBack"))->isEnabled(), QStringLiteral("Navigate Back enabled after a jump"));
    pressCommand(win, QStringLiteral("navigateBack"));
    check(waitFor([&]() { return tab->view()->userSelectedRow() == beforeGoTo; }, 5000),
          QStringLiteral("Alt+Left returns before Go to (%1, expected %2)")
              .arg(tab->view()->userSelectedRow()).arg(beforeGoTo));
    pressCommand(win, QStringLiteral("navigateForward"));
    check(waitFor([&]() { return tab->view()->userSelectedRow() == lineTenRow; }, 5000),
          QStringLiteral("Alt+Right goes forward to line 10"));

    section(QStringLiteral("next error"));
    pressCommand(win, QStringLiteral("nextError"));
    check(waitFor([&]() {
        const auto e = selectedEntry(tab);
        return e && tab->view()->userSelectedRow() != lineTenRow
            && (e->level() == LogLevel::Error || e->level() == LogLevel::Fatal);
    }, 10000), QStringLiteral("F8 selects the next error"));

    section(QStringLiteral("bookmarks"));
    const int markRow = tab->view()->userSelectedRow();
    const auto markEntry = selectedEntry(tab);
    pressCommand(win, QStringLiteral("toggleBookmark"));
    check(waitFor([&]() { return tab->bookmarks().count() == 1; }, 5000), QStringLiteral("Ctrl+F2 adds a bookmark"));
    tab->view()->goToRow(0);
    pressCommand(win, QStringLiteral("nextBookmark"));
    check(waitFor([&]() { return tab->view()->userSelectedRow() == markRow; }, 5000),
          QStringLiteral("F2 jumps to the bookmark"));
    tab->view()->goToRow(model->rowCount() - 1);
    pressCommand(win, QStringLiteral("prevBookmark"));
    check(waitFor([&]() { return tab->view()->userSelectedRow() == markRow; }, 5000),
          QStringLiteral("Shift+F2 jumps back to the bookmark"));
    auto* bookmarkDock = win.findChild<QDockWidget*>(QStringLiteral("bookmarksDockWidget"));
    pressCommand(win, QStringLiteral("panelBookmarks"));
    check(waitFor([&]() { return bookmarkDock->isVisible(); }), QStringLiteral("Ctrl+F11 shows Bookmarks"));
    auto* count = bookmarkDock->findChild<QLabel*>(QStringLiteral("bookmarkCount"));
    check(count && count->text().contains(QLatin1Char('1')),
          QStringLiteral("Bookmarks panel counts the bookmark: %1").arg(count ? count->text() : QString()));
    // Кнопка Go карточки — переход к закладке.
    tab->view()->goToRow(0);
    auto* go = bookmarkDock->findChild<QAbstractButton*>(QStringLiteral("bookmarkGo"));
    check(go, QStringLiteral("bookmark card has Go"));
    if (go)
        go->click();
    check(waitFor([&]() { return tab->view()->userSelectedRow() == markRow; }, 5000),
          QStringLiteral("Go on the bookmark card jumps to it (%1, expected %2)")
              .arg(tab->view()->userSelectedRow()).arg(markRow));
    pressCommand(win, QStringLiteral("panelBookmarks"));
    check(waitFor([&]() { return !bookmarkDock->isVisible(); }), QStringLiteral("Ctrl+F11 hides Bookmarks"));
    (void)markEntry;
}

// Смена вкладки: кнопки уровней, follow-tail, история переходов, кодировка,
// панель закладок следуют за активной вкладкой.
void scenarioTabSwitch(MainWindow& win, const Fixtures& fx)
{
    section(QStringLiteral("tab switch"));
    const int ia = tabIndexForFile(win, fx.a);
    const int ib = tabIndexForFile(win, fx.b);
    const int ic = tabIndexForFile(win, fx.c);
    LogViewWidget* a = tabAt(win, ia);
    LogViewWidget* b = tabAt(win, ib);
    auto* indicator = win.findChild<EncodingIndicator*>(QStringLiteral("encodingIndicator"));

    tabs(win)->setCurrentIndex(ia);
    a->view()->setFocus();
    pressCommand(win, QStringLiteral("levelError"));
    check(testdocs::settle(*a->model()), QStringLiteral("level filter on alpha settles"));
    tabs(win)->setCurrentIndex(ib);
    b->view()->setFocus();
    pressCommand(win, QStringLiteral("followTail"));
    check(b->view()->followTail(), QStringLiteral("Shift+F5 turns follow tail on for beta"));
    check(!command(win, QStringLiteral("levelError"))->isChecked(), QStringLiteral("beta: Error button is not checked"));
    check(!command(win, QStringLiteral("navigateBack"))->isEnabled(), QStringLiteral("beta: no navigation history"));
    check(command(win, QStringLiteral("followTail"))->isChecked(), QStringLiteral("beta: Follow Tail checked"));
    auto* bookmarkDock = win.findChild<QDockWidget*>(QStringLiteral("bookmarksDockWidget"));
    auto* bookmarkCount = bookmarkDock->findChild<QLabel*>(QStringLiteral("bookmarkCount"));
    const QString betaBookmarks = bookmarkCount->text();

    tabs(win)->setCurrentIndex(ia);
    check(command(win, QStringLiteral("levelError"))->isChecked(), QStringLiteral("alpha: Error button checked again"));
    check(!command(win, QStringLiteral("followTail"))->isChecked(), QStringLiteral("alpha: Follow Tail unchecked"));
    check(command(win, QStringLiteral("navigateBack"))->isEnabled(), QStringLiteral("alpha: navigation history back"));
    check(bookmarkCount->text() != betaBookmarks,
          QStringLiteral("bookmarks panel follows the tab (%1 vs %2)").arg(bookmarkCount->text(), betaBookmarks));
    const QString alphaEncoding = indicator->text();
    tabs(win)->setCurrentIndex(ic);
    check(waitFor([&]() { return indicator->text().contains(QStringLiteral("Windows-1251")); }, 5000),
          QStringLiteral("encoding indicator follows the tab (%1 → %2)").arg(alphaEncoding, indicator->text()));
    check(!alphaEncoding.contains(QStringLiteral("1251")), QStringLiteral("alpha is not Windows-1251: %1").arg(alphaEncoding));

    // Вернуть как было.
    tabs(win)->setCurrentIndex(ib);
    pressCommand(win, QStringLiteral("followTail"));
    check(!b->view()->followTail(), QStringLiteral("Shift+F5 turns follow tail off"));
    tabs(win)->setCurrentIndex(ia);
    pressCommand(win, QStringLiteral("levelAll"));
    check(testdocs::settle(*a->model()), QStringLiteral("show all settles"));
}

QAction* filtersToolBarAction(MainWindow& win, const QString& text);

// Полный лог (R-3): команда сочетанием и из меню, плашка и её кнопка,
// контекстное меню строки, кнопки уровней и пункт меню до / во время / после,
// смена вкладки; правка фильтра и Reset заканчивают показ.
void scenarioFullLog(MainWindow& win, const Fixtures& fx)
{
    section(QStringLiteral("full log"));
    const int ia = tabIndexForFile(win, fx.a);
    const int ib = tabIndexForFile(win, fx.b);
    LogViewWidget* a = tabAt(win, ia);
    LogModel* model = a->model();
    QAction* fullLog = command(win, QStringLiteral("fullLog"));
    QAction* levelError = command(win, QStringLiteral("levelError"));
    if (!fullLog || !levelError)
        return;
    auto* bar = a->findChild<QFrame*>(QStringLiteral("fullLogBar"));
    auto* returnButton = a->findChild<QPushButton*>(QStringLiteral("returnToFilteredButton"));
    check(bar && returnButton, QStringLiteral("full log bar exists"));
    if (!bar || !returnButton)
        return;
    tabs(win)->setCurrentIndex(ia);
    a->view()->setFocus();
    const int allRows = model->rowCount();

    // Без фильтров — объяснение в строке статуса.
    pressCommand(win, QStringLiteral("fullLog"));
    check(!a->isFullLogShown() && statusText(win).contains(QStringLiteral("already shown")),
          QStringLiteral("no filters: status explains (%1)").arg(statusText(win)));

    pressCommand(win, QStringLiteral("levelError"));
    check(testdocs::settle(*model), QStringLiteral("level filter settles"));
    const int row = model->rowCount() / 2;
    a->view()->goToRow(row);
    const qint64 line = model->keyAt(row).line;
    const auto selectedLine = [&]() {
        const int r = a->view()->userSelectedRow();
        return r >= 0 ? model->keyAt(r).line : -1;
    };
    check(!fullLog->isChecked() && plainText(fullLog->text()) == QStringLiteral("Show in Full Log"),
          QStringLiteral("before: menu item offers the full log"));

    // Сочетание.
    pressCommand(win, QStringLiteral("fullLog"));
    check(testdocs::settle(*model), QStringLiteral("full log settles"));
    check(a->isFullLogShown() && model->rowCount() == allRows && bar->isVisible(),
          QStringLiteral("Ctrl+Shift+L shows the full log with the bar (%1 of %2 rows)")
              .arg(model->rowCount()).arg(allRows));
    check(selectedLine() == line, QStringLiteral("selected line kept: %1 vs %2").arg(selectedLine()).arg(line));
    check(!levelError->isChecked(), QStringLiteral("during: Error level button shows the effective state"));
    check(fullLog->isChecked() && plainText(fullLog->text()) == QStringLiteral("Return to Filtered View"),
          QStringLiteral("during: menu item returns"));
    check(!filtersToolBarAction(win, QStringLiteral("Text"))->isChecked(),
          QStringLiteral("during: no text filter indicator"));

    // Смена вкладки: индикаторы — по вкладке.
    tabs(win)->setCurrentIndex(ib);
    check(!fullLog->isChecked() && !levelError->isChecked(), QStringLiteral("beta: no full log, no level filter"));
    tabs(win)->setCurrentIndex(ia);
    check(fullLog->isChecked() && !levelError->isChecked() && a->isFullLogShown(),
          QStringLiteral("alpha again: full log still shown"));

    // Кнопка плашки.
    returnButton->click();
    check(testdocs::settle(*model), QStringLiteral("return settles"));
    check(!a->isFullLogShown() && !bar->isVisible() && model->logLevelFilter().contains(LogLevel::Error),
          QStringLiteral("bar button returns the filters"));
    check(levelError->isChecked() && !fullLog->isChecked(), QStringLiteral("after: Error button checked again"));
    check(selectedLine() == line, QStringLiteral("selected line kept after return: %1").arg(selectedLine()));

    // Пункт меню — туда и обратно.
    if (QAction* item = menuAction(win, QStringLiteral("Show in Full Log")))
        item->trigger();
    check(testdocs::settle(*model) && a->isFullLogShown(), QStringLiteral("menu item shows the full log"));
    if (QAction* item = menuAction(win, QStringLiteral("Return to Filtered View")))
        item->trigger();
    check(testdocs::settle(*model) && !a->isFullLogShown() && levelError->isChecked(),
          QStringLiteral("menu item returns"));

    // Контекстное меню строки.
    bool picked = false;
    g_driver->expect([&](QWidget* w) {
        auto* menu = qobject_cast<QMenu*>(w);
        for (QAction* act : menu ? menu->actions() : QList<QAction*>())
            if (plainText(act->text()) == QStringLiteral("Show in Full Log")) {
                picked = true;
                menu->setActiveAction(act);
                QTest::keyClick(menu, Qt::Key_Return);
                return;
            }
        DialogDriver::close(w);
    });
    {
        const QRect rect = a->view()->visualRect(model->index(a->view()->userSelectedRow(), 0));
        const QPoint at = rect.center();
        QContextMenuEvent menuEvent(QContextMenuEvent::Mouse, at, a->view()->viewport()->mapToGlobal(at));
        QApplication::sendEvent(a->view()->viewport(), &menuEvent);
    }
    check(waitFor([&]() { return picked && a->isFullLogShown(); }, 10000),
          QStringLiteral("row context menu shows the full log"));
    check(testdocs::settle(*model) && selectedLine() == line,
          QStringLiteral("context menu keeps the line: %1").arg(selectedLine()));

    // Правка фильтра во время показа заканчивает его: новый фильтр — правда.
    pressCommand(win, QStringLiteral("levelWarn"));
    check(testdocs::settle(*model), QStringLiteral("level filter settles"));
    check(!a->isFullLogShown() && !bar->isVisible() && !fullLog->isChecked()
              && model->logLevelFilter() == QSet<LogLevel>{ LogLevel::Warn },
          QStringLiteral("level button during the full log ends it"));

    // Reset всех фильтров во время показа: полный лог, без приостановленных.
    pressCommand(win, QStringLiteral("fullLog"));
    check(testdocs::settle(*model) && a->isFullLogShown(), QStringLiteral("full log again"));
    pressCommand(win, QStringLiteral("resetFilters"));
    check(testdocs::settle(*model), QStringLiteral("reset settles"));
    check(!a->isFullLogShown() && !model->hasFilters() && !bar->isVisible() && !fullLog->isChecked(),
          QStringLiteral("Reset Filters during the full log drops the paused filters"));
}

QAction* filtersToolBarAction(MainWindow& win, const QString& text)
{
    auto* tb = win.findChild<QToolBar*>(QStringLiteral("filtersToolBar"));
    for (QAction* a : tb ? tb->actions() : QList<QAction*>())
        if (a->text() == text)
            return a;
    check(false, QStringLiteral("Filters toolbar has %1").arg(text));
    return nullptr;
}

// Глубже в склейку: Save View As в фоне, OK в Settings, смена схемы полей
// на всех вкладках, протяжка по таймлайну → фильтр времени и кнопки
// тулбара Filters, текстовый фильтр по вкладкам, Entry Details.
void scenarioGlue(MainWindow& win, const Fixtures& fx, const QDir& dir)
{
    const int ia = tabIndexForFile(win, fx.a);
    const int ib = tabIndexForFile(win, fx.b);
    tabs(win)->setCurrentIndex(ia);
    LogViewWidget* a = currentTab(win);
    LogModel* model = a->model();
    const int allRows = model->rowCount();

    section(QStringLiteral("save view as"));
    a->view()->setFocus();
    pressCommand(win, QStringLiteral("levelError"));
    check(testdocs::settle(*model), QStringLiteral("level filter settles"));
    const int visible = model->rowCount();
    const QString exported = dir.filePath(QStringLiteral("exported.txt"));
    QFile::remove(exported);
    bool asked = false;
    g_driver->expect([&](QWidget* w) {
        asked = true;
        if (auto* dialog = qobject_cast<QFileDialog*>(w))
            typeFileName(dialog, exported);
        else
            DialogDriver::close(w);
    });
    // Ждём состояние — конец задания экспорта (его сигнал), а не строку
    // статуса: строка — следствие, и при падении нужна причина (P-10, CI на
    // Windows под ctest -j: файл записан, а «Saved» за 30 с не увиден).
    auto* exportJob = win.findChild<ViewExportJob*>();
    check(exportJob != nullptr, QStringLiteral("the window owns a ViewExportJob"));
    bool exportDone = false;
    ViewExport::Result exportResult;
    QMetaObject::Connection exportConn;
    if (exportJob)
        exportConn = QObject::connect(exportJob, &ViewExportJob::finished, &win,
                                      [&](const ViewExport::Result& r) { exportDone = true; exportResult = r; });
    // Конец разбора файла прямо перед экспортом: его сообщение через 3 с
    // убирается, но только своё — итог экспорта оставаться обязан (под
    // ctest -j конец разбора и конец экспорта сошлись, и «Saved» стирался).
    emit a->fileParsingFinished(a->loadedFiles().value(0), allRows);
    check(statusText(win).contains(QStringLiteral("Finished parsing")),
          QStringLiteral("a finished parse is reported: %1").arg(statusText(win)));
    pressCommand(win, QStringLiteral("saveAs"));
    check(waitFor([&]() { return asked; }, 5000), QStringLiteral("Ctrl+Shift+S asks for a file"));
    const bool finished = waitFor([&]() { return exportDone; }, 60000);
    QObject::disconnect(exportConn);
    const QString statusAfter = statusText(win); // после ожидания, не в аргументах check
    check(finished && exportResult.ok(),
          QStringLiteral("Save View As finishes without error (finished %1, error %2 \"%3\", job running %4)")
              .arg(finished).arg(int(exportResult.error)).arg(exportResult.detail)
              .arg(exportJob && exportJob->isRunning()));
    // Слот окна подключён к сигналу раньше (в конструкторе) и уже отработал.
    check(statusAfter.contains(QStringLiteral("Saved")) && statusAfter.contains(QStringLiteral("exported.txt")),
          QStringLiteral("Save View As reports the result: %1").arg(statusAfter));
    // Таймер сообщения о разборе (3 с) истёк — итог экспорта на месте. Здесь
    // проверяется именно таймер, поэтому ждём время, а не состояние.
    testdocs::pump(3500);
    const QString statusLater = statusText(win);
    check(statusLater.contains(QStringLiteral("Saved")) && statusLater.contains(QStringLiteral("exported.txt")),
          QStringLiteral("the parse message timer leaves the export result alone: %1").arg(statusLater));
    QFile out(exported);
    QByteArray bytes;
    if (out.open(QIODevice::ReadOnly))
        bytes = out.readAll();
    check(bytes.count('\n') == visible || bytes.count('\n') + 1 == visible,
          QStringLiteral("Save View As writes the %1 visible lines (file has %2)").arg(visible).arg(bytes.count('\n')));
    pressCommand(win, QStringLiteral("levelAll"));
    check(testdocs::settle(*model), QStringLiteral("show all settles"));

    section(QStringLiteral("settings ok"));
    bool settingsShown = false;
    g_driver->expect([&](QWidget* w) {
        settingsShown = true;
        auto* box = w->findChild<QDialogButtonBox*>();
        if (box && box->button(QDialogButtonBox::Ok))
            box->button(QDialogButtonBox::Ok)->click();
        else
            DialogDriver::close(w);
    });
    pressCommand(win, QStringLiteral("settings"));
    check(waitFor([&]() { return settingsShown && !QApplication::activeModalWidget(); }, 5000),
          QStringLiteral("Settings opens and closes with OK"));
    check(waitAllLoaded(win) && model->rowCount() == allRows, QStringLiteral("tabs keep their rows after Settings OK"));

    section(QStringLiteral("schema switch"));
    auto* fieldsDock = win.findChild<QDockWidget*>(QStringLiteral("fieldVisibilityDockWidget"));
    auto* combo = fieldsDock->findChild<QComboBox*>();
    check(combo && combo->count() > 1, QStringLiteral("Log Fields offers schemas (%1)").arg(combo ? combo->count() : 0));
    if (combo && combo->count() > 1) {
        // Две схемы по очереди: изначально схема может быть не выбрана (-1).
        combo->setCurrentIndex(0);
        check(waitAllLoaded(win), QStringLiteral("tabs settle after the first schema"));
        const QString first = a->parserPattern();
        combo->setCurrentIndex(1);
        check(waitAllLoaded(win), QStringLiteral("tabs settle after the schema switch"));
        bool same = true;
        for (int i = 0; i < tabs(win)->count(); ++i)
            same = same && tabAt(win, i)->parserPattern() == a->parserPattern();
        check(!first.isEmpty() && a->parserPattern() != first, QStringLiteral("schema switch reaches the tab"));
        check(same, QStringLiteral("schema switch reaches every tab"));
        check(model->rowCount() == allRows, QStringLiteral("schema switch keeps the rows"));
        combo->setCurrentIndex(0);
        check(waitAllLoaded(win) && a->parserPattern() == first, QStringLiteral("schema switches back"));
    }

    section(QStringLiteral("timeline range"));
    auto* timeline = win.findChild<TimelineHistogramWidget*>();
    check(timeline, QStringLiteral("window has a timeline"));
    QAction* timeStatus = filtersToolBarAction(win, QStringLiteral("Time"));
    if (timeline && timeStatus) {
        const QDateTime base = testdocs::fixtureBaseTime();
        emit timeline->timeRangeSelected(base.addSecs(60), base.addSecs(300));
        check(testdocs::settle(*model), QStringLiteral("time filter settles"));
        check(model->startTimeFilter().isValid() && model->rowCount() < allRows && model->rowCount() > 0,
              QStringLiteral("timeline range filters the tab (%1 of %2)").arg(model->rowCount()).arg(allRows));
        check(timeStatus->isChecked(), QStringLiteral("Filters → Time is checked"));
        tabs(win)->setCurrentIndex(ib);
        check(!timeStatus->isChecked(), QStringLiteral("Filters → Time follows the tab (beta has none)"));
        tabs(win)->setCurrentIndex(ia);
        check(timeStatus->isChecked(), QStringLiteral("Filters → Time back on alpha"));
        timeStatus->trigger(); // отжать = Reset
        check(testdocs::settle(*model), QStringLiteral("time reset settles"));
        check(!model->startTimeFilter().isValid() && model->rowCount() == allRows,
              QStringLiteral("Filters → Time off resets the time filter"));
        check(!timeStatus->isChecked(), QStringLiteral("Filters → Time unchecked after reset"));
    }

    section(QStringLiteral("entry details"));
    auto* detailsDock = win.findChild<QDockWidget*>(QStringLiteral("entryDetailsDockWidget"));
    pressCommand(win, QStringLiteral("panelEntryDetails"));
    check(waitFor([&]() { return detailsDock->isVisible(); }), QStringLiteral("Ctrl+F8 shows Entry Details"));
    a->view()->goToRow(7);
    const auto entry = selectedEntry(a);
    auto* browser = detailsDock->findChild<QTextBrowser*>();
    const QString needle = entry ? entry->message().left(23) : QString(); // метка времени
    check(entry && browser && waitFor([&]() { return browser->toPlainText().contains(needle); }, 5000),
          QStringLiteral("Entry Details shows the selected line (%1)").arg(needle));
    pressCommand(win, QStringLiteral("panelEntryDetails"));
    check(waitFor([&]() { return !detailsDock->isVisible(); }), QStringLiteral("Ctrl+F8 hides Entry Details"));
}

bool inRecentFiles(MainWindow& win, const QString& path)
{
    for (QAction* a : recentFilesMenu(win)->actions())
        if (LogViewWidget::samePath(QDir::fromNativeSeparators(a->toolTip()), path))
            return true;
    return false;
}

// Открытие из диалога Open и из Directory Scanner; закрытие крестиком.
void scenarioOpenMore(MainWindow& win, const Fixtures& fx)
{
    section(QStringLiteral("open dialog"));
    const int before = tabs(win)->count();
    const QString scanOne = QDir(fx.dir).filePath(QStringLiteral("one.log"));
    const QString scanTwo = QDir(fx.dir).filePath(QStringLiteral("two.log"));
    bool asked = false;
    g_driver->expect([&](QWidget* w) {
        asked = true;
        if (auto* dialog = qobject_cast<QFileDialog*>(w))
            typeFileName(dialog, fx.gone);
        else
            DialogDriver::close(w);
    });
    pressCommand(win, QStringLiteral("open"));
    check(waitFor([&]() { return asked && tabIndexForFile(win, fx.gone) >= 0; }),
          QStringLiteral("Ctrl+O opens the chosen file"));
    check(tabs(win)->count() == before + 1, QStringLiteral("Ctrl+O opens a new tab"));
    check(inRecentFiles(win, fx.gone), QStringLiteral("file from Open is in Recent Files"));
    check(waitAllLoaded(win), QStringLiteral("opened file loads"));

    section(QStringLiteral("close button"));
    const int gone = tabIndexForFile(win, fx.gone);
    QWidget* closeButton = tabs(win)->tabBar()->tabButton(gone, QTabBar::RightSide);
    if (!closeButton)
        closeButton = tabs(win)->tabBar()->tabButton(gone, QTabBar::LeftSide);
    check(closeButton, QStringLiteral("tab has a close button"));
    if (auto* button = qobject_cast<QAbstractButton*>(closeButton))
        button->click();
    check(waitFor([&]() { return tabIndexForFile(win, fx.gone) < 0; }), QStringLiteral("close button closes the tab"));
    check(tabs(win)->count() == before, QStringLiteral("tab count after the close button"));

    section(QStringLiteral("directory scanner opens files"));
    auto* scanner = win.findChild<DirectoryScanner*>();
    emit scanner->filesActivated({scanOne, scanTwo}); // выбор в дереве + Open
    check(waitFor([&]() { return tabIndexForFile(win, scanOne) >= 0; }), QStringLiteral("scanner opens the selected files"));
    check(tabIndexForFile(win, scanOne) == tabIndexForFile(win, scanTwo),
          QStringLiteral("files selected together share a tab"));
    check(waitAllLoaded(win), QStringLiteral("scanner files load"));
    check(inRecentFiles(win, scanOne) && inRecentFiles(win, scanTwo),
          QStringLiteral("files opened from the scanner are in Recent Files"));
    emit scanner->fileActivated(fx.gone); // двойной щелчок по файлу
    check(waitFor([&]() { return tabIndexForFile(win, fx.gone) >= 0; }), QStringLiteral("double click in the scanner opens a file"));
    check(waitAllLoaded(win), QStringLiteral("double-clicked file loads"));

    // Лишнее закрыть: дальше сценарии ждут alpha, beta, cyr1251.
    for (const QString& path : {scanOne, fx.gone}) {
        const int i = tabIndexForFile(win, path);
        if (i < 0)
            continue;
        const int n = tabs(win)->count();
        tabs(win)->setCurrentIndex(i);
        command(win, QStringLiteral("closeTab"))->trigger();
        waitFor([&]() { return tabs(win)->count() == n - 1; });
    }
    check(tabs(win)->count() == before, QStringLiteral("back to %1 tabs").arg(before));
}

// Панель Search следует за активной вкладкой, пока поиск запущен.
void scenarioSearchFollowsTab(MainWindow& win, const Fixtures& fx)
{
    section(QStringLiteral("search follows tab"));
    auto* searchDock = win.findChild<QDockWidget*>(QStringLiteral("searchResultsDockWidget"));
    LogListView* resultsView = nullptr;
    for (LogListView* v : searchDock->findChildren<LogListView*>())
        resultsView = v;
    auto* resultsModel = resultsView ? qobject_cast<LogModel*>(resultsView->model()) : nullptr;
    if (!resultsModel) {
        check(false, QStringLiteral("Search panel results model"));
        return;
    }
    tabs(win)->setCurrentIndex(tabIndexForFile(win, fx.a));
    auto* field = win.findChild<QLineEdit*>(QStringLiteral("searchLineEdit"));
    field->setText(QStringLiteral("Disk"));
    pressCommand(win, QStringLiteral("findAll"));
    check(waitFor([&]() { return searchDock->isVisible() && !resultsModel->isFiltering() && resultsModel->rowCount() > 0; }),
          QStringLiteral("Find All lists alpha's matches"));
    const int alphaHits = resultsModel->rowCount();
    tabs(win)->setCurrentIndex(tabIndexForFile(win, fx.c)); // в кириллическом логе «Disk» нет
    check(waitFor([&]() { return !resultsModel->isFiltering() && resultsModel->rowCount() == 0; }),
          QStringLiteral("results follow the tab without matches (%1)").arg(resultsModel->rowCount()));
    tabs(win)->setCurrentIndex(tabIndexForFile(win, fx.a));
    check(waitFor([&]() { return !resultsModel->isFiltering() && resultsModel->rowCount() == alphaHits; }),
          QStringLiteral("results come back with alpha (%1 of %2)").arg(resultsModel->rowCount()).arg(alphaHits));
    searchDock->toggleViewAction()->trigger();
}

// Индикатор кодировки и смена кодировки через его меню.
void scenarioEncoding(MainWindow& win, const Fixtures& fx)
{
    section(QStringLiteral("encoding indicator"));
    tabs(win)->setCurrentIndex(tabIndexForFile(win, fx.c));
    LogViewWidget* tab = currentTab(win);
    auto* indicator = win.findChild<EncodingIndicator*>(QStringLiteral("encodingIndicator"));
    check(indicator && indicator->isVisible(), QStringLiteral("encoding indicator is shown"));
    if (!indicator)
        return;
    check(waitFor([&]() { return indicator->text().contains(QStringLiteral("Windows-1251")); }, 10000),
          QStringLiteral("indicator shows the detected encoding: %1").arg(indicator->text()));
    auto* koi = indicator->findChild<QAction*>(QStringLiteral("encodingAction_KOI8-R"));
    check(koi, QStringLiteral("indicator menu offers KOI8-R"));
    if (koi)
        koi->trigger();
    check(waitFor([&]() { return indicator->text().contains(QStringLiteral("KOI8-R")); }, 10000),
          QStringLiteral("choosing KOI8-R changes the indicator: %1").arg(indicator->text()));
    check(waitLoaded(tab), QStringLiteral("tab reloads in the chosen encoding"));
    const LogFilePtr file = tab->loadedFiles().value(0);
    check(file && tab->fileEncoding(file).source == FileEncoding::Source::User, QStringLiteral("encoding is manual after the menu"));
    auto* autoAction = indicator->findChild<QAction*>(QStringLiteral("encodingAutoAction"));
    if (autoAction)
        autoAction->trigger();
    check(waitFor([&]() { return indicator->text().contains(QStringLiteral("Windows-1251")); }, 10000),
          QStringLiteral("Detect automatically returns to Windows-1251: %1").arg(indicator->text()));
    check(waitLoaded(tab), QStringLiteral("tab reloads after automatic encoding"));
}

// Память файла: закрыть вкладку, открыть файл снова — закладки, позиция и
// плашка Restore Filters; Ctrl+Shift+T.
void scenarioFileMemory(MainWindow& win, const Fixtures& fx)
{
    section(QStringLiteral("file memory"));
    tabs(win)->setCurrentIndex(tabIndexForFile(win, fx.a));
    LogViewWidget* tab = currentTab(win);
    const int bookmarks = tab->bookmarks().count();
    check(bookmarks == 1, QStringLiteral("alpha has its bookmark before closing"));
    tab->view()->setFocus();
    pressCommand(win, QStringLiteral("levelError"));
    check(testdocs::settle(*tab->model()), QStringLiteral("level filter settles before closing"));
    tab->view()->goToRow(tab->model()->rowCount() / 2);
    const auto keep = selectedEntry(tab);
    const int keepLine = keep ? keep->originalLineNumber() : -1;
    const int before = tabs(win)->count();
    pressCommand(win, QStringLiteral("closeTab"));
    check(waitFor([&]() { return tabs(win)->count() == before - 1; }), QStringLiteral("Ctrl+W closes the tab"));
    check(tabIndexForFile(win, fx.a) < 0, QStringLiteral("alpha tab is gone"));

    // Открыть снова — из меню Recent Files.
    QAction* recent = nullptr;
    for (QAction* a : recentFilesMenu(win)->actions())
        if (LogViewWidget::samePath(QDir::fromNativeSeparators(a->toolTip()), fx.a))
            recent = a;
    check(recent, QStringLiteral("Recent Files lists alpha"));
    if (!recent)
        return;
    recent->trigger();
    check(waitFor([&]() { return tabIndexForFile(win, fx.a) >= 0; }), QStringLiteral("Recent Files reopens alpha"));
    tabs(win)->setCurrentIndex(tabIndexForFile(win, fx.a));
    tab = currentTab(win);
    check(waitLoaded(tab), QStringLiteral("reopened alpha loads"));
    check(waitFor([&]() { return tab->bookmarks().count() == bookmarks; }, 10000),
          QStringLiteral("reopened alpha has its bookmark"));
    check(waitFor([&]() { return !tab->isPositionPending() && selectedEntry(tab); }, 10000),
          QStringLiteral("reopened alpha restores the selection"));
    const auto restored = selectedEntry(tab);
    check(restored && restored->originalLineNumber() == keepLine,
          QStringLiteral("reopened alpha selects line %1 (got %2)")
              .arg(keepLine).arg(restored ? restored->originalLineNumber() : -1));
    auto* offer = tab->findChild<QWidget*>(QStringLiteral("filtersOfferBar"));
    check(offer && waitFor([&]() { return offer->isVisible(); }, 5000),
          QStringLiteral("Restore Filters bar is offered"));
    check(tab->model()->logLevelFilter().isEmpty(), QStringLiteral("filters are not applied before the offer is taken"));
    if (auto* restore = tab->findChild<QPushButton*>(QStringLiteral("restoreFiltersButton")))
        restore->click();
    check(testdocs::settle(*tab->model()), QStringLiteral("restored filters settle"));
    check(tab->model()->logLevelFilter().contains(LogLevel::Error), QStringLiteral("Restore Filters brings back the level filter"));
    check(command(win, QStringLiteral("levelError"))->isChecked(), QStringLiteral("level button follows the restored filter"));
    pressCommand(win, QStringLiteral("levelAll"));
    check(testdocs::settle(*tab->model()), QStringLiteral("show all settles"));

    section(QStringLiteral("reopen closed tab"));
    tabs(win)->setCurrentIndex(tabIndexForFile(win, fx.b));
    const int count = tabs(win)->count();
    pressCommand(win, QStringLiteral("closeTab"));
    check(waitFor([&]() { return tabs(win)->count() == count - 1; }), QStringLiteral("beta tab closes"));
    check(command(win, QStringLiteral("reopenTab"))->isEnabled(), QStringLiteral("Reopen Closed Tab is enabled"));
    pressCommand(win, QStringLiteral("reopenTab"));
    check(waitFor([&]() { return tabIndexForFile(win, fx.b) >= 0; }), QStringLiteral("Ctrl+Shift+T reopens beta"));
    check(tabs(win)->count() == count, QStringLiteral("tab count is back"));
    check(waitAllLoaded(win), QStringLiteral("reopened beta loads"));
}

// Объединение вкладок: контекстное меню заголовка и перетаскивание.
void scenarioMerge(MainWindow& win, const Fixtures& fx)
{
    section(QStringLiteral("merge tabs"));
    // Перетаскивание вкладки на вкладку (LogTabBar → mergeTabsRequested):
    // сигнал вместо мыши — жест таббара проверяется не здесь.
    const int from = tabIndexForFile(win, fx.c);
    const int to = tabIndexForFile(win, fx.b);
    emit tabs(win)->mergeTabsRequested(from, to);
    check(waitFor([&]() { return tabs(win)->count() == 2; }), QStringLiteral("dragging a tab onto a tab merges them"));
    check(tabIndexForFile(win, fx.c) == tabIndexForFile(win, fx.b), QStringLiteral("beta and cyr1251 share a tab"));
    check(waitAllLoaded(win), QStringLiteral("merged tab loads"));

    // Контекстное меню: «Merge All Tabs Into This One».
    bool merged = false;
    g_driver->expect([&](QWidget* w) {
        auto* menu = qobject_cast<QMenu*>(w);
        for (QAction* a : menu ? menu->actions() : QList<QAction*>())
            if (plainText(a->text()) == QStringLiteral("Merge All Tabs Into This One")) {
                merged = true;
                // Как с клавиатуры: exec() вернёт выбранный пункт.
                menu->setActiveAction(a);
                QTest::keyClick(menu, Qt::Key_Return);
                return;
            }
        DialogDriver::close(w);
    });
    QTabBar* bar = tabs(win)->tabBar();
    const QPoint at = bar->tabRect(0).center();
    QContextMenuEvent menuEvent(QContextMenuEvent::Mouse, at, bar->mapToGlobal(at));
    QApplication::sendEvent(bar, &menuEvent);
    check(waitFor([&]() { return merged && tabs(win)->count() == 1; }),
          QStringLiteral("tab context menu merges all tabs"));
    check(waitAllLoaded(win), QStringLiteral("merged tab loads"));
    check(currentTab(win)->fileCount() == 3, QStringLiteral("merged tab has three files"));

    // Ctrl+Shift+T возвращает слитую вкладку целиком.
    pressCommand(win, QStringLiteral("closeTab"));
    check(waitFor([&]() { return tabs(win)->count() == 0; }), QStringLiteral("merged tab closes"));
    pressCommand(win, QStringLiteral("reopenTab"));
    check(waitFor([&]() { return tabs(win)->count() == 1; }), QStringLiteral("Ctrl+Shift+T reopens the merged tab"));
    check(waitAllLoaded(win), QStringLiteral("reopened merged tab loads"));
    check(currentTab(win)->fileCount() == 3, QStringLiteral("reopened merged tab has three files (%1)").arg(currentTab(win)->fileCount()));
}

// Сессия: Save Session As, Open Session, отчёт о пропавшем файле, бросок
// .dlsession.
void scenarioSession(MainWindow& win, const Fixtures& fx)
{
    section(QStringLiteral("session"));
    win.openCommandLine(OpenRouting::parseCommandLine({fx.gone}));
    check(waitFor([&]() { return tabIndexForFile(win, fx.gone) >= 0; }), QStringLiteral("gone.log opens"));
    check(waitAllLoaded(win), QStringLiteral("gone.log loads"));
    const int tabsInSession = tabs(win)->count();

    QFile::remove(fx.sessionPath);
    bool saveShown = false;
    g_driver->expect([&](QWidget* w) {
        saveShown = true;
        auto* dialog = qobject_cast<QFileDialog*>(w);
        if (!dialog) {
            DialogDriver::close(w);
            return;
        }
        typeFileName(dialog, fx.sessionPath);
    });
    menuAction(win, QStringLiteral("Save Session As"))->trigger();
    check(saveShown, QStringLiteral("Save Session As asks for a file"));
    check(waitFor([&]() { return QFileInfo::exists(fx.sessionPath); }, 5000),
          QStringLiteral("Save Session As writes the file (status: %1; dir: %2)")
              .arg(statusText(win), QFileInfo(fx.sessionPath).dir().entryList(QDir::Files).join(',')));

    // Файл сессии пропал с диска — отчёт.
    const QString hidden = fx.gone + QStringLiteral(".away");
    check(QFile::rename(fx.gone, hidden), QStringLiteral("gone.log moved away"));
    bool openShown = false;
    g_driver->expect([&](QWidget* w) {
        openShown = true;
        auto* dialog = qobject_cast<QFileDialog*>(w);
        if (!dialog) {
            DialogDriver::close(w);
            return;
        }
        typeFileName(dialog, fx.sessionPath);
    });
    menuAction(win, QStringLiteral("Open Session"))->trigger();
    check(openShown, QStringLiteral("Open Session asks for a file"));
    check(waitFor([&]() { return tabs(win)->count() == 2 * tabsInSession; }),
          QStringLiteral("Open Session adds the session tabs (%1)").arg(tabs(win)->count()));
    auto* link = win.findChild<QLabel*>(QStringLiteral("sessionReportLink"));
    check(link && waitFor([&]() { return link->isVisible(); }, 5000),
          QStringLiteral("missing file shows the report link"));
    if (link && link->isVisible()) {
        emit link->linkActivated(QStringLiteral("report"));
        auto* report = win.findChild<SessionReportDialog*>();
        check(report && waitFor([&]() { return report->isVisible(); }, 5000),
              QStringLiteral("report link opens the report"));
        if (report)
            report->close();
    }
    check(waitAllLoaded(win), QStringLiteral("session tabs load"));
    // Вкладку с пропавшим файлом закрыли — отчёт пуст, ссылка гаснет.
    if (link && link->isVisible()) {
        for (int i = tabs(win)->count() - 1; i >= tabsInSession; --i) {
            const int n = tabs(win)->count();
            tabs(win)->setCurrentIndex(i);
            command(win, QStringLiteral("closeTab"))->trigger();
            waitFor([&]() { return tabs(win)->count() == n - 1; });
        }
        check(waitFor([&]() { return !link->isVisible(); }, 5000),
              QStringLiteral("report link hides once the tabs with problems are closed"));
    }
    QFile::rename(hidden, fx.gone);

    // .dlsession, брошенный в окно, — сессия, а не лог.
    const int beforeDrop = tabs(win)->count();
    check(beforeDrop == tabsInSession, QStringLiteral("only the first session's tabs are left (%1)").arg(beforeDrop));
    dropPaths(win, {fx.sessionPath});
    check(waitFor([&]() { return tabs(win)->count() == beforeDrop + tabsInSession; }),
          QStringLiteral("dropped .dlsession opens as a session (%1 tabs)").arg(tabs(win)->count()));
    check(waitAllLoaded(win), QStringLiteral("dropped session loads"));

    // Лишнее закрыть: остаются вкладки одной сессии.
    while (tabs(win)->count() > tabsInSession) {
        const int n = tabs(win)->count();
        tabs(win)->setCurrentIndex(n - 1);
        command(win, QStringLiteral("closeTab"))->trigger();
        waitFor([&]() { return tabs(win)->count() == n - 1; });
    }
}

// Каждый док показывается и скрывается своим действием.
void scenarioDocks(MainWindow& win)
{
    section(QStringLiteral("docks"));
    QStringList viewMenuActions;
    for (QAction* a : menuActions(win))
        viewMenuActions << describeAction(a);
    const QList<QDockWidget*> docks = win.findChildren<QDockWidget*>();
    check(docks.size() >= 10, QStringLiteral("window has its docks (%1)").arg(docks.size()));
    for (QDockWidget* dock : docks) {
        QAction* toggle = dock->toggleViewAction();
        const QString name = dock->objectName();
        check(!toggle->property("commandId").toString().isEmpty(),
              QStringLiteral("dock %1 toggle has a shortcut command").arg(name));
        bool inMenu = false;
        for (QAction* a : menuActions(win))
            inMenu = inMenu || a == toggle;
        check(inMenu, QStringLiteral("dock %1 toggle is in a menu").arg(name));
        const bool wasVisible = dock->isVisible();
        if (!toggle->shortcut().isEmpty())
            press(win, toggle->shortcut());
        else
            toggle->trigger();
        check(waitFor([&]() { return dock->isVisible() != wasVisible; }, 5000),
              QStringLiteral("dock %1 toggles by its shortcut %2 (was visible: %3, focus: %4)")
                  .arg(name, toggle->shortcut().toString()).arg(wasVisible)
                  .arg(QApplication::focusWidget() ? QString::fromLatin1(QApplication::focusWidget()->metaObject()->className()) + QLatin1Char('/') + QApplication::focusWidget()->objectName() : QStringLiteral("none"))
                  + QStringLiteral(" active: %1, focusWindow: %2").arg(QApplication::activeWindow() ? QApplication::activeWindow()->metaObject()->className() : "none", QGuiApplication::focusWindow() ? QGuiApplication::focusWindow()->objectName() : QStringLiteral("none")));
        toggle->trigger();
        check(waitFor([&]() { return dock->isVisible() == wasVisible; }, 5000),
              QStringLiteral("dock %1 toggles back").arg(name));
    }
}

// Перенос одной строки (U-23): Пробел в фокусе лога — команда
// toggleLineWrap; на индексной вкладке переноса нет, и Пробел показывает
// полный текст строки в доке Entry Details. В поле ввода Пробел — текст.
void scenarioLineWrap(MainWindow& win, const QDir& dir, bool indexed)
{
    section(QStringLiteral("wrap current line"));
    QByteArray bytes;
    for (int i = 0; i < 6; ++i) {
        bytes += "2026-03-05 10:00:1" + QByteArray::number(i) + ".000 [w] INFO - line " + QByteArray::number(i) + ' ';
        bytes += QByteArray(i == 2 ? 6000 : 8, 'z') + '\n';
    }
    const QString path = testdocs::writeFile(dir, QStringLiteral("long-line.log"), bytes);
    const int before = tabs(win)->count();
    win.openCommandLine(OpenRouting::parseCommandLine({path}));
    check(waitFor([&]() { return tabIndexForFile(win, path) >= 0; }), QStringLiteral("long-line log opens"));
    tabs(win)->setCurrentIndex(tabIndexForFile(win, path));
    LogViewWidget* tab = currentTab(win);
    check(waitLoaded(tab, 6), QStringLiteral("long-line log loads"));
    QAction* wrapLine = command(win, QStringLiteral("toggleLineWrap"));
    check(wrapLine && menuAction(win, QStringLiteral("Wrap / Unwrap Current Line")) == wrapLine,
          QStringLiteral("View menu has Wrap / Unwrap Current Line"));
    auto* detailsDock = win.findChild<QDockWidget*>(QStringLiteral("entryDetailsDockWidget"));
    detailsDock->hide();
    tab->view()->goToRow(2);
    tab->view()->setFocus(Qt::MouseFocusReason);
    check(tab->view()->rowWrapInfo(2).hasToggle, QStringLiteral("the long line has a wrap toggle"));
    press(win, QKeySequence(Qt::Key_Space));
    if (indexed) {
        check(waitFor([&]() { return detailsDock->isVisible(); }, 5000),
              QStringLiteral("Space on an indexed tab shows Entry Details"));
        check(!tab->view()->rowWrapInfo(2).expanded, QStringLiteral("the indexed row stays one line"));
        detailsDock->hide();
    } else {
        // Перенос всего вида мог остаться включённым прежними сценариями.
        const bool wrapped = tab->view()->isWordWrap();
        check(waitFor([&]() { return tab->view()->rowWrapInfo(2).expanded != wrapped; }, 5000),
              QStringLiteral("Space wraps / unwraps the current line"));
        press(win, QKeySequence(Qt::Key_Space));
        check(waitFor([&]() { return tab->view()->rowWrapInfo(2).expanded == wrapped; }, 5000),
              QStringLiteral("Space again returns it"));
        check(!detailsDock->isVisible(), QStringLiteral("Entry Details stays hidden on a resident tab"));
    }
    // Пробел в поле быстрого поиска — пробел, а не команда.
    auto* field = win.findChild<QLineEdit*>(QStringLiteral("searchLineEdit"));
    if (field && wrapLine) {
        field->setFocus(Qt::MouseFocusReason);
        field->clear();
        QSignalSpy spy(wrapLine, &QAction::triggered);
        QTest::keyClick(field, Qt::Key_Space);
        check(spy.isEmpty() && field->text() == QStringLiteral(" "),
              QStringLiteral("Space in the search field types a space"));
        field->clear();
    }
    if (QAction* close = command(win, QStringLiteral("closeTab")))
        close->trigger();
    check(waitFor([&]() { return tabs(win)->count() == before; }), QStringLiteral("long-line tab closes"));
}

// Каждое действие меню вызывается хотя бы раз (модальные закрывает драйвер).
void scenarioMenuSweep(MainWindow& win)
{
    section(QStringLiteral("menu sweep"));
    // Выход — отдельным сценарием; Check for Updates — без сети (шов).
    const QStringList skip { QStringLiteral("Exit") };
    int triggered = 0;
    // Пункты Recent Files пересоздаются по ходу — список снят заранее, а
    // действие могло уже исчезнуть вместе с меню.
    QList<QPointer<QAction>> all;
    for (QAction* a : menuActions(win))
        all << a;
    for (const QPointer<QAction>& guard : all) {
        QAction* a = guard.data();
        if (!a || !a->isEnabled() || skip.contains(plainText(a->text())))
            continue;
        const QString name = describeAction(a);
        std::fprintf(stderr, "  menu: %s\n", qPrintable(name));
        a->trigger();
        ++triggered;
        testdocs::pump(20);
        check(waitFor([]() { return !QApplication::activeModalWidget() && !QApplication::activePopupWidget(); }, 10000),
              QStringLiteral("menu item %1 leaves no dialog open").arg(name));
        if (!guard)
            continue;
        // Тогл — вернуть как было.
        if (a->isCheckable() && a->isEnabled()) {
            a->trigger();
            testdocs::pump(20);
        }
    }
    check(triggered > 20, QStringLiteral("menu sweep triggered %1 items").arg(triggered));
    check(waitAllLoaded(win), QStringLiteral("tabs settle after the sweep"));
}

// ---------------------------------------------------------------------------
// Таблица сочетаний в справке (обе версии) — против умолчаний ShortcutManager.
// Строка таблицы привязана к командам по английской подписи; русская таблица
// идёт строка в строку и обязана давать те же клавиши. Каждая команда —
// ровно в одной строке. Строки без команд (Ctrl+C, Space, выделение) — клавиши
// самого view, их сверяет только равенство двух языков.
QStringList helpTableRows(const QString& resource, const QString& heading)
{
    QFile f(resource);
    check(f.open(QIODevice::ReadOnly), QStringLiteral("help resource %1 opens").arg(resource));
    const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
    QStringList rows;
    bool in = false;
    for (QString line : lines) {
        line = line.trimmed();
        if (line.startsWith(QLatin1String("## ")))
            in = line.startsWith(heading);
        else if (in && line.startsWith(QLatin1Char('|')) && !line.startsWith(QLatin1String("|---"))
                 && !line.startsWith(QStringLiteral("| Action")) && !line.startsWith(QStringLiteral("| Действие")))
            rows << line;
    }
    check(!rows.isEmpty(), QStringLiteral("%1 has a shortcut table").arg(resource));
    return rows;
}

QStringList helpCells(const QString& row)
{
    QStringList cells = row.split(QLatin1Char('|'));
    for (QString& c : cells)
        c = c.trimmed();
    cells.removeAll(QString());
    return cells;
}

// Клавиши ячейки — в виде PortableText: стрелки словами, «Пробел» — Space,
// диапазоны («Alt+1 … Alt+6», «Ctrl+F3…F11») развёрнуты, одиночный
// модификатор («Ctrl + колесо») — не клавиша.
QSet<QString> helpKeys(const QString& cell)
{
    QStringList tokens;
    static const QRegularExpression code(QStringLiteral("`([^`]+)`"));
    for (auto it = code.globalMatch(cell); it.hasNext();)
        tokens << it.next().captured(1);
    QSet<QString> keys;
    auto add = [&](QString t) {
        t.replace(QChar(0x2190), QLatin1String("Left")).replace(QChar(0x2192), QLatin1String("Right"));
        t.replace(QStringLiteral("Пробел"), QLatin1String("Space"));
        static const QStringList modifiers = {QStringLiteral("Ctrl"), QStringLiteral("Alt"), QStringLiteral("Shift")};
        if (!modifiers.contains(t))
            keys.insert(t);
    };
    static const QRegularExpression fRange(QStringLiteral("^(.*\\+F)(\\d+)\\x{2026}F(\\d+)$"));
    for (int i = 0; i < tokens.size(); ++i) {
        const QString& t = tokens[i];
        if (const auto m = fRange.match(t); m.hasMatch()) {
            for (int n = m.captured(2).toInt(); n <= m.captured(3).toInt(); ++n)
                add(m.captured(1) + QString::number(n));
            continue;
        }
        // «`Alt+1` … `Alt+6`»: между двумя соседними токенами — многоточие.
        if (i + 1 < tokens.size()
            && cell.contains(QStringLiteral("`%1` … `%2`").arg(t, tokens[i + 1]))) {
            const QString prefix = t.left(t.size() - 1);
            for (int n = t.right(1).toInt(); n <= tokens[i + 1].right(1).toInt(); ++n)
                add(prefix + QString::number(n));
            ++i;
            continue;
        }
        add(t);
    }
    return keys;
}

void checkHelpShortcutTable()
{
    section(QStringLiteral("help: shortcut table vs ShortcutManager defaults"));
    const std::map<QString, QStringList> commandsOfRow = {
        {QStringLiteral("Open log file(s)"), {QStringLiteral("open")}},
        {QStringLiteral("Save View As"), {QStringLiteral("saveAs")}},
        {QStringLiteral("Close tab"), {QStringLiteral("closeTab")}},
        {QStringLiteral("Reopen closed tab"), {QStringLiteral("reopenTab")}},
        {QStringLiteral("Exit"), {QStringLiteral("quit")}},
        {QStringLiteral("Reload file"), {QStringLiteral("reload")}},
        {QStringLiteral("Auto-reload this tab"), {QStringLiteral("autoReload")}},
        {QStringLiteral("Follow tail"), {QStringLiteral("followTail")}},
        {QStringLiteral("Settings"), {QStringLiteral("settings")}},
        {QStringLiteral("Focus search field"), {QStringLiteral("focusSearch")}},
        {QStringLiteral("Search next / previous"), {QStringLiteral("searchNext"), QStringLiteral("searchPrev")}},
        {QStringLiteral("Find all (Search panel)"), {QStringLiteral("findAll")}},
        {QStringLiteral("Quick find: match case / whole word / regex"),
         {QStringLiteral("findMatchCase"), QStringLiteral("findWholeWord"), QStringLiteral("findRegex")}},
        {QStringLiteral("Go to line or time"), {QStringLiteral("goTo")}},
        {QStringLiteral("Navigate back / forward"), {QStringLiteral("navigateBack"), QStringLiteral("navigateForward")}},
        {QStringLiteral("Next / previous error (Error, Fatal)"), {QStringLiteral("nextError"), QStringLiteral("prevError")}},
        {QStringLiteral("Next / previous warning"), {QStringLiteral("nextWarning"), QStringLiteral("prevWarning")}},
        {QStringLiteral("Add / remove bookmark"), {QStringLiteral("toggleBookmark")}},
        {QStringLiteral("Next / previous bookmark"), {QStringLiteral("nextBookmark"), QStringLiteral("prevBookmark")}},
        {QStringLiteral("Go to new lines (below the screen)"), {QStringLiteral("goToNewLines")}},
        {QStringLiteral("Toggle word wrap"), {QStringLiteral("wordWrap")}},
        {QStringLiteral("Larger / smaller / default font"),
         {QStringLiteral("zoomIn"), QStringLiteral("zoomOut"), QStringLiteral("zoomReset")}},
        {QStringLiteral("Show only Fatal … Trace (toggle)"),
         {QStringLiteral("levelFatal"), QStringLiteral("levelError"), QStringLiteral("levelWarn"),
          QStringLiteral("levelInfo"), QStringLiteral("levelDebug"), QStringLiteral("levelTrace")}},
        {QStringLiteral("Show all levels"), {QStringLiteral("levelAll")}},
        {QStringLiteral("Reset filters of this tab"), {QStringLiteral("resetFilters")}},
        {QStringLiteral("Show in full log / return to filtered view"), {QStringLiteral("fullLog")}},
        {QStringLiteral("Show/Hide panels"),
         {QStringLiteral("panelTextFilters"), QStringLiteral("panelDirScanner"), QStringLiteral("panelTimeFilter"),
          QStringLiteral("panelFields"), QStringLiteral("panelRowHighlighters"), QStringLiteral("panelTimeline"),
          QStringLiteral("panelSearchResults"), QStringLiteral("panelEntryDetails"),
          QStringLiteral("panelStatistics"), QStringLiteral("panelBookmarks")}},
        // Клавиши самого view, не команды ShortcutManager.
        {QStringLiteral("Copy selection"), {}},
        {QStringLiteral("Wrap / unwrap current line"), {QStringLiteral("toggleLineWrap")}},
        {QStringLiteral("Extend selection by character"), {}},
        {QStringLiteral("Extend selection by token"), {}},
    };

    const QStringList en = helpTableRows(QStringLiteral(":/help/help_en.md"), QStringLiteral("## Keyboard shortcuts"));
    const QStringList ru = helpTableRows(QStringLiteral(":/help/help_ru.md"), QStringLiteral("## Горячие клавиши"));
    check(en.size() == ru.size(), QStringLiteral("help tables have the same rows: en %1, ru %2").arg(en.size()).arg(ru.size()));

    const ShortcutManager& sm = ShortcutManager::instance();
    QMap<QString, int> covered;
    for (int i = 0; i < en.size(); ++i) {
        const QStringList e = helpCells(en[i]);
        if (e.size() != 2) {
            check(false, QStringLiteral("help_en row has two cells: %1").arg(en[i]));
            continue;
        }
        if (i < ru.size()) {
            const QStringList r = helpCells(ru[i]);
            const QSet<QString> ek = helpKeys(e[1]);
            const QSet<QString> rk = r.size() == 2 ? helpKeys(r[1]) : QSet<QString>();
            check(ek == rk, QStringLiteral("help row %1 (%2): the same keys in both languages: en {%3}, ru {%4}")
                                .arg(i + 1).arg(e[0], QStringList(ek.values()).join(QLatin1Char(' ')),
                                                QStringList(rk.values()).join(QLatin1Char(' '))));
        }
        const auto row = commandsOfRow.find(e[0]);
        check(row != commandsOfRow.end(), QStringLiteral("help_en row \"%1\" is known to the test").arg(e[0]));
        if (row == commandsOfRow.end() || row->second.isEmpty())
            continue;
        QSet<QString> expected;
        for (const QString& id : row->second) {
            ++covered[id];
            const QKeySequence seq = sm.defaultSequence(id);
            check(!seq.isEmpty(), QStringLiteral("command %1 has a default shortcut").arg(id));
            expected.insert(seq.toString(QKeySequence::PortableText));
        }
        const QSet<QString> shown = helpKeys(e[1]);
        check(shown == expected, QStringLiteral("help row \"%1\": shows {%2}, defaults are {%3}")
                                     .arg(e[0], QStringList(shown.values()).join(QLatin1Char(' ')),
                                          QStringList(expected.values()).join(QLatin1Char(' '))));
    }
    for (const ShortcutManager::Command& c : sm.commands())
        check(covered.value(c.id) == 1, QStringLiteral("command %1 (%2) is in the help table once (got %3)")
                                            .arg(c.id, c.label).arg(covered.value(c.id)));
}

// ---------------------------------------------------------------------------
// Обновление с 0.5.0: каталог настроек, каким его оставила 0.5.0 (ini со
// своими сочетаниями и раскладкой доков без панели Bookmarks, свои схемы в
// patterns/, недавние файлы; нет memory/ и last-session.json), открывается
// без потерь и без сообщений. Переназначенные пользователем клавиши, которые
// в 0.6 стали умолчаниями новых команд, остаются у его команд; новая команда
// остаётся без клавиши, неоднозначностей нет.
void scenarioUpgradeFrom050(const QDir& root)
{
    section(QStringLiteral("upgrade from 0.5.0"));
    const QDir dir(root.filePath(QStringLiteral("upgrade")));
    QDir().mkpath(dir.absolutePath());
    const QString log = testdocs::writeFile(dir, QStringLiteral("old.log"),
        testdocs::makeLogBytes(200, 77, "\n", testdocs::fixtureBaseTime(), false, true, true));

    // Раскладка доков 0.5.0: та же, но без Bookmarks (её в 0.5.0 не было).
    QByteArray geometry, state;
    {
        AppSettings::overrideConfigDirForTesting(dir.filePath(QStringLiteral("probe-config")));
        AppSettings::instance().load();
        MainWindow probe;
        showWindow(probe);
        if (auto* bookmarks = probe.findChild<QDockWidget*>(QStringLiteral("bookmarksDockWidget")))
            probe.removeDockWidget(bookmarks);
        if (auto* stats = probe.findChild<QDockWidget*>(QStringLiteral("statisticsDockWidget")))
            stats->show(); // раскладка пользователя, а не умолчание
        geometry = probe.saveGeometry();
        state = probe.saveState();
        probe.hide();
    }

    const QString config = dir.filePath(QStringLiteral("config"));
    AppSettings::overrideConfigDirForTesting(config);
    QDir().mkpath(QDir(config).filePath(QStringLiteral("patterns")));
    // Своя схема пользователя — файл формата 0.5.0 (SchemaStore с тех пор не менялся).
    {
        QFile schema(QDir(config).filePath(QStringLiteral("patterns/my-python.json")));
        check(schema.open(QIODevice::WriteOnly), QStringLiteral("0.5.0 schema file is written"));
        schema.write(R"({"name":"My Python","version":3,"type":"dynamic-block-schema","linePrefix":"","blocks":[)"
                     R"({"name":"Level","kind":"regex","leadingText":"","closingText":"","separator":":","customRegex":"CRITICAL|ERROR|WARNING|INFO|DEBUG"},)"
                     R"({"name":"Logger","kind":"text","leadingText":"","closingText":"","separator":":","customRegex":""},)"
                     R"({"name":"Message","kind":"remainder","leadingText":"","closingText":"","separator":"","customRegex":""}]})");
    }
    QString mySchema;
    for (const SchemaStore::SchemaEntry& e : SchemaStore::loadAll())
        if (e.name == QLatin1String("My Python"))
            mySchema = e.schema;
    check(!mySchema.isEmpty(), QStringLiteral("0.5.0 schema file loads"));
    {
        // Ключи, которые писала 0.5.0 (AppSettings::save, MainWindow::saveSettings).
        QSettings s(AppSettings::iniFilePath(), QSettings::IniFormat);
        s.beginGroup(QStringLiteral("App"));
        s.setValue(QStringLiteral("wordWrap"), false);
        s.setValue(QStringLiteral("autoReload"), true);
        s.setValue(QStringLiteral("autoReloadIntervalSecs"), 3);
        s.setValue(QStringLiteral("fontSize"), 11);
        s.setValue(QStringLiteral("indexedThresholdMB"), 512);
        s.setValue(QStringLiteral("theme"), QStringLiteral("dark"));
        s.endGroup();
        s.beginGroup(QStringLiteral("Window"));
        s.setValue(QStringLiteral("geometry"), geometry);
        s.setValue(QStringLiteral("state"), state);
        s.endGroup();
        s.beginGroup(QStringLiteral("View"));
        s.setValue(QStringLiteral("conversionPattern"), mySchema);
        s.setValue(QStringLiteral("schemaOrder"), QStringList{QStringLiteral("My Python")});
        s.endGroup();
        s.beginGroup(QStringLiteral("RecentFiles"));
        s.setValue(QStringLiteral("files"), QStringList{log});
        s.endGroup();
        // Клавиши, которые пользователь 0.5.0 отдал своим командам и которые
        // в 0.6 стали умолчаниями новых: F2, Ctrl+Shift+T, Alt+←, Ctrl+Shift+L.
        s.beginGroup(QStringLiteral("Shortcuts"));
        s.setValue(QStringLiteral("goTo"), QStringLiteral("F2"));
        s.setValue(QStringLiteral("wordWrap"), QStringLiteral("Ctrl+Shift+T"));
        s.setValue(QStringLiteral("panelStatistics"), QStringLiteral("Alt+Left"));
        s.setValue(QStringLiteral("followTail"), QStringLiteral("Ctrl+Shift+L"));
        s.endGroup();
    }
    AppSettings::instance().load();
    check(AppSettings::instance().rememberFileState(), QStringLiteral("file memory is on after the upgrade"));
    check(!AppSettings::instance().restoreSessionOnStartup(), QStringLiteral("restore on startup is off after the upgrade"));
    check(AppSettings::instance().themeMode() == ThemeMode::Dark, QStringLiteral("0.5.0 theme choice is kept"));

    const int dialogsBefore = g_driver->closedByDefault().size();
    {
        MainWindow win;
        showWindow(win);
        win.startUp(OpenRouting::parseCommandLine({}), /*primary=*/true);
        testdocs::pump(200);
        check(g_driver->closedByDefault().size() == dialogsBefore,
              QStringLiteral("0.5.0 settings open without a dialog: %1")
                  .arg(g_driver->closedByDefault().mid(dialogsBefore).join(QStringLiteral("; "))));
        check(!statusText(win).contains(QLatin1String("rror")),
              QStringLiteral("no error in the status bar: %1").arg(statusText(win)));

        const auto keyOf = [&](const char* id) {
            QAction* a = command(win, QString::fromLatin1(id));
            return a ? a->shortcut().toString(QKeySequence::PortableText) : QStringLiteral("<no action>");
        };
        const std::pair<const char*, const char*> expected[] = {
            {"goTo", "F2"}, {"nextBookmark", ""}, {"prevBookmark", "Shift+F2"},
            {"wordWrap", "Ctrl+Shift+T"}, {"reopenTab", ""},
            {"panelStatistics", "Alt+Left"}, {"navigateBack", ""}, {"navigateForward", "Alt+Right"},
            {"followTail", "Ctrl+Shift+L"}, {"fullLog", ""},
            {"toggleBookmark", "Ctrl+F2"}, {"panelDirScanner", "Ctrl+F10"}, {"panelBookmarks", "Ctrl+F11"},
        };
        for (const auto& [id, key] : expected)
            check(keyOf(id) == QLatin1String(key), QStringLiteral("after the upgrade %1 is \"%2\" (got \"%3\")")
                                                     .arg(QLatin1String(id), QLatin1String(key), keyOf(id)));
        checkShortcutAmbiguity(win, QStringLiteral("upgrade from 0.5.0"));

        auto* bookmarksDock = win.findChild<QDockWidget*>(QStringLiteral("bookmarksDockWidget"));
        check(bookmarksDock && !bookmarksDock->isVisible(), QStringLiteral("new Bookmarks panel starts hidden"));
        auto* statsDock = win.findChild<QDockWidget*>(QStringLiteral("statisticsDockWidget"));
        check(statsDock && statsDock->isVisible(), QStringLiteral("0.5.0 dock layout is restored"));
        check(inRecentFiles(win, log), QStringLiteral("0.5.0 recent files are kept"));
        bool schemaSelected = false;
        for (QComboBox* combo : win.findChildren<QComboBox*>())
            if (combo->findText(QStringLiteral("My Python")) >= 0)
                schemaSelected = combo->currentText() == QLatin1String("My Python");
        check(schemaSelected, QStringLiteral("0.5.0 schema is listed and still selected"));

        win.openCommandLine(OpenRouting::parseCommandLine({log}));
        check(waitFor([&]() { return tabs(win)->count() == 1; }), QStringLiteral("a log opens after the upgrade"));
        check(waitAllLoaded(win), QStringLiteral("the log loads"));
        tabAt(win, 0)->view()->goToRow(10);
        pressCommand(win, QStringLiteral("toggleBookmark"));
        check(waitFor([&]() { return tabAt(win, 0)->bookmarks().count() == 1; }, 5000),
              QStringLiteral("Ctrl+F2 bookmarks a line after the upgrade"));
        pressCommand(win, QStringLiteral("quit"));
        check(waitFor([&]() { return !win.isVisible(); }, 10000), QStringLiteral("window closes"));
    }
    check(QFileInfo::exists(AppSettings::lastSessionFile()), QStringLiteral("exit writes last-session.json"));
    check(!QDir(AppSettings::fileMemoryDir()).entryList({QStringLiteral("*.json")}, QDir::Files).isEmpty(),
          QStringLiteral("file memory is written"));
    QSettings s(AppSettings::iniFilePath(), QSettings::IniFormat);
    check(s.value(QStringLiteral("Shortcuts/goTo")).toString() == QLatin1String("F2")
              && s.value(QStringLiteral("Shortcuts/wordWrap")).toString() == QLatin1String("Ctrl+Shift+T")
              && s.value(QStringLiteral("Shortcuts/panelStatistics")).toString() == QLatin1String("Alt+Left")
              && s.value(QStringLiteral("Shortcuts/followTail")).toString() == QLatin1String("Ctrl+Shift+L")
              && !s.contains(QStringLiteral("Shortcuts/nextBookmark")),
          QStringLiteral("the user's shortcuts are saved back unchanged"));
    check(s.value(QStringLiteral("App/theme")).toString() == QLatin1String("dark")
              && s.value(QStringLiteral("App/autoReloadIntervalSecs")).toInt() == 3
              && s.value(QStringLiteral("App/fontSize")).toInt() == 11,
          QStringLiteral("0.5.0 [App] values are saved back unchanged"));
    check(QFileInfo::exists(QDir(config).filePath(QStringLiteral("patterns/my-python.json"))),
          QStringLiteral("0.5.0 schema file stays"));
}

// ---------------------------------------------------------------------------
int runBackend(const QDir& root, bool indexed, const QString& instanceKey)
{
    const QString backend = indexed ? QStringLiteral("indexed") : QStringLiteral("resident");
    section(QStringLiteral("===== backend: %1").arg(backend));
    const QDir dir(root.filePath(backend));
    QDir().mkpath(dir.absolutePath());
    const Fixtures fx = makeFixtures(dir);

    AppSettings::overrideConfigDirForTesting(dir.filePath(QStringLiteral("config")));
    AppSettings& settings = AppSettings::instance();
    settings.load();
    settings.setIndexedThresholdMB(indexed ? 0 : 512);
    settings.setRestoreSessionOnStartup(false);
    // Запасная кодировка по умолчанию зависит от локали машины.
    settings.setFallbackEncodingChoice(TextEncoding::Windows1251);
    settings.save();

    int restoredTabs = 0;
    QVector<QSet<QString>> exitBookmarks;
    QString exitTabs;
    {
        MainWindow win;
        showWindow(win);
        win.startUp(OpenRouting::parseCommandLine({}), /*primary=*/true);

        section(QStringLiteral("shortcuts: empty window"));
        checkShortcutAmbiguity(win, QStringLiteral("empty window"));
        checkShortcutsReachable(win);
        checkEveryShortcutFires(win, QStringLiteral("empty window"));
        scenarioMenuSweep(win);
        check(tabs(win)->count() == 0, QStringLiteral("empty-window sweep opens no tab"));

        scenarioOpen(win, fx, indexed, instanceKey);
        scenarioOpenMore(win, fx);
        section(QStringLiteral("shortcuts: with tabs"));
        checkShortcutAmbiguity(win, QStringLiteral("with tabs"));
        scenarioNavigate(win, fx);
        scenarioTabSwitch(win, fx);
        scenarioFullLog(win, fx);
        scenarioGlue(win, fx, dir);
        scenarioSearchFollowsTab(win, fx);
        scenarioEncoding(win, fx);
        scenarioFileMemory(win, fx);
        scenarioMerge(win, fx);
        scenarioSession(win, fx);
        scenarioDocks(win);
        scenarioLineWrap(win, dir, indexed);
        section(QStringLiteral("shortcuts: every one fires"));
        checkEveryShortcutFires(win, QStringLiteral("toolbars shown"));
        checkEveryShortcutFires(win, QStringLiteral("toolbars hidden"), /*hideToolBars=*/true);
        {
            // Команда поиска при скрытом тулбаре Find показывает его: Ctrl+F
            // ставит фокус в поле, F3 и Alt+R не действуют вслепую.
            auto* field = win.findChild<QLineEdit*>(QStringLiteral("searchLineEdit"));
            for (const char* id : {"focusSearch", "searchNext", "findRegex"}) {
                setToolBarsVisible(win, false);
                pressCommand(win, QString::fromLatin1(id));
                check(waitFor([&]() { return field->isVisible(); }, 5000),
                      QStringLiteral("%1 shows the hidden Find toolbar").arg(QLatin1String(id)));
                if (QAction* a = command(win, QString::fromLatin1(id)); a && a->isCheckable())
                    a->trigger(); // переключатель — как был
            }
            setToolBarsVisible(win, false);
            pressCommand(win, QStringLiteral("focusSearch"));
            check(waitFor([&]() { return field->isVisible() && field->hasFocus(); }, 5000),
                  QStringLiteral("Ctrl+F focuses the field of the shown Find toolbar"));
        }
        setToolBarsVisible(win, true);
        scenarioMenuSweep(win);

        section(QStringLiteral("exit"));
        // Свип мог закрыть вкладки (Close Tab) — для сессии нужна хоть одна.
        if (tabs(win)->count() == 0) {
            win.openCommandLine(OpenRouting::parseCommandLine({fx.a, fx.b}));
            waitFor([&]() { return tabs(win)->count() == 2; });
        }
        check(waitAllLoaded(win), QStringLiteral("tabs loaded before exit"));
        restoredTabs = tabs(win)->count();
        exitBookmarks = tabBookmarks(win);
        exitTabs = describeTabs(win);
        QFile::remove(AppSettings::lastSessionFile());
        pressCommand(win, QStringLiteral("quit"));
        check(waitFor([&]() { return !win.isVisible(); }, 10000), QStringLiteral("Ctrl+Q closes the window"));
        check(QFileInfo::exists(AppSettings::lastSessionFile()),
              QStringLiteral("exit writes last-session.json"));
    }

    section(QStringLiteral("restore on startup"));
    settings.setRestoreSessionOnStartup(true);
    settings.save();
    {
        MainWindow win;
        showWindow(win);
        win.startUp(OpenRouting::parseCommandLine({}), true);
        check(waitFor([&]() { return tabs(win)->count() == restoredTabs; }),
              QStringLiteral("startup restores %1 tabs (got %2)").arg(restoredTabs).arg(tabs(win)->count()));
        check(waitAllLoaded(win), QStringLiteral("restored tabs load"));
        // Закладки сессии объединяются с памятью файла (docs/design/R-5.md):
        // восстановленная вкладка может получить и закладки другой вкладки
        // того же файла, но свои терять не должна.
        const QVector<QSet<QString>> restored = tabBookmarks(win);
        bool kept = restored.size() == exitBookmarks.size();
        for (int i = 0; kept && i < restored.size(); ++i)
            kept = restored[i].contains(exitBookmarks[i]);
        check(kept, QStringLiteral("restored tabs keep their bookmarks: %1 / at exit %2")
                        .arg(describeTabs(win), exitTabs));
        win.close();
    }

    section(QStringLiteral("secondary window"));
    {
        // --new-instance / stdin: окно не владеет прошлым сеансом.
        const QByteArray saved = [] {
            QFile f(AppSettings::lastSessionFile());
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        }();
        MainWindow win;
        showWindow(win);
        win.startUp(OpenRouting::parseCommandLine({QStringLiteral("--new-instance"), fx.b}), /*primary=*/false);
        check(waitFor([&]() { return tabs(win)->count() == 1; }), QStringLiteral("secondary window opens its file"));
        check(waitAllLoaded(win), QStringLiteral("secondary window loads"));
        win.close();
        QFile f(AppSettings::lastSessionFile());
        check(f.open(QIODevice::ReadOnly) && f.readAll() == saved,
              QStringLiteral("secondary window leaves last-session.json alone"));
    }

    section(QStringLiteral("restore link"));
    settings.setRestoreSessionOnStartup(false);
    settings.save();
    {
        MainWindow win;
        showWindow(win);
        win.startUp(OpenRouting::parseCommandLine({}), true);
        // startUp отработал, когда ссылка видна; до неё вкладок нет.
        auto* link = win.findChild<QLabel*>(QStringLiteral("restoreSessionLink"));
        check(link && waitFor([&]() { return link->isVisible(); }, 10000),
              QStringLiteral("welcome screen offers the previous session"));
        check(tabs(win)->count() == 0, QStringLiteral("no tabs restored with the setting off"));
        if (link && link->isVisible()) {
            emit link->linkActivated(QStringLiteral("restore"));
            check(waitFor([&]() { return tabs(win)->count() == restoredTabs; }),
                  QStringLiteral("restore link opens the previous session"));
            check(waitAllLoaded(win), QStringLiteral("previous session loads"));
            check(!link->isVisible(), QStringLiteral("restore link hides after use"));
        }
        win.close();
    }
    return 0;
}


// Q-20: окно --new-instance открыто первым и ключ одного экземпляра не
// занимает. Обычный запуск рядом с ним — первичное окно: принимает файлы
// третьего запуска и при выходе пишет last-session.json; отдельное окно
// файлов не получает и последнюю сессию не пишет. Окна и SingleInstance —
// как их собирает main.cpp.
void scenarioSeparateWindowFirst(const QDir& root, const QString& instanceKey)
{
    section(QStringLiteral("separate window first (Q-20)"));
    const QDir dir(root.filePath(QStringLiteral("separate-first")));
    QDir().mkpath(dir.absolutePath());
    const Fixtures fx = makeFixtures(dir);
    AppSettings::overrideConfigDirForTesting(dir.filePath(QStringLiteral("config")));
    AppSettings& settings = AppSettings::instance();
    settings.load();
    settings.setRestoreSessionOnStartup(false);
    settings.setFallbackEncodingChoice(TextEncoding::Windows1251);
    settings.save();
    const QString lastSession = AppSettings::lastSessionFile();
    QFile::remove(lastSession);
    const QString key = instanceKey + QStringLiteral("-separate");

    // Как main.cpp: роль решает SingleInstance, окно получает итог.
    const auto startWindow = [&](MainWindow& win, SingleInstance& guard, const OpenRouting::CommandLine& cl) {
        showWindow(win);
        if (guard.isPrimary())
            win.attachSingleInstance(&guard);
        win.startUp(cl, guard.isPrimary());
    };

    const OpenRouting::CommandLine separateCl =
        OpenRouting::parseCommandLine({QStringLiteral("--new-instance"), fx.a});
    SingleInstance separateGuard(key, separateCl);
    check(separateGuard.role() == SingleInstance::Role::Separate && !separateGuard.isPrimary(),
          QStringLiteral("--new-instance window does not take the single-instance key"));
    auto separate = std::make_unique<MainWindow>();
    startWindow(*separate, separateGuard, separateCl);
    check(waitFor([&]() { return tabIndexForFile(*separate, fx.a) >= 0; }),
          QStringLiteral("separate window opens its file"));

    const OpenRouting::CommandLine primaryCl = OpenRouting::parseCommandLine({fx.b});
    SingleInstance primaryGuard(key, primaryCl);
    check(primaryGuard.isPrimary(), QStringLiteral("plain launch next to it becomes the primary window"));
    auto primary = std::make_unique<MainWindow>();
    startWindow(*primary, primaryGuard, primaryCl);
    check(waitFor([&]() { return tabIndexForFile(*primary, fx.b) >= 0; }),
          QStringLiteral("primary window opens its file"));

    const OpenRouting::CommandLine thirdCl = OpenRouting::parseCommandLine({fx.c});
    SingleInstance third(key, thirdCl);
    check(third.role() == SingleInstance::Role::Forward, QStringLiteral("third launch forwards"));
    const QStringList forwarded = OpenRouting::forwardedArguments(thirdCl);
    QFuture<bool> sent = QtConcurrent::run([&third, forwarded]() { return third.sendToPrimary(forwarded); });
    check(waitFor([&]() { return sent.isFinished(); }), QStringLiteral("third launch send finishes"));
    check(sent.result(), QStringLiteral("third launch delivers its files"));
    check(waitFor([&]() { return tabIndexForFile(*primary, fx.c) >= 0; }),
          QStringLiteral("file of the third launch opens in the primary window"));
    check(tabIndexForFile(*separate, fx.c) < 0 && tabs(*separate)->count() == 1,
          QStringLiteral("separate window gets no files of the third launch"));
    check(waitAllLoaded(*primary) && waitAllLoaded(*separate), QStringLiteral("windows load"));

    separate->close();
    check(waitFor([&]() { return !separate->isVisible(); }), QStringLiteral("separate window closes"));
    separate.reset();
    check(!QFileInfo::exists(lastSession), QStringLiteral("separate window does not write last-session.json"));

    primary->close();
    check(waitFor([&]() { return !primary->isVisible(); }), QStringLiteral("primary window closes"));
    primary.reset();
    QFile f(lastSession);
    const QByteArray written = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    check(!written.isEmpty(), QStringLiteral("primary window writes last-session.json on exit"));
    check(written.contains(QFileInfo(fx.b).fileName().toUtf8())
              && written.contains(QFileInfo(fx.c).fileName().toUtf8())
              && !written.contains(QFileInfo(fx.a).fileName().toUtf8()),
          QStringLiteral("last session is the primary window's tabs"));
}

} // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("DendroLogSmoke"));
    QApplication::setApplicationName(QStringLiteral("DendroLogSmoke"));
    QApplication::setApplicationVersion(QStringLiteral("0.0.0"));
    QApplication::setStyle(new CompactStyle(QStyleFactory::create(QStringLiteral("Fusion"))));
    QStandardPaths::setTestModeEnabled(true);
    UpdateChecker::disableNetworkForTesting();

    QTemporaryDir root;
    if (!root.isValid()) {
        std::fprintf(stderr, "cannot create a temporary directory\n");
        return 2;
    }
    DialogDriver driver;
    g_driver = &driver;

    checkHelpShortcutTable();

    // Свой ключ одного экземпляра: не настоящий сокет DendroLog.
    const QString instanceKey = QStringLiteral("DendroLogSmoke-%1").arg(QCoreApplication::applicationPid());
    // Аргумент resident / indexed — один бэкенд, upgrade / separate — один
    // сценарий (для отладки).
    const QString only = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    for (const bool indexed : {false, true})
        if (only.isEmpty() || only == (indexed ? QLatin1String("indexed") : QLatin1String("resident")))
            runBackend(QDir(root.path()), indexed, instanceKey);
    if (only.isEmpty() || only == QLatin1String("upgrade"))
        scenarioUpgradeFrom050(QDir(root.path()));
    if (only.isEmpty() || only == QLatin1String("separate"))
        scenarioSeparateWindowFirst(QDir(root.path()), instanceKey);

    std::fprintf(stderr, "dialogs closed by the driver: %s\n",
                 qPrintable(driver.closedByDefault().join(QStringLiteral("; "))));
    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
