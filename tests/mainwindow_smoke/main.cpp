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
#include "apptheme.h"
#include "commandspooler.h"
#include "commandstatusbar.h"
#include "bookmarkstore.h"
#include "compactstyle.h"
#include "directoryscanner.h"
#include "encodingindicator.h"
#include "entrydetailspanel.h"
#include "filelegend.h"
#include "filememorystore.h"
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
#include "keyedfixtures.h"
#include "gzipbytes.h"
#include "tarbytes.h"
#include "zipbytes.h"
#include "directoryscannerpanel.h"
#include "archiveregistry.h"
#include "updatechecker.h"
#include "markerpanelwidget.h"
#include "spooldir.h"
#include "stdinspooler.h"
#include "viewidle.h"
#include "welcomewidget.h"
#include "colorschemecontroller.h"

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
#include <QProgressBar>
#include <QToolButton>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <QToolBar>
#include <QComboBox>
#include <QCheckBox>
#include <QThread>
#include <QDialogButtonBox>
#include <QTextBrowser>
#include <QClipboard>
#include <QDateTimeEdit>
#include <QInputDialog>
#include <QKeySequenceEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSpinBox>
#include <QTextDocumentFragment>
#include <QTreeWidget>
#include "timelinehistogramwidget.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

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
        // Имя источника, а не файла, откуда читают: у сжатого — временная
        // копия со своим номером в каждом запуске.
        QHash<QString, QString> names;
        for (const LogFilePtr& lf : tabAt(win, i)->loadedFiles())
            names.insert(lf->filePath, lf->shortName());
        for (const Bookmark& b : tabAt(win, i)->bookmarks().bookmarks())
            marks.insert(QStringLiteral("%1:%2").arg(names.value(b.filePath, QFileInfo(b.filePath).fileName())).arg(b.line));
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
    QString jsonl;        // JSON Lines (F-4): схема меняется на открытой вкладке
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
    // Файл, который один находит фильтр сканера по содержимому (P-9).
    testdocs::writeFile(scan, QStringLiteral("needle.log"),
        QByteArrayLiteral("2026-11-26 09:00:00.000 [main] INFO - NEEDLE-P9 found here\n"
                          "2026-11-26 09:00:01.000 [main] INFO - nothing else\n"));
    f.dir = scan.absolutePath();
    f.sessionPath = root.filePath(QStringLiteral("saved.dlsession"));
    f.jsonl = testdocs::writeFile(root, QStringLiteral("events.jsonl"),
                                  keyedfx::make(KeyedDefinition::Format::JsonObject, 40).bytes);
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
    // F-9: имя .csv — CSV тем же заданием: BOM, заголовок, запись на строку.
    {
        const QString csv = dir.filePath(QStringLiteral("exported.csv"));
        QFile::remove(csv);
        bool csvAsked = false;
        g_driver->expect([&](QWidget* w) {
            csvAsked = true;
            if (auto* dialog = qobject_cast<QFileDialog*>(w))
                typeFileName(dialog, csv);
            else
                DialogDriver::close(w);
        });
        bool csvDone = false;
        ViewExport::Result csvResult;
        const auto conn = QObject::connect(exportJob, &ViewExportJob::finished, &win,
                                           [&](const ViewExport::Result& r) { csvDone = true; csvResult = r; });
        pressCommand(win, QStringLiteral("saveAs"));
        check(waitFor([&]() { return csvAsked; }, 5000), QStringLiteral("Save View As asks for a CSV file"));
        check(waitFor([&]() { return csvDone; }, 60000) && csvResult.ok(),
              QStringLiteral("CSV Save View As finishes without error"));
        QObject::disconnect(conn);
        check(exportJob->format() == ViewExport::Format::Csv, QStringLiteral("a .csv name exports CSV"));
        QFile csvFile(csv);
        QByteArray csvBytes;
        if (csvFile.open(QIODevice::ReadOnly))
            csvBytes = csvFile.readAll();
        check(csvBytes.startsWith("\xEF\xBB\xBF_time,"),
              QStringLiteral("CSV starts with a BOM and the header: %1").arg(QString::fromUtf8(csvBytes.left(60))));
        check(csvBytes.count("\r\n") >= visible + 1,
              QStringLiteral("CSV has a record per visible line (%1 CRLF, %2 lines)").arg(csvBytes.count("\r\n")).arg(visible));
    }
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

    section(QStringLiteral("schema per tab"));
    auto* fieldsDock = win.findChild<QDockWidget*>(QStringLiteral("fieldVisibilityDockWidget"));
    auto* combo = fieldsDock->findChild<QComboBox*>();
    check(combo && combo->count() > 1, QStringLiteral("Log Fields offers schemas (%1)").arg(combo ? combo->count() : 0));
    QAction* applyToAll = win.findChild<QAction*>(QStringLiteral("applySchemaToAllTabs"));
    check(applyToAll, QStringLiteral("Apply Schema to All Tabs action"));
    const int smoke = combo ? combo->findText(QStringLiteral("Smoke Fields")) : -1;
    check(smoke >= 0, QStringLiteral("the smoke schema is in the catalogue"));
    LogViewWidget* b = tabAt(win, ib);
    if (combo && smoke >= 0 && applyToAll && b) {
        const int other = smoke == 0 ? 1 : 0;
        QCheckBox* filterBlocks = nullptr;
        const auto columnBoxes = [&]() {
            QList<QCheckBox*> found;
            for (QCheckBox* cb : fieldsDock->findChildren<QCheckBox*>()) {
                if (cb->text() == QLatin1String("Filter blocks"))
                    filterBlocks = cb;
                else if (!cb->isTristate() && cb->isVisibleTo(fieldsDock))
                    found << cb;
            }
            return found;
        };
        // beta — другая схема.
        tabs(win)->setCurrentIndex(ib);
        combo->setCurrentIndex(other);
        check(waitAllLoaded(win), QStringLiteral("beta settles after its schema"));
        const QString betaSchema = b->parserPattern();
        // alpha — схема фикстур, «Filter blocks», колонка Level.
        tabs(win)->setCurrentIndex(ia);
        combo->setCurrentIndex(smoke);
        check(waitAllLoaded(win), QStringLiteral("alpha settles after its schema"));
        check(a->parserPattern() == testdocs::buildSchema() && b->parserPattern() == betaSchema,
              QStringLiteral("the combo changes only the active tab"));
        columnBoxes();
        if (filterBlocks && !filterBlocks->isChecked())
            filterBlocks->click();
        check(waitAllLoaded(win), QStringLiteral("alpha settles after Filter blocks"));
        for (QCheckBox* cb : columnBoxes())
            if (cb->isChecked() != (cb->text() == QLatin1String("Level")))
                cb->click();
        check(model->fieldDisplayFilterEnabled() && model->visibleFieldIndexes() == QVector<int>{2},
              QStringLiteral("alpha shows the Level column"));
        // Фильтр по полю через Text Filters — привязан к схеме alpha.
        auto* textDock = win.findChild<QDockWidget*>(QStringLiteral("textFilterDockWidget"));
        FilterPanelWidget* filterPanel = textDock->findChild<FilterPanelWidget*>();
        QAbstractButton* applyRules = nullptr;
        QAbstractButton* resetRules = nullptr;
        for (QAbstractButton* btn : filterPanel->findChildren<QAbstractButton*>()) {
            if (btn->text().endsWith(QStringLiteral("Apply")))
                applyRules = btn;
            else if (btn->text().endsWith(QStringLiteral("Reset")))
                resetRules = btn;
        }
        FilterRuleSet byLevel;
        FilterRule levelRule;
        levelRule.text = QStringLiteral("ERROR");
        levelRule.fieldName = QStringLiteral("Level");
        byLevel.rules << levelRule;
        filterPanel->setRuleSet(byLevel);
        if (applyRules)
            applyRules->click();
        check(testdocs::settle(*model) && !model->filterRules().fieldMissing(0)
                  && model->rowCount() > 0 && model->rowCount() < allRows,
              QStringLiteral("field filter on alpha (%1 of %2)").arg(model->rowCount()).arg(allRows));
        const int errorRows = model->rowCount();

        // beta: схема меняется ещё раз, Filter blocks — alpha не трогается.
        tabs(win)->setCurrentIndex(ib);
        check(combo->currentIndex() == other, QStringLiteral("the combo follows the active tab (beta)"));
        columnBoxes();
        check(filterBlocks && !filterBlocks->isChecked(), QStringLiteral("Filter blocks follows the tab (beta: off)"));
        combo->setCurrentIndex(smoke);
        check(waitAllLoaded(win), QStringLiteral("beta settles"));
        combo->setCurrentIndex(other);
        if (filterBlocks)
            filterBlocks->click();
        check(waitAllLoaded(win), QStringLiteral("beta settles after Filter blocks"));
        check(a->parserPattern() == testdocs::buildSchema() && model->fieldDisplayFilterEnabled()
                  && model->visibleFieldIndexes() == QVector<int>{2}
                  && !model->filterRules().fieldMissing(0) && model->rowCount() == errorRows,
              QStringLiteral("alpha's columns and field filter survive beta's schema changes"));
        tabs(win)->setCurrentIndex(ia);
        check(combo->currentIndex() == smoke, QStringLiteral("the combo follows the active tab (alpha)"));
        QStringList checkedColumns;
        for (QCheckBox* cb : columnBoxes())
            if (cb->isChecked())
                checkedColumns << cb->text();
        check(filterBlocks && filterBlocks->isChecked() && checkedColumns == QStringList{QStringLiteral("Level")},
              QStringLiteral("Log Fields shows alpha's Filter blocks and columns (%1)").arg(checkedColumns.join(',')));

        // Новая вкладка — схема по умолчанию: последний выбор в доке (на
        // beta — её схема с Filter blocks); закрыть и вернуть — её схема.
        auto* scanner = win.findChild<DirectoryScanner*>();
        const int before = tabs(win)->count();
        // gone уже открывался: его память файла со своей схемой — убрать
        // (новый файл вытеснил бы alpha из Recent Files, его ждут дальше).
        const QString fresh = fx.gone;
        QFile::remove(QDir(AppSettings::configDir()).filePath(
            QStringLiteral("memory/") + FileMemoryStore::fileNameForPath(fresh)));
        if (scanner)
            emit scanner->fileActivated(fresh);
        check(waitFor([&]() { return tabIndexForFile(win, fresh) >= 0; }) && waitAllLoaded(win),
              QStringLiteral("a new tab opens"));
        const int ig = tabIndexForFile(win, fresh);
        LogViewWidget* g = ig >= 0 ? tabAt(win, ig) : nullptr;
        check(g && g->parserPattern() == b->parserPattern()
                  && g->fieldSettings().extraction == b->fieldSettings().extraction,
              QStringLiteral("the new tab takes the default schema (the last choice)"));
        if (g) {
            tabs(win)->setCurrentIndex(ig);
            combo->setCurrentIndex(smoke);
            check(waitAllLoaded(win), QStringLiteral("new tab settles after its schema"));
            const QString closedSchema = g->parserPattern();
            pressCommand(win, QStringLiteral("closeTab"));
            check(waitFor([&]() { return tabs(win)->count() == before; }), QStringLiteral("the new tab closes"));
            pressCommand(win, QStringLiteral("reopenTab"));
            check(waitFor([&]() { return tabIndexForFile(win, fresh) >= 0; }) && waitAllLoaded(win),
                  QStringLiteral("Ctrl+Shift+T reopens it"));
            const int ir = tabIndexForFile(win, fresh);
            check(ir >= 0 && tabAt(win, ir)->parserPattern() == closedSchema,
                  QStringLiteral("the reopened tab keeps its schema"));
            if (ir >= 0) {
                tabs(win)->setCurrentIndex(ir);
                pressCommand(win, QStringLiteral("closeTab"));
                waitFor([&]() { return tabs(win)->count() == before; });
            }
        }

        // «Apply Schema to All Tabs» — схема alpha на всех вкладках.
        tabs(win)->setCurrentIndex(ia);
        applyToAll->trigger();
        check(waitAllLoaded(win), QStringLiteral("tabs settle after Apply Schema to All Tabs"));
        bool all = true;
        for (int i = 0; i < tabs(win)->count(); ++i) {
            LogViewWidget* t = tabAt(win, i);
            all = all && t->parserPattern() == testdocs::buildSchema() && t->model()->fieldDisplayFilterEnabled()
                && t->model()->visibleFieldIndexes() == QVector<int>{2};
        }
        check(all, QStringLiteral("Apply Schema to All Tabs reaches every tab"));
        check(model->rowCount() == errorRows, QStringLiteral("alpha keeps its field filter"));

        // Как было: без фильтра, первая схема и Filter blocks выключен на всех.
        if (resetRules)
            resetRules->click();
        check(testdocs::settle(*model) && model->rowCount() == allRows, QStringLiteral("text filter reset"));
        combo->setCurrentIndex(0);
        for (QCheckBox* cb : columnBoxes())
            if (!cb->isChecked())
                cb->click();
        if (filterBlocks && filterBlocks->isChecked())
            filterBlocks->click();
        applyToAll->trigger();
        check(waitAllLoaded(win) && model->rowCount() == allRows, QStringLiteral("schema back to the first one"));
    }

    // «Filter blocks» и колонки дока Log Fields — на активной вкладке (F-5);
    // кнопка Fields тулбара Filters — та же галочка (FieldSchemaController).
    if (combo && combo->count() > 0) {
        QCheckBox* filterBlocks = nullptr;
        // Чекбоксы колонок окно может пересоздать при смене схемы и флага —
        // искать заново после каждого такого шага.
        const auto columnBoxes = [&]() {
            QList<QCheckBox*> found;
            for (QCheckBox* cb : fieldsDock->findChildren<QCheckBox*>()) {
                if (cb->text() == QLatin1String("Filter blocks"))
                    filterBlocks = cb;
                else if (!cb->isTristate() && cb->isVisibleTo(fieldsDock))
                    found << cb;
            }
            return found;
        };
        QList<QCheckBox*> columns = columnBoxes();
        check(filterBlocks && !columns.isEmpty(),
              QStringLiteral("Log Fields shows Filter blocks and the columns (%1)").arg(columns.size()));
        QAction* fieldsStatus = filtersToolBarAction(win, QStringLiteral("Fields"));
        if (filterBlocks && !columns.isEmpty() && fieldsStatus) {
            const bool wasOn = filterBlocks->isChecked();
            if (!wasOn)
                filterBlocks->click();
            check(waitAllLoaded(win), QStringLiteral("tabs settle after Filter blocks"));
            columns = columnBoxes();
            const auto everyTab = [&](const std::function<bool(LogModel*)>& pred) {
                return pred(model); // схема на вкладку: действует на активную
            };
            check(fieldsStatus->isChecked()
                      && everyTab([](LogModel* m) { return m->fieldDisplayFilterEnabled(); }),
                  QStringLiteral("Filter blocks reaches the active tab and the Fields button"));
            for (QCheckBox* cb : columns)
                if (!cb->isChecked())
                    cb->click();
            const int all = int(columns.size());
            columns.first()->click(); // снять первую колонку
            check(everyTab([&](LogModel* m) {
                      return m->visibleFieldIndexes().size() == all - 1
                          && !m->visibleFieldIndexes().contains(0);
                  }),
                  QStringLiteral("column choice reaches the active tab"));
            columns.first()->click();
            check(everyTab([&](LogModel* m) { return m->visibleFieldIndexes().size() == all; }),
                  QStringLiteral("column choice comes back"));
            fieldsStatus->trigger();
            check(waitAllLoaded(win) && !filterBlocks->isChecked() && !fieldsStatus->isChecked()
                      && everyTab([](LogModel* m) { return !m->fieldDisplayFilterEnabled(); }),
                  QStringLiteral("Fields button turns Filter blocks off on the active tab"));
            check(model->rowCount() == allRows, QStringLiteral("Filter blocks keeps the rows"));
            if (wasOn)
                filterBlocks->click();
        }
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

// Сжатый лог (F-2, шаг 2): из командной строки — вкладкой с именем архива,
// путь архива в подсказке и недавних, копия — в спуле каталога настроек
// теста, кнопка отмены распаковки после загрузки скрыта, закрытие — копии нет.
void scenarioCompressed(MainWindow& win, const Fixtures& fx)
{
    section(QStringLiteral("compressed file"));
    const int before = tabs(win)->count();
    QFile source(fx.a);
    source.open(QIODevice::ReadOnly);
    const QByteArray bytes = source.readAll();
    const QString gz = QFileInfo(fx.a).dir().filePath(QStringLiteral("alpha-copy.log.gz"));
    {
        QFile out(gz);
        out.open(QIODevice::WriteOnly);
        out.write(testgzip::gzip(bytes));
    }
    win.openCommandLine(OpenRouting::parseCommandLine({gz}));
    check(waitFor([&]() { return tabs(win)->count() == before + 1; }), QStringLiteral(".gz opens a tab"));
    const int index = tabs(win)->count() - 1;
    LogViewWidget* tab = tabAt(win, index);
    check(waitLoaded(tab), QStringLiteral(".gz loads"));
    check(tab->model()->rowCount() == bytes.count('\n') + (bytes.endsWith('\n') ? 0 : 1),
          QStringLiteral(".gz rows equal the plain file"));
    check(tabs(win)->tabText(index) == QLatin1String("alpha-copy.log.gz"), QStringLiteral(".gz tab name"));
    check(tabs(win)->tabToolTip(index).contains(QDir::toNativeSeparators(gz)), QStringLiteral(".gz tab tooltip shows the archive"));
    check(inRecentFiles(win, gz), QStringLiteral(".gz path is in Recent Files"));
    LogFilePtr lf = tab->loadedFiles().value(0);
    check(lf && lf->filePath.startsWith(AppSettings::spoolRoot()), QStringLiteral(".gz copy in the test spool"));
    auto* cancel = win.findChild<QToolButton*>(QStringLiteral("cancelUnpackButton"));
    check(cancel && !cancel->isVisible(), QStringLiteral("Cancel Unpacking hidden after the load"));
    // Ушли на другую вкладку и вернулись посреди распаковки: процент приходит
    // — полоса прогресса снова видна (ревью шагов 2–3). Сигналы — как от вкладки.
    auto* bar = win.findChild<QProgressBar*>(QStringLiteral("statusProgressBar"));
    if (index > 0 && bar) {
        emit tab->unpackingStarted(lf);
        check(bar->isVisible(), QStringLiteral("unpacking progress bar shown"));
        tabs(win)->setCurrentIndex(0);
        check(!bar->isVisible(), QStringLiteral("unpacking progress bar hidden on another tab"));
        tabs(win)->setCurrentIndex(index);
        emit tab->unpackingProgress(lf, 40);
        check(bar->isVisible() && bar->value() == 40, QStringLiteral("unpacking progress bar back with the tab"));
        emit tab->unpackingFinished(lf, true);
        check(!bar->isVisible(), QStringLiteral("unpacking progress bar hidden after the end"));
    } else {
        check(false, QStringLiteral("status progress bar and a second tab"));
    }
    const QString copy = lf ? lf->filePath : QString();
    lf.reset(); // копию держит последний LogFile
    tabs(win)->setCurrentIndex(index);
    command(win, QStringLiteral("closeTab"))->trigger();
    check(waitFor([&]() { return tabs(win)->count() == before; }), QStringLiteral(".gz tab closes"));
    check(waitFor([&]() { return !QFileInfo::exists(copy); }, 10000), QStringLiteral(".gz copy removed with the tab"));
}


// zip (F-2, шаг 4): из командной строки, броска, диалога Open и недавних —
// в Directory Scanner извлечённым; файл оттуда — вкладка члена архива, в
// недавних — архив; «назад» возвращает прежнюю папку.
void scenarioZip(MainWindow& win, const Fixtures& fx)
{
    section(QStringLiteral("zip archive"));
    QFile source(fx.a);
    source.open(QIODevice::ReadOnly);
    const QByteArray bytes = source.readAll();
    const QDir root = QFileInfo(fx.a).dir();
    const QString zip = root.filePath(QStringLiteral("bundle.zip"));
    const QByteArray zipBytes = testzip::zip({ testzip::file("logs/alpha.log", bytes),
                                               testzip::file("logs/notes.md", "not a log\n") });
    {
        QFile out(zip);
        out.open(QIODevice::WriteOnly);
        out.write(zipBytes);
    }
    auto* panel = win.findChild<DirectoryScannerPanel*>();
    auto* scanner = win.findChild<DirectoryScanner*>();
    auto* dock = win.findChild<QDockWidget*>(QStringLiteral("directoryScannerDockWidget"));
    if (!panel || !scanner || !dock) {
        check(false, QStringLiteral("scanner panel and dock"));
        return;
    }
    const int before = tabs(win)->count();
    win.openCommandLine(OpenRouting::parseCommandLine({zip}));
    check(waitFor([&]() { return !panel->isExtractingArchive() && panel->archivePath() == zip; }),
          QStringLiteral("command line: zip goes to the scanner"));
    check(dock->isVisible(), QStringLiteral("scanner dock shown for the archive"));
    check(tabs(win)->count() == before, QStringLiteral("zip opens no tab"));
    check(inRecentFiles(win, zip), QStringLiteral("zip in Recent Files"));
    const QString extracted = scanner->rootPath() + QStringLiteral("/logs/alpha.log");
    check(scanner->rootPath().startsWith(AppSettings::spoolRoot()), QStringLiteral("archive extracted into the test spool"));
    check(QFileInfo::exists(extracted), QStringLiteral("log member extracted"));
    check(!QFileInfo::exists(scanner->rootPath() + QStringLiteral("/logs/notes.md")), QStringLiteral("non-log member skipped"));

    emit scanner->fileActivated(extracted); // двойной щелчок в дереве
    check(waitFor([&]() { return tabs(win)->count() == before + 1; }), QStringLiteral("member opens a tab"));
    const int index = tabs(win)->count() - 1;
    LogViewWidget* tab = tabAt(win, index);
    check(waitLoaded(tab), QStringLiteral("member loads"));
    check(tab->model()->rowCount() == bytes.count('\n') + (bytes.endsWith('\n') ? 0 : 1),
          QStringLiteral("member rows equal the plain file"));
    check(tabs(win)->tabText(index) == QLatin1String("alpha.log"), QStringLiteral("member tab name"));
    check(tabs(win)->tabToolTip(index).contains(QDir::toNativeSeparators(zip) + QStringLiteral(" › logs/alpha.log")),
          QStringLiteral("member tooltip: archive › member (%1)").arg(tabs(win)->tabToolTip(index)));
    const LogFilePtr lf = tab->loadedFiles().value(0);
    check(lf && lf->source().member == QLatin1String("logs/alpha.log"), QStringLiteral("tab source is the member"));
    check(!inRecentFiles(win, extracted), QStringLiteral("extracted path not in Recent Files"));
    check(inRecentFiles(win, zip), QStringLiteral("archive stays in Recent Files"));
    tabs(win)->setCurrentIndex(index);
    command(win, QStringLiteral("closeTab"))->trigger();
    check(waitFor([&]() { return tabs(win)->count() == before; }), QStringLiteral("member tab closes"));

    // Назад — к папке до архива.
    win.openCommandLine(OpenRouting::parseCommandLine({fx.dir}));
    check(waitFor([&]() { return scanner->rootPath() == QDir::cleanPath(fx.dir); }), QStringLiteral("folder scanned"));
    dropPaths(win, {zip});
    check(waitFor([&]() { return !panel->isExtractingArchive() && panel->archivePath() == zip; }),
          QStringLiteral("dropped zip goes to the scanner"));
    const QString dropRoot = scanner->rootPath();
    auto* back = panel->findChild<QToolButton*>(QStringLiteral("scannerBackButton"));
    check(back && back->isVisible(), QStringLiteral("back button shown"));
    if (back)
        back->click();
    check(scanner->rootPath() == QDir::cleanPath(fx.dir), QStringLiteral("back returns to the folder"));
    check(waitFor([&]() { return !QFileInfo::exists(dropRoot); }, 10000), QStringLiteral("extraction removed after back"));

    // zip под чужим именем — по сигнатуре, через Open.
    const QString disguised = root.filePath(QStringLiteral("bundle.data"));
    {
        QFile out(disguised);
        out.open(QIODevice::WriteOnly);
        out.write(zipBytes);
    }
    bool asked = false;
    g_driver->expect([&](QWidget* w) {
        asked = true;
        if (auto* dialog = qobject_cast<QFileDialog*>(w))
            typeFileName(dialog, disguised);
        else
            DialogDriver::close(w);
    });
    pressCommand(win, QStringLiteral("open"));
    check(waitFor([&]() { return asked && !panel->isExtractingArchive() && panel->archivePath() == disguised; }),
          QStringLiteral("Open: zip by signature goes to the scanner"));
    check(tabs(win)->count() == before, QStringLiteral("Open of a zip opens no tab"));

    // Битый архив — сообщение в строке статуса, вид прежний.
    const QString broken = root.filePath(QStringLiteral("broken.zip"));
    {
        QFile out(broken);
        out.open(QIODevice::WriteOnly);
        out.write(zipBytes.left(zipBytes.size() - 8));
    }
    win.openCommandLine(OpenRouting::parseCommandLine({broken}));
    auto* status = win.findChild<QLabel*>(QStringLiteral("statusMessageLabel"));
    check(waitFor([&]() { return !panel->isExtractingArchive(); }), QStringLiteral("broken zip finishes"));
    check(panel->archivePath() == disguised, QStringLiteral("broken zip keeps the shown archive"));
    check(!status || status->text().contains(QStringLiteral("broken.zip")),
          QStringLiteral("broken zip reported: %1").arg(status ? status->text() : QString()));
    if (back)
        back->click();
}

// tar (F-2, шаг 5): `.tar.gz` из командной строки и `.gz` с tar внутри под
// чужим именем — в сканер, член — вкладкой; `.gz` с одним логом — вкладка.
void scenarioTar(MainWindow& win, const Fixtures& fx)
{
    section(QStringLiteral("tar archive"));
    QFile source(fx.a);
    source.open(QIODevice::ReadOnly);
    const QByteArray bytes = source.readAll();
    const QDir root = QFileInfo(fx.a).dir();
    const QByteArray tarBytes = testtar::tar({ testtar::file("logs/alpha.log", bytes),
                                               testtar::file("logs/notes.md", "not a log\n") });
    const auto write = [&](const QString& name, const QByteArray& data) {
        const QString path = root.filePath(name);
        QFile out(path);
        out.open(QIODevice::WriteOnly);
        out.write(data);
        return path;
    };
    const QString tgz = write(QStringLiteral("tar-bundle.tar.gz"), testgzip::gzip(tarBytes));
    const QString disguised = write(QStringLiteral("tar-support.gz"), testgzip::gzip(tarBytes));
    auto* panel = win.findChild<DirectoryScannerPanel*>();
    auto* scanner = win.findChild<DirectoryScanner*>();
    if (!panel || !scanner) {
        check(false, QStringLiteral("scanner panel"));
        return;
    }
    const int before = tabs(win)->count();
    win.openCommandLine(OpenRouting::parseCommandLine({tgz}));
    check(waitFor([&]() { return !panel->isExtractingArchive() && panel->archivePath() == tgz; }),
          QStringLiteral("command line: .tar.gz goes to the scanner"));
    check(tabs(win)->count() == before, QStringLiteral(".tar.gz opens no tab"));
    check(inRecentFiles(win, tgz), QStringLiteral(".tar.gz in Recent Files"));
    const QString extracted = scanner->rootPath() + QStringLiteral("/logs/alpha.log");
    check(QFileInfo::exists(extracted), QStringLiteral("tar member extracted"));
    check(!QFileInfo::exists(scanner->rootPath() + QStringLiteral("/logs/notes.md")), QStringLiteral("tar: non-log member skipped"));
    emit scanner->fileActivated(extracted);
    check(waitFor([&]() { return tabs(win)->count() == before + 1; }), QStringLiteral("tar member opens a tab"));
    const int index = tabs(win)->count() - 1;
    LogViewWidget* tab = tabAt(win, index);
    check(waitLoaded(tab), QStringLiteral("tar member loads"));
    check(tab->model()->rowCount() == bytes.count('\n') + (bytes.endsWith('\n') ? 0 : 1),
          QStringLiteral("tar member rows equal the plain file"));
    check(tabs(win)->tabToolTip(index).contains(QDir::toNativeSeparators(tgz) + QStringLiteral(" › logs/alpha.log")),
          QStringLiteral("tar member tooltip: archive › member (%1)").arg(tabs(win)->tabToolTip(index)));
    tabs(win)->setCurrentIndex(index);
    command(win, QStringLiteral("closeTab"))->trigger();
    check(waitFor([&]() { return tabs(win)->count() == before; }), QStringLiteral("tar member tab closes"));

    // `.gz` с tar внутри под чужим именем — по содержимому, броском.
    dropPaths(win, {disguised});
    check(waitFor([&]() { return !panel->isExtractingArchive() && panel->archivePath() == disguised; }),
          QStringLiteral("dropped .gz with a tar inside goes to the scanner"));
    check(tabs(win)->count() == before, QStringLiteral(".gz with a tar inside opens no tab"));
    // Архив, узнанный по содержимому, вместе с папкой (ревью шагов 4–5): у
    // сканера один корень — первый по порядку, папка названа пропущенной;
    // извлечение архива не отменяется молча.
    if (auto* back = panel->findChild<QToolButton*>(QStringLiteral("scannerBackButton")); back && back->isVisible())
        back->click();
    auto* status = win.findChild<QLabel*>(QStringLiteral("statusMessageLabel"));
    dropPaths(win, {disguised, fx.dir});
    check(waitFor([&]() { return !panel->isExtractingArchive() && panel->archivePath() == disguised; }),
          QStringLiteral("archive by content dropped with a folder: the first one is scanned (%1)").arg(scanner->rootPath()));
    check(status && status->text().contains(QStringLiteral("skipped")),
          QStringLiteral("archive by content with a folder: the folder is named skipped (%1)").arg(status ? status->text() : QString()));
    check(tabs(win)->count() == before, QStringLiteral("archive by content with a folder opens no tab"));
    // Архив, выбранный в дереве сканера (двойной щелчок), — в сканер, без
    // пустой вкладки.
    emit scanner->fileActivated(tgz);
    check(waitFor([&]() { return !panel->isExtractingArchive() && panel->archivePath() == tgz; }),
          QStringLiteral("archive activated in the scanner tree is opened in the scanner"));
    check(tabs(win)->count() == before, QStringLiteral("archive activated in the scanner tree opens no tab"));
    // `.gz` с одним логом — по-прежнему вкладка.
    const QString single = write(QStringLiteral("tar-single.log.gz"), testgzip::gzip(bytes));
    win.openCommandLine(OpenRouting::parseCommandLine({single}));
    check(waitFor([&]() { return tabs(win)->count() == before + 1; }), QStringLiteral(".gz with one log opens a tab"));
    LogViewWidget* singleTab = tabAt(win, tabs(win)->count() - 1);
    check(waitLoaded(singleTab) && singleTab->model()->rowCount() == bytes.count('\n') + (bytes.endsWith('\n') ? 0 : 1),
          QStringLiteral(".gz with one log loads as text"));
    tabs(win)->setCurrentIndex(tabs(win)->count() - 1);
    command(win, QStringLiteral("closeTab"))->trigger();
    check(waitFor([&]() { return tabs(win)->count() == before; }), QStringLiteral(".gz tab closes"));
    if (auto* back = panel->findChild<QToolButton*>(QStringLiteral("scannerBackButton")); back && back->isVisible())
        back->click();
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
        // Locate File… — пропавший файл указан на новом месте (P-9).
        auto* list = report ? report->findChild<QTreeWidget*>(QStringLiteral("sessionReportList")) : nullptr;
        auto* locate = report ? report->findChild<QPushButton*>(QStringLiteral("sessionReportLocate")) : nullptr;
        check(list && list->topLevelItemCount() == 1 && list->topLevelItem(0)->text(1).contains(QStringLiteral("gone.log")),
              QStringLiteral("the report names the missing file"));
        check(locate && locate->isEnabled(), QStringLiteral("Locate File is enabled for the missing file"));
        if (list && locate && locate->isEnabled()) {
            bool asked = false;
            g_driver->expect([&](QWidget* w) {
                asked = true;
                if (auto* dialog = qobject_cast<QFileDialog*>(w))
                    typeFileName(dialog, hidden);
                else
                    DialogDriver::close(w);
            });
            locate->click();
            check(waitFor([&]() { return asked && tabIndexForFile(win, hidden) >= tabsInSession; }),
                  QStringLiteral("Locate File opens the file in the session tab (%1)").arg(describeTabs(win)));
            g_driver->dropExpectations();
            check(tabIndexForFile(win, hidden) >= 0 && waitLoaded(tabAt(win, tabIndexForFile(win, hidden)), 50),
                  QStringLiteral("the located file loads"));
            check(waitFor([&]() { return list->topLevelItemCount() == 1 && !locate->isEnabled(); }, 5000),
                  QStringLiteral("the report marks the file as located: %1")
                      .arg(list->topLevelItemCount() ? list->topLevelItem(0)->text(2) : QString()));
        }
        if (report)
            report->close();
    }
    check(waitAllLoaded(win), QStringLiteral("session tabs load"));
    // Вкладки второй сессии закрыли — отчёт пуст, ссылка гаснет.
    for (int i = tabs(win)->count() - 1; i >= tabsInSession; --i) {
        const int n = tabs(win)->count();
        tabs(win)->setCurrentIndex(i);
        command(win, QStringLiteral("closeTab"))->trigger();
        waitFor([&]() { return tabs(win)->count() == n - 1; });
    }
    check(!link || waitFor([&]() { return !link->isVisible(); }, 5000),
          QStringLiteral("report link hides once the tabs with problems are closed"));
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

// Цвета ANSI (F-3): коды не видны в тексте и не мешают уровню; цвет фона
// ANSI в отрисовке вкладки — по настройке: View → ANSI Colors и Settings
// выключают и включают его на открытой вкладке.
static int ansiBgPixels(LogListView* view)
{
    const QColor red = AppTheme::ansiColor(1, view->palette());
    const QImage image = view->viewport()->grab().toImage();
    int count = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            count += QColor(image.pixel(x, y)).rgb() == red.rgb();
    return count;
}

void scenarioAnsiColors(MainWindow& win, const QDir& dir)
{
    section(QStringLiteral("ANSI colors"));
    AppSettings::instance().setAnsiColors(true); // свип меню мог выключить
    QByteArray bytes;
    for (int i = 0; i < 6; ++i)
        bytes += "\x1b[2m2026-03-05 10:00:1" + QByteArray::number(i) + ".000\x1b[0m [w] "
               + (i == 2 ? QByteArray("\x1b[41;97mERROR\x1b[0m ") : QByteArray("\x1b[32mINFO\x1b[0m "))
               + "line " + QByteArray::number(i) + " \x1b]0;title\x07" "end\n";
    const QString path = testdocs::writeFile(dir, QStringLiteral("ansi-colors.log"), bytes);
    const int before = tabs(win)->count();
    win.openCommandLine(OpenRouting::parseCommandLine({path}));
    check(waitFor([&]() { return tabIndexForFile(win, path) >= 0; }), QStringLiteral("colored log opens"));
    tabs(win)->setCurrentIndex(tabIndexForFile(win, path));
    LogViewWidget* tab = currentTab(win);
    check(waitLoaded(tab, 6), QStringLiteral("colored log loads"));
    LogModel* model = tab->model();
    check(model->messageAt(2) == QStringLiteral("2026-03-05 10:00:12.000 [w] ERROR line 2 end")
              && model->visibleLevelAt(2) == LogLevel::Error,
          QStringLiteral("escape codes are not in the text, the colored level word counts"));
    LogListView* view = tab->view();
    view->goToRow(0);
    const auto settle = [&]() {
        for (LogListView* v : win.findChildren<LogListView*>())
            waitFor([v]() { return !v->hasPendingLayout(); }, 5000);
        QCoreApplication::processEvents();
    };
    settle();
    QAction* ansi = win.findChild<QAction*>(QStringLiteral("ansiColorsAction"));
    check(ansi && menuAction(win, QStringLiteral("ANSI Colors")) == ansi && ansi->isChecked(),
          QStringLiteral("View → ANSI Colors exists and is on by default"));
    check(view->ansiColorsEnabled() && ansiBgPixels(view) > 0,
          QStringLiteral("the ANSI background is drawn (%1 px)").arg(ansiBgPixels(view)));
    if (ansi) {
        ansi->trigger();
        settle();
        bool all = true;
        for (LogListView* v : win.findChildren<LogListView*>())
            all = all && !v->ansiColorsEnabled();
        check(!AppSettings::instance().ansiColors() && all && ansiBgPixels(view) == 0,
              QStringLiteral("View → ANSI Colors turns the colors off in every view"));
        check(model->messageAt(2).startsWith(QStringLiteral("2026-03-05")),
              QStringLiteral("codes stay hidden with the colors off"));
        ansi->trigger();
        settle();
        check(AppSettings::instance().ansiColors() && ansiBgPixels(view) > 0,
              QStringLiteral("and on again"));
    }
    // Settings → Appearance: тот же выключатель.
    for (const bool on : {false, true}) {
        bool shown = false;
        g_driver->expect([&](QWidget* w) {
            shown = true;
            auto* box = w->findChild<QCheckBox*>(QStringLiteral("ansiColorsCheckBox"));
            auto* buttons = w->findChild<QDialogButtonBox*>();
            if (box && buttons && buttons->button(QDialogButtonBox::Ok)) {
                box->setChecked(on);
                buttons->button(QDialogButtonBox::Ok)->click();
            } else {
                DialogDriver::close(w);
            }
        });
        pressCommand(win, QStringLiteral("settings"));
        check(waitFor([&]() { return shown && !QApplication::activeModalWidget(); }, 5000),
              QStringLiteral("Settings opens for ANSI colors"));
        settle();
        check(AppSettings::instance().ansiColors() == on && view->ansiColorsEnabled() == on
                  && (ansiBgPixels(view) > 0) == on && (!ansi || ansi->isChecked() == on),
              QStringLiteral("Settings → Show ANSI colors %1").arg(on ? "on" : "off"));
    }
    if (QAction* close = command(win, QStringLiteral("closeTab")))
        close->trigger();
    check(waitFor([&]() { return tabs(win)->count() == before; }), QStringLiteral("colored tab closes"));
}

// View → File Labels (U-23): режим меток — на всех вкладках и в выдаче Search.
void scenarioFileLabels(MainWindow& win)
{
    section(QStringLiteral("file labels"));
    const struct { const char* id; FileLabelMode mode; } modes[] = {
        {"fileLabelsColorOnly", FileLabelMode::ColorOnly},
        {"fileLabelsHidden", FileLabelMode::Hidden},
        {"fileLabelsName", FileLabelMode::Name},
    };
    for (const auto& m : modes) {
        QAction* a = command(win, QString::fromLatin1(m.id));
        if (!a)
            continue;
        a->trigger();
        check(a->isChecked() && AppSettings::instance().fileLabelMode() == m.mode,
              QStringLiteral("%1 sets the mode").arg(QLatin1String(m.id)));
        bool all = tabs(win)->count() > 0;
        for (int i = 0; i < tabs(win)->count(); ++i)
            all = all && tabAt(win, i)->view()->fileLabelMode() == m.mode;
        for (LogListView* v : win.findChildren<LogListView*>())
            all = all && v->fileLabelMode() == m.mode;
        check(all, QStringLiteral("%1 reaches every log view").arg(QLatin1String(m.id)));
    }
    check(menuAction(win, QStringLiteral("Color Only")) == command(win, QStringLiteral("fileLabelsColorOnly")),
          QStringLiteral("View → File Labels → Color Only"));

    // Фильтр по файлу (U-23, часть 4): на слитой вкладке щелчок по чипу
    // легенды скрывает файл — горит индикатор Files тулбара Filters; Reset
    // Filters of This Tab снимает и его.
    {
        int merged = -1;
        for (int i = 0; i < tabs(win)->count() && merged < 0; ++i)
            if (tabAt(win, i)->fileCount() >= 2)
                merged = i;
        check(merged >= 0, QStringLiteral("a merged tab is open for the file filter"));
        if (merged >= 0) {
            tabs(win)->setCurrentIndex(merged);
            LogViewWidget* tab = currentTab(win);
            check(waitLoaded(tab), QStringLiteral("merged tab loaded"));
            QAction* files = nullptr;
            if (auto* tb = win.findChild<QToolBar*>(QStringLiteral("filtersToolBar")))
                for (QAction* a : tb->actions())
                    if (plainText(a->text()) == QStringLiteral("Files"))
                        files = a;
            check(files && !files->isChecked(), QStringLiteral("Filters → Files is off"));
            FileLegend* legend = tab->fileLegend();
            check(legend && legend->chipCount() >= 2 && legend->toggleFile(0, false),
                  QStringLiteral("a legend click hides a file"));
            check(waitLoaded(tab, 0) && files && waitFor([&]() { return files->isChecked(); }, 5000),
                  QStringLiteral("Filters → Files lights up when a file is hidden"));
            pressCommand(win, QStringLiteral("resetFilters"));
            check(waitFor([&]() { return tab->model()->hiddenFiles().isEmpty() && !files->isChecked(); }, 5000),
                  QStringLiteral("Reset Filters of This Tab shows the hidden file again"));
        }
    }

    // View → Line Numbers (U-23, часть 3): по умолчанию выключены, пункт
    // включает колонку номеров у всех view и пишет [View] lineNumbers.
    QAction* numbers = command(win, QStringLiteral("lineNumbers"));
    check(numbers, QStringLiteral("View → Line Numbers exists"));
    const auto settledViews = [&win]() {
        for (LogListView* v : win.findChildren<LogListView*>())
            waitFor([v]() { return !v->hasPendingLayout(); }, 5000);
    };
    if (numbers) {
        check(!numbers->isChecked() && !AppSettings::instance().lineNumbers(), QStringLiteral("line numbers are off by default"));
        numbers->trigger();
        settledViews();
        bool all = true;
        for (LogListView* v : win.findChildren<LogListView*>())
            all = all && v->lineNumbersVisible();
        check(numbers->isChecked() && all && tabs(win)->count() > 0
                  && currentTab(win)->view()->lineNumberWidth() > 0,
              QStringLiteral("View → Line Numbers shows the numbers in every log view"));
        numbers->trigger();
        settledViews();
        check(!AppSettings::instance().lineNumbers() && currentTab(win)->view()->lineNumberWidth() == 0,
              QStringLiteral("and hides them again"));
    }
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
        {QStringLiteral("File labels: Name / Color Only / Hidden"),
         {QStringLiteral("fileLabelsName"), QStringLiteral("fileLabelsColorOnly"), QStringLiteral("fileLabelsHidden")}},
        {QStringLiteral("Show / hide line numbers"), {QStringLiteral("lineNumbers")}},
        {QStringLiteral("Show / hide the time delta column"), {QStringLiteral("timeDelta")}},
        {QStringLiteral("Set / clear time zero"), {QStringLiteral("timeZero")}},
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
            // Команда без сочетания по умолчанию — строка справки с «—».
            const QKeySequence seq = sm.defaultSequence(id);
            if (!seq.isEmpty())
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
// Вывод команды (F-7): File → Open Command Output…, диалог, вкладка-поток,
// код возврата на плашке и в имени вкладки, недавние команды, закрытие
// вкладки завершает процесс и убирает спул. Команда — сам этот exe в режиме
// генератора (`--emit-lines`).
QString selfCommand(const QString& args)
{
    return QLatin1Char('"') + QDir::toNativeSeparators(QCoreApplication::applicationFilePath())
        + QStringLiteral("\" ") + args;
}

void runCommandDialog(MainWindow& win, const QString& line, bool* shown)
{
    g_driver->expect([line, shown](QWidget* w) {
        *shown = w->objectName() == QLatin1String("commandOutputDialog");
        auto* combo = w->findChild<QComboBox*>(QStringLiteral("commandLineCombo"));
        auto* buttons = w->findChild<QDialogButtonBox*>();
        if (!*shown || !combo || !buttons) {
            DialogDriver::close(w);
            return;
        }
        combo->setEditText(line);
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    QAction* action = win.findChild<QAction*>(QStringLiteral("actionOpenCommandOutput"));
    check(action && menuActions(win).contains(action), QStringLiteral("File menu has Open Command Output"));
    if (action)
        action->trigger();
}

void scenarioCommand(MainWindow& win)
{
    section(QStringLiteral("command output"));
    const int before = tabs(win)->count();
    bool shown = false;
    runCommandDialog(win, selfCommand(QStringLiteral("--emit-lines 40")), &shown);
    check(waitFor([&]() { return shown && tabs(win)->count() == before + 1; }),
          QStringLiteral("Run in the dialog opens a command tab"));
    LogViewWidget* tab = tabAt(win, tabs(win)->count() - 1);
    CommandSpooler* cmd = tab ? tab->commands().value(0) : nullptr;
    check(cmd != nullptr, QStringLiteral("the tab owns the command"));
    if (!tab || !cmd)
        return;
    check(tab->autoReload() && tab->view()->followTail(), QStringLiteral("command tab: auto-reload and follow-tail"));
    check(waitFor([&]() { return !cmd->isRunning() && tab->model()->rowCount() == 40; }),
          QStringLiteral("command lines arrive (%1 rows)").arg(tab->model()->rowCount()));
    const int index = tabs(win)->indexOf(tab);
    check(waitFor([&]() { return tabs(win)->tabText(index).endsWith(QStringLiteral("\u25A0")); }, 5000),
          QStringLiteral("finished command is marked in the tab name: %1").arg(tabs(win)->tabText(index)));
    check(tabs(win)->tabToolTip(index).contains(QStringLiteral("exited with code 0")),
          QStringLiteral("tab tooltip names the exit code"));
    auto* bar = tab->findChild<CommandStatusBar*>(QStringLiteral("commandStatusBar"));
    check(bar && bar->text().contains(QStringLiteral("exited with code 0")), QStringLiteral("bar names the exit code"));
    check(!inRecentFiles(win, cmd->spoolFilePath()), QStringLiteral("spool is not a recent file"));
    const QList<CommandSpec> recent = CommandHistory::fromStrings(AppSettings::instance().recentCommands());
    check(!recent.isEmpty() && recent.first().commandLine == selfCommand(QStringLiteral("--emit-lines 40")),
          QStringLiteral("command is in the recent commands"));

    // Живая команда: закрытие вкладки завершает процесс и убирает спул.
    shown = false;
    runCommandDialog(win, selfCommand(QStringLiteral("--emit-lines -1")), &shown);
    check(waitFor([&]() { return shown && tabs(win)->count() == before + 2; }),
          QStringLiteral("second command opens its own tab"));
    LogViewWidget* live = tabAt(win, tabs(win)->count() - 1);
    CommandSpooler* liveCmd = live ? live->commands().value(0) : nullptr;
    check(liveCmd && waitFor([&]() { return liveCmd->processId() > 0 && live->model()->rowCount() > 5; }),
          QStringLiteral("live command streams"));
    if (!liveCmd)
        return;
    QPointer<CommandSpooler> guard(liveCmd);
    const QString spool = liveCmd->spoolFilePath();
    tabs(win)->setCurrentIndex(tabs(win)->count() - 1);
    command(win, QStringLiteral("closeTab"))->trigger();
    check(waitFor([&]() { return guard.isNull(); }, 15000), QStringLiteral("closing the tab stops the process"));
    check(waitFor([&]() { return !QFileInfo::exists(spool); }, 10000), QStringLiteral("spool removed with the tab"));

    const QString finishedSpool = cmd->spoolFilePath();
    tabs(win)->setCurrentIndex(tabs(win)->indexOf(tab));
    command(win, QStringLiteral("closeTab"))->trigger();
    // Закрытие из сценария идёт вне цикла событий: deleteLater спулера
    // доставляется явно (в программе — сам цикл).
    check(waitFor([&]() {
              QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
              return tabs(win)->count() == before && !QFileInfo::exists(finishedSpool);
          }, 10000),
          QStringLiteral("finished command tab closes with its spool"));
}

// ---------------------------------------------------------------------------
// P-9: то, что смоук P-8 не проходил.
// ---------------------------------------------------------------------------

// Следующее всплывающее меню: выбрать пункт по тексту (без «&» и
// многоточий; prefix — по началу), как с клавиатуры. *enabled — состояние
// найденного пункта; выключенный не выбирается, меню закрывается.
void pickMenuItem(const QString& text, bool* picked, bool* enabled = nullptr, bool prefix = false)
{
    g_driver->expect([=](QWidget* w) {
        auto* menu = qobject_cast<QMenu*>(w);
        for (QAction* a : menu ? menu->actions() : QList<QAction*>()) {
            const QString t = plainText(a->text());
            if (prefix ? !t.startsWith(text) : t != text)
                continue;
            if (enabled)
                *enabled = a->isEnabled();
            if (!a->isEnabled())
                break;
            *picked = true;
            menu->setActiveAction(a);
            QTest::keyClick(menu, Qt::Key_Return);
            return;
        }
        DialogDriver::close(w);
    });
}

// Правый щелчок по строке лога (по тексту, а не по жёлобу).
void rowContextMenu(LogViewWidget* tab, int row, int x = -1)
{
    LogListView* v = tab->view();
    const QRect rect = v->visualRect(tab->model()->index(row, 0));
    const QPoint at(x >= 0 ? x : v->textLeft() + 8, rect.center().y());
    QContextMenuEvent e(QContextMenuEvent::Mouse, at, v->viewport()->mapToGlobal(at));
    QApplication::sendEvent(v->viewport(), &e);
}

// Правый щелчок по вкладке.
void tabContextMenu(MainWindow& win, int index)
{
    QTabBar* bar = tabs(win)->tabBar();
    const QPoint at = bar->tabRect(index).center();
    QContextMenuEvent e(QContextMenuEvent::Mouse, at, bar->mapToGlobal(at));
    QApplication::sendEvent(bar, &e);
}

QAbstractButton* buttonEndingWith(QWidget* parent, const QString& text)
{
    for (QAbstractButton* b : parent->findChildren<QAbstractButton*>())
        if (b->text().endsWith(text))
            return b;
    check(false, QStringLiteral("%1 has a button '%2'").arg(parent->objectName(), text));
    return nullptr;
}

QDockWidget* dock(MainWindow& win, const char* name)
{
    auto* d = win.findChild<QDockWidget*>(QString::fromLatin1(name));
    check(d, QStringLiteral("window has dock %1").arg(QLatin1String(name)));
    return d;
}

bool showDock(MainWindow& win, QDockWidget* d, bool visible = true)
{
    if (d->isVisible() != visible)
        d->toggleViewAction()->trigger();
    return waitFor([&]() { return d->isVisible() == visible; }, 5000);
}

// Первая строка не раньше from, в показанном тексте которой есть word
// (с учётом регистра: её слово выделяется двойным щелчком).
int rowWithText(LogModel* model, const QString& word, int from = 0)
{
    for (int r = from; r < model->rowCount(); ++r)
        if (model->data(model->index(r, 0), Qt::DisplayRole).toString().contains(word))
            return r;
    return -1;
}

int rowsWithText(LogModel* model, const QString& word)
{
    int n = 0;
    for (int r = 0; r < model->rowCount(); ++r)
        if (model->entryAt(r)->message().contains(word, Qt::CaseInsensitive))
            ++n;
    return n;
}

// Двойной щелчок по слову word в строке row — выделение текста мышью, как у
// пользователя. Позиция — по метрике шрифта view (не по ширине «M»).
QString selectWord(LogViewWidget* tab, int row, const QString& word, int* xOut = nullptr)
{
    LogListView* v = tab->view();
    v->goToRow(row);
    testdocs::idleViews();
    const QString text = tab->model()->data(tab->model()->index(row, 0), Qt::DisplayRole).toString();
    const int col = text.indexOf(word);
    if (col < 0)
        return QString();
    const QFontMetricsF fm(v->font());
    const QRect rect = v->visualRect(tab->model()->index(row, 0));
    const int x = v->textLeft() + 4
        + int(fm.horizontalAdvance(text.left(col)) + fm.horizontalAdvance(word.left(2)));
    const QPoint at(x, rect.top() + fm.height() / 2 + 1);
    QTest::mouseDClick(v->viewport(), Qt::LeftButton, Qt::NoModifier, at);
    if (xOut)
        *xOut = x;
    // Выделенное — через Ctrl+C, как его видит пользователь.
    QApplication::clipboard()->clear();
    QKeyEvent copyKey(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QApplication::sendEvent(v, &copyKey);
    return QApplication::clipboard()->text();
}

// F-4, PR 3: ключевая схема на открытой вкладке. Выбор встроенной JSON Lines
// перечитывает файл — момент и уровень из ключей, Entry Details — все ключи
// строки; регексная схема (и «Apply Schema to All Tabs» с ней) — обратно
// эвристика; JSON Lines всем вкладкам — снова ключи.
void scenarioKeyedSchema(MainWindow& win, const Fixtures& fx)
{
    section(QStringLiteral("keyed schema on an open tab"));
    auto* fieldsDock = win.findChild<QDockWidget*>(QStringLiteral("fieldVisibilityDockWidget"));
    auto* combo = fieldsDock ? fieldsDock->findChild<QComboBox*>() : nullptr;
    QAction* applyToAll = win.findChild<QAction*>(QStringLiteral("applySchemaToAllTabs"));
    auto* scanner = win.findChild<DirectoryScanner*>();
    const int jsonLines = combo ? combo->findText(QStringLiteral("JSON Lines")) : -1;
    const int smoke = combo ? combo->findText(QStringLiteral("Smoke Fields")) : -1;
    check(jsonLines >= 0 && smoke >= 0 && applyToAll && scanner,
          QStringLiteral("JSON Lines and the smoke schema are in the catalogue"));
    if (jsonLines < 0 || smoke < 0 || !applyToAll || !scanner)
        return;
    const auto setFilterBlocks = [&](bool on) {
        for (QCheckBox* cb : fieldsDock->findChildren<QCheckBox*>())
            if (cb->text() == QLatin1String("Filter blocks") && cb->isChecked() != on)
                cb->click();
    };

    // Вторая вкладка — обычный лог (для «Apply Schema to All Tabs»).
    if (tabIndexForFile(win, fx.b) < 0) {
        emit scanner->fileActivated(fx.b);
        check(waitFor([&]() { return tabIndexForFile(win, fx.b) >= 0; }) && waitAllLoaded(win),
              QStringLiteral("a plain log opens in a tab"));
    }
    const int before = tabs(win)->count();
    emit scanner->fileActivated(fx.jsonl);
    check(waitFor([&]() { return tabIndexForFile(win, fx.jsonl) >= 0; }) && waitAllLoaded(win),
          QStringLiteral("the JSON Lines file opens in a tab"));
    const int ij = tabIndexForFile(win, fx.jsonl);
    LogViewWidget* t = ij >= 0 ? tabAt(win, ij) : nullptr;
    if (!t)
        return;
    tabs(win)->setCurrentIndex(ij);
    LogModel* m = t->model();
    // Запись 2 фикстур (строка 3): эпоха в мс, pino 50 — эвристика её не видит.
    const qint64 epochLine = 3;
    const qint64 keyMoment = keyedfx::baseMs() + 2000;
    const auto rowOfEpoch = [&]() {
        for (int row = 0; row < m->rowCount(); ++row)
            if (m->keyAt(row).line == epochLine)
                return row;
        return -1;
    };
    const auto momentOfEpoch = [&]() {
        const int row = rowOfEpoch();
        const QDateTime ts = row >= 0 ? m->visibleTimestampAt(row) : QDateTime();
        return ts.isValid() ? ts.toMSecsSinceEpoch() : kNoTimestampMs;
    };
    // Эвристика: строка с эпохой — продолжение записи над ней (её момент).
    check(rowOfEpoch() >= 0 && momentOfEpoch() != keyMoment,
          QStringLiteral("without the schema the epoch is no moment"));

    combo->setCurrentIndex(jsonLines);
    setFilterBlocks(true);
    check(waitAllLoaded(win), QStringLiteral("the tab settles after JSON Lines"));
    check(momentOfEpoch() == keyMoment && rowOfEpoch() >= 0
              && m->visibleLevelAt(rowOfEpoch()) == LogLevel::Error,
          QStringLiteral("JSON Lines on the open tab: moment and level from the keys"));

    // Entry Details: все ключи строки, не только колонки схемы.
    auto* detailsDock = win.findChild<QDockWidget*>(QStringLiteral("entryDetailsDockWidget"));
    auto* browser = detailsDock ? detailsDock->findChild<QTextBrowser*>() : nullptr;
    check(browser, QStringLiteral("Entry Details has a browser"));
    if (detailsDock && browser) {
        const bool wasVisible = detailsDock->isVisible();
        detailsDock->show();
        t->view()->goToRow(rowOfEpoch());
        check(waitFor([&]() {
                  const QString text = browser->toPlainText();
                  return text.contains(QStringLiteral("Keys (4)")) && text.contains(QStringLiteral("worker-3"));
              }),
              QStringLiteral("Entry Details lists every key of the line: %1")
                  .arg(browser->toPlainText().left(300)));
        if (!wasVisible)
            detailsDock->hide();
    }

    combo->setCurrentIndex(smoke);
    check(waitAllLoaded(win) && momentOfEpoch() != keyMoment,
          QStringLiteral("keyed → regex on the open tab: back to the heuristic"));
    combo->setCurrentIndex(jsonLines);
    check(waitAllLoaded(win) && momentOfEpoch() == keyMoment,
          QStringLiteral("regex → keyed on the open tab: moments from the keys again"));

    // «Apply Schema to All Tabs»: регексная схема — на вкладку JSON
    // Lines тоже, затем JSON Lines — на все (обычный лог — beta).
    const int ib = tabIndexForFile(win, fx.b);
    LogViewWidget* plainTab = ib >= 0 ? tabAt(win, ib) : nullptr;
    if (!plainTab)
        return;
    tabs(win)->setCurrentIndex(ib);
    combo->setCurrentIndex(smoke);
    setFilterBlocks(true);
    applyToAll->trigger();
    check(waitAllLoaded(win) && momentOfEpoch() != keyMoment,
          QStringLiteral("Apply Schema to All Tabs (regex) rereads the keyed tab"));
    const int plainRows = plainTab->model()->rowCount();
    tabs(win)->setCurrentIndex(tabIndexForFile(win, fx.jsonl));
    combo->setCurrentIndex(jsonLines);
    applyToAll->trigger();
    check(waitAllLoaded(win) && momentOfEpoch() == keyMoment,
          QStringLiteral("Apply Schema to All Tabs (JSON Lines) rereads with the keys"));
    check(plainTab->parserPattern() == t->parserPattern() && plainTab->model()->rowCount() == plainRows,
          QStringLiteral("the plain log under JSON Lines keeps its lines"));

    // Как было: первая схема без Filter blocks на всех вкладках.
    combo->setCurrentIndex(0);
    setFilterBlocks(false);
    applyToAll->trigger();
    check(waitAllLoaded(win), QStringLiteral("schema back to the first one"));
    tabs(win)->setCurrentIndex(tabIndexForFile(win, fx.jsonl));
    pressCommand(win, QStringLiteral("closeTab"));
    check(waitFor([&]() { return tabs(win)->count() == before; }), QStringLiteral("the JSON Lines tab closes"));
}


// Схема файла слитой вкладки (F-5, PR 3): «Applies to» в доке Log Fields —
// только у вкладки из нескольких файлов; выбор файла — комбобокс меняет его
// схему (JSON Lines у файла JSON Lines, регексная — у обычного лога), «All
// files» при разных схемах — «(mixed)»; выбор схемы всем файлам снимает свои.
void scenarioFileSchemas(MainWindow& win, const Fixtures& fx)
{
    section(QStringLiteral("schema per file in a merged tab"));
    auto* fieldsDock = win.findChild<QDockWidget*>(QStringLiteral("fieldVisibilityDockWidget"));
    auto* combo = fieldsDock ? fieldsDock->findChild<QComboBox*>() : nullptr;
    auto* target = fieldsDock ? fieldsDock->findChild<QComboBox*>(QStringLiteral("schemaTargetCombo"))
                              : nullptr;
    auto* scanner = win.findChild<DirectoryScanner*>();
    const int jsonLines = combo ? combo->findText(QStringLiteral("JSON Lines")) : -1;
    const int smoke = combo ? combo->findText(QStringLiteral("Smoke Fields")) : -1;
    check(combo && target && jsonLines >= 0 && smoke >= 0 && scanner,
          QStringLiteral("Log Fields has the schema and Applies to combos"));
    if (!combo || !target || jsonLines < 0 || smoke < 0 || !scanner)
        return;
    const auto setFilterBlocks = [&](bool on) {
        for (QCheckBox* cb : fieldsDock->findChildren<QCheckBox*>())
            if (cb->text() == QLatin1String("Filter blocks") && cb->isChecked() != on)
                cb->click();
    };

    const int before = tabs(win)->count();
    // Своя вкладка: копия обычного лога (вкладки alpha и beta нужны дальше).
    const QString plain = QFileInfo(fx.a).dir().filePath(QStringLiteral("file-schemas.log"));
    QFile::remove(plain);
    check(QFile::copy(fx.a, plain), QStringLiteral("a copy of the plain log"));
    emit scanner->fileActivated(plain);
    check(waitFor([&]() { return tabIndexForFile(win, plain) >= 0; }) && waitAllLoaded(win),
          QStringLiteral("a plain log opens in a tab"));
    const int ip = tabIndexForFile(win, plain);
    LogViewWidget* t = ip >= 0 ? tabAt(win, ip) : nullptr;
    if (!t)
        return;
    tabs(win)->setCurrentIndex(ip);
    combo->setCurrentIndex(smoke);
    setFilterBlocks(true);
    check(waitAllLoaded(win), QStringLiteral("the plain tab settles with the smoke schema"));
    check(!target->isVisibleTo(fieldsDock), QStringLiteral("a one-file tab has no Applies to"));

    // JSON Lines уже открывался (его память файла принесла бы свою схему —
    // это проверяет document_state): начать с чистого.
    QFile::remove(QDir(AppSettings::configDir()).filePath(
        QStringLiteral("memory/") + FileMemoryStore::fileNameForPath(fx.jsonl)));
    t->addLogFile(fx.jsonl);
    check(waitAllLoaded(win) && t->fileCount() == 2, QStringLiteral("the JSON Lines file joins the tab"));
    check(target->isVisibleTo(fieldsDock) && target->count() == 3
              && target->currentText() == QStringLiteral("All files"),
          QStringLiteral("Applies to: All files and each file (%1 items)").arg(target->count()));
    LogFilePtr jsonFile, plainFile;
    for (const LogFilePtr& f : t->loadedFiles()) {
        if (f && LogViewWidget::samePath(f->filePath, fx.jsonl))
            jsonFile = f;
        else if (f)
            plainFile = f;
    }
    if (!jsonFile || !plainFile)
        return;
    LogModel* m = t->model();
    // Запись 2 фикстуры JSON Lines (строка 3): эпоха в мс, pino 50.
    const qint64 keyMoment = keyedfx::baseMs() + 2000;
    const auto momentOfEpoch = [&]() {
        for (int row = 0; row < m->rowCount(); ++row) {
            const RecordKey key = m->keyAt(row);
            if (key.line == 3 && key.file == jsonFile) {
                const QDateTime ts = m->visibleTimestampAt(row);
                return ts.isValid() ? ts.toMSecsSinceEpoch() : kNoTimestampMs;
            }
        }
        return std::numeric_limits<qint64>::max(); // строки нет
    };
    check(momentOfEpoch() != keyMoment, QStringLiteral("the JSON Lines file reads with the tab's regex schema"));

    target->setCurrentIndex(target->findData(jsonFile->filePath));
    check(combo->currentIndex() == smoke, QStringLiteral("the combo shows the file's schema (the tab's)"));
    combo->setCurrentIndex(jsonLines);
    check(waitAllLoaded(win), QStringLiteral("the tab settles after the file's schema"));
    check(t->fileSchema(jsonFile).schemaName == QStringLiteral("JSON Lines")
              && t->fileSchema(plainFile).schemaName == QStringLiteral("Smoke Fields")
              && t->fieldSettings().schemaName == QStringLiteral("Smoke Fields"),
          QStringLiteral("each file has its own schema"));
    check(momentOfEpoch() == keyMoment, QStringLiteral("the JSON Lines file: moments from the keys"));
    check(m->availableFields().contains(QStringLiteral("Thread")) && m->availableFields().contains(QStringLiteral("Level")),
          QStringLiteral("the tab's columns are the union (%1)").arg(m->availableFields().join(',')));

    target->setCurrentIndex(target->findData(plainFile->filePath));
    check(combo->currentIndex() == smoke, QStringLiteral("the combo shows the plain file's schema"));
    target->setCurrentIndex(0);
    check(combo->currentIndex() == -1 && combo->placeholderText() == QStringLiteral("(mixed)"),
          QStringLiteral("All files with different schemas — (mixed): %1").arg(combo->currentText()));

    // Соседняя вкладка и обратно: цель — снова все файлы.
    target->setCurrentIndex(target->findData(jsonFile->filePath));
    const int other = ip == 0 ? 1 : 0;
    if (other < tabs(win)->count()) {
        tabs(win)->setCurrentIndex(other);
        check(!target->isVisibleTo(fieldsDock) || tabAt(win, other)->fileCount() > 1,
              QStringLiteral("Applies to follows the active tab"));
        tabs(win)->setCurrentIndex(ip);
        check(target->currentIndex() == 0, QStringLiteral("back on the merged tab: All files"));
    }

    // Схема всем файлам — свои снимаются.
    target->setCurrentIndex(0);
    combo->setCurrentIndex(smoke);
    check(waitAllLoaded(win) && !t->hasFileSchemas() && combo->currentIndex() == smoke
              && momentOfEpoch() != keyMoment,
          QStringLiteral("a schema for All files drops the files' own schemas"));

    tabs(win)->setCurrentIndex(tabIndexForFile(win, plain));
    pressCommand(win, QStringLiteral("closeTab"));
    check(waitFor([&]() { return tabs(win)->count() == before; }), QStringLiteral("the merged tab closes"));
}

// Контекстное меню лога над выделенным текстом (U-…): найти, Find All,
// оставить / скрыть строки с ним, маркер.
void scenarioTextActions(MainWindow& win, const Fixtures& fx)
{
    section(QStringLiteral("log context menu over selected text"));
    tabs(win)->setCurrentIndex(tabIndexForFile(win, fx.a));
    LogViewWidget* tab = currentTab(win);
    LogModel* model = tab->model();
    const int allRows = model->rowCount();
    const QString word = QStringLiteral("Disk");
    const int expected = rowsWithText(model, word);
    check(expected > 0 && expected < allRows, QStringLiteral("alpha has lines with %1 (%2)").arg(word).arg(expected));

    // Выделить слово и выбрать пункт меню над ним.
    const auto menuOverWord = [&](const QString& item) {
        const int row = rowWithText(model, word, allRows / 3);
        int x = -1;
        const QString selected = row >= 0 ? selectWord(tab, row, word, &x) : QString();
        check(selected == word, QStringLiteral("double click selects '%1' (got '%2')").arg(word, selected));
        bool picked = false;
        pickMenuItem(item + QStringLiteral(" “"), &picked, nullptr, /*prefix=*/true);
        rowContextMenu(tab, row, x); // меню — вложенным циклом внутри события
        g_driver->dropExpectations();
        check(picked, QStringLiteral("context menu offers '%1 “%2”'").arg(item, word));
        return row;
    };

    // Find «…» — быстрый поиск этого текста, литералом.
    {
        auto* field = win.findChild<QLineEdit*>(QStringLiteral("searchLineEdit"));
        const int from = menuOverWord(QStringLiteral("Find"));
        check(waitFor([&]() {
            const auto e = selectedEntry(tab);
            return !tab->isQuickSearchRunning() && e && tab->view()->userSelectedRow() > from
                && e->message().contains(word);
        }), QStringLiteral("Find selects the next line with the text"));
        check(field && field->text() == word, QStringLiteral("Find puts the text into the search field: %1").arg(field ? field->text() : QString()));
        check(!command(win, QStringLiteral("findRegex"))->isChecked(), QStringLiteral("Find searches the text literally"));
    }

    // Find All «…» — панель Search.
    {
        auto* searchDock = dock(win, "searchResultsDockWidget");
        menuOverWord(QStringLiteral("Find All"));
        check(waitFor([&]() { return searchDock->isVisible(); }), QStringLiteral("Find All shows the Search panel"));
        LogListView* resultsView = nullptr;
        for (LogListView* v : searchDock->findChildren<LogListView*>())
            resultsView = v;
        auto* resultsModel = resultsView ? qobject_cast<LogModel*>(resultsView->model()) : nullptr;
        check(resultsModel && waitFor([&]() { return !resultsModel->isFiltering() && resultsModel->rowCount() == expected; }),
              QStringLiteral("Find All lists every line with the text (%1 expected, %2)")
                  .arg(expected).arg(resultsModel ? resultsModel->rowCount() : -1));
        showDock(win, searchDock, false);
    }

    // Show Only Lines with «…» — правило в Text Filters, применено к вкладке.
    {
        auto* textDock = dock(win, "textFilterDockWidget");
        // Панель описывает вкладку (обе пусты) — правило идёт в панель.
        if (auto* panel = textDock->findChild<FilterPanelWidget*>())
            panel->setRuleSet(FilterRuleSet());
        menuOverWord(QStringLiteral("Show Only Lines with"));
        check(testdocs::settle(*model), QStringLiteral("include filter settles"));
        bool allMatch = model->rowCount() > 0;
        for (int r = 0; r < model->rowCount() && allMatch; ++r) {
            const auto entry = model->entryAt(r);
            allMatch = entry->message().contains(word, Qt::CaseInsensitive)
                || !entry->timestamp().isValid() || entry->message().trimmed().isEmpty();
        }
        check(allMatch && model->rowCount() < allRows,
              QStringLiteral("Show Only Lines with keeps the lines with the text (%1 of %2)").arg(model->rowCount()).arg(allRows));
        check(textDock->isVisible(), QStringLiteral("Show Only Lines with shows Text Filters"));
        FilterPanelWidget* panel = textDock->findChild<FilterPanelWidget*>();
        const FilterRuleSet panelRules = panel ? panel->ruleSet() : FilterRuleSet();
        check(panelRules == model->filterRules() && !panelRules.rules.isEmpty()
                  && panelRules.rules.constLast().text == word,
              QStringLiteral("the rule is in Text Filters and applied to the tab"));
        check(filtersToolBarAction(win, QStringLiteral("Text"))->isChecked(), QStringLiteral("Filters → Text is checked"));
        pressCommand(win, QStringLiteral("resetFilters"));
        check(testdocs::settle(*model) && model->rowCount() == allRows, QStringLiteral("Reset Filters shows every line"));
        if (panel)
            panel->setRuleSet(FilterRuleSet());
        showDock(win, textDock, false);
    }

    // Hide Lines with «…» — в панели другие, не применённые правила: их не
    // трогают, правило идёт только к вкладке, строка статуса объясняет.
    {
        FilterPanelWidget* panel = dock(win, "textFilterDockWidget")->findChild<FilterPanelWidget*>();
        FilterRuleSet other;
        FilterRule otherRule;
        otherRule.text = QStringLiteral("not applied");
        other.rules << otherRule;
        if (panel)
            panel->setRuleSet(other);
        menuOverWord(QStringLiteral("Hide Lines with"));
        check(panel && panel->ruleSet().rules.size() == 1 && panel->ruleSet().rules[0].text == otherRule.text,
              QStringLiteral("Hide Lines with leaves the panel's own rules alone"));
        check(statusText(win).contains(QStringLiteral("Filter added to this tab")),
              QStringLiteral("the status line explains where the filter went: %1").arg(statusText(win)));
        check(testdocs::settle(*model), QStringLiteral("exclude filter settles"));
        bool noneMatch = true;
        for (int r = 0; r < model->rowCount() && noneMatch; ++r)
            noneMatch = !model->entryAt(r)->message().contains(word, Qt::CaseInsensitive);
        check(noneMatch && model->rowCount() == allRows - expected,
              QStringLiteral("Hide Lines with hides exactly the lines with the text (%1 of %2, %3 hidden)")
                  .arg(model->rowCount()).arg(allRows).arg(expected));
        pressCommand(win, QStringLiteral("resetFilters"));
        check(testdocs::settle(*model) && model->rowCount() == allRows, QStringLiteral("Reset Filters after Hide"));
        if (panel)
            panel->setRuleSet(FilterRuleSet());
        showDock(win, dock(win, "textFilterDockWidget"), false);
    }

    // Highlight Lines with «…» — маркер в Row Highlighters и на вкладке.
    {
        auto* markerDock = dock(win, "rowMarkerDockWidget");
        menuOverWord(QStringLiteral("Highlight Lines with"));
        check(waitFor([&]() { return !model->rowMarkers().isEmpty(); }, 5000)
                  && model->rowMarkers().constLast().text == word,
              QStringLiteral("Highlight Lines with adds a marker to the tab"));
        check(markerDock && markerDock->isVisible(), QStringLiteral("Highlight Lines with shows Row Highlighters"));
        const int hit = rowWithText(model, word);
        const int miss = [&]() {
            for (int r = 0; r < model->rowCount(); ++r)
                if (!model->entryAt(r)->message().contains(word, Qt::CaseInsensitive))
                    return r;
            return -1;
        }();
        check(hit >= 0 && model->data(model->index(hit, 0), LogModel::RowMarkerColorRole).value<QColor>().isValid(),
              QStringLiteral("a line with the text gets the marker colour"));
        check(miss >= 0 && !model->data(model->index(miss, 0), LogModel::RowMarkerColorRole).value<QColor>().isValid(),
              QStringLiteral("a line without the text has no marker colour"));
        check(filtersToolBarAction(win, QStringLiteral("Markers"))->isChecked(), QStringLiteral("Filters → Markers is checked"));
        if (auto* reset = markerDock ? buttonEndingWith(markerDock, QStringLiteral("Reset")) : nullptr)
            reset->click();
        check(waitFor([&]() { return model->rowMarkers().isEmpty(); }, 5000), QStringLiteral("Row Highlighters Reset clears the tab's markers"));
        if (auto* panel = markerDock ? markerDock->findChild<MarkerPanelWidget*>() : nullptr)
            panel->setMarkers({});
        showDock(win, markerDock, false);
    }
}

// Доки Time Filter (поля, Whole Log, Apply / Reset, ошибка диапазона), Row
// Highlighters (Apply / Reset), Statistics (переход по ссылке).
void scenarioPanels(MainWindow& win, const Fixtures& fx)
{
    const int ia = tabIndexForFile(win, fx.a);
    const int ib = tabIndexForFile(win, fx.b);
    tabs(win)->setCurrentIndex(ia);
    LogViewWidget* tab = currentTab(win);
    LogModel* model = tab->model();
    const int allRows = model->rowCount();

    section(QStringLiteral("time filter dock"));
    {
        auto* timeDock = dock(win, "timeFilterDockWidget");
        pressCommand(win, QStringLiteral("panelTimeFilter"));
        check(waitFor([&]() { return timeDock->isVisible(); }), QStringLiteral("the shortcut shows Time Filter"));
        auto* from = timeDock->findChild<QDateTimeEdit*>(QStringLiteral("timeFilterFrom"));
        auto* to = timeDock->findChild<QDateTimeEdit*>(QStringLiteral("timeFilterTo"));
        auto* apply = timeDock->findChild<QPushButton*>(QStringLiteral("timeFilterApply"));
        auto* reset = timeDock->findChild<QPushButton*>(QStringLiteral("timeFilterReset"));
        auto* whole = timeDock->findChild<QPushButton*>(QStringLiteral("timeFilterWholeLog"));
        auto* error = timeDock->findChild<QLabel*>(QStringLiteral("timeFilterError"));
        auto* info = timeDock->findChild<QLabel*>(QStringLiteral("timeFilterInfo"));
        check(from && to && apply && reset && whole && error && info, QStringLiteral("Time Filter has its fields and buttons"));
        if (from && to && apply && reset && whole && error && info) {
            const auto range = model->fullTimeRange();
            check(from->dateTime() == range.first && to->dateTime() == range.second,
                  QStringLiteral("the fields show the range of the tab (%1 – %2)")
                      .arg(from->dateTime().toString(Qt::ISODateWithMs), to->dateTime().toString(Qt::ISODateWithMs)));
            const QDateTime base = testdocs::fixtureBaseTime();
            from->setDateTime(base.addSecs(60));
            to->setDateTime(base.addSecs(300));
            apply->click();
            check(testdocs::settle(*model), QStringLiteral("time filter settles"));
            check(model->startTimeFilter() == base.addSecs(60) && model->endTimeFilter() == base.addSecs(300)
                      && model->rowCount() > 0 && model->rowCount() < allRows,
                  QStringLiteral("Apply filters the tab by time (%1 of %2)").arg(model->rowCount()).arg(allRows));
            bool inRange = true;
            for (int r = 0; r < model->rowCount() && inRange; ++r) {
                const QDateTime t = model->entryAt(r)->timestamp();
                inRange = !t.isValid() || (t >= base.addSecs(60) && t <= base.addSecs(300));
            }
            check(inRange, QStringLiteral("every shown line is in the range"));
            check(filtersToolBarAction(win, QStringLiteral("Time"))->isChecked(), QStringLiteral("Filters → Time is checked"));
            check(!info->text().isEmpty() && error->isHidden(), QStringLiteral("the info line describes the filter: %1").arg(info->text()));

            // Другая вкладка — её поля; назад — поля фильтра alpha.
            tabs(win)->setCurrentIndex(ib);
            const auto betaRange = tabAt(win, ib)->model()->fullTimeRange();
            check(from->dateTime() == betaRange.first && to->dateTime() == betaRange.second,
                  QStringLiteral("the fields follow the tab (beta: its range)"));
            tabs(win)->setCurrentIndex(ia);
            check(from->dateTime() == base.addSecs(60) && to->dateTime() == base.addSecs(300),
                  QStringLiteral("the fields follow the tab (alpha: its filter)"));

            // From позже To — объяснение, фильтр вкладки не тронут.
            const int filtered = model->rowCount();
            from->setDateTime(base.addSecs(400));
            apply->click();
            check(!error->isHidden() && !error->text().isEmpty(), QStringLiteral("From later than To explains: %1").arg(error->text()));
            check(testdocs::settle(*model) && model->rowCount() == filtered && model->startTimeFilter() == base.addSecs(60),
                  QStringLiteral("an inverted range leaves the filter alone"));

            whole->click();
            check(from->dateTime() == range.first && to->dateTime() == range.second,
                  QStringLiteral("Whole Log puts the range of the log into the fields"));
            check(model->startTimeFilter() == base.addSecs(60), QStringLiteral("Whole Log applies nothing"));
            reset->click();
            check(testdocs::settle(*model) && !model->startTimeFilter().isValid() && model->rowCount() == allRows,
                  QStringLiteral("Reset turns the time filter off"));
            check(!filtersToolBarAction(win, QStringLiteral("Time"))->isChecked(), QStringLiteral("Filters → Time unchecked after Reset"));
        }
        pressCommand(win, QStringLiteral("panelTimeFilter"));
        check(waitFor([&]() { return !timeDock->isVisible(); }), QStringLiteral("the shortcut hides Time Filter"));
    }

    section(QStringLiteral("row highlighters dock"));
    {
        auto* markerDock = dock(win, "rowMarkerDockWidget");
        pressCommand(win, QStringLiteral("panelRowHighlighters"));
        check(waitFor([&]() { return markerDock->isVisible(); }), QStringLiteral("the shortcut shows Row Highlighters"));
        auto* panel = markerDock->findChild<MarkerPanelWidget*>();
        check(panel, QStringLiteral("Row Highlighters has its panel"));
        if (panel) {
            HighlightPattern p;
            p.text = QStringLiteral("timeout");
            p.color = QColor(Qt::yellow);
            panel->setMarkers({ p });
            check(model->rowMarkers().isEmpty(), QStringLiteral("markers are not applied before Apply"));
            if (auto* apply = buttonEndingWith(markerDock, QStringLiteral("Apply")))
                apply->click();
            check(waitFor([&]() { return model->rowMarkers().size() == 1; }, 5000),
                  QStringLiteral("Apply puts the marker on the active tab"));
            const int hit = rowWithText(model, QStringLiteral("imeout"));
            check(hit >= 0 && model->data(model->index(hit, 0), LogModel::RowMarkerColorRole).value<QColor>() == QColor(Qt::yellow),
                  QStringLiteral("a matching line gets the marker colour"));
            check(model->rowCount() == allRows, QStringLiteral("markers do not filter"));
            tabs(win)->setCurrentIndex(ib);
            check(tabAt(win, ib)->model()->rowMarkers().isEmpty() && !filtersToolBarAction(win, QStringLiteral("Markers"))->isChecked(),
                  QStringLiteral("markers are per tab (beta has none)"));
            tabs(win)->setCurrentIndex(ia);
            check(filtersToolBarAction(win, QStringLiteral("Markers"))->isChecked(), QStringLiteral("Filters → Markers on alpha"));
            if (auto* reset = buttonEndingWith(markerDock, QStringLiteral("Reset")))
                reset->click();
            check(waitFor([&]() { return model->rowMarkers().isEmpty(); }, 5000), QStringLiteral("Reset takes the markers off the tab"));
            check(panel->markers().size() == 1, QStringLiteral("Reset keeps the markers in the panel"));
            panel->setMarkers({});
        }
        pressCommand(win, QStringLiteral("panelRowHighlighters"));
        check(waitFor([&]() { return !markerDock->isVisible(); }), QStringLiteral("the shortcut hides Row Highlighters"));
    }

    section(QStringLiteral("statistics dock"));
    {
        auto* statsDock = dock(win, "statisticsDockWidget");
        pressCommand(win, QStringLiteral("panelStatistics"));
        check(waitFor([&]() { return statsDock->isVisible(); }), QStringLiteral("the shortcut shows Statistics"));
        QTextBrowser* browser = statsDock->findChild<QTextBrowser*>();
        static const QRegularExpression link(QStringLiteral("<a href=\"(nav:\\d+)\"[^>]*>(.*?)</a>"));
        QList<QPair<QString, QString>> links; // ссылка, текст
        check(browser && waitFor([&]() {
            links.clear();
            for (auto it = link.globalMatch(browser->toHtml()); it.hasNext();) {
                const auto m = it.next();
                QTextDocumentFragment text = QTextDocumentFragment::fromHtml(m.captured(2));
                links << qMakePair(m.captured(1), text.toPlainText());
            }
            return !links.isEmpty();
        }), QStringLiteral("Statistics shows links to lines"));
        int moved = 0, matched = 0;
        static const QRegularExpression firstWord(QStringLiteral("[A-Za-z]{4,}"));
        for (const auto& [href, text] : std::as_const(links)) {
            // Щелчок по ссылке — с последней строки, а если цель она сама, —
            // с первой.
            bool ok = false;
            for (const int from : { model->rowCount() - 1, 0 }) {
                tab->view()->goToRow(from);
                emit browser->anchorClicked(QUrl(href));
                ok = waitFor([&]() { return tab->view()->userSelectedRow() != from; }, 3000);
                if (ok)
                    break;
            }
            moved += ok ? 1 : 0;
            check(ok, QStringLiteral("Statistics link %1 (%2) selects a line").arg(href, text));
            // Ссылка-шаблон сообщения — строка с этим сообщением.
            const auto word = firstWord.match(text);
            const auto entry = selectedEntry(tab);
            if (ok && word.hasMatch() && entry && text.contains(QLatin1Char('#'))) {
                check(entry->message().contains(word.captured(0)),
                      QStringLiteral("the message link '%1' selects such a line: %2").arg(text, entry->message()));
                ++matched;
            }
        }
        check(moved > 0 && matched > 0, QStringLiteral("Statistics links: %1 jump, %2 to messages").arg(moved).arg(matched));
        pressCommand(win, QStringLiteral("panelStatistics"));
        check(waitFor([&]() { return !statsDock->isVisible(); }), QStringLiteral("the shortcut hides Statistics"));
    }
}

// Directory Scanner: кнопки панели (скан с выбором папки, обновление,
// расширения), фильтр по содержимому — Apply и Reset.
void scenarioScannerPanel(MainWindow& win, const Fixtures& fx)
{
    section(QStringLiteral("directory scanner panel"));
    auto* scannerDock = dock(win, "directoryScannerDockWidget");
    auto* panel = win.findChild<DirectoryScannerPanel*>();
    auto* scanner = win.findChild<DirectoryScanner*>();
    auto* tree = scannerDock->findChild<QTreeWidget*>();
    const auto button = [&](const char* name) {
        auto* b = scannerDock->findChild<QAbstractButton*>(QString::fromLatin1(name));
        check(b, QStringLiteral("scanner has %1").arg(QLatin1String(name)));
        return b;
    };
    QAbstractButton* scan = button("scannerScanButton");
    QAbstractButton* refresh = button("scannerRefreshButton");
    QAbstractButton* filters = button("scannerFiltersToggle");
    QAbstractButton* exts = button("scannerExtensionsButton");
    QAbstractButton* apply = button("scannerApplyButton");
    QAbstractButton* reset = button("scannerResetButton");
    auto* content = scannerDock->findChild<QLineEdit*>(QStringLiteral("scannerContentEdit"));
    if (!panel || !scanner || !tree || !scan || !refresh || !filters || !exts || !apply || !reset || !content)
        return;
    check(showDock(win, scannerDock), QStringLiteral("the scanner dock shows"));

    const auto shown = [&]() {
        QStringList names;
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (!tree->topLevelItem(i)->isHidden())
                names << tree->topLevelItem(i)->text(0);
        names.sort();
        return names;
    };
    const QStringList all { QStringLiteral("needle.log"), QStringLiteral("one.log"), QStringLiteral("two.log") };

    // Скан: кнопка → выбор папки.
    bool asked = false;
    g_driver->expect([&](QWidget* w) {
        asked = true;
        if (auto* dialog = qobject_cast<QFileDialog*>(w))
            typeFileName(dialog, fx.dir);
        else
            DialogDriver::close(w);
    });
    scan->click();
    check(waitFor([&]() { return asked && QDir(scanner->rootPath()) == QDir(fx.dir) && shown() == all; }),
          QStringLiteral("the scan button scans the chosen folder: %1 [%2]").arg(scanner->rootPath(), shown().join(',')));
    g_driver->dropExpectations(); // не дождались диалога — обработчик ссылается на локальные

    // Обновление: новый файл появляется.
    const QString late = testdocs::writeFile(QDir(fx.dir), QStringLiteral("late.log"),
        testdocs::makeLogBytes(5, 66, "\n", testdocs::fixtureBaseTime(), false, true, true));
    check(refresh->isEnabled(), QStringLiteral("refresh is enabled after a scan"));
    refresh->click();
    check(waitFor([&]() { return shown().contains(QStringLiteral("late.log")) && !scanner->isRescanning(); }),
          QStringLiteral("refresh picks up a new file [%1]").arg(shown().join(',')));
    // Удалить, как пользователь: на Windows файл, который анализ сканера ещё
    // читает (элемент дерева появляется раньше счётчиков), не удаляется.
    check(waitFor([&]() { return QFile::remove(late) || !QFileInfo::exists(late); }, 10000),
          QStringLiteral("late.log can be removed after the refresh"));
    refresh->click();
    check(waitFor([&]() { return shown() == all && !scanner->isRescanning(); }),
          QStringLiteral("refresh drops a removed file [%1]").arg(shown().join(',')));

    // Фильтр по содержимому: ⚙ → текст → Apply; Reset.
    filters->click();
    check(waitFor([&]() { return content->isVisible(); }, 5000), QStringLiteral("⚙ shows the filters"));
    bool finished = false;
    const auto conn = QObject::connect(scanner, &DirectoryScanner::contentFilterFinished, &win,
                                       [&](int, int) { finished = true; });
    content->setText(QStringLiteral("needle-p9"));
    apply->click();
    check(waitFor([&]() { return finished && !scanner->isContentFilterRunning(); }), QStringLiteral("content filter finishes"));
    check(shown() == QStringList{ QStringLiteral("needle.log") },
          QStringLiteral("content filter keeps the file with the text [%1]").arg(shown().join(',')));
    // Enter в поле — то же, что Apply.
    finished = false;
    content->setText(QStringLiteral("Disk"));
    QTest::keyClick(content, Qt::Key_Return);
    check(waitFor([&]() { return finished && !scanner->isContentFilterRunning(); }), QStringLiteral("Enter runs the content filter"));
    check(shown() == QStringList({ QStringLiteral("one.log"), QStringLiteral("two.log") }),
          QStringLiteral("Enter applies the new text [%1]").arg(shown().join(',')));
    QObject::disconnect(conn);
    reset->click();
    check(waitFor([&]() { return shown() == all; }), QStringLiteral("Reset shows every file [%1]").arg(shown().join(',')));
    check(content->text().isEmpty(), QStringLiteral("Reset clears the content field"));
    filters->click();

    // Расширения — Settings.
    QString title;
    g_driver->expect([&](QWidget* w) {
        title = w->windowTitle();
        DialogDriver::close(w);
    });
    exts->click();
    check(waitFor([&]() { return !title.isEmpty(); }, 5000) && title == QStringLiteral("Settings"),
          QStringLiteral("the extensions button opens Settings (%1)").arg(title));
    g_driver->dropExpectations();
    check(showDock(win, scannerDock, false), QStringLiteral("the scanner dock hides"));
}

// Редактор схем (Tools → Field Schemas…): образцы — строки активной
// вкладки, новая схема по автоопределению, «Use Selected» — схема вкладки и
// файл в каталоге схем.
void scenarioFieldSchemasDialog(MainWindow& win, const Fixtures& fx)
{
    section(QStringLiteral("field schemas dialog"));
    const int ia = tabIndexForFile(win, fx.a);
    tabs(win)->setCurrentIndex(ia);
    LogViewWidget* tab = currentTab(win);
    LogModel* model = tab->model();
    const int allRows = model->rowCount();
    tab->view()->goToRow(5);
    const QString sample = model->messageAt(5);
    const QString before = tab->fieldSettings().schemaName;
    const QString beforeSchema = tab->parserPattern();
    const QString created = QStringLiteral("Smoke Detected");
    bool shown = false;
    QString samples;
    g_driver->expect([&](QWidget* w) {
        shown = true;
        auto* sampleEdit = w->findChild<QPlainTextEdit*>(QStringLiteral("sampleEdit"));
        auto* add = w->findChild<QPushButton*>(QStringLiteral("addSchemaBtn"));
        auto* name = w->findChild<QLineEdit*>(QStringLiteral("nameEdit"));
        auto* detect = w->findChild<QPushButton*>(QStringLiteral("autoDetectBtn"));
        auto* applyBtn = w->findChild<QPushButton*>(QStringLiteral("applyBtn"));
        auto* use = w->findChild<QPushButton*>(QStringLiteral("useBtn"));
        if (!sampleEdit || !add || !name || !detect || !applyBtn || !use) {
            check(false, QStringLiteral("Field Schemas has its controls"));
            DialogDriver::close(w);
            return;
        }
        samples = sampleEdit->toPlainText();
        add->click();
        name->setText(created);
        emit name->textEdited(created);
        detect->click();
        check(applyBtn->isEnabled(), QStringLiteral("Auto-detect leaves an unsaved schema (Apply enabled)"));
        applyBtn->click();
        use->click(); // принять схему и закрыть
    });
    if (QAction* item = menuAction(win, QStringLiteral("Field Schemas")))
        item->trigger();
    check(waitFor([&]() { return shown && !QApplication::activeModalWidget(); }, 10000), QStringLiteral("Field Schemas opens and closes"));
    g_driver->dropExpectations();
    check(samples.startsWith(sample), QStringLiteral("the preview starts at the selected line: %1").arg(samples.left(80)));
    check(waitAllLoaded(win), QStringLiteral("tabs settle after the schema"));
    check(tab->fieldSettings().schemaName == created, QStringLiteral("Use Selected sets the schema of the tab (%1)").arg(tab->fieldSettings().schemaName));
    check(model->rowCount() == allRows, QStringLiteral("the tab keeps its rows"));
    auto* combo = dock(win, "fieldVisibilityDockWidget")->findChild<QComboBox*>();
    check(combo && combo->currentText() == created, QStringLiteral("Log Fields shows the new schema"));
    bool saved = false;
    for (const QString& f : QDir(SchemaStore::patternsDir()).entryList({ QStringLiteral("*.json") }, QDir::Files)) {
        QFile file(QDir(SchemaStore::patternsDir()).filePath(f));
        saved = saved || (file.open(QIODevice::ReadOnly) && file.readAll().contains(created.toUtf8()));
    }
    check(saved, QStringLiteral("the new schema is saved in the patterns folder"));
    check(tab->fieldSettings().hasSchema() && tab->parserPattern() != beforeSchema,
          QStringLiteral("the tab parses with the detected schema"));
    // Как было: прежняя схема вкладки.
    if (combo && combo->findText(before) >= 0)
        combo->setCurrentIndex(combo->findText(before));
    check(waitAllLoaded(win) && tab->fieldSettings().schemaName == before, QStringLiteral("the tab returns to %1").arg(before));
}

// Settings с изменёнными значениями: шрифт, порог индексного бэкенда, тема,
// сочетание (конфликт — предупреждение, OK не закрывает), затем Clear
// Memory… (No / Yes) и сброс сочетаний.
void scenarioSettingsValues(MainWindow& win, const Fixtures& fx, bool indexed)
{
    section(QStringLiteral("settings values"));
    AppSettings& s = AppSettings::instance();
    const int fontSize = s.fontSize();
    const ThemeMode theme = s.themeMode();
    const int threshold0 = s.indexedThresholdMB();
    const QKeySequence goToKeys = command(win, QStringLiteral("goTo"))->shortcut();
    const QKeySequence newKeys(QStringLiteral("Ctrl+Alt+G"));
    QString warning;
    bool shown = false;
    g_driver->expect([&](QWidget* w) {
        shown = true;
        auto* size = w->findChild<QSpinBox*>(QStringLiteral("fontSizeSpinBox"));
        auto* themeBox = w->findChild<QComboBox*>(QStringLiteral("themeComboBox"));
        auto* threshold = w->findChild<QSpinBox*>(QStringLiteral("indexedThresholdSpinBox"));
        auto* goTo = w->findChild<QKeySequenceEdit*>(QStringLiteral("shortcut_goTo"));
        auto* box = w->findChild<QDialogButtonBox*>();
        if (!size || !themeBox || !threshold || !goTo || !box) {
            check(false, QStringLiteral("Settings has its controls"));
            DialogDriver::close(w);
            return;
        }
        check(goTo->keySequence() == goToKeys, QStringLiteral("the shortcut field shows the current keys"));
        size->setValue(fontSize + 2);
        themeBox->setCurrentIndex(themeBox->findData(int(ThemeMode::Dark)));
        threshold->setValue(indexed ? 512 : 0);
        // Сочетание другой команды — конфликт: предупреждение, диалог открыт.
        goTo->setKeySequence(command(win, QStringLiteral("open"))->shortcut());
        g_driver->expect([&](QWidget* m) {
            warning = m->windowTitle();
            DialogDriver::close(m);
        });
        box->button(QDialogButtonBox::Ok)->click();
        check(!warning.isEmpty() && w->isVisible(), QStringLiteral("a shortcut conflict warns and keeps Settings open (%1)").arg(warning));
        goTo->setKeySequence(newKeys);
        box->button(QDialogButtonBox::Ok)->click();
    });
    pressCommand(win, QStringLiteral("settings"));
    check(waitFor([&]() { return shown && !QApplication::activeModalWidget(); }, 10000), QStringLiteral("Settings opens and closes"));
    g_driver->dropExpectations();
    check(s.fontSize() == fontSize + 2 && s.themeMode() == ThemeMode::Dark && s.indexedThresholdMB() == (indexed ? 512 : 0),
          QStringLiteral("OK stores the values"));
    bool fonts = tabs(win)->count() > 0;
    for (int i = 0; i < tabs(win)->count(); ++i)
        fonts = fonts && tabAt(win, i)->view()->font().pointSize() == fontSize + 2;
    check(fonts, QStringLiteral("every tab gets the new font size"));
    check(QApplication::palette().color(QPalette::Base).lightness() < 128,
          QStringLiteral("the dark theme is applied at once"));
    check(command(win, QStringLiteral("goTo"))->shortcut() == newKeys,
          QStringLiteral("the action gets the new shortcut: %1").arg(command(win, QStringLiteral("goTo"))->shortcut().toString()));
    bool goToShown = false;
    g_driver->expect([&](QWidget* w) {
        goToShown = w->windowTitle().startsWith(QStringLiteral("Go to"));
        DialogDriver::close(w);
    });
    press(win, newKeys);
    check(waitFor([&]() { return goToShown; }, 5000), QStringLiteral("the new shortcut opens Go to"));
    g_driver->dropExpectations();
    check(testdocs::idleViews(), QStringLiteral("views settle after the font change"));

    // Порог: следующий файл открывается другим бэкендом.
    win.openCommandLine(OpenRouting::parseCommandLine({ fx.gone }));
    check(waitFor([&]() { return tabIndexForFile(win, fx.gone) >= 0; }), QStringLiteral("a file opens after the threshold change"));
    LogViewWidget* opened = tabAt(win, tabIndexForFile(win, fx.gone));
    check(waitLoaded(opened) && opened->model()->isIndexedBackend() == !indexed,
          QStringLiteral("the new threshold picks the %1 backend").arg(indexed ? "resident" : "indexed"));
    {
        const int n = tabs(win)->count();
        tabs(win)->setCurrentWidget(opened);
        command(win, QStringLiteral("closeTab"))->trigger();
        waitFor([&]() { return tabs(win)->count() == n - 1; });
    }

    // Как было; заодно Clear Memory… (сначала отказ, затем согласие).
    const int memories = int(QDir(AppSettings::fileMemoryDir()).entryList({ QStringLiteral("*.json") }, QDir::Files).size());
    check(memories > 0, QStringLiteral("file memory has files to clear (%1)").arg(memories));
    shown = false;
    g_driver->expect([&](QWidget* w) {
        shown = true;
        auto* size = w->findChild<QSpinBox*>(QStringLiteral("fontSizeSpinBox"));
        auto* themeBox = w->findChild<QComboBox*>(QStringLiteral("themeComboBox"));
        auto* threshold = w->findChild<QSpinBox*>(QStringLiteral("indexedThresholdSpinBox"));
        auto* resetKeys = w->findChild<QPushButton*>(QStringLiteral("resetShortcutsButton"));
        auto* clear = w->findChild<QPushButton*>(QStringLiteral("clearFileMemoryButton"));
        auto* cleared = w->findChild<QLabel*>(QStringLiteral("clearFileMemoryLabel"));
        auto* box = w->findChild<QDialogButtonBox*>();
        if (!size || !themeBox || !threshold || !resetKeys || !clear || !cleared || !box) {
            check(false, QStringLiteral("Settings has its controls (second time)"));
            DialogDriver::close(w);
            return;
        }
        check(size->value() == fontSize + 2 && threshold->value() == (indexed ? 512 : 0),
              QStringLiteral("Settings shows the stored values"));
        const auto answer = [&](QMessageBox::StandardButton which) {
            g_driver->expect([which](QWidget* m) {
                auto* mb = qobject_cast<QMessageBox*>(m);
                if (mb && mb->button(which))
                    mb->button(which)->click();
                else
                    DialogDriver::close(m);
            });
        };
        // Запись памяти идёт в фоне (закрытая только что вкладка): файлов
        // может только прибавиться, поэтому сверка — «не меньше».
        const auto memoryFiles = []() {
            return int(QDir(AppSettings::fileMemoryDir()).entryList({ QStringLiteral("*.json") }, QDir::Files).size());
        };
        const int beforeNo = memoryFiles();
        answer(QMessageBox::No);
        clear->click();
        const int afterNo = memoryFiles();
        check(beforeNo >= memories && afterNo >= beforeNo && cleared->text().isEmpty(),
              QStringLiteral("Clear Memory… → No keeps the memory (%1 → %2)").arg(beforeNo).arg(afterNo));
        answer(QMessageBox::Yes);
        clear->click(); // FileMemoryStore::clear ждёт фоновые записи
        const int afterYes = memoryFiles();
        static const QRegularExpression number(QStringLiteral("\\d+"));
        const int forgot = number.match(cleared->text()).captured(0).toInt();
        check(afterYes == 0 && forgot >= afterNo,
              QStringLiteral("Clear Memory… → Yes forgets every file (%1 left; %2)").arg(afterYes).arg(cleared->text()));
        size->setValue(fontSize);
        themeBox->setCurrentIndex(themeBox->findData(int(theme)));
        threshold->setValue(threshold0);
        resetKeys->click();
        box->button(QDialogButtonBox::Ok)->click();
    });
    pressCommand(win, QStringLiteral("settings"));
    check(waitFor([&]() { return shown && !QApplication::activeModalWidget(); }, 10000), QStringLiteral("Settings opens again"));
    g_driver->dropExpectations();
    check(s.fontSize() == fontSize && s.themeMode() == theme && s.indexedThresholdMB() == threshold0
              && command(win, QStringLiteral("goTo"))->shortcut() == goToKeys,
          QStringLiteral("Settings returns the values and the default shortcuts"));
    check(testdocs::idleViews() && waitAllLoaded(win), QStringLiteral("tabs settle after Settings"));
}

// Заметка закладки (QInputDialog): «Add Bookmark with Note…» с текстом и
// отменой, «Edit Bookmark Note…», заметка — в карточке панели.
void scenarioBookmarkNote(MainWindow& win, const Fixtures& fx)
{
    section(QStringLiteral("bookmark note"));
    tabs(win)->setCurrentIndex(tabIndexForFile(win, fx.b));
    LogViewWidget* tab = currentTab(win);
    LogModel* model = tab->model();
    BookmarkStore& store = tab->bookmarks();
    const int before = store.count();
    const int row = 12;
    tab->view()->goToRow(row);
    check(!model->data(model->index(row, 0), LogModel::BookmarkRole).toBool(), QStringLiteral("line 12 has no bookmark yet"));
    const auto answerNote = [&](const QString& text, bool accept, QString* label) {
        g_driver->expect([=](QWidget* w) {
            auto* dialog = qobject_cast<QInputDialog*>(w);
            if (label)
                *label = dialog ? dialog->labelText() : QString();
            if (!dialog || !accept) {
                DialogDriver::close(w);
                return;
            }
            dialog->setTextValue(text);
            dialog->accept();
        });
    };
    // Меню и диалог заметки идут вложенными циклами внутри события: к
    // возврату оба обработчика отработали (остаток — сброшен, он ссылается
    // на локальные переменные).
    const auto noteMenu = [&](const QString& item, const QString& text, bool accept, QString* label) {
        bool picked = false;
        pickMenuItem(item, &picked);
        answerNote(text, accept, label);
        rowContextMenu(tab, row);
        g_driver->dropExpectations();
        return picked;
    };

    // Отмена «Add Bookmark with Note…» — закладки нет.
    check(noteMenu(QStringLiteral("Add Bookmark with Note"), QString(), false, nullptr),
          QStringLiteral("row menu offers Add Bookmark with Note"));
    check(waitFor([]() { return !QApplication::activeModalWidget(); }, 5000) && store.count() == before,
          QStringLiteral("cancelled Add Bookmark with Note adds nothing"));

    QString label;
    check(noteMenu(QStringLiteral("Add Bookmark with Note"), QStringLiteral("first note"), true, &label), QStringLiteral("row menu offers Add Bookmark with Note again"));
    check(waitFor([&]() { return store.count() == before + 1; }, 5000), QStringLiteral("Add Bookmark with Note adds a bookmark"));
    std::optional<Bookmark> added;
    for (const Bookmark& b : store.bookmarks()) // список — копией
        if (b.line == model->keyAt(row).line)
            added = b;
    check(added && added->note == QStringLiteral("first note"), QStringLiteral("the bookmark has the note"));
    check(label.contains(QString::number(model->keyAt(row).line)), QStringLiteral("the note dialog names the line: %1").arg(label));
    const int id = added ? added->id : -1;

    check(noteMenu(QStringLiteral("Edit Bookmark Note"), QStringLiteral("edited note"), true, nullptr), QStringLiteral("row menu offers Edit Bookmark Note"));
    check(waitFor([&]() { const Bookmark* b = store.find(id); return b && b->note == QStringLiteral("edited note"); }, 5000),
          QStringLiteral("Edit Bookmark Note changes the note"));
    check(noteMenu(QStringLiteral("Edit Bookmark Note"), QStringLiteral("discarded"), false, nullptr), QStringLiteral("row menu offers Edit Bookmark Note again"));
    check(waitFor([]() { return !QApplication::activeModalWidget(); }, 5000) && store.find(id)
              && store.find(id)->note == QStringLiteral("edited note"),
          QStringLiteral("cancelled Edit Bookmark Note keeps the note and the bookmark"));

    auto* bookmarkDock = dock(win, "bookmarksDockWidget");
    showDock(win, bookmarkDock);
    bool inCard = false;
    for (QLineEdit* e : bookmarkDock->findChildren<QLineEdit*>(QStringLiteral("bookmarkNote")))
        inCard = inCard || e->text() == QStringLiteral("edited note");
    check(inCard, QStringLiteral("the bookmark card shows the note"));
    showDock(win, bookmarkDock, false);
}

// Растущий файл в окне: авто-обновление кнопкой тулбара (таймер), follow-tail
// на дозаписи, F5, правый щелчок по Reload.
// Дельта времени (F-8): View → Time Delta, «нулевая» сочетанием и из
// контекстного меню строки, обе дельты в Entry Details.
void scenarioTimeDelta(MainWindow& win, const Fixtures& fx)
{
    section(QStringLiteral("time delta"));
    tabs(win)->setCurrentIndex(tabIndexForFile(win, fx.b));
    LogViewWidget* tab = currentTab(win);
    LogModel* model = tab->model();
    LogListView* view = tab->view();
    QAction* column = command(win, QStringLiteral("timeDelta"));
    check(column && !column->isChecked() && !AppSettings::instance().timeDelta() && view->timeDeltaWidth() == 0,
          QStringLiteral("View → Time Delta exists and is off by default"));
    if (column)
        column->trigger();
    check(waitFor([&]() { return view->timeDeltaWidth() > 0 && !view->hasPendingLayout(); }, 5000)
              && AppSettings::instance().timeDelta(),
          QStringLiteral("View → Time Delta shows the column"));

    const auto timedRow = [&](int from) {
        for (int r = from; r < model->rowCount(); ++r)
            if (hasTimestampMs(model->visibleMetaAt(r).timestampMs))
                return r;
        return -1;
    };
    auto* details = win.findChild<EntryDetailsPanel*>();
    const int zeroRow = timedRow(12);
    view->goToRow(zeroRow);
    pressCommand(win, QStringLiteral("timeZero"));
    check(waitFor([&]() { return model->isTimeZeroRow(zeroRow); }, 5000)
              && view->timeDeltaText(zeroRow) == QStringLiteral("= 0"),
          QStringLiteral("Ctrl+Shift+D makes the selected line the time zero"));
    check(details && details->timeDeltaSummary().contains(QStringLiteral("time zero")),
          QStringLiteral("Entry Details names the zero: %1").arg(details ? details->timeDeltaSummary() : QString()));
    const int later = timedRow(zeroRow + 5);
    view->goToRow(later);
    check(details && waitFor([&]() {
              return details->timeDeltaSummary().contains(QStringLiteral("Δ previous: "))
                  && details->timeDeltaSummary().contains(QStringLiteral("Δ time zero: +"));
          }, 5000),
          QStringLiteral("Entry Details shows both deltas: %1").arg(details ? details->timeDeltaSummary() : QString()));
    view->goToRow(zeroRow);
    pressCommand(win, QStringLiteral("timeZero"));
    check(waitFor([&]() { return !model->hasTimeZero(); }, 5000),
          QStringLiteral("Ctrl+Shift+D on the zero line clears it"));

    // Контекстное меню строки.
    const int menuRow = timedRow(5);
    bool picked = false;
    pickMenuItem(QStringLiteral("Set as Time Zero"), &picked);
    rowContextMenu(tab, menuRow);
    g_driver->dropExpectations();
    check(picked && waitFor([&]() { return model->isTimeZeroRow(menuRow); }, 5000),
          QStringLiteral("row menu → Set as Time Zero"));
    picked = false;
    pickMenuItem(QStringLiteral("Clear Time Zero"), &picked);
    rowContextMenu(tab, timedRow(30));
    g_driver->dropExpectations();
    check(picked && waitFor([&]() { return !model->hasTimeZero(); }, 5000),
          QStringLiteral("row menu → Clear Time Zero"));

    if (column)
        column->trigger();
    check(waitFor([&]() { return view->timeDeltaWidth() == 0 && !view->hasPendingLayout(); }, 5000)
              && !AppSettings::instance().timeDelta(),
          QStringLiteral("and View → Time Delta hides it again"));
}

void scenarioAutoReload(MainWindow& win, const QDir& dir)
{
    section(QStringLiteral("auto-reload a growing file"));
    const auto lines = [](int from, int count) {
        QByteArray out;
        for (int i = from; i < from + count; ++i)
            out += QStringLiteral("2026-11-26 10:%1:%2.000 [main] INFO - grow line %3\n")
                       .arg((i / 60) % 60, 2, 10, QLatin1Char('0')).arg(i % 60, 2, 10, QLatin1Char('0')).arg(i).toUtf8();
        return out;
    };
    const QString path = testdocs::writeFile(dir, QStringLiteral("grow.log"), lines(0, 300));
    win.openCommandLine(OpenRouting::parseCommandLine({ path }));
    check(waitFor([&]() { return tabIndexForFile(win, path) >= 0; }), QStringLiteral("grow.log opens"));
    tabs(win)->setCurrentIndex(tabIndexForFile(win, path));
    LogViewWidget* tab = currentTab(win);
    LogModel* model = tab->model();
    check(waitLoaded(tab, 300), QStringLiteral("grow.log loads"));
    LogListView* view = tab->view();
    QScrollBar* bar = view->verticalScrollBar();
    QAction* autoReload = command(win, QStringLiteral("autoReload"));
    QAction* reload = command(win, QStringLiteral("reload"));
    QToolButton* autoButton = nullptr;
    QToolButton* reloadButton = nullptr;
    for (QToolBar* tb : win.findChildren<QToolBar*>()) {
        if (auto* b = qobject_cast<QToolButton*>(tb->widgetForAction(autoReload)))
            autoButton = b;
        if (auto* b = qobject_cast<QToolButton*>(tb->widgetForAction(reload)))
            reloadButton = b;
    }
    check(autoButton && reloadButton, QStringLiteral("the toolbar has Reload and Auto-Reload"));
    if (!autoButton || !reloadButton || !autoReload)
        return;
    check(!tab->autoReload() && !autoReload->isChecked(), QStringLiteral("a file opens without auto-reload"));

    autoButton->click();
    check(tab->autoReload() && autoReload->isChecked(), QStringLiteral("the toolbar button turns auto-reload on"));
    view->setFocus();
    pressCommand(win, QStringLiteral("followTail"));
    check(view->followTail(), QStringLiteral("follow tail on"));
    const auto atTail = [&](int rows) {
        return model->rowCount() == rows && !tab->isLoading() && !view->hasPendingLayout()
            && bar->maximum() > 0 && bar->value() == bar->maximum();
    };
    check(waitFor([&]() { return atTail(300); }, 10000), QStringLiteral("follow tail scrolls to the end"));
    // Таймер (интервал 1 с) дочитывает хвост, вид следует за ним.
    testdocs::appendToFile(path, lines(300, 200));
    check(waitFor([&]() { return atTail(500); }, 15000),
          QStringLiteral("the timer reads the appended lines and follow tail keeps the end in view (%1 rows, %2 of %3)")
              .arg(model->rowCount()).arg(bar->value()).arg(bar->maximum()));
    check(view->followTail(), QStringLiteral("follow tail stays on after the append"));

    // Без авто-обновления — F5.
    autoButton->click();
    check(!tab->autoReload() && !autoReload->isChecked(), QStringLiteral("the toolbar button turns auto-reload off"));
    pressCommand(win, QStringLiteral("followTail"));
    check(!view->followTail(), QStringLiteral("follow tail off"));
    view->goToRow(10);
    testdocs::appendToFile(path, lines(500, 20));
    pressCommand(win, QStringLiteral("reload"));
    check(waitFor([&]() { return model->rowCount() == 520 && !tab->isLoading(); }, 15000),
          QStringLiteral("F5 reads the appended lines (%1)").arg(model->rowCount()));
    check(testdocs::idleViews() && view->userSelectedRow() == 10 && bar->value() < bar->maximum(),
          QStringLiteral("without follow tail the view stays at the selected line"));
    // Прежний жест: правый щелчок по Reload — авто-обновление вкладки.
    {
        const QPoint at = reloadButton->rect().center();
        QContextMenuEvent e(QContextMenuEvent::Mouse, at, reloadButton->mapToGlobal(at));
        QApplication::sendEvent(reloadButton, &e);
    }
    check(tab->autoReload() && autoReload->isChecked(), QStringLiteral("right click on Reload turns auto-reload on"));
    autoReload->trigger();
    check(!tab->autoReload(), QStringLiteral("Auto-Reload action turns it off"));
    const int n = tabs(win)->count();
    command(win, QStringLiteral("closeTab"))->trigger();
    check(waitFor([&]() { return tabs(win)->count() == n - 1; }), QStringLiteral("grow.log closes"));
}

// Раскладка доков и тулбаров перед выходом — её должно восстановить
// следующее окно (saveState / restoreState через ini).
struct DockLayout {
    QMap<QString, QPair<bool, int>> docks; // видим, область
    bool filtersToolBar = true;
};

DockLayout dockLayout(MainWindow& win)
{
    DockLayout l;
    for (QDockWidget* d : win.findChildren<QDockWidget*>())
        l.docks.insert(d->objectName(), qMakePair(d->isVisible(), int(win.dockWidgetArea(d))));
    if (auto* tb = win.findChild<QToolBar*>(QStringLiteral("filtersToolBar")))
        l.filtersToolBar = tb->isVisible();
    return l;
}

QString describeLayout(const DockLayout& l)
{
    QStringList out;
    for (auto it = l.docks.cbegin(); it != l.docks.cend(); ++it)
        out << QStringLiteral("%1:%2/%3").arg(it.key()).arg(it.value().first).arg(it.value().second);
    return out.join(' ') + QStringLiteral(" filters:%1").arg(l.filtersToolBar);
}

DockLayout arrangeDocks(MainWindow& win)
{
    section(QStringLiteral("dock layout before exit"));
    auto* bookmarks = dock(win, "bookmarksDockWidget");
    auto* stats = dock(win, "statisticsDockWidget");
    // Перетаскивание дока к краю окна — тем же вызовом, что делает Qt.
    win.addDockWidget(Qt::LeftDockWidgetArea, bookmarks);
    showDock(win, bookmarks);
    win.addDockWidget(Qt::TopDockWidgetArea, stats);
    showDock(win, stats);
    if (auto* tb = win.findChild<QToolBar*>(QStringLiteral("filtersToolBar")); tb && tb->isVisible())
        tb->toggleViewAction()->trigger();
    const DockLayout l = dockLayout(win);
    check(l.docks.value(bookmarks->objectName()) == qMakePair(true, int(Qt::LeftDockWidgetArea))
              && l.docks.value(stats->objectName()) == qMakePair(true, int(Qt::TopDockWidgetArea)) && !l.filtersToolBar,
          QStringLiteral("docks arranged: %1").arg(describeLayout(l)));
    return l;
}

void checkDockLayout(MainWindow& win, const DockLayout& expected)
{
    const DockLayout now = dockLayout(win);
    check(now.docks == expected.docks && now.filtersToolBar == expected.filtersToolBar,
          QStringLiteral("the next window restores the dock layout:\n  now %1\n  was %2")
              .arg(describeLayout(now), describeLayout(expected)));
}

// Меню вкладки: Copy Full Path, Close Tabs to the Right, Close Others,
// Close All (и что недоступно) — в отдельном окне, как его открывает
// --new-instance: окно сценариев основного не теряет вкладок.
void scenarioTabMenu(const Fixtures& fx)
{
    section(QStringLiteral("tab context menu"));
    MainWindow win;
    showWindow(win);
    win.startUp(OpenRouting::parseCommandLine({ QStringLiteral("--new-instance"), fx.a, fx.b, fx.c, fx.gone }), false);
    check(waitFor([&]() { return tabs(win)->count() == 4; }) && waitAllLoaded(win), QStringLiteral("four tabs open"));
    const auto choose = [&](int index, const QString& item, bool* enabled = nullptr) {
        bool picked = false;
        bool seen = false;
        pickMenuItem(item, &picked, &seen);
        tabContextMenu(win, index); // меню — вложенным циклом внутри события
        g_driver->dropExpectations();
        if (enabled)
            *enabled = seen;
        return picked;
    };

    QApplication::clipboard()->clear();
    check(choose(1, QStringLiteral("Copy Full Path")), QStringLiteral("Copy Full Path is offered"));
    check(QApplication::clipboard()->text() == QDir::toNativeSeparators(fx.b),
          QStringLiteral("Copy Full Path copies the path: %1").arg(QApplication::clipboard()->text()));
    bool enabled = true;
    choose(3, QStringLiteral("Close Tabs to the Right"), &enabled);
    check(!enabled && tabs(win)->count() == 4, QStringLiteral("Close Tabs to the Right is off on the last tab"));
    check(choose(1, QStringLiteral("Close Tabs to the Right")), QStringLiteral("Close Tabs to the Right is offered"));
    check(waitFor([&]() { return tabs(win)->count() == 2; }) && tabIndexForFile(win, fx.a) == 0 && tabIndexForFile(win, fx.b) == 1,
          QStringLiteral("Close Tabs to the Right keeps the tabs up to it (%1)").arg(describeTabs(win)));
    check(choose(1, QStringLiteral("Close Others")), QStringLiteral("Close Others is offered"));
    check(waitFor([&]() { return tabs(win)->count() == 1; }) && tabIndexForFile(win, fx.b) == 0,
          QStringLiteral("Close Others keeps only that tab (%1)").arg(describeTabs(win)));
    enabled = true;
    choose(0, QStringLiteral("Close Others"), &enabled);
    check(!enabled, QStringLiteral("Close Others is off with one tab"));
    pressCommand(win, QStringLiteral("reopenTab"));
    check(waitFor([&]() { return tabs(win)->count() == 2; }) && tabIndexForFile(win, fx.a) == 0,
          QStringLiteral("Ctrl+Shift+T brings back a tab closed by Close Others (%1)").arg(describeTabs(win)));
    check(choose(0, QStringLiteral("Close All")), QStringLiteral("Close All is offered"));
    check(waitFor([&]() { return tabs(win)->count() == 0; }), QStringLiteral("Close All closes every tab"));
    auto* welcome = win.findChild<WelcomeWidget*>();
    check(!welcome || waitFor([&]() { return welcome->isVisible(); }, 5000), QStringLiteral("the start screen is back"));
    win.close();
}

// stdin (`DendroLog -`): своё окно, вкладка «stdin» растёт по мере записи
// в пайп, follow-tail держит конец, файл спула ни в недавних, ни во вкладке
// после закрытия. Пайп — вместо дескриптора 0 (шов спулера).
void scenarioStdin(const Fixtures& fx)
{
    section(QStringLiteral("stdin"));
    int fds[2] = { -1, -1 };
#ifdef Q_OS_WIN
    const bool piped = _pipe(fds, 64 * 1024, _O_BINARY) == 0;
#else
    const bool piped = ::pipe(fds) == 0;
#endif
    check(piped, QStringLiteral("a pipe for stdin"));
    if (!piped)
        return;
    const auto writeLines = [&](int from, int count) {
        QByteArray out;
        for (int i = from; i < from + count; ++i)
            out += QStringLiteral("2026-11-26 11:00:%1.%2 [main] INFO - piped line %3\n")
                       .arg((i / 1000) % 60, 2, 10, QLatin1Char('0')).arg(i % 1000, 3, 10, QLatin1Char('0')).arg(i).toUtf8();
#ifdef Q_OS_WIN
        return _write(fds[1], out.constData(), unsigned(out.size())) == out.size();
#else
        return ::write(fds[1], out.constData(), size_t(out.size())) == out.size();
#endif
    };
    const auto closeWriteEnd = [&]() {
        if (fds[1] < 0)
            return;
#ifdef Q_OS_WIN
        _close(fds[1]);
#else
        ::close(fds[1]);
#endif
        fds[1] = -1;
    };
    StdinSpooler::setInputDescriptorForTesting(fds[0]);
    {
        MainWindow win;
        showWindow(win);
        const OpenRouting::CommandLine cl = OpenRouting::parseCommandLine({ QStringLiteral("-") });
        check(cl.readStdin, QStringLiteral("'-' reads standard input"));
        win.startUp(cl, /*primary=*/false);
        check(waitFor([&]() { return tabs(win)->count() == 1; }), QStringLiteral("stdin opens a tab"));
        LogViewWidget* tab = tabs(win)->count() ? currentTab(win) : nullptr;
        if (!tab) {
            closeWriteEnd();
            StdinSpooler::setInputDescriptorForTesting(-1);
            return; // fds[0] остаётся открытым — тест уже упал
        }
        check(tabs(win)->tabText(0).contains(QStringLiteral("stdin")), QStringLiteral("the tab is named stdin: %1").arg(tabs(win)->tabText(0)));
        check(tab->autoReload() && tab->view()->followTail() && command(win, QStringLiteral("autoReload"))->isChecked(),
              QStringLiteral("stdin tab: auto-reload and follow tail on"));
        check(writeLines(0, 200), QStringLiteral("write to the pipe"));
        check(waitFor([&]() { return tab->model()->rowCount() == 200 && !tab->isLoading(); }, 15000),
              QStringLiteral("piped lines show up (%1)").arg(tab->model()->rowCount()));
        check(writeLines(200, 300), QStringLiteral("write more"));
        QScrollBar* bar = tab->view()->verticalScrollBar();
        check(waitFor([&]() {
            return tab->model()->rowCount() == 500 && !tab->isLoading() && !tab->view()->hasPendingLayout()
                && bar->maximum() > 0 && bar->value() == bar->maximum();
        }, 15000), QStringLiteral("more piped lines follow the tail (%1 rows, %2 of %3)")
                       .arg(tab->model()->rowCount()).arg(bar->value()).arg(bar->maximum()));
        const auto last = tab->model()->entryAt(tab->model()->rowCount() - 1);
        check(last && last->message().endsWith(QStringLiteral("piped line 499")), QStringLiteral("the last line is the last piped one"));
        const QString spool = tab->loadedFiles().value(0) ? tab->loadedFiles().value(0)->filePath : QString();
        check(SpoolDir::isStreamFile(spool), QStringLiteral("the tab reads a stream spool: %1").arg(spool));
        check(!inRecentFiles(win, spool), QStringLiteral("the spool is not in Recent Files"));
        closeWriteEnd(); // EOF
        win.close();
    }
    StdinSpooler::setInputDescriptorForTesting(-1);
#ifdef Q_OS_WIN
    _close(fds[0]);
#else
    ::close(fds[0]);
#endif
    (void)fx;
}

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
    // Таймер авто-обновления — 1 с (сценарий растущего файла ждёт его тик).
    settings.setAutoReloadIntervalSecs(1);
    settings.save();

    // Схема фикстур в каталоге (сценарий «schema per tab»).
    {
        SchemaStore::SchemaEntry smoke;
        smoke.name = QStringLiteral("Smoke Fields");
        smoke.schema = testdocs::buildSchema();
        QDir().mkpath(SchemaStore::patternsDir());
        QFile file(QDir(SchemaStore::patternsDir()).filePath(QStringLiteral("smoke-fields.json")));
        if (file.open(QIODevice::WriteOnly))
            file.write(SchemaStore::fileBytes(smoke));
    }

    int restoredTabs = 0;
    DockLayout layoutAtExit;
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
        scenarioCompressed(win, fx);
        scenarioCommand(win);
        scenarioZip(win, fx);
        section(QStringLiteral("shortcuts: with tabs"));
        checkShortcutAmbiguity(win, QStringLiteral("with tabs"));
        scenarioNavigate(win, fx);
        scenarioTabSwitch(win, fx);
        scenarioFullLog(win, fx);
        scenarioGlue(win, fx, dir);
        scenarioTextActions(win, fx);
        scenarioPanels(win, fx);
        scenarioScannerPanel(win, fx);
        scenarioFieldSchemasDialog(win, fx);
        scenarioBookmarkNote(win, fx);
        scenarioTimeDelta(win, fx);
        scenarioSearchFollowsTab(win, fx);
        scenarioEncoding(win, fx);
        scenarioFileMemory(win, fx);
        scenarioSettingsValues(win, fx, indexed);
        scenarioMerge(win, fx);
        scenarioSession(win, fx);
        scenarioDocks(win);
        scenarioLineWrap(win, dir, indexed);
        scenarioAnsiColors(win, dir);
        scenarioFileLabels(win);
        scenarioAutoReload(win, dir);
        scenarioTar(win, fx); // после сценариев, которым нужен alpha в Recent Files
        scenarioKeyedSchema(win, fx);
        scenarioFileSchemas(win, fx);
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

        layoutAtExit = arrangeDocks(win);
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
        checkDockLayout(win, layoutAtExit);
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

    scenarioTabMenu(fx);
    scenarioStdin(fx);
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
    // Генератор строк для сценария команды (F-7): N строк, N < 0 — без конца.
    if (argc > 2 && std::strcmp(argv[1], "--emit-lines") == 0) {
        const int n = std::atoi(argv[2]);
        for (int i = 1; n < 0 || i <= n; ++i) {
            std::printf("2026-11-26 10:00:%02d.%03d INFO smoke command line %d\n", (i / 1000) % 60, i % 1000, i);
            std::fflush(stdout);
            if (n < 0)
                QThread::msleep(20);
        }
        return 0;
    }
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
    // Тема окна — как у приложения (main.cpp); тема «ОС» — светлая на любой
    // машине (сценарий Settings переключает на тёмную и обратно).
    ColorSchemeController colorScheme;
    colorScheme.setSystemSchemeSource([]() { return Qt::ColorScheme::Light; });

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
