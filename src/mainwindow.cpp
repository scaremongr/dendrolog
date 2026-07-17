#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "logviewwidget.h"
#include "conversionpatterndialog.h"
#include "directoryscanner.h"
#include "directoryscannerpanel.h"
#include "appsettings.h"
#include "shortcutmanager.h"
#include "settingsdialog.h"
#include "filterpanelwidget.h"
#include "markerpanelwidget.h"
#include "timelinehistogramwidget.h"
#include "entrydetailspanel.h"
#include "statisticspanel.h"
#include "schemastore.h"
#include "apptheme.h"
#include "updatechecker.h"
#include "stdinspooler.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QProgressBar>
#include <QLabel>
#include <QTimer>
#include <QToolBar>
#include <QDateTimeEdit>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDockWidget>
#include <QTreeWidget>
#include <QHeaderView>
#include <QDir>
#include <QDirIterator>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QMenu>
#include <QApplication>
#include <QToolTip>
#include <QFormLayout>
#include <QSettings>
#include <QCloseEvent>
#include <QMessageBox>
#include <QComboBox>
#include <QToolButton>
#include <QMouseEvent>
#include <QItemSelectionModel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTextStream>
#include <QFile>
#include <QIcon>
#include <QPixmap>
#include <QPainter>
#include <QKeySequence>
#include <QSvgRenderer>
#include <QTextBrowser>
#include <QDialog>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QLocale>
#include <QUrl>
#include <QDate>

#include <algorithm>
#include <limits>

// Renders an SVG resource and recolours it to `color`, so monochrome toolbar
// glyphs stay visible on both light and dark palettes. Uses QSvgRenderer (Qt
// Svg is linked) rather than QPixmap so it does not depend on the SVG image
// format plugin being deployed.
static QIcon tintedIcon(const QString& resourcePath, const QColor& color)
{
    QSvgRenderer renderer(resourcePath);
    if (!renderer.isValid())
        return QIcon(resourcePath);

    QSize size = renderer.defaultSize();
    if (!size.isValid() || size.isEmpty())
        size = QSize(64, 64);
    size.scale(64, 64, Qt::KeepAspectRatio);

    QPixmap result(size);
    result.fill(Qt::transparent);
    QPainter p(&result);
    renderer.render(&p);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(result.rect(), color);
    p.end();
    return QIcon(result);
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow), m_progressBar(nullptr), m_statusLabel(nullptr), m_activeLogView(nullptr), m_lineInfoLabel(nullptr), m_timeFilterFrom(nullptr), m_timeFilterTo(nullptr), m_applyTimeFilterButton(nullptr), m_resetTimeFilterButton(nullptr)
{
    ui->setupUi(this);

    // Load persistent application preferences before anything else so that
    // setup methods (e.g. setupDirectoryScanner) can read correct values.
    AppSettings::instance().load();
    ShortcutManager::instance().load();

    setupStatusBar();
    setupTimeFilterDockContents();
    setupTextFilterDockContents();
    setupRowMarkerDock();
    setupTimelineDock();
    setupSearchResultsDock();
    setupEntryDetailsDock();
    setupStatisticsDock();
    setupDirectoryScanner();
    setupFieldVisibilityDock();
    // После setupFieldVisibilityDock (нужен m_fieldFilterEnabledCheckBox) и
    // ДО loadSettings — restoreState() должен знать тулбар по objectName.
    setupFilterStatusToolbar();

    // Propagate settings changes that originate from the Settings dialog
    // (e.g. word-wrap toggled there) back into the UI without requiring a restart.
    connect(&AppSettings::instance(), &AppSettings::settingsChanged,
            this, [this]() {
        ui->actionWordWrap->setChecked(AppSettings::instance().wordWrap());

        // Apply font to all open log views. setFont() triggers changeEvent(FontChange)
        // inside LogListView which rebuilds the metrics and height cache.
        QFont f(AppSettings::instance().fontFamily());
        f.setPointSize(AppSettings::instance().fontSize());
        f.setWeight(QFont::Medium);
        for (int t = 0; t < ui->tabWidget->count(); ++t) {
            auto* lvw = qobject_cast<LogViewWidget*>(ui->tabWidget->widget(t));
            if (!lvw)
                continue;
            LogListView* lv = lvw->view();
            if (!lv)
                continue;
            // setFont() is a no-op (no changeEvent) when the font didn't change,
            // so we always call viewport()->update() to repaint for colour changes.
            lv->setFont(f);
            lv->viewport()->update();
        }

        // Re-apply auto-reload timer whenever settings change.
        applyAutoReloadSettings();
    });

    // Auto-reload timer – ticks for every tab that has per-tab auto-reload on
    m_autoReloadTimer = new QTimer(this);
    m_autoReloadTimer->setSingleShot(false);
    connect(m_autoReloadTimer, &QTimer::timeout, this, &MainWindow::onAutoReloadTimerTick);

    // Env-gated замер отзывчивости GUI: DENDRO_UI_TRACE=<файл> — таймер 100 мс
    // пишет монотонные метки времени; разрывы между соседними метками = фризы
    // event loop. В обычных запусках недостижимо.
    if (const QString tracePath = qEnvironmentVariable("DENDRO_UI_TRACE");
        !tracePath.isEmpty()) {
        auto* traceFile = new QFile(tracePath, this);
        if (traceFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            auto clock = std::make_shared<QElapsedTimer>();
            clock->start();
            auto* hb = new QTimer(this);
            connect(hb, &QTimer::timeout, this, [traceFile, clock]() {
                traceFile->write(QByteArray::number(qlonglong(clock->elapsed())) + '\n');
                traceFile->flush();
            });
            hb->start(100);
        }
    }

    // --- Reload toolbar button (icon + checkable) ---
    // actionReloadFile keeps the F5 shortcut and menu entry; the toolbar shows a
    // custom QToolButton so we can distinguish left-click (manual) from right-click
    // (toggle per-tab auto-reload). The action text is kept for the menu only.
    const QColor toolGlyphColor = palette().buttonText().color();
    ui->actionReloadFile->setIcon(tintedIcon(QStringLiteral(":/icons/reload.svg"), toolGlyphColor));
    // Replace the plain action widget in the toolbar with a proper QToolButton
    if (QToolButton* btn = qobject_cast<QToolButton*>(ui->toolBar->widgetForAction(ui->actionReloadFile))) {
        m_reloadButton = btn;
        m_reloadButton->setCheckable(true);
        m_reloadButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
        m_reloadButton->setFixedSize(26, 26);
        m_reloadButton->setToolTip(tr("Reload file (F5)\nRight-click to toggle auto-reload for this tab"));
        m_reloadButton->installEventFilter(this);
    }

    // Connect the manual reload action (F5 / menu) – does NOT toggle auto-reload
    connect(ui->actionReloadFile, &QAction::triggered, this, &MainWindow::onReloadFileTriggered);

    // --- Word Wrap toolbar button (icon + checkable), styled like Reload ---
    ui->actionWordWrap->setIcon(tintedIcon(QStringLiteral(":/icons/wordwrap.svg"), toolGlyphColor));
    if (QToolButton* wwBtn = qobject_cast<QToolButton*>(ui->toolBar->widgetForAction(ui->actionWordWrap))) {
        wwBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
        wwBtn->setFixedSize(26, 26);
        wwBtn->setToolTip(tr("Toggle word wrap"));
    }

    // --- Follow-tail toolbar toggle (автопрокрутка к концу растущего лога) ---
    m_followTailAction = new QAction(QStringLiteral("⤓"), this);
    m_followTailAction->setCheckable(true);
    m_followTailAction->setToolTip(
        tr("Follow tail: auto-scroll to new lines (Shift+F5).\n"
           "Scrolling up turns it off."));
    m_followTailAction->setShortcut(QKeySequence(QStringLiteral("Shift+F5")));
    ui->toolBar->addAction(m_followTailAction);
    if (QToolButton* ftBtn = qobject_cast<QToolButton*>(ui->toolBar->widgetForAction(m_followTailAction))) {
        ftBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        ftBtn->setFixedSize(26, 26);
    }
    connect(m_followTailAction, &QAction::toggled, this, [this](bool on) {
        if (m_activeLogView && m_activeLogView->view()
            && m_activeLogView->view()->followTail() != on)
            m_activeLogView->view()->setFollowTail(on);
    });

    // "Tools" menu with Settings action.
    QMenu* menuTools = menuBar()->addMenu(tr("&Tools"));
    QAction* settingsAction = menuTools->addAction(tr("Settings..."));
    connect(settingsAction, &QAction::triggered, this, &MainWindow::onSettingsTriggered);
    m_shortcutActions.insert(QStringLiteral("settings"), settingsAction);

    setupHelpMenu();

    // В конец меню View — сброс раскладки панелей к дефолтной. Док-переключатели
    // вставляются перед ПЕРВЫМ сепаратором меню, так что этот блок им не мешает.
    if (ui->menuView) {
        ui->menuView->addSeparator();
        QAction* resetLayout = ui->menuView->addAction(tr("Reset Panel Layout"));
        connect(resetLayout, &QAction::triggered,
                this, &MainWindow::applyDefaultPanelLayout);
    }

    // Файлы можно бросать в окно из проводника.
    setAcceptDrops(true);

    // Search input is created in code because the .ui currently defines only actions.
    m_searchLineEdit = new QLineEdit(ui->searchToolBar);
    m_searchLineEdit->setObjectName("searchLineEdit");
    m_searchLineEdit->setMaximumWidth(300);
    m_searchLineEdit->setPlaceholderText(tr("Search..."));
    ui->searchToolBar->insertWidget(ui->actionSearchPrevious, m_searchLineEdit);

    // Connect search actions
    connect(m_searchLineEdit, &QLineEdit::returnPressed, this, &MainWindow::onSearchEnterPressed);
    connect(ui->actionSearchNext, &QAction::triggered, this, &MainWindow::onSearchNextTriggered);
    connect(ui->actionSearchPrevious, &QAction::triggered, this, &MainWindow::onSearchPreviousTriggered);

    // Quick-search focus action (Ctrl+F by default): just move focus to the
    // search field and pre-select its text for an immediate new query.
    QAction* focusSearchAction = new QAction(tr("Find"), this);
    connect(focusSearchAction, &QAction::triggered, this, [this]() {
        m_searchLineEdit->setFocus(Qt::ShortcutFocusReason);
        m_searchLineEdit->selectAll();
    });
    addAction(focusSearchAction);   // window-level shortcut
    m_shortcutActions.insert(QStringLiteral("focusSearch"), focusSearchAction);

    // Replace static dock-toggle actions with QDockWidget::toggleViewAction()
    // so checkmarks automatically reflect actual dock visibility
    {
        auto setupDockToggle = [this](QMenu* menu, QAction* staticAction, QDockWidget* dock,
                                      const QString& text, const QString& shortcutId) -> QAction* {
            QAction* a = dock->toggleViewAction();
            a->setText(text);
            menu->insertAction(staticAction, a);
            menu->removeAction(staticAction);
            if (!shortcutId.isEmpty())
                m_shortcutActions.insert(shortcutId, a);
            return a;
        };
        setupDockToggle(ui->menuView, ui->actionToggle_Text_Filters_Panel,
                        ui->textFilterDockWidget, tr("Text Filters Panel"),
                        QStringLiteral("panelTextFilters"));
        setupDockToggle(ui->menuView, ui->actionToggle_Directory_Scanner_Panel,
                        ui->directoryScannerDockWidget, tr("Directory Scanner Panel"),
                        QStringLiteral("panelDirScanner"));
        setupDockToggle(ui->menuView, ui->actionToggle_Time_Filter_Panel,
                        ui->timeFilterDockWidget, tr("Time Filter Panel"),
                        QStringLiteral("panelTimeFilter"));
        setupDockToggle(ui->menuView, ui->actionToggle_Field_Visibility_Panel,
                        ui->fieldVisibilityDockWidget, tr("Log Fields Panel"),
                        QStringLiteral("panelFields"));
    }

    if (ui->tabWidget)
    {
        connect(ui->tabWidget, &QTabWidget::tabCloseRequested, this, [this](int index)
                {
            QWidget *page = ui->tabWidget->widget(index);
            LogViewWidget* logView = qobject_cast<LogViewWidget*>(page);
            if (logView) {
                disconnectFromLogView(logView);
            }
            ui->tabWidget->removeTab(index);
            delete page;
            // The closed tab may have had auto-reload on; update the timer.
            updateAutoReloadTimer();
            if (ui->tabWidget->count() == 0)
                updateStatusBarDefaultText();
            syncReloadButton(); });
        connect(ui->tabWidget, &QTabWidget::currentChanged, this, &MainWindow::onCurrentTabChanged);

        if (ui->tabWidget->count() > 0)
        {
            onCurrentTabChanged(ui->tabWidget->currentIndex());
        }
    }

    if (!m_activeLogView)
    {
        updateStatusBarDefaultText();
        updateLogLevelFilterButtons();
        updateFilterInputsFromModel();
    }

    loadSettings();
    applyAutoReloadSettings();

    // Configurable keyboard shortcuts: map the remaining actions and assign the
    // current sequences. Re-apply automatically when the user edits them.
    registerShortcutActions();
    applyShortcuts();
    connect(&ShortcutManager::instance(), &ShortcutManager::shortcutsChanged,
            this, &MainWindow::applyShortcuts);
}

// ---------------------------------------------------------------------------
void MainWindow::registerShortcutActions()
{
    // Dock-toggle actions and the Settings action are inserted into
    // m_shortcutActions where they are created; register the static ones here.
    m_shortcutActions.insert(QStringLiteral("open"),       ui->actionOpen);
    m_shortcutActions.insert(QStringLiteral("saveAs"),     ui->actionSaveAs);
    m_shortcutActions.insert(QStringLiteral("reload"),     ui->actionReloadFile);
    m_shortcutActions.insert(QStringLiteral("searchNext"), ui->actionSearchNext);
    m_shortcutActions.insert(QStringLiteral("searchPrev"), ui->actionSearchPrevious);
    m_shortcutActions.insert(QStringLiteral("wordWrap"),   ui->actionWordWrap);
}

// ---------------------------------------------------------------------------
void MainWindow::applyShortcuts()
{
    const ShortcutManager& mgr = ShortcutManager::instance();
    for (const auto& cmd : mgr.commands()) {
        if (QAction* a = m_shortcutActions.value(cmd.id, nullptr))
            a->setShortcut(mgr.sequence(cmd.id));
    }
}

// ---------------------------------------------------------------------------
QString MainWindow::logFileDialogFilter() const
{
    QStringList patterns;
    for (const QString& ext : AppSettings::instance().scanExtensions()) {
        const QString e = ext.trimmed();
        if (!e.isEmpty())
            patterns << (QStringLiteral("*.") + e);
    }
    if (patterns.isEmpty())
        patterns << QStringLiteral("*.log") << QStringLiteral("*.txt");

    return tr("Log files (%1);;All files (*)").arg(patterns.join(QLatin1Char(' ')));
}

MainWindow::~MainWindow()
{
    // Make sure no worker thread is still writing into entries we are about
    // to release (closeEvent usually handles this, but not every teardown
    // path goes through it).
    cancelFieldExtraction();
    delete ui;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // Stop any background field re-extraction before our members (which its
    // workers reference) are torn down.
    cancelFieldExtraction();
    saveSettings();
    QMainWindow::closeEvent(event);
}

static QString settingsFilePath()
{
    return AppSettings::iniFilePath();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    const QMimeData* mime = event->mimeData();
    if (!mime->hasUrls())
        return;
    for (const QUrl& url : mime->urls()) {
        if (url.isLocalFile()) {
            event->acceptProposedAction();
            return;
        }
    }
}

void MainWindow::dropEvent(QDropEvent* event)
{
    QStringList paths;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (!url.isLocalFile())
            continue;
        const QFileInfo info(url.toLocalFile());
        if (info.isFile())
            paths << info.absoluteFilePath();
    }
    if (!paths.isEmpty()) {
        event->acceptProposedAction();
        openFilesFromCommandLine(paths);
    }
}

void MainWindow::applyDefaultPanelLayout()
{
    // Нижняя зона занимает всю ширину окна: таймлайн — шкала времени, ей
    // нужна длина; правая колонка живёт между тулбарами и нижней зоной.
    setCorner(Qt::BottomLeftCorner,  Qt::BottomDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);

    const QList<QDockWidget*> sideDocks {
        ui->textFilterDockWidget,
        ui->timeFilterDockWidget,
        ui->fieldVisibilityDockWidget,
        m_markerDockWidget,
        m_statsDockWidget,
        ui->directoryScannerDockWidget,
    };
    const QList<QDockWidget*> bottomDocks {
        m_timelineDockWidget,
        m_searchResultsDockWidget,
        m_detailsDockWidget,
    };

    // Стартовое состояние — только список логов. Панели включаются по мере
    // надобности (меню View / Ctrl+F1…) и появляются уже на местах,
    // расставленных ниже: скрытый док запоминает позицию и вкладочную группу.
    for (const auto& docks : { sideDocks, bottomDocks })
        for (QDockWidget* d : docks)
            if (d) {
                d->setFloating(false);
                d->hide();
            }

    // Правая колонка: все панели одной вкладочной группой — сколько бы их ни
    // включили, они делят одно место, а не сжимают друг друга по вертикали.
    QDockWidget* prev = nullptr;
    for (QDockWidget* d : sideDocks) {
        if (!d)
            continue;
        addDockWidget(Qt::RightDockWidgetArea, d);
        if (prev)
            tabifyDockWidget(prev, d);
        prev = d;
    }

    // Низ: таймлайн во всю ширину; под ним — результаты поиска; Entry Details
    // вкладкой при результатах (обе — «инспекция» текущей позиции).
    if (m_timelineDockWidget)
        addDockWidget(Qt::BottomDockWidgetArea, m_timelineDockWidget);
    if (m_searchResultsDockWidget) {
        addDockWidget(Qt::BottomDockWidgetArea, m_searchResultsDockWidget);
        if (m_timelineDockWidget)
            splitDockWidget(m_timelineDockWidget, m_searchResultsDockWidget,
                            Qt::Vertical);
    }
    if (m_detailsDockWidget) {
        addDockWidget(Qt::BottomDockWidgetArea, m_detailsDockWidget);
        if (m_searchResultsDockWidget)
            tabifyDockWidget(m_searchResultsDockWidget, m_detailsDockWidget);
    }
}

void MainWindow::setupHelpMenu()
{
    QMenu* menuHelp = menuBar()->addMenu(tr("&Help"));

    QAction* helpAction = menuHelp->addAction(tr("Quick Help"));
    helpAction->setShortcut(QKeySequence::HelpContents); // F1
    connect(helpAction, &QAction::triggered, this, &MainWindow::showHelp);

    menuHelp->addSeparator();

    QAction* updatesAction = menuHelp->addAction(tr("Check for Updates..."));
    connect(updatesAction, &QAction::triggered,
            this, [this]() { checkForUpdates(/*interactive=*/true); });

    QAction* aboutAction = menuHelp->addAction(tr("About DendroLog"));
    connect(aboutAction, &QAction::triggered, this, &MainWindow::showAbout);

    // Тихая проверка обновлений не чаще раза в неделю, с задержкой после
    // старта, чтобы не задерживать открытие файлов из командной строки.
    QSettings s(settingsFilePath(), QSettings::IniFormat);
    const QDate lastCheck =
        QDate::fromString(s.value(QStringLiteral("Updates/lastCheckDate")).toString(),
                          Qt::ISODate);
    if (!lastCheck.isValid() || lastCheck.daysTo(QDate::currentDate()) >= 7)
        QTimer::singleShot(3000, this, [this]() { checkForUpdates(/*interactive=*/false); });
}

void MainWindow::showHelp()
{
    if (!m_helpDialog) {
        m_helpDialog = new QDialog(this);
        m_helpDialog->setWindowTitle(tr("DendroLog — Help"));
        auto* layout = new QVBoxLayout(m_helpDialog);
        auto* browser = new QTextBrowser(m_helpDialog);
        browser->setOpenExternalLinks(true);
        const QString resource = QLocale().language() == QLocale::Russian
            ? QStringLiteral(":/help/help_ru.md")
            : QStringLiteral(":/help/help_en.md");
        QFile f(resource);
        if (f.open(QIODevice::ReadOnly))
            browser->setMarkdown(QString::fromUtf8(f.readAll()));
        layout->addWidget(browser);
        m_helpDialog->resize(780, 640);
    }
    m_helpDialog->show();
    m_helpDialog->raise();
    m_helpDialog->activateWindow();
}

void MainWindow::showAbout()
{
    const QString text = tr(
        "<h3>DendroLog %1</h3>"
        "<p>A fast viewer for large log files: multi-file tabs, structured field "
        "extraction, filtering, highlighting and live reload.</p>"
        "<p><a href=\"%2\">%2</a></p>"
        "<p>Copyright © 2026 Anton Petrov. Licensed under the MIT License.</p>"
        "<p>Built with Qt %3 (dynamically linked, LGPLv3).</p>")
        .arg(QApplication::applicationVersion(),
             UpdateChecker::repoUrl(),
             QString::fromLatin1(qVersion()));
    QMessageBox::about(this, tr("About DendroLog"), text);
}

void MainWindow::checkForUpdates(bool interactive)
{
    if (!m_updateChecker) {
        m_updateChecker = new UpdateChecker(this);

        connect(m_updateChecker, &UpdateChecker::updateAvailable,
                this, [this](const QString& version, const QString& url) {
            QSettings s(settingsFilePath(), QSettings::IniFormat);
            // Фоновая проверка напоминает об одной и той же версии только раз.
            const QString notifiedKey = QStringLiteral("Updates/lastNotifiedVersion");
            if (!m_updateCheckInteractive && s.value(notifiedKey).toString() == version)
                return;
            s.setValue(notifiedKey, version);

            QMessageBox box(this);
            box.setWindowTitle(tr("Update Available"));
            box.setText(tr("DendroLog %1 is available (you have %2).")
                            .arg(version, QApplication::applicationVersion()));
            QPushButton* open = box.addButton(tr("Open Download Page"),
                                              QMessageBox::AcceptRole);
            box.addButton(QMessageBox::Close);
            box.exec();
            if (box.clickedButton() == open)
                QDesktopServices::openUrl(QUrl(url));
        });

        connect(m_updateChecker, &UpdateChecker::upToDate, this, [this]() {
            if (m_updateCheckInteractive)
                QMessageBox::information(this, tr("Check for Updates"),
                                         tr("You are running the latest version (%1).")
                                             .arg(QApplication::applicationVersion()));
        });

        connect(m_updateChecker, &UpdateChecker::checkFailed,
                this, [this](const QString& error) {
            if (m_updateCheckInteractive)
                QMessageBox::warning(this, tr("Check for Updates"),
                                     tr("Could not check for updates:\n%1").arg(error));
        });
    }

    m_updateCheckInteractive = interactive;
    QSettings s(settingsFilePath(), QSettings::IniFormat);
    s.setValue(QStringLiteral("Updates/lastCheckDate"),
               QDate::currentDate().toString(Qt::ISODate));
    m_updateChecker->check();
}

void MainWindow::saveSettings()
{
    QSettings s(settingsFilePath(), QSettings::IniFormat);

    s.beginGroup("Window");
    s.setValue("geometry", saveGeometry());
    s.setValue("state",    saveState());
    s.endGroup();

    s.beginGroup("Files");
    s.setValue("lastOpenDir", m_lastOpenDir);
    s.setValue("lastScanDir", m_lastScanDir);
    s.endGroup();

    s.beginGroup("View");
    {
        s.setValue("fieldFilterEnabled",
                   m_fieldFilterEnabledCheckBox && m_fieldFilterEnabledCheckBox->isChecked());
        s.setValue("fieldVisibleNames", selectedVisibleFieldNames());
    }
    s.setValue("conversionPattern", m_conversionPattern);
    // Schemas themselves live as files in <configDir>/patterns/ (see
    // SchemaStore); the INI only remembers their display order, keyed by name.
    {
        QStringList order;
        order.reserve(m_patternList.size());
        for (const auto& e : m_patternList)
            order << e.first;
        s.setValue("schemaOrder", order);
    }
    s.endGroup();

    s.beginGroup("Filters");
    if (m_filterPanel) {
        const QJsonDocument doc(m_filterPanel->profilesToJson());
        s.setValue("textFilterProfiles", QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
        s.remove("textFilterRules"); // legacy одиночный набор больше не пишем
        s.setValue("textFilterMode", static_cast<int>(m_filterPanel->mode()));
        s.setValue("highlightInMainView", m_filterPanel->highlightInMainView());
    }
    if (m_markerPanel) {
        const QJsonDocument doc(QJsonObject{
            {QStringLiteral("markers"), highlightPatternsToJson(m_markerPanel->markers())}});
        s.setValue("rowMarkers", QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
    }
    s.endGroup();

    s.beginGroup("RecentFiles");
    s.setValue("files", m_recentFiles);
    s.endGroup();

    // Sync the current word-wrap toggle state back to AppSettings and persist
    // all app-level preferences (scan extensions, word wrap, etc.) in one call.
    AppSettings::instance().setWordWrap(ui->actionWordWrap->isChecked());
    AppSettings::instance().save();
    ShortcutManager::instance().save();
}

void MainWindow::loadSettings()
{
    QSettings s(settingsFilePath(), QSettings::IniFormat);

    s.beginGroup("Window");
    QByteArray geometry = s.value("geometry").toByteArray();
    QByteArray state    = s.value("state").toByteArray();
    s.endGroup();

    if (!geometry.isEmpty())
        restoreGeometry(geometry);
    if (!state.isEmpty())
        restoreState(state);
    else
        // Первый запуск (или удалённый ini): вместо сырой Qt-раскладки —
        // аккуратная дефолтная, со скрытыми панелями.
        applyDefaultPanelLayout();

    // Layout, сохранённый версией без таймлайн-дока, оставляет новому доку
    // нулевую высоту. До show() resizeDocks игнорируется, поэтому проверка
    // откладывается до первого прохода event loop; трогаем размер только у
    // реально сплющенного дока, чтобы не затирать выбранную пользователем высоту.
    QTimer::singleShot(0, this, [this]() {
        if (m_timelineDockWidget && m_timelinePanel && !m_timelineDockWidget->isFloating()
            && m_timelineDockWidget->isVisible()
            && m_timelinePanel->height() < m_timelinePanel->minimumSizeHint().height())
        {
            resizeDocks({ m_timelineDockWidget },
                        { m_timelinePanel->sizeHint().height() }, Qt::Vertical);
        }
    });

    s.beginGroup("Files");
    m_lastOpenDir = s.value("lastOpenDir").toString();
    m_lastScanDir = s.value("lastScanDir").toString();
    s.endGroup();

    // Word wrap is owned by AppSettings (already loaded in the constructor
    // before this method is called).
    ui->actionWordWrap->setChecked(AppSettings::instance().wordWrap());

    s.beginGroup("View");
    const bool filterEnabled = s.value("fieldFilterEnabled", false).toBool();
    m_savedVisibleFieldNames = s.value("fieldVisibleNames").toStringList();
    const int legacyMask = s.value("fieldVisibilityMask", -1).toInt();

    m_conversionPattern = s.value("conversionPattern").toString();
    {
        // Schemas are loaded from <exe>/patterns/*.json; the INI keeps only
        // their display order, by name.
        const QStringList order = s.value("schemaOrder").toStringList();
        m_patternList = SchemaStore::loadAll(order);

        // One-time migration of the old INI-stored schema list into files.
        if (m_patternList.isEmpty()) {
            const QStringList names  = s.value("patternNames").toStringList();
            const QStringList values = s.value("patternValues").toStringList();
            const int cnt = qMin(names.size(), values.size());
            m_patternList.reserve(cnt);
            for (int i = 0; i < cnt; ++i)
                m_patternList.append({names[i], values[i]});
            // Even older single-string list.
            if (m_patternList.isEmpty()) {
                const QStringList old = s.value("conversionPatternList").toStringList();
                for (const auto& p : old)
                    if (!p.trimmed().isEmpty())
                        m_patternList.append({p.trimmed(), p.trimmed()});
            }
            if (!m_patternList.isEmpty()) {
                SchemaStore::sync(m_patternList);
                s.remove("patternNames");
                s.remove("patternValues");
                s.remove("conversionPatternList");
            }
        }
        if (m_conversionPatternCombo) {
            m_conversionPatternCombo->blockSignals(true);
            m_conversionPatternCombo->clear();
            for (const auto& e : m_patternList)
                m_conversionPatternCombo->addItem(e.first);
            // Restore the active selection
            for (int i = 0; i < m_patternList.size(); ++i) {
                if (m_patternList[i].second == m_conversionPattern) {
                    m_conversionPatternCombo->setCurrentIndex(i);
                    break;
                }
            }
            m_conversionPatternCombo->blockSignals(false);
        }
    }

    if (m_savedVisibleFieldNames.isEmpty() && legacyMask >= 0) {
        const QStringList legacyFieldNames = LogPattern(m_conversionPattern).fieldNames();
        for (int i = 0; i < legacyFieldNames.size() && i < 31; ++i) {
            if (legacyMask & (1 << i))
                m_savedVisibleFieldNames.append(legacyFieldNames[i]);
        }
    }

    if (m_fieldFilterEnabledCheckBox) {
        m_fieldFilterEnabledCheckBox->blockSignals(true);
        m_fieldFilterEnabledCheckBox->setChecked(filterEnabled);
        m_fieldFilterEnabledCheckBox->blockSignals(false);
    }
    rebuildFieldVisibilityControls(LogPattern(m_conversionPattern).fieldNames());
    s.endGroup();

    // Чекбокс Log Fields восстановлен с blockSignals — синхронизировать
    // кнопку-индикатор Fields вручную.
    updateFilterStatusButtons();

    s.beginGroup("Filters");
    if (m_filterPanel) {
        const QByteArray profilesJson = s.value("textFilterProfiles").toString().toUtf8();
        if (!profilesJson.isEmpty()) {
            const QJsonDocument doc = QJsonDocument::fromJson(profilesJson);
            if (doc.isObject())
                m_filterPanel->profilesFromJson(doc.object());
        } else {
            // Миграция старого формата: одиночный textFilterRules → профиль «Default».
            const QByteArray rulesJson = s.value("textFilterRules").toString().toUtf8();
            if (!rulesJson.isEmpty()) {
                const QJsonDocument doc = QJsonDocument::fromJson(rulesJson);
                if (doc.isObject())
                    m_filterPanel->setRuleSet(FilterRuleSet::fromJson(doc.object()));
            }
        }
        // Режим и галочка восстанавливаются тихо (setMode/setHighlightInMainView
        // блокируют сигналы) — видимость самого дока восстановит restoreState.
        m_filterPanel->setHighlightInMainView(
            s.value("highlightInMainView", true).toBool());
        m_filterPanel->setMode(static_cast<FilterPanelWidget::Mode>(
            s.value("textFilterMode", static_cast<int>(FilterPanelWidget::Mode::Filter)).toInt()));
    }
    if (m_markerPanel) {
        const QByteArray markersJson = s.value("rowMarkers").toString().toUtf8();
        if (!markersJson.isEmpty()) {
            const QJsonDocument doc = QJsonDocument::fromJson(markersJson);
            if (doc.isObject()) {
                m_markerPanel->setMarkers(highlightPatternsFromJson(
                    doc.object()[QStringLiteral("markers")].toArray()));
            }
        }
    }
    s.endGroup();
    // Привязать загруженные правила к восстановленной схеме полей.
    // К документам ничего не применяется: фильтрация пер-вкладочная,
    // пользователь применяет правила к нужному документу сам.
    updateFilterPanelFieldNames();

    s.beginGroup("RecentFiles");
    m_recentFiles = s.value("files").toStringList();
    s.endGroup();

    updateRecentFilesMenu();
}

// =============================================================================
// Field-visibility dock
// =============================================================================

void MainWindow::setupFieldVisibilityDock()
{
    QWidget* contents = ui->fieldVisibilityContentsWidget;
    if (!contents) return;

    auto* rootLayout = new QVBoxLayout(contents);
    rootLayout->setContentsMargins(4, 4, 4, 4);
    rootLayout->setSpacing(2);

    // ---- Master enable/disable toggle ----
    m_fieldFilterEnabledCheckBox = new QCheckBox(tr("Filter blocks"), contents);
    m_fieldFilterEnabledCheckBox->setChecked(false); // default: no filtering
    m_fieldFilterEnabledCheckBox->setToolTip(tr(
        "When checked, only the selected blocks below are shown.\n"
        "Uncheck to display the original line regardless of block selection."));
    {
        QFont f = m_fieldFilterEnabledCheckBox->font();
        f.setBold(true);
        m_fieldFilterEnabledCheckBox->setFont(f);
    }
    rootLayout->addWidget(m_fieldFilterEnabledCheckBox);

    // Container for the per-field controls — enabled/disabled together.
    m_fieldFilterControlsWidget = new QWidget(contents);
    auto* filterControlsLayout = new QVBoxLayout(m_fieldFilterControlsWidget);
    filterControlsLayout->setContentsMargins(16, 0, 0, 0);
    filterControlsLayout->setSpacing(2);
    filterControlsLayout->setSizeConstraint(QLayout::SetMinAndMaxSize);

    auto* fieldsLabel = new QLabel(tr("<b>Visible blocks:</b>"), m_fieldFilterControlsWidget);
    filterControlsLayout->addWidget(fieldsLabel);

    // "All blocks" tri-state master toggle
    m_allFieldsCheckBox = new QCheckBox(tr("(all blocks)"), m_fieldFilterControlsWidget);
    m_allFieldsCheckBox->setTristate(true);
    m_allFieldsCheckBox->setCheckState(Qt::Checked);
    connect(m_allFieldsCheckBox, &QCheckBox::stateChanged, this, [this](int state) {
        if (static_cast<Qt::CheckState>(state) == Qt::PartiallyChecked) return;
        const bool checkAll = (state == Qt::Checked);
        for (QCheckBox* cb : m_fieldCheckBoxes) {
            if (!cb)
                continue;
            cb->blockSignals(true);
            cb->setChecked(checkAll);
            cb->blockSignals(false);
        }
        onFieldVisibilityChanged();
    });
    filterControlsLayout->addWidget(m_allFieldsCheckBox);

    m_fieldCheckboxLayout = new QVBoxLayout();
    m_fieldCheckboxLayout->setContentsMargins(0, 0, 0, 0);
    m_fieldCheckboxLayout->setSpacing(2);
    filterControlsLayout->addLayout(m_fieldCheckboxLayout);

    rootLayout->addWidget(m_fieldFilterControlsWidget);

    connect(m_fieldFilterEnabledCheckBox, &QCheckBox::toggled, this, [this](bool enabled) {
        if (m_fieldFilterControlsWidget)
            m_fieldFilterControlsWidget->setEnabled(enabled && !m_fieldCheckBoxes.isEmpty());
        // applyPatternToAllViews() включает/выключает извлечение полей,
        // ре-экстрагирует поля на уже загруженных записях и перепривязывает
        // колоночные правила конструктора фильтров (галочка "Filter blocks"
        // управляет доступностью строгой фильтрации по колонкам).
        applyPatternToAllViews();
    });
    if (m_fieldFilterControlsWidget)
        m_fieldFilterControlsWidget->setEnabled(false);

    rootLayout->addSpacing(4);

    // ---- Section: field schema ----
    auto* patternLabel = new QLabel(tr("<b>Field schema:</b>"), contents);
    patternLabel->setToolTip(tr(
        "A schema is an ordered list of blocks.\n"
        "Each block has a name and a match rule: timestamp, level, integer, text until separator,\n"
        "greedy text, custom regex, or remainder of line.\n\n"
        "Use 'Manage...' to build and edit schemas."));
    rootLayout->addWidget(patternLabel);

    m_conversionPatternCombo = new QComboBox(contents);
    m_conversionPatternCombo->setEditable(false);
    m_conversionPatternCombo->setToolTip(patternLabel->toolTip());
    connect(m_conversionPatternCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onPatternComboChanged);
    rootLayout->addWidget(m_conversionPatternCombo);

    auto* manageBtn = new QPushButton(tr("Manage..."), contents);
    manageBtn->setToolTip(tr("Create, edit and delete named field schemas."));
    connect(manageBtn, &QPushButton::clicked,
            this, &MainWindow::onManagePatterns);
    rootLayout->addWidget(manageBtn);

        // Keep controls packed at the top of the dock.
        rootLayout->addStretch();

    m_fieldFilterControlsWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    contents->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    ui->fieldVisibilityDockWidget->setSizePolicy(QSizePolicy::Preferred,
                                                  QSizePolicy::Preferred);

    rebuildFieldVisibilityControls(LogPattern(m_conversionPattern).fieldNames());
}

void MainWindow::rebuildFieldVisibilityControls(const QStringList& fieldNames)
{
    if (!m_fieldCheckboxLayout)
        return;

    while (QLayoutItem* item = m_fieldCheckboxLayout->takeAt(0)) {
        if (QWidget* widget = item->widget())
            delete widget;
        delete item;
    }
    m_fieldCheckBoxes.clear();

    bool anySavedMatch = false;
    for (const QString& fieldName : fieldNames) {
        auto* cb = new QCheckBox(fieldName, m_fieldFilterControlsWidget);
        const bool shouldCheck = m_savedVisibleFieldNames.isEmpty()
            || m_savedVisibleFieldNames.contains(fieldName);
        anySavedMatch = anySavedMatch || (!m_savedVisibleFieldNames.isEmpty() && shouldCheck);
        cb->setChecked(shouldCheck);
        connect(cb, &QCheckBox::toggled, this, &MainWindow::onFieldVisibilityChanged);
        m_fieldCheckBoxes.push_back(cb);
        m_fieldCheckboxLayout->addWidget(cb);
    }

    if (!fieldNames.isEmpty() && !m_savedVisibleFieldNames.isEmpty() && !anySavedMatch) {
        for (QCheckBox* cb : m_fieldCheckBoxes)
            cb->setChecked(true);
    }

    if (fieldNames.isEmpty()) {
        auto* placeholder = new QLabel(
            tr("No active schema. Build one in 'Manage...' and apply it to the open tabs."),
            m_fieldFilterControlsWidget);
        placeholder->setWordWrap(true);
        m_fieldCheckboxLayout->addWidget(placeholder);
    }

    if (m_allFieldsCheckBox)
        m_allFieldsCheckBox->setEnabled(!m_fieldCheckBoxes.isEmpty());
    if (m_fieldFilterControlsWidget && m_fieldFilterEnabledCheckBox)
        m_fieldFilterControlsWidget->setEnabled(m_fieldFilterEnabledCheckBox->isChecked() && !m_fieldCheckBoxes.isEmpty());

    onFieldVisibilityChanged();
}

QVector<int> MainWindow::selectedVisibleFieldIndexes() const
{
    QVector<int> indexes;
    indexes.reserve(m_fieldCheckBoxes.size());
    for (int i = 0; i < m_fieldCheckBoxes.size(); ++i) {
        if (m_fieldCheckBoxes[i] && m_fieldCheckBoxes[i]->isChecked())
            indexes.push_back(i);
    }
    return indexes;
}

QStringList MainWindow::selectedVisibleFieldNames() const
{
    QStringList names;
    names.reserve(m_fieldCheckBoxes.size());
    for (QCheckBox* cb : m_fieldCheckBoxes) {
        if (cb && cb->isChecked())
            names.push_back(cb->text());
    }
    return names;
}

void MainWindow::onFieldVisibilityChanged()
{
    m_savedVisibleFieldNames = selectedVisibleFieldNames();

    if (m_allFieldsCheckBox) {
        int checkedCount = 0;
        for (QCheckBox* cb : m_fieldCheckBoxes)
            if (cb && cb->isChecked())
                ++checkedCount;
        m_allFieldsCheckBox->blockSignals(true);
        if (m_fieldCheckBoxes.isEmpty() || checkedCount == 0)
            m_allFieldsCheckBox->setCheckState(Qt::Unchecked);
        else if (checkedCount == m_fieldCheckBoxes.size())
            m_allFieldsCheckBox->setCheckState(Qt::Checked);
        else
            m_allFieldsCheckBox->setCheckState(Qt::PartiallyChecked);
        m_allFieldsCheckBox->blockSignals(false);
    }
    applyFieldVisibilityToAllViews();
}

void MainWindow::applyFieldVisibilityToAllViews()
{
    const bool filterEnabled = m_fieldFilterEnabledCheckBox
        && m_fieldFilterEnabledCheckBox->isChecked()
        && !m_fieldCheckBoxes.isEmpty();
    const QVector<int> visibleIndexes = filterEnabled ? selectedVisibleFieldIndexes()
                                                      : QVector<int>();

    for (int t = 0; t < ui->tabWidget->count(); ++t) {
        auto* lv = qobject_cast<LogViewWidget*>(ui->tabWidget->widget(t));
        if (lv && lv->model())
            lv->model()->setFieldDisplaySelection(filterEnabled, visibleIndexes);
    }
}

void MainWindow::onConversionPatternApply()
{
    applyPatternToAllViews();
}

void MainWindow::onPatternComboChanged(int index)
{
    if (index >= 0 && index < m_patternList.size())
        m_conversionPattern = m_patternList[index].second;
    else
        m_conversionPattern.clear();

    // Combo selection change must immediately rebind parser schema and field list.
    applyPatternToAllViews();
}

void MainWindow::onManagePatterns()
{
    // Refresh from the folder so schema files dropped in (or edited) since the
    // last load are picked up, preserving the current display order.
    {
        QStringList order;
        order.reserve(m_patternList.size());
        for (const auto& e : m_patternList)
            order << e.first;
        m_patternList = SchemaStore::loadAll(order);
    }

    // Offer log lines as live-preview samples, starting from the row the
    // user is currently looking at (top of the log when nothing is
    // selected) — schema problems usually live at the current position.
    QStringList sampleLines;
    if (auto* lv = qobject_cast<LogViewWidget*>(ui->tabWidget->currentWidget())) {
        if (LogModel* model = lv->model()) {
            const int total = model->rowCount();
            int row = lv->view() ? lv->view()->currentIndex().row() : -1;
            if (row < 0)
                row = 0;
            for (; row < total && sampleLines.size() < 8; ++row) {
                const QString text = model->messageAt(row);
                if (!text.trimmed().isEmpty())
                    sampleLines.append(text);
            }
            if (sampleLines.isEmpty())
                sampleLines = model->sampleMessages(8);
        }
    }

    ConversionPatternDialog dlg(m_patternList, this, sampleLines, m_conversionPattern);
    if (dlg.exec() != QDialog::Accepted) return;

    m_patternList = dlg.resultPatterns();

    // Persist the (possibly edited) list back to the patterns folder.
    QString syncError;
    if (!SchemaStore::sync(m_patternList, &syncError) && !syncError.isEmpty())
        QMessageBox::warning(this, tr("Schemas"),
            tr("Could not save schemas to the patterns folder:\n%1").arg(syncError));

    // Rebuild combo names.  Set index to -1 first so any subsequent
    // setCurrentIndex() always triggers currentIndexChanged, even for index 0.
    m_conversionPatternCombo->blockSignals(true);
    m_conversionPatternCombo->clear();
    for (const auto& e : m_patternList)
        m_conversionPatternCombo->addItem(e.first);
    m_conversionPatternCombo->setCurrentIndex(-1);
    m_conversionPatternCombo->blockSignals(false);

    const QString chosen    = dlg.chosenPattern();
    const int     chosenIdx = dlg.chosenResultIndex();
    if (!chosen.isEmpty()) {
        m_conversionPattern = chosen;
        // Directly select by result index — no string comparison needed.
        if (chosenIdx >= 0 && chosenIdx < m_conversionPatternCombo->count())
            m_conversionPatternCombo->setCurrentIndex(chosenIdx);
        applyPatternToAllViews();
    } else {
        // Dialog closed without "Use Selected" — restore previous active pattern
        // in the combo, or leave unselected if it's no longer in the list.
        for (int i = 0; i < m_patternList.size(); ++i) {
            if (m_patternList[i].second == m_conversionPattern) {
                m_conversionPatternCombo->setCurrentIndex(i);
                break;
            }
        }
    }
}


void MainWindow::cancelFieldExtraction()
{
    if (!m_fieldWatcher)
        return;
    // Detach our slots first so the cancelled run does not finalize, then
    // wait for the worker threads to actually stop touching entry->fields().
    m_fieldWatcher->disconnect(this);
    m_fieldWatcher->cancel();
    m_fieldWatcher->waitForFinished();
    m_fieldWatcher->deleteLater();
    m_fieldWatcher = nullptr;
    m_pendingFieldEntries.clear();
    m_pendingFieldPattern.reset();
}

void MainWindow::applyPatternToAllViews()
{
    // A schema switch supersedes any re-extraction still in flight.
    cancelFieldExtraction();

    auto pattern = std::make_shared<LogPattern>(m_conversionPattern);
    rebuildFieldVisibilityControls(pattern->fieldNames());

    const bool filterEnabled = m_fieldFilterEnabledCheckBox
        && m_fieldFilterEnabledCheckBox->isChecked()
        && !m_fieldCheckBoxes.isEmpty();
    const bool doExtract = filterEnabled && pattern->isValid();

    // Point every view's parser at the new schema up-front so newly loaded
    // lines are parsed correctly even while the back-fill runs.
    QVector<std::shared_ptr<LogEntry>> entries;
    for (int t = 0; t < ui->tabWidget->count(); ++t) {
        auto* lv = qobject_cast<LogViewWidget*>(ui->tabWidget->widget(t));
        if (!lv)
            continue;

        lv->setParserPattern(m_conversionPattern);
        lv->setExtractionEnabled(filterEnabled);

        if (!lv->model())
            continue;

        // Индексный бэкенд: спаны полей не хранятся — извлечение идёт по
        // требованию новым паттерном (он уже передан через setParserPattern).
        // Мутировать нечего; отображение обновит finishPatternApplication().
        if (lv->model()->isIndexedBackend())
            continue;

        for (const auto& entry : lv->model()->residentEntriesForFieldMutation()) {
            if (!entry)
                continue;
            if (doExtract)
                entries.append(entry);
            else
                entry->setFields(LogEntryFields());
        }
    }

    // Small batches (or "no extraction") complete instantly on the GUI
    // thread — no worker, no progress bar, no display flicker.
    constexpr int kAsyncThreshold = 4000;
    if (!doExtract || entries.size() < kAsyncThreshold) {
        for (const auto& entry : entries)
            entry->setFields(pattern->extractFields(entry->message()));
        finishPatternApplication();
        return;
    }

    // Large batch: re-extract in the background and show the same status-bar
    // progress as file loading. While it runs we turn OFF field display so
    // formatDisplayMessage never reads entry->fields() concurrently with the
    // workers that overwrite them (it falls back to the raw message text).
    for (int t = 0; t < ui->tabWidget->count(); ++t) {
        auto* lv = qobject_cast<LogViewWidget*>(ui->tabWidget->widget(t));
        if (lv && lv->model())
            lv->model()->setFieldDisplaySelection(false, QVector<int>());
    }

    // Warm up the regex on this thread (QRegularExpression compiles lazily).
    pattern->extractFields(QString());

    m_pendingFieldPattern = pattern;
    m_pendingFieldEntries = std::move(entries);

    m_fieldWatcher = new QFutureWatcher<void>(this);
    connect(m_fieldWatcher, &QFutureWatcher<void>::progressRangeChanged,
            m_progressBar, &QProgressBar::setRange);
    connect(m_fieldWatcher, &QFutureWatcher<void>::progressValueChanged,
            m_progressBar, &QProgressBar::setValue);
    connect(m_fieldWatcher, &QFutureWatcher<void>::finished,
            this, &MainWindow::onFieldExtractionFinished);

    m_statusLabel->setText(tr("Applying field schema…"));
    m_progressBar->setRange(0, m_pendingFieldEntries.size());
    m_progressBar->setValue(0);
    m_progressBar->show();

    // Воркеры ниже перезаписывают entry->fields(), а фоновая перефильтрация
    // модели эти поля читает — останавливаем фильтр-джобы всех вкладок
    // (с ожиданием) до старта. Фильтры переприменятся по завершении схемы
    // в finishPatternApplication().
    for (int t = 0; t < ui->tabWidget->count(); ++t) {
        auto* lv = qobject_cast<LogViewWidget*>(ui->tabWidget->widget(t));
        if (lv && lv->model())
            lv->model()->cancelPendingFilter(true);
    }

    auto worker = [pattern](const std::shared_ptr<LogEntry>& entry) {
        if (entry)
            entry->setFields(pattern->extractFields(entry->message()));
    };
    m_fieldWatcher->setFuture(QtConcurrent::map(m_pendingFieldEntries, worker));
}

void MainWindow::onFieldExtractionFinished()
{
    if (m_fieldWatcher) {
        m_fieldWatcher->deleteLater();
        m_fieldWatcher = nullptr;
    }
    m_pendingFieldEntries.clear();
    m_pendingFieldPattern.reset();

    m_progressBar->hide();
    updateStatusBarDefaultText();

    finishPatternApplication();
}

void MainWindow::finishPatternApplication()
{
    for (int t = 0; t < ui->tabWidget->count(); ++t) {
        auto* lv = qobject_cast<LogViewWidget*>(ui->tabWidget->widget(t));
        if (lv && lv->model()) {
            lv->model()->refreshDisplay();
            // Фильтр-джоб, отменённый на время переизвлечения полей, мог
            // оставить список строк со старыми настройками — доводим.
            lv->model()->reapplyFilterIfStale();
        }
    }

    // Re-enables field display with the active selection (it was turned off
    // during a background re-extraction).
    applyFieldVisibilityToAllViews();

    // Схема изменилась — обновляем список колонок в конструкторе фильтров и
    // перепривязываем уже применённые колоночные правила каждой вкладки
    // (не навязывая правила панели вкладкам без фильтров).
    updateFilterPanelFieldNames();
    rebindFiltersOnAllViews();
}

LogViewWidget* MainWindow::createLogViewWidget()
{
    auto* view = new LogViewWidget(this);
    view->setParserPattern(m_conversionPattern);
    const bool filterEnabled = m_fieldFilterEnabledCheckBox
        && m_fieldFilterEnabledCheckBox->isChecked()
        && !m_fieldCheckBoxes.isEmpty();
    view->setExtractionEnabled(filterEnabled);
    if (view->model())
        view->model()->setFieldDisplaySelection(filterEnabled, selectedVisibleFieldIndexes());

    // Новый документ открывается БЕЗ фильтров и маркеров: применение
    // пер-вкладочное — пользователь нажимает Apply в панели фильтров,
    // когда хочет отфильтровать именно этот документ. Сами правила в
    // панелях при этом сохраняются (и переживают перезапуск).

    // Start the timer if this new tab has auto-reload enabled by default.
    updateAutoReloadTimer();
    return view;
}

void MainWindow::setupStatusBar()
{
    m_lineInfoLabel = new QLabel(this);
    m_lineInfoLabel->setMinimumWidth(120);
    ui->statusbar->addWidget(m_lineInfoLabel);
    updateLineInfoLabel(-1, 0);

    m_statusLabel = new QLabel(tr("Ready"), this);
    ui->statusbar->addWidget(m_statusLabel, 1);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    m_progressBar->setFixedWidth(200);
    m_progressBar->hide();
    ui->statusbar->addPermanentWidget(m_progressBar);
}

void MainWindow::setupTimeFilterDockContents()
{
    if (!ui->timeFilterContentsWidget)
    {
        qWarning("timeFilterContentsWidget not found in UI. Time filter dock cannot be populated.");
        return;
    }

    QVBoxLayout *timeFilterMainLayout = new QVBoxLayout(ui->timeFilterContentsWidget);
    timeFilterMainLayout->setContentsMargins(5, 5, 5, 5);
    timeFilterMainLayout->setSpacing(5);

    // Using QFormLayout for a nice label-field alignment
    QFormLayout *formLayout = new QFormLayout();
    formLayout->setLabelAlignment(Qt::AlignRight);

    m_timeFilterFrom = new QDateTimeEdit(this);
    m_timeFilterFrom->setCalendarPopup(true);
    m_timeFilterFrom->setDateTime(QDateTime::currentDateTime().addDays(-1).date().startOfDay());
    m_timeFilterFrom->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred); // Allow vertical expansion, respect minimumHeight
    formLayout->addRow(tr("From:"), m_timeFilterFrom);

    m_timeFilterTo = new QDateTimeEdit(this);
    m_timeFilterTo->setCalendarPopup(true);
    m_timeFilterTo->setDateTime(QDateTime::currentDateTime().date().endOfDay());
    m_timeFilterTo->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred); // Allow vertical expansion, respect minimumHeight
    formLayout->addRow(tr("To:"), m_timeFilterTo);

    timeFilterMainLayout->addLayout(formLayout);

    m_applyTimeFilterButton = new QPushButton(tr("Apply Time Filter"), this);
    m_applyTimeFilterButton->setCheckable(true);
    connect(m_applyTimeFilterButton, &QPushButton::clicked, this, &MainWindow::onApplyTimeFilterClicked);

    m_resetTimeFilterButton = new QPushButton(tr("Reset"), this);
    connect(m_resetTimeFilterButton, &QPushButton::clicked, this, &MainWindow::onResetTimeFilterClicked);

    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch(1); // Ensure buttons are pushed to the right
    buttonLayout->addWidget(m_applyTimeFilterButton);
    buttonLayout->addWidget(m_resetTimeFilterButton);

    timeFilterMainLayout->addLayout(buttonLayout);
    timeFilterMainLayout->addStretch(); // Push all content to the top

    // Ensure the content widget can influence the dock's size
    ui->timeFilterContentsWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding); // Content can expand vertically at least to its minimum hint
    ui->timeFilterDockWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding);     // Dock itself should also be able to expand vertically
}

void MainWindow::setupTextFilterDockContents()
{
    QWidget* container = ui->textFilterContentsWidget;
    if (!container)
    {
        qWarning("textFilterContentsWidget not found in UI. Text filter dock will be empty.");
        container = new QWidget(ui->textFilterDockWidget);
        ui->textFilterDockWidget->setWidget(container);
    }

    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);

    m_filterPanel = new FilterPanelWidget(container);
    layout->addWidget(m_filterPanel);

    connect(m_filterPanel, &FilterPanelWidget::applyRequested,
            this, &MainWindow::onApplyAllTextFiltersClicked);
    connect(m_filterPanel, &FilterPanelWidget::resetRequested,
            this, &MainWindow::onResetTextFiltersClicked);
    connect(m_filterPanel, &FilterPanelWidget::modeChanged,
            this, [this]() { onFilterModeChanged(); });
    connect(m_filterPanel, &FilterPanelWidget::highlightInMainViewChanged,
            this, [this]() { onHighlightInMainViewChanged(); });

    // Allow the dock widget to expand vertically as its content grows.
    ui->textFilterDockWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding);
}

void MainWindow::setupRowMarkerDock()
{
    m_markerDockWidget = new QDockWidget(tr("Row Highlighters"), this);
    // objectName обязателен: saveState()/restoreState() узнают док по нему.
    m_markerDockWidget->setObjectName(QStringLiteral("rowMarkerDockWidget"));

    m_markerPanel = new MarkerPanelWidget(m_markerDockWidget);
    m_markerDockWidget->setWidget(m_markerPanel);
    addDockWidget(Qt::RightDockWidgetArea, m_markerDockWidget);

    connect(m_markerPanel, &MarkerPanelWidget::applyRequested,
            this, &MainWindow::onApplyRowMarkersClicked);
    connect(m_markerPanel, &MarkerPanelWidget::resetRequested,
            this, &MainWindow::onResetRowMarkersClicked);

    if (ui->menuView) {
        QAction* toggle = m_markerDockWidget->toggleViewAction();
        toggle->setText(tr("Row Highlighters Panel"));
        // Group it with the other panel toggles (above the first separator),
        // instead of appending it to the very bottom of the View menu.
        QAction* beforeAction = nullptr;
        for (QAction* a : ui->menuView->actions()) {
            if (a->isSeparator()) {
                beforeAction = a;
                break;
            }
        }
        if (beforeAction)
            ui->menuView->insertAction(beforeAction, toggle);
        else
            ui->menuView->addAction(toggle);
        m_shortcutActions.insert(QStringLiteral("panelRowHighlighters"), toggle);
    }
}

void MainWindow::setupTimelineDock()
{
    m_timelineDockWidget = new QDockWidget(tr("Timeline"), this);
    // objectName обязателен: saveState()/restoreState() узнают док по нему.
    m_timelineDockWidget->setObjectName(QStringLiteral("timelineDockWidget"));
    // Горизонтальная шкала времени — только верх/низ окна.
    m_timelineDockWidget->setAllowedAreas(Qt::TopDockWidgetArea | Qt::BottomDockWidgetArea);

    m_timelinePanel = new TimelineHistogramWidget(m_timelineDockWidget);
    m_timelineDockWidget->setWidget(m_timelinePanel);
    addDockWidget(Qt::BottomDockWidgetArea, m_timelineDockWidget);

    connect(m_timelinePanel, &TimelineHistogramWidget::timeClicked,
            this, &MainWindow::onTimelineTimeClicked);
    connect(m_timelinePanel, &TimelineHistogramWidget::timeRangeSelected,
            this, &MainWindow::onTimelineRangeSelected);
    // «Reset time filter» из контекстного меню таймлайна — тот же сброс,
    // что и кнопка Reset в доке Time Filter.
    connect(m_timelinePanel, &TimelineHistogramWidget::resetRequested,
            this, &MainWindow::onResetTimeFilterClicked);

    if (ui->menuView) {
        QAction* toggle = m_timelineDockWidget->toggleViewAction();
        toggle->setText(tr("Timeline Panel"));
        // Group it with the other panel toggles (above the first separator),
        // instead of appending it to the very bottom of the View menu.
        QAction* beforeAction = nullptr;
        for (QAction* a : ui->menuView->actions()) {
            if (a->isSeparator()) {
                beforeAction = a;
                break;
            }
        }
        if (beforeAction)
            ui->menuView->insertAction(beforeAction, toggle);
        else
            ui->menuView->addAction(toggle);
        m_shortcutActions.insert(QStringLiteral("panelTimeline"), toggle);
    }
}

void MainWindow::setupSearchResultsDock()
{
    m_searchResultsDockWidget = new QDockWidget(tr("Search Results"), this);
    // objectName обязателен: saveState()/restoreState() узнают док по нему.
    m_searchResultsDockWidget->setObjectName(QStringLiteral("searchResultsDockWidget"));
    // Список совпадений — горизонтальная лента строк, логичнее внизу/вверху.
    m_searchResultsDockWidget->setAllowedAreas(Qt::TopDockWidgetArea | Qt::BottomDockWidgetArea);

    auto* container = new QWidget(m_searchResultsDockWidget);
    auto* vbox = new QVBoxLayout(container);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    m_searchResultsStatusLabel = new QLabel(tr("No search active."), container);
    m_searchResultsStatusLabel->setContentsMargins(6, 3, 6, 3);
    vbox->addWidget(m_searchResultsStatusLabel);

    // Вторая LogListView поверх отдельной LogModel: та же отрисовка/подсветка,
    // что и в основном view, но со своим (отфильтрованным) набором записей.
    m_searchResultsModel = new LogModel(this);
    m_searchResultsView = new LogListView(container);
    // Однострочный режим (klogg-style): word-wrap НЕ включаем.
    m_searchResultsView->setModel(m_searchResultsModel);

    // Тот же шрифт, что и у основных view — ради консистентности.
    {
        QFont f(AppSettings::instance().fontFamily());
        f.setPointSize(AppSettings::instance().fontSize());
        f.setWeight(QFont::Medium);
        m_searchResultsView->setFont(f);
    }
    vbox->addWidget(m_searchResultsView, /*stretch=*/1);

    m_searchResultsDockWidget->setWidget(container);
    addDockWidget(Qt::BottomDockWidgetArea, m_searchResultsDockWidget);
    // Обычная панель: её видимость не меняется при переключении режима — только
    // содержимое (пусто, если режим не Search). Управляется пользователем через
    // меню View; после первого запуска состояние восстановит restoreState.
    // Открыли панель в режиме Search — сразу подтянуть актуальные результаты.
    connect(m_searchResultsDockWidget, &QDockWidget::visibilityChanged,
            this, [this](bool visible) { if (visible) scheduleSearchRefresh(); });

    // Выбор строки в результатах (мышь или клавиатура) → прыжок в основном view.
    if (m_searchResultsView->selectionModel())
        connect(m_searchResultsView->selectionModel(), &QItemSelectionModel::currentRowChanged,
                this, [this](const QModelIndex& current, const QModelIndex&) {
                    onSearchResultActivated(current);
                });

    // Пересчёт счётчика совпадений после (в т.ч. асинхронной) фильтрации.
    connect(m_searchResultsModel, &LogModel::modelFiltered, this, [this](int count) {
        if (m_searchResultsStatusLabel)
            m_searchResultsStatusLabel->setText(tr("%n match(es)", "", count));
    });

    // Дебаунс живого обновления результатов (tail auto-reload / рефильтрация).
    m_searchRefreshTimer = new QTimer(this);
    m_searchRefreshTimer->setSingleShot(true);
    m_searchRefreshTimer->setInterval(200);
    connect(m_searchRefreshTimer, &QTimer::timeout, this, [this]() { runSearchIntoResults(); });

    if (ui->menuView) {
        QAction* toggle = m_searchResultsDockWidget->toggleViewAction();
        toggle->setText(tr("Search Results Panel"));
        // Сгруппировать с прочими переключателями панелей (до первого сепаратора).
        QAction* beforeAction = nullptr;
        for (QAction* a : ui->menuView->actions()) {
            if (a->isSeparator()) {
                beforeAction = a;
                break;
            }
        }
        if (beforeAction)
            ui->menuView->insertAction(beforeAction, toggle);
        else
            ui->menuView->addAction(toggle);
        m_shortcutActions.insert(QStringLiteral("panelSearchResults"), toggle);
    }
}

void MainWindow::setupEntryDetailsDock()
{
    m_detailsDockWidget = new QDockWidget(tr("Entry Details"), this);
    // objectName обязателен: saveState()/restoreState() узнают док по нему.
    m_detailsDockWidget->setObjectName(QStringLiteral("entryDetailsDockWidget"));

    m_detailsPanel = new EntryDetailsPanel(m_detailsDockWidget);
    m_detailsDockWidget->setWidget(m_detailsPanel);
    addDockWidget(Qt::BottomDockWidgetArea, m_detailsDockWidget);
    // Панель по требованию: по умолчанию скрыта (View → Entry Details Panel,
    // Ctrl+F8). Дальше открытость/позицию переживает saveState/restoreState.
    // Пока док скрыт, HTML не строится — панель лишь запоминает текущую запись.
    m_detailsDockWidget->hide();

    if (ui->menuView) {
        QAction* toggle = m_detailsDockWidget->toggleViewAction();
        toggle->setText(tr("Entry Details Panel"));
        // Сгруппировать с прочими переключателями панелей (до первого сепаратора).
        QAction* beforeAction = nullptr;
        for (QAction* a : ui->menuView->actions()) {
            if (a->isSeparator()) {
                beforeAction = a;
                break;
            }
        }
        if (beforeAction)
            ui->menuView->insertAction(beforeAction, toggle);
        else
            ui->menuView->addAction(toggle);
        m_shortcutActions.insert(QStringLiteral("panelEntryDetails"), toggle);
    }
}

void MainWindow::setupStatisticsDock()
{
    m_statsDockWidget = new QDockWidget(tr("Statistics"), this);
    // objectName обязателен: saveState()/restoreState() узнают док по нему.
    m_statsDockWidget->setObjectName(QStringLiteral("statisticsDockWidget"));

    m_statsPanel = new StatisticsPanel(m_statsDockWidget);
    m_statsDockWidget->setWidget(m_statsPanel);
    addDockWidget(Qt::RightDockWidgetArea, m_statsDockWidget);
    // Панель по требованию: по умолчанию скрыта (View → Statistics Panel,
    // Ctrl+F9). Пока док скрыт, сбор статистики не запускается — данные
    // помечаются dirty и пересчитываются при показе.
    m_statsDockWidget->hide();

    // Клик по времени всплеска/паузы — переход к моменту (по дорожке ошибок —
    // к ближайшей ошибке), та же логика, что и клик по таймлайну.
    connect(m_statsPanel, &StatisticsPanel::jumpToTimeRequested,
            this, &MainWindow::onTimelineTimeClicked);
    // Клик по шаблону сообщения — переход к его первому вхождению.
    connect(m_statsPanel, &StatisticsPanel::jumpToEntryRequested,
            this, [this](int logicalEntryId, const LogFilePtr& file) {
                if (!m_activeLogView || !m_activeLogView->model()
                    || !m_activeLogView->view())
                    return;
                const int row = m_activeLogView->model()->nearestVisibleRow(
                    logicalEntryId, file.get());
                if (row < 0)
                    return;
                const QModelIndex idx = m_activeLogView->model()->index(row, 0);
                m_activeLogView->view()->setCurrentIndex(idx);
                m_activeLogView->view()->scrollTo(idx,
                                                  QAbstractItemView::PositionAtCenter);
            });

    if (ui->menuView) {
        QAction* toggle = m_statsDockWidget->toggleViewAction();
        toggle->setText(tr("Statistics Panel"));
        // Сгруппировать с прочими переключателями панелей (до первого сепаратора).
        QAction* beforeAction = nullptr;
        for (QAction* a : ui->menuView->actions()) {
            if (a->isSeparator()) {
                beforeAction = a;
                break;
            }
        }
        if (beforeAction)
            ui->menuView->insertAction(beforeAction, toggle);
        else
            ui->menuView->addAction(toggle);
        m_shortcutActions.insert(QStringLiteral("panelStatistics"), toggle);
    }
}

void MainWindow::setupFilterStatusToolbar()
{
    QToolBar* tb = addToolBar(tr("Filters"));
    tb->setObjectName(QStringLiteral("filterStatusToolBar"));

    const auto makeAction = [&](const QString& text) {
        QAction* a = tb->addAction(text);
        a->setCheckable(true);
        return a;
    };
    m_timeFilterStatusAction  = makeAction(tr("Time"));
    m_textFilterStatusAction  = makeAction(tr("Text"));
    m_fieldFilterStatusAction = makeAction(tr("Fields"));
    m_markerStatusAction      = makeAction(tr("Markers"));

    // triggered приходит только от клика пользователя (не от setChecked при
    // синхронизации). После Apply/Reset реальное состояние перечитывается из
    // модели — если применять было нечего, кнопка сама вернётся в «отжато».
    connect(m_timeFilterStatusAction, &QAction::triggered, this, [this](bool on) {
        if (on)
            onApplyTimeFilterClicked();
        else
            onResetTimeFilterClicked();
        updateFilterStatusButtons();
    });
    connect(m_textFilterStatusAction, &QAction::triggered, this, [this](bool on) {
        if (on)
            onApplyAllTextFiltersClicked();
        else
            onResetTextFiltersClicked();
        updateFilterStatusButtons();
    });
    connect(m_fieldFilterStatusAction, &QAction::triggered, this, [this](bool on) {
        // Дальше отрабатывает toggled-пайплайн самого чекбокса Log Fields.
        if (m_fieldFilterEnabledCheckBox)
            m_fieldFilterEnabledCheckBox->setChecked(on);
        updateFilterStatusButtons();
    });
    connect(m_markerStatusAction, &QAction::triggered, this, [this](bool on) {
        if (on)
            onApplyRowMarkersClicked();
        else
            onResetRowMarkersClicked();
        updateFilterStatusButtons();
    });

    // Глобальный чекбокс Log Fields могут переключить и из самого дока.
    if (m_fieldFilterEnabledCheckBox)
        connect(m_fieldFilterEnabledCheckBox, &QCheckBox::toggled,
                this, &MainWindow::updateFilterStatusButtons);

    updateFilterStatusButtons();
}

void MainWindow::updateFilterStatusButtons()
{
    if (!m_timeFilterStatusAction)
        return; // тулбар ещё не создан

    LogModel* model = (m_activeLogView && m_activeLogView->model())
                          ? m_activeLogView->model()
                          : nullptr;

    // Time (пер-вкладочный)
    {
        const bool active = model && (model->startTimeFilter().isValid()
                                      || model->endTimeFilter().isValid());
        m_timeFilterStatusAction->setEnabled(model != nullptr);
        m_timeFilterStatusAction->setChecked(active);
        m_timeFilterStatusAction->setToolTip(active
            ? tr("Time filter: %1 – %2\nClick to reset")
                  .arg(model->startTimeFilter().toString(QStringLiteral("dd.MM.yyyy HH:mm:ss")),
                       model->endTimeFilter().toString(QStringLiteral("dd.MM.yyyy HH:mm:ss")))
            : tr("Time filter is off\nClick to apply the range from the Time Filter panel"));
    }

    // Text (пер-вкладочный)
    {
        int ruleCount = 0;
        if (model) {
            for (const FilterRule& r : model->filterRules().rules)
                if (r.isActive())
                    ++ruleCount;
        }
        m_textFilterStatusAction->setEnabled(model != nullptr);
        m_textFilterStatusAction->setChecked(ruleCount > 0);
        m_textFilterStatusAction->setToolTip(ruleCount > 0
            ? tr("Text filters: %n active rule(s)\nClick to reset", nullptr, ruleCount)
            : tr("Text filters are off\nClick to apply rules from the Text Filters panel"));
    }

    // Fields (глобальный — действует на все вкладки)
    {
        const bool active = m_fieldFilterEnabledCheckBox
                         && m_fieldFilterEnabledCheckBox->isChecked();
        m_fieldFilterStatusAction->setChecked(active);
        m_fieldFilterStatusAction->setToolTip(active
            ? tr("Field filtering: %1 of %2 blocks shown (all tabs)\nClick to show full lines")
                  .arg(selectedVisibleFieldIndexes().size())
                  .arg(m_fieldCheckBoxes.size())
            : tr("Field filtering is off\nClick to show only the blocks selected in the Log Fields panel"));
    }

    // Row highlighters (пер-вкладочные; строки не скрывают, но индикатор полезен)
    {
        const int markerCount = model ? model->rowMarkers().size() : 0;
        m_markerStatusAction->setEnabled(model != nullptr);
        m_markerStatusAction->setChecked(markerCount > 0);
        m_markerStatusAction->setToolTip(markerCount > 0
            ? tr("Row highlighters: %n marker(s) applied\nClick to clear", nullptr, markerCount)
            : tr("Row highlighters are off\nClick to apply markers from the Row Highlighters panel"));
    }
}

void MainWindow::setupDirectoryScanner()
{
    // The dock content is a self-contained panel (header card + results tree).
    m_dirScannerPanel = new DirectoryScannerPanel(ui->directoryScannerDockWidget);
    ui->directoryScannerDockWidget->setWidget(m_dirScannerPanel);

    m_dirScanner = m_dirScannerPanel->scanner();
    m_dirScanner->setFileExtensions(AppSettings::instance().scanExtensions());
    m_dirScanner->setConversionPattern(m_conversionPattern);

    connect(m_dirScannerPanel, &DirectoryScannerPanel::scanRequested,
            this, &MainWindow::onScanDirectoryClicked);
    connect(m_dirScannerPanel, &DirectoryScannerPanel::configureExtensionsRequested,
            this, &MainWindow::onConfigureScanExtensionsClicked);

    connect(m_dirScanner, &DirectoryScanner::fileActivated,
            this, [this](const QString& path) {
        onOpenSelectedDirectoryFiles({path});
    });
    connect(m_dirScanner, &DirectoryScanner::filesActivated,
            this, &MainWindow::onOpenSelectedDirectoryFiles);
}

void MainWindow::connectToLogView(LogViewWidget *logView)
{
    if (!logView)
        return;
    if (m_activeLogView && m_activeLogView != logView)
    {
        disconnectFromLogView(m_activeLogView);
    }
    m_activeLogView = logView;

    // Apply current global word wrap setting to the newly active view
    logView->view()->setWordWrap(ui->actionWordWrap->isChecked());

    connect(logView, &LogViewWidget::fileParsingStarted, this, &MainWindow::handleFileParsingStarted);
    connect(logView, &LogViewWidget::fileParsingProgress, this, &MainWindow::handleFileParsingProgress);
    connect(logView, &LogViewWidget::fileParsingFinished, this, &MainWindow::handleFileParsingFinished);
    connect(logView, &LogViewWidget::fileParsingFailed, this, &MainWindow::handleFileParsingFailed);

    connect(logView, &LogViewWidget::totalRowCountChanged, this, &MainWindow::handleTotalRowCountChanged);
    connect(logView, &LogViewWidget::currentRowChanged, this, &MainWindow::updateLineInfoLabel);
    connect(logView, &LogViewWidget::modelFiltered, this, &MainWindow::handleModelFiltered);

    if (logView->view()) {
        connect(logView->view(), &LogListView::timeFilterBoundRequested,
                this, &MainWindow::onTimeFilterBoundRequested);

        // Синхронизация тогла follow-tail с активной вкладкой (в т.ч.
        // автоматическое выключение при уходе пользователя от низа).
        m_followTailConn = connect(logView->view(), &LogListView::followTailChanged,
                                   this, [this](bool on) {
            if (m_followTailAction && m_followTailAction->isChecked() != on)
                m_followTailAction->setChecked(on);
        });
        if (m_followTailAction)
            m_followTailAction->setChecked(logView->view()->followTail());
    }

    // Таймлайн следит за моделью активной вкладки.
    if (m_timelinePanel)
        m_timelinePanel->setModel(logView->model());

    // Панель статистики — тоже (сама пересоберётся, если видима).
    if (m_statsPanel)
        m_statsPanel->setModel(logView->model());

    // Живое обновление панели результатов: дозагрузка строк (rowsInserted) или
    // полная перестройка (modelReset) активной вкладки → дебаунс-пересборка.
    if (m_searchRefreshTimer && logView->model()) {
        m_searchModelInsertConn = connect(logView->model(), &QAbstractItemModel::rowsInserted,
            this, [this](const QModelIndex&, int, int) { scheduleSearchRefresh(); });
        m_searchModelResetConn = connect(logView->model(), &QAbstractItemModel::modelReset,
            this, [this]() { scheduleSearchRefresh(); });
    }

    if (m_activeLogView && m_activeLogView->model() && m_activeLogView->view())
    {
        int totalRows = m_activeLogView->model()->rowCount();
        QModelIndex currentModelIndex = m_activeLogView->view()->currentIndex();
        int currentRow = currentModelIndex.isValid() ? currentModelIndex.row() : -1;
        updateLineInfoLabel(currentRow, totalRows);
    }
    syncReloadButton();
    updateFilterInputsFromModel();
    updateFilterStatusButtons();

    // Смена вкладки в режиме поиска: результаты старой вкладки указывали бы на
    // чужие записи — пересобираем под новую активную вкладку (если док видим).
    if (m_filterPanel && m_filterPanel->mode() == FilterPanelWidget::Mode::Search
        && m_searchResultsDockWidget && m_searchResultsDockWidget->isVisible())
        runSearchIntoResults();
}

void MainWindow::disconnectFromLogView(LogViewWidget *logView)
{
    if (!logView)
        return;
    disconnect(logView, &LogViewWidget::fileParsingStarted, this, &MainWindow::handleFileParsingStarted);
    disconnect(logView, &LogViewWidget::fileParsingProgress, this, &MainWindow::handleFileParsingProgress);
    disconnect(logView, &LogViewWidget::fileParsingFinished, this, &MainWindow::handleFileParsingFinished);
    disconnect(logView, &LogViewWidget::fileParsingFailed, this, &MainWindow::handleFileParsingFailed);

    disconnect(logView, &LogViewWidget::totalRowCountChanged, this, &MainWindow::handleTotalRowCountChanged);
    disconnect(logView, &LogViewWidget::currentRowChanged, this, &MainWindow::updateLineInfoLabel);
    disconnect(logView, &LogViewWidget::modelFiltered, this, &MainWindow::handleModelFiltered);

    if (logView->view())
        disconnect(logView->view(), &LogListView::timeFilterBoundRequested,
                   this, &MainWindow::onTimeFilterBoundRequested);

    disconnect(m_followTailConn);

    if (m_timelinePanel)
        m_timelinePanel->setModel(nullptr);

    if (m_statsPanel)
        m_statsPanel->setModel(nullptr);

    // Рвём соединения живого обновления с моделью уходящей вкладки.
    disconnect(m_searchModelInsertConn);
    disconnect(m_searchModelResetConn);

    if (m_activeLogView == logView)
    {
        m_activeLogView = nullptr;
    }
}

void MainWindow::onCurrentTabChanged(int index)
{
    if (m_activeLogView)
    {
        disconnectFromLogView(m_activeLogView);
    }

    LogViewWidget *currentView = qobject_cast<LogViewWidget *>(ui->tabWidget->widget(index));
    if (currentView)
    {
        connectToLogView(currentView);
    }
    else
    {
        m_activeLogView = nullptr;
        // Вкладок не осталось — очистить счётчик строк, маркер таймлайна
        // и панель деталей (иначе они показывали бы закрытый документ).
        updateLineInfoLabel(-1, 0);
    }
    updateStatusBarDefaultText();
    updateLogLevelFilterButtons();
    updateFilterInputsFromModel();
    updateFilterStatusButtons();
}

void MainWindow::on_actionOpen_triggered()
{
    auto files = QFileDialog::getOpenFileNames(
        this,
        tr("Open log files"),
        m_lastOpenDir,
        logFileDialogFilter());

    if (files.isEmpty())
        return;

    m_lastOpenDir = QFileInfo(files.first()).absolutePath();

    for (const QString& f : files)
        addToRecentFiles(f);

    LogViewWidget *view = nullptr;
    int tabIndex = -1;
    if (ui->tabWidget->count() == 0 ||
        ((view = qobject_cast<LogViewWidget *>(ui->tabWidget->currentWidget())) && view && view->fileCount() > 0))
    {
        view = createLogViewWidget();
        tabIndex = ui->tabWidget->addTab(view, tr("Logs"));
        ui->tabWidget->setCurrentIndex(tabIndex);
    }
    else
    {
        view = qobject_cast<LogViewWidget *>(ui->tabWidget->currentWidget());
        if (!view)
        {
            view = createLogViewWidget();
            tabIndex = ui->tabWidget->addTab(view, tr("Logs"));
            ui->tabWidget->setCurrentIndex(tabIndex);
        }
        else
        {
            if (m_activeLogView != view)
                connectToLogView(view);
        }
    }

    for (const QString &f : files)
    {
        if (view)
            view->addLogFile(f);
    }

    if (view)
    {
        int currentTabIndex = ui->tabWidget->indexOf(view);
        if (currentTabIndex == -1)
            currentTabIndex = ui->tabWidget->currentIndex();

        int fileCount = view->fileCount();
        QString tabText;
        QString tabToolTip;

        if (fileCount == 1 && !view->loadedFiles().isEmpty())
        {
            const auto logFile = view->loadedFiles().first(); // РЅРµ СЃСЃС‹Р»РєР° вЂ” loadedFiles() РІРѕР·РІСЂР°С‰Р°РµС‚ РІСЂРµРјРµРЅРЅС‹Р№ СЃРїРёСЃРѕРє
            tabText = logFile->shortName();
            tabToolTip = logFile->filePath;
        }
        else if (fileCount > 1)
        {
            tabText = tr("Logs (%1)").arg(fileCount);
            QStringList fileNames;
            for (const auto &lf : view->loadedFiles())
            {
                fileNames << lf->filePath;
            }
            tabToolTip = fileNames.join("\n");
        }
        else if (fileCount == 0 && files.size() == 1)
        {
            tabText = QFileInfo(files.first()).fileName();
            tabToolTip = files.first();
        }
        else if (fileCount == 0 && files.size() > 1)
        {
            tabText = tr("Logs (%1)").arg(files.size());
            QStringList fileNames = files;
            tabToolTip = fileNames.join("\n");
        }
        else
        {
            tabText = tr("Logs");
            tabToolTip = "";
        }
        ui->tabWidget->setTabText(currentTabIndex, tabText);
        ui->tabWidget->setTabToolTip(currentTabIndex, tabToolTip);
    }
}

void MainWindow::on_actionSaveAs_triggered()
{
    if (!m_activeLogView || !m_activeLogView->model() ||
        m_activeLogView->model()->rowCount() == 0)
    {
        QMessageBox::information(this, tr("Save View As"),
            tr("There is nothing to save — the current view is empty."));
        return;
    }

    const QString fileName = QFileDialog::getSaveFileName(
        this,
        tr("Save view as"),
        m_lastOpenDir,
        logFileDialogFilter());

    if (fileName.isEmpty())
        return;

    // Never overwrite a file that is currently open in any tab: the request is
    // explicitly to save into a NEW file, leaving the originals untouched.
    const QString targetPath = QFileInfo(fileName).absoluteFilePath();
    for (int t = 0; t < ui->tabWidget->count(); ++t) {
        auto* lvw = qobject_cast<LogViewWidget*>(ui->tabWidget->widget(t));
        if (!lvw)
            continue;
        for (const auto& lf : lvw->loadedFiles()) {
            if (lf && QFileInfo(lf->filePath).absoluteFilePath() == targetPath) {
                QMessageBox::warning(this, tr("Save View As"),
                    tr("This file is currently open. Please choose a different, new file."));
                return;
            }
        }
    }

    QFile out(fileName);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Save View As"),
            tr("Could not open '%1' for writing.").arg(fileName));
        return;
    }

    // Save exactly what the view currently shows: iterate the model's visible
    // rows (already filtered + merged across all documents in this tab) and use
    // the display text, so the active Log Fields selection is honoured too.
    QTextStream stream(&out);
    stream.setEncoding(QStringConverter::Utf8);
    LogModel* model = m_activeLogView->model();
    const int rows = model->rowCount();
    for (int r = 0; r < rows; ++r) {
        const QString line = model->data(model->index(r), Qt::DisplayRole).toString();
        stream << line << '\n';
    }
    out.close();

    m_lastOpenDir = QFileInfo(fileName).absolutePath();
    m_statusLabel->setText(tr("Saved %1 lines to %2")
                               .arg(rows)
                               .arg(QFileInfo(fileName).fileName()));
}

void MainWindow::handleFileParsingStarted(const LogFilePtr &logFile)
{
    if (!logFile)
        return;
    m_statusLabel->setText(tr("Parsing: %1...").arg(logFile->shortName()));
    m_progressBar->setValue(0);
    m_progressBar->show();
}

void MainWindow::handleFileParsingProgress(const LogFilePtr &logFile, int progressPercentage)
{
    if (m_statusLabel->text().contains(logFile->shortName()))
    {
        m_progressBar->setValue(progressPercentage);
    }
}

void MainWindow::handleFileParsingFinished(const LogFilePtr &logFile, int totalEntries)
{
    if (!logFile)
        return;
    m_statusLabel->setText(tr("Finished parsing: %1 (%2 entries)").arg(logFile->shortName()).arg(totalEntries));
    m_progressBar->hide();

    // Основная обработка файла закончена — пнуть сбор статистики. Страхует
    // редкий случай, когда слитый батч целиком скрыт активным фильтром и
    // модельные сигналы (rowsInserted/modelReset) не приходили.
    if (m_statsPanel)
        m_statsPanel->scheduleRefresh();

    // Update tab text to reflect the loaded file
    // (вкладка спула stdin сохраняет своё имя — файл там технический).
    const bool isStdinSpool = m_stdinSpooler
        && logFile->filePath == m_stdinSpooler->spoolFilePath();
    for (int i = 0; i < ui->tabWidget->count() && !isStdinSpool; ++i) {
        auto *view = qobject_cast<LogViewWidget*>(ui->tabWidget->widget(i));
        if (view && view->loadedFiles().contains(logFile)) {
            int count = view->fileCount();
            if (count == 1) {
                ui->tabWidget->setTabText(i, logFile->shortName());
                ui->tabWidget->setTabToolTip(i, logFile->filePath);
            } else if (count > 1) {
                ui->tabWidget->setTabText(i, tr("Logs (%1)").arg(count));
                QStringList paths;
                for (const auto &lf : view->loadedFiles()) {
                    paths << lf->filePath;
                }
                ui->tabWidget->setTabToolTip(i, paths.join("\n"));
            }
            break;
        }
    }

    QTimer::singleShot(3000, this, [this]()
                       {
        if (m_progressBar->isHidden()) {
            updateStatusBarDefaultText();
        } });

    maybeStartBaselineDump();
}

// ---------------------------------------------------------------------------
// Env-gated хук верификации: DENDRO_BASELINE_DUMP=<каталог> — после окончания
// парсинга первого файла прогоняет фиксированную последовательность фильтров
// над активной моделью, дампит счётчики и видимый текст в файлы и завершает
// приложение. Использует только стабильные интерфейсы модели (rowCount/data/
// сеттеры фильтров), поэтому результат не зависит от способа хранения записей
// и служит байтовым базлайном при рефакторингах хранилища.
// ---------------------------------------------------------------------------
void MainWindow::maybeStartBaselineDump()
{
    static bool started = false;
    const QString outDir = qEnvironmentVariable("DENDRO_BASELINE_DUMP");
    if (outDir.isEmpty() || started || !m_activeLogView)
        return;
    started = true;

    LogModel* model = m_activeLogView->model();
    auto json = std::make_shared<QJsonObject>();

    auto record = [model, json](const QString& key) {
        QJsonObject step;
        const int rows = model->rowCount();
        step["rows"] = rows;
        if (rows > 0) {
            step["first"] = model->data(model->index(0, 0), Qt::DisplayRole).toString();
            step["mid"] = model->data(model->index(rows / 2, 0), Qt::DisplayRole).toString();
            step["last"] = model->data(model->index(rows - 1, 0), Qt::DisplayRole).toString();
            step["midLen"] = model->displayTextLength(rows / 2);
        }
        (*json)[key] = step;
    };

    (*json)["backend"] = model->isIndexedBackend() ? QStringLiteral("indexed")
                                                   : QStringLiteral("resident");
    record(QStringLiteral("unfiltered"));
    const QModelIndex hit =
        model->findNextOccurrence(QStringLiteral("Checksum"), 0, Qt::CaseInsensitive);
    (*json)["findChecksum"] = hit.isValid() ? hit.row() : -1;
    const QModelIndex hitBack =
        model->findPreviousOccurrence(QStringLiteral("переполнен"), 5, Qt::CaseSensitive);
    (*json)["findBackCyrillic"] = hitBack.isValid() ? hitBack.row() : -1;

    auto finish = [this, model, json, outDir]() {
        QFile view(QDir(outDir).filePath(QStringLiteral("baseline_view.txt")));
        if (view.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            QTextStream stream(&view);
            stream.setEncoding(QStringConverter::Utf8);
            const int rows = model->rowCount();
            for (int r = 0; r < rows; ++r)
                stream << model->data(model->index(r, 0), Qt::DisplayRole).toString() << '\n';
        }
        QFile out(QDir(outDir).filePath(QStringLiteral("baseline.json")));
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            out.write(QJsonDocument(*json).toJson(QJsonDocument::Indented));
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
    };

    // Каждый сеттер фильтра завершается ровно одним modelFiltered (синхронно
    // или из фонового джоба), поэтому шаги строго чередуются «мутация→сигнал».
    auto step = std::make_shared<int>(0);
    connect(model, &LogModel::modelFiltered, this,
            [model, record, finish, step](int) {
        switch (++(*step)) {
        case 1:
            record(QStringLiteral("levelErrorFatal"));
            model->setTimeRangeFilter(
                QDateTime(QDate(2026, 7, 10), QTime(9, 10, 0)),
                QDateTime(QDate(2026, 7, 10), QTime(9, 20, 0)));
            break;
        case 2:
            record(QStringLiteral("levelPlusTime"));
            model->setTimeRangeFilter(QDateTime(), QDateTime());
            break;
        case 3:
            record(QStringLiteral("levelAfterTimeReset"));
            model->setLogLevelFilter({});
            break;
        case 4: {
            record(QStringLiteral("unfilteredAgain"));
            FilterRuleSet rules;
            FilterRule inc1;
            inc1.text = QStringLiteral("Timeout");
            FilterRule inc2;
            inc2.text = QStringLiteral("Ошибка");
            inc2.connector = FilterRule::Connector::Or;
            rules.rules = {inc1, inc2};
            rules.bindFields({}, false);
            model->setFilterRules(rules);
            break;
        }
        case 5:
            record(QStringLiteral("textTimeoutOrCyrillic"));
            finish();
            break;
        default:
            break;
        }
    });

    model->setLogLevelFilter({LogLevel::Error, LogLevel::Fatal});
}

void MainWindow::handleFileParsingFailed(const LogFilePtr &logFile)
{
    if (!logFile)
        return;
    m_statusLabel->setText(tr("Failed to parse: %1").arg(logFile->shortName()));
    m_progressBar->hide();
    QTimer::singleShot(3000, this, [this]()
                       {
        if (m_progressBar->isHidden()) {
            updateStatusBarDefaultText();
        } });
}

void MainWindow::handleTotalRowCountChanged(int totalRows)
{
    int currentRow = -1;
    if (m_activeLogView && m_activeLogView->view() && m_activeLogView->view()->selectionModel())
    {
        QModelIndex currentIndex = m_activeLogView->view()->currentIndex();
        if (currentIndex.isValid())
        {
            currentRow = currentIndex.row();
        }
    }
    updateLineInfoLabel(currentRow, totalRows);
}

void MainWindow::setFilterLogLvl(LogLevel level, bool add)
{
    auto view = qobject_cast<LogViewWidget*>(ui->tabWidget->currentWidget());
    if (view && view->model())
    {
        auto currentLogLvlFilter = view->model()->logLevelFilter();
        if (add)
        {
            currentLogLvlFilter.insert(level);
        }
        else
        {
            currentLogLvlFilter.remove(level);
        }
        view->model()->setLogLevelFilter(currentLogLvlFilter);
        updateLogLevelFilterButtons();
    }
}

void MainWindow::on_actionFatal_toggled(bool checked)
{
    setFilterLogLvl(LogLevel::Fatal, checked);
}

void MainWindow::on_actionError_toggled(bool checked)
{
    setFilterLogLvl(LogLevel::Error, checked);
}

void MainWindow::on_actionWarn_toggled(bool checked)
{
    setFilterLogLvl(LogLevel::Warn, checked);
}

void MainWindow::updateStatusBarDefaultText()
{
    m_statusLabel->setText(tr("Ready"));
    m_progressBar->hide();
}

void MainWindow::updateLineInfoLabel(int currentRow, int totalRows)
{
    if (totalRows > 0 && currentRow >= 0)
    {
        m_lineInfoLabel->setText(QString("%1 / %2").arg(currentRow + 1).arg(totalRows));
    }
    else if (totalRows > 0)
    {
        m_lineInfoLabel->setText(tr("Total: %1").arg(totalRows));
    }
    else
    {
        m_lineInfoLabel->setText("-");
    }

    // Текущая запись в отфильтрованном списке активной вкладки (если есть).
    std::shared_ptr<LogEntry> currentEntry;
    LogModel* model = m_activeLogView ? m_activeLogView->model() : nullptr;
    if (currentRow >= 0 && model)
        currentEntry = model->entryAt(currentRow);

    // Маркер позиции текущей строки на таймлайн-гистограмме.
    if (m_timelinePanel)
        m_timelinePanel->setCurrentTime(currentEntry ? currentEntry->timestamp()
                                                     : QDateTime());

    // Панель деталей следует за текущей строкой. Скармливаем запись всегда:
    // пока док скрыт, панель лишь запоминает её и строит HTML при показе.
    if (m_detailsPanel)
    {
        if (currentEntry)
            m_detailsPanel->showEntry(currentEntry,
                                      model->logicalRecordLines(currentEntry),
                                      model->availableFields());
        else
            m_detailsPanel->clearEntry();
    }
}

void MainWindow::on_actionInfo_toggled(bool checked)
{
    setFilterLogLvl(LogLevel::Info, checked);
}

void MainWindow::on_actionDebug_toggled(bool checked)
{
    setFilterLogLvl(LogLevel::Debug, checked);
}

void MainWindow::on_actionTrace_toggled(bool checked)
{
    setFilterLogLvl(LogLevel::Trace, checked);
}

void MainWindow::on_actionWordWrap_toggled(bool checked)
{
    for (int i = 0; i < ui->tabWidget->count(); ++i) {
        auto *v = qobject_cast<LogViewWidget *>(ui->tabWidget->widget(i));
        if (v)
            v->view()->setWordWrap(checked);
    }
}

void MainWindow::updateLogLevelFilterButtons()
{
    LogViewWidget *currentView = qobject_cast<LogViewWidget *>(ui->tabWidget->currentWidget());
    LogModel *model = nullptr;
    if (currentView)
    {
        model = currentView->model();
    }

    // Determine if the general UI theme is light or dark based on window text color
    bool isDarkTheme = QApplication::palette().text().color().lightnessF() > 0.5;

    auto updateButtonState = [&](QAction *action, LogLevel level)
    {
        bool active = false;
        // Неактивная кнопка — насыщенный цвет уровня, активная — пастельный фон.
        QColor baseColor = AppTheme::instance().forLevel(level);

        if (model)
        {
            active = model->logLevelFilter().contains(level);
            if (active)
                baseColor = AppTheme::instance().dimForLevel(level);
        }
        else
        {
            active = action->isChecked();
        }
        action->setChecked(active);

        QWidget *button = ui->toolBar->widgetForAction(action);
        if (button)
        {
            QString styleSheet = "QToolButton { border: 1px solid transparent; }"; // Default: nearly invisible border
            if (active)
            {
                QColor bgColor = baseColor;
                QColor textColor = QApplication::palette().buttonText().color(); // Start with default button text
                QColor borderColor = baseColor.darker(130);

                // Adjust background for better contrast with typical text colors
                if (isDarkTheme)
                { // Dark theme: light text on controls
                    // We want the background to be distinct but not clash with light text
                    bgColor = baseColor.darker(120); // Darken the base color for background
                    if (bgColor.lightnessF() * 100 < 30)
                        bgColor = bgColor.lighter(130); // ensure not too dark
                    // Text color on this darker bg should ideally be light
                    textColor = Qt::white;
                }
                else
                {                                     // Light theme: dark text on controls
                    bgColor = baseColor.lighter(150); // Lighten the base color for background
                    if (bgColor.lightnessF() * 100 > 230)
                        bgColor = baseColor.lighter(120); // ensure not too pale/white
                    // Text color on this lighter bg should ideally be dark
                    textColor = Qt::black;
                }

                // Ensure text and background have some contrast - very basic check
                if (qAbs(bgColor.lightnessF() - textColor.lightnessF()) < 0.3)
                {
                    if (isDarkTheme)
                        textColor = Qt::white;
                    else
                        textColor = Qt::black;
                }

                styleSheet = QString("QToolButton { background-color: %1; color: %2; border: 1px solid %3; } "
                                     "QToolButton:checked { border: 2px solid %4; }"
                                     "QToolButton:hover { border: 1px solid %5; }")
                                 .arg(bgColor.name())
                                 .arg(textColor.name())
                                 .arg(borderColor.name())
                                 .arg(baseColor.darker(160).name())  // Stronger border when checked
                                 .arg(baseColor.darker(130).name()); // Border on hover
            }
            else
            {
                // Reset to default appearance (or a neutral one)
                styleSheet = QString("QToolButton { background-color: none; border: none; color: %1; }")
                                 .arg(QApplication::palette().buttonText().color().name());
            }
            button->setStyleSheet(styleSheet);
        }
    };

    updateButtonState(ui->actionTrace, LogLevel::Trace);
    updateButtonState(ui->actionDebug, LogLevel::Debug);
    updateButtonState(ui->actionInfo, LogLevel::Info);
    updateButtonState(ui->actionWarn, LogLevel::Warn);
    updateButtonState(ui->actionError, LogLevel::Error);
    updateButtonState(ui->actionFatal, LogLevel::Fatal);
}

void MainWindow::onApplyTimeFilterClicked()
{
    if (m_activeLogView && m_activeLogView->model())
    {
        QDateTime fromDateTime = m_timeFilterFrom->dateTime();
        QDateTime toDateTime = m_timeFilterTo->dateTime();

        if (!fromDateTime.isValid() || !toDateTime.isValid() || fromDateTime > toDateTime)
        {
            // Optionally show a warning to the user
            // For now, we just clear the filter if dates are invalid
            m_activeLogView->model()->setTimeRangeFilter(QDateTime(), QDateTime());
            m_applyTimeFilterButton->setChecked(false);
        }
        else
        {
            m_activeLogView->model()->setTimeRangeFilter(fromDateTime, toDateTime);
            m_applyTimeFilterButton->setChecked(true);
        }
    }
}

void MainWindow::onResetTimeFilterClicked()
{
    if (m_activeLogView && m_activeLogView->model())
    {
        m_activeLogView->model()->setTimeRangeFilter(QDateTime(), QDateTime()); // Clear filter in model
    }
    m_applyTimeFilterButton->setChecked(false);
    // Optionally, re-trigger onApplyTimeFilterClicked if other logic needs to run
    // onApplyTimeFilterClicked(); // This would immediately re-apply the (now cleared) filter
}

void MainWindow::onTimeFilterBoundRequested(const QDateTime& dt, bool isStart)
{
    if (!dt.isValid())
        return;

    // Подставляем выбранный таймстамп в нужное поле и показываем панель фильтра
    // по времени, но НЕ применяем фильтр автоматически — пользователь сам решает,
    // когда нажать Apply (вторая граница может быть ещё не задана).
    if (isStart)
        m_timeFilterFrom->setDateTime(dt);
    else
        m_timeFilterTo->setDateTime(dt);

    if (ui->timeFilterDockWidget) {
        ui->timeFilterDockWidget->setVisible(true);
        ui->timeFilterDockWidget->raise();
    }
}

void MainWindow::onTimelineTimeClicked(const QDateTime& time, bool preferErrors)
{
    if (!time.isValid() || !m_activeLogView || !m_activeLogView->model()
        || !m_activeLogView->view())
        return;

    LogModel* model = m_activeLogView->model();
    const int visibleRows = model->rowCount();
    if (visibleRows == 0)
        return;

    // Первая строка с timestamp >= time. Список отсортирован по времени,
    // строки без валидной метки — в конце и считаются «больше» любого времени.
    int row = qMin(model->firstVisibleRowAtOrAfter(time), visibleRows - 1);

    if (preferErrors) {
        // Клик в дорожке ошибок — ближайшая по времени строка Warn/Error/Fatal.
        const auto isError = [model](int i) {
            const LogLevel lvl = model->visibleLevelAt(i);
            return lvl == LogLevel::Warn || lvl == LogLevel::Error
                || lvl == LogLevel::Fatal;
        };
        int before = -1, after = -1;
        for (int i = row; i >= 0; --i)
            if (isError(i)) { before = i; break; }
        for (int i = row + 1; i < visibleRows; ++i)
            if (isError(i)) { after = i; break; }

        const auto distanceMs = [model, &time](int i) {
            const QDateTime ts = model->visibleTimestampAt(i);
            return ts.isValid() ? qAbs(ts.msecsTo(time))
                                : std::numeric_limits<qint64>::max();
        };
        if (before >= 0 && after >= 0)
            row = distanceMs(before) <= distanceMs(after) ? before : after;
        else if (before >= 0)
            row = before;
        else if (after >= 0)
            row = after;
        // ни одной ошибки в видимом списке — остаёмся на строке по времени
    }

    const QModelIndex idx = model->index(row, 0);
    m_activeLogView->view()->setCurrentIndex(idx);
    m_activeLogView->view()->scrollTo(idx, QAbstractItemView::PositionAtCenter);
}

void MainWindow::onTimelineRangeSelected(const QDateTime& from, const QDateTime& to)
{
    if (!from.isValid() || !to.isValid() || from >= to
        || !m_activeLogView || !m_activeLogView->model())
        return;

    // Синхронизируем поля дока Time Filter, чтобы Apply/Reset там работали
    // с выделенным интервалом, но сам док не показываем и не поднимаем.
    if (m_timeFilterFrom)
        m_timeFilterFrom->setDateTime(from);
    if (m_timeFilterTo)
        m_timeFilterTo->setDateTime(to);

    // Интервал покрывает весь файл (zoom-out «до упора») — честнее снять
    // фильтр по времени совсем, чем держать фильтр шире данных.
    const auto fullRange = m_activeLogView->model()->fullTimeRange();
    if (fullRange.first.isValid()
        && from <= fullRange.first && to >= fullRange.second)
    {
        m_activeLogView->model()->setTimeRangeFilter(QDateTime(), QDateTime());
        if (m_applyTimeFilterButton)
            m_applyTimeFilterButton->setChecked(false);
        return;
    }

    // Фильтр применяется исходными границами, не значениями QDateTimeEdit:
    // редактор обрезает время до отображаемых секций (теряет миллисекунды),
    // и записи на краях интервала выпадали бы из выборки.
    m_activeLogView->model()->setTimeRangeFilter(from, to);
    if (m_applyTimeFilterButton)
        m_applyTimeFilterButton->setChecked(true);
}

void MainWindow::onApplyAllTextFiltersClicked()
{
    applyTextFiltersToActiveView();
}

void MainWindow::onResetTextFiltersClicked()
{
    if (!m_activeLogView)
        return;

    if (m_filterPanel && m_filterPanel->mode() == FilterPanelWidget::Mode::Search) {
        // Режим поиска: основной view и так не был отфильтрован. Очищаем панель
        // результатов и снимаем подсветку совпадений; правила в панели остаются.
        clearSearchResults();
        if (m_activeLogView->view())
            m_activeLogView->view()->setTextHighlightPatterns({});
        return;
    }

    // Режим фильтра: снять фильтры и подсветку с активного документа; правила
    // в панели остаются и могут быть применены заново.
    if (m_activeLogView->model())
        m_activeLogView->model()->setFilterRules(FilterRuleSet{});
    if (m_activeLogView->view())
        m_activeLogView->view()->setTextHighlightPatterns({});
}

void MainWindow::onApplyRowMarkersClicked()
{
    applyRowMarkersToActiveView();
    // Маркеры не перефильтровывают модель (modelFiltered не придёт) —
    // кнопку-индикатор обновляем явно.
    updateFilterStatusButtons();
}

void MainWindow::onResetRowMarkersClicked()
{
    // Снять окраску маркеров с активного документа; маркеры в панели
    // остаются и могут быть применены заново.
    if (m_activeLogView && m_activeLogView->model())
        m_activeLogView->model()->setRowMarkers({});
    updateFilterStatusButtons();
}

void MainWindow::applyTextFiltersToActiveView()
{
    if (!m_filterPanel || !m_activeLogView)
        return;

    // Режим поиска: основной view не фильтруем — совпадения уходят в панель
    // результатов, main остаётся полным (только опциональная подсветка).
    if (m_filterPanel->mode() == FilterPanelWidget::Mode::Search) {
        if (m_activeLogView->model())
            m_activeLogView->model()->setFilterRules(FilterRuleSet{});
        runSearchIntoResults();
        // Явное нажатие «Search» поднимает панель результатов, если она скрыта
        // (в отличие от простой смены режима, которая её не трогает).
        if (m_searchResultsDockWidget && !m_searchResultsDockWidget->isVisible())
            m_searchResultsDockWidget->show();
        return;
    }

    // Фильтрация пер-вкладочная: Apply действует только на текущий документ.
    // Остальные документы сохраняют свои (или никакие) фильтры.
    FilterRuleSet rules = m_filterPanel->ruleSet();
    const bool fieldScope = m_fieldFilterEnabledCheckBox && m_fieldFilterEnabledCheckBox->isChecked();
    rules.bindFields(LogPattern(m_conversionPattern).fieldNames(), fieldScope);

    if (m_activeLogView->model())
        m_activeLogView->model()->setFilterRules(rules);
    // Подсветка совпадений в основном view — по галочке (работает в обоих режимах).
    if (m_activeLogView->view())
        m_activeLogView->view()->setTextHighlightPatterns(
            m_filterPanel->highlightInMainView() ? rules.highlightPatterns()
                                                 : QVector<HighlightPattern>{});
}

void MainWindow::runSearchIntoResults()
{
    if (!m_filterPanel || !m_searchResultsModel || !m_activeLogView
        || !m_activeLogView->model()
        || m_filterPanel->mode() != FilterPanelWidget::Mode::Search)
        return;

    LogModel* active = m_activeLogView->model();

    FilterRuleSet rules = m_filterPanel->ruleSet();
    // Пустой запрос в режиме поиска = нет результатов (а не «пропустить всё»,
    // как трактует пустой набор фильтр). Иначе панель заполнилась бы всем логом.
    if (!rules.isActive()) {
        clearSearchResults();
        if (m_activeLogView->view())
            m_activeLogView->view()->setTextHighlightPatterns({});
        return;
    }

    // Поиск ведём над текущим ВИДИМЫМ набором активной вкладки (после Time/Level/
    // Fields-фильтров) — тогда любой результат гарантированно виден в main и клик
    // всегда попадает точно на строку.
    const bool fieldScope = m_fieldFilterEnabledCheckBox && m_fieldFilterEnabledCheckBox->isChecked();
    rules.bindFields(LogPattern(m_conversionPattern).fieldNames(), fieldScope);

    // Зеркалим отображение полей активной модели, чтобы текст строк совпадал.
    m_searchResultsModel->setAvailableFields(active->availableFields());
    m_searchResultsModel->setFieldDisplaySelection(active->fieldDisplayFilterEnabled(),
                                                   active->visibleFieldIndexes());

    // Подавляем авто-навигацию: reset модели результатов дёрнет currentRowChanged.
    m_suppressResultNavigation = true;
    m_searchResultsModel->seedFromVisible(*active);
    m_searchResultsModel->setFilterRules(rules);
    m_suppressResultNavigation = false;

    // Подсветка совпадений: всегда в панели результатов; в основном view — по галочке.
    m_searchResultsView->setTextHighlightPatterns(rules.highlightPatterns());
    if (m_activeLogView->view())
        m_activeLogView->view()->setTextHighlightPatterns(
            m_filterPanel->highlightInMainView() ? rules.highlightPatterns()
                                                 : QVector<HighlightPattern>{});

    // На малых логах setFilterRules фильтрует синхронно и modelFiltered мог
    // прийти ДО setEntries-контента — обновим счётчик явно.
    if (m_searchResultsStatusLabel)
        m_searchResultsStatusLabel->setText(
            tr("%n match(es)", "", m_searchResultsModel->rowCount()));
}

void MainWindow::clearSearchResults()
{
    if (m_searchRefreshTimer)
        m_searchRefreshTimer->stop();
    if (!m_searchResultsModel)
        return;
    // Reset модели дёрнет currentRowChanged — не даём ему прыгнуть в main.
    m_suppressResultNavigation = true;
    m_searchResultsModel->setEntries({});
    m_suppressResultNavigation = false;
    if (m_searchResultsStatusLabel)
        m_searchResultsStatusLabel->setText(tr("No search active."));
}

void MainWindow::onFilterModeChanged()
{
    if (!m_filterPanel)
        return;

    const bool search = (m_filterPanel->mode() == FilterPanelWidget::Mode::Search);

    if (search) {
        // Входим в режим поиска: снимаем скрывающий фильтр (строки возвращаются).
        // Док НЕ показываем принудительно — это обычная панель (меню View).
        // runSearchIntoResults сам заполнит результаты или очистит их, если
        // запрос пуст.
        if (m_activeLogView && m_activeLogView->model())
            m_activeLogView->model()->setFilterRules(FilterRuleSet{});
        runSearchIntoResults();
    } else {
        // Возврат в режим фильтра: панель результатов просто пустеет (не
        // прячется), снимаем подсветку поиска. Скрывающий фильтр НЕ применяем
        // автоматически (без сюрпризов).
        clearSearchResults();
        if (m_activeLogView && m_activeLogView->view())
            m_activeLogView->view()->setTextHighlightPatterns({});
    }
    updateFilterStatusButtons();
}

void MainWindow::onHighlightInMainViewChanged()
{
    // Галочка подсветки работает в ОБОИХ режимах (Filter и Search).
    if (!m_filterPanel || !m_activeLogView || !m_activeLogView->view())
        return;

    FilterRuleSet rules = m_filterPanel->ruleSet();
    const bool fieldScope = m_fieldFilterEnabledCheckBox && m_fieldFilterEnabledCheckBox->isChecked();
    rules.bindFields(LogPattern(m_conversionPattern).fieldNames(), fieldScope);
    m_activeLogView->view()->setTextHighlightPatterns(
        m_filterPanel->highlightInMainView() ? rules.highlightPatterns()
                                             : QVector<HighlightPattern>{});
}

void MainWindow::onSearchResultActivated(const QModelIndex& current)
{
    if (m_suppressResultNavigation || !current.isValid()
        || !m_activeLogView || !m_activeLogView->model() || !m_activeLogView->view())
        return;

    const LogModel::EntryKey key = m_searchResultsModel->keyForRow(current.row());
    if (key.logicalEntryId < 0)
        return;

    const int mainRow = m_activeLogView->model()->nearestVisibleRow(
        key.logicalEntryId, key.sourceFile);
    if (mainRow < 0)
        return;

    const QModelIndex idx = m_activeLogView->model()->index(mainRow, 0);
    // Двигаем текущую строку и центрируем — фокус остаётся на панели результатов,
    // так что стрелками можно продолжать листать совпадения.
    m_activeLogView->view()->setCurrentIndex(idx);
    m_activeLogView->view()->scrollTo(idx, QAbstractItemView::PositionAtCenter);
}

void MainWindow::scheduleSearchRefresh()
{
    if (!m_filterPanel || m_filterPanel->mode() != FilterPanelWidget::Mode::Search)
        return;
    if (!m_searchResultsDockWidget || !m_searchResultsDockWidget->isVisible())
        return;
    if (m_searchRefreshTimer)
        m_searchRefreshTimer->start();
}

void MainWindow::applyRowMarkersToActiveView()
{
    if (!m_markerPanel || !m_activeLogView || !m_activeLogView->model())
        return;
    m_activeLogView->model()->setRowMarkers(m_markerPanel->markers());
}

void MainWindow::rebindFiltersOnAllViews()
{
    // Схема полей или галочка "Filter blocks" изменились: каждая вкладка
    // сохраняет СВОЙ применённый набор правил, но колоночные привязки
    // должны быть пересчитаны под новую схему.
    const QStringList fieldNames = LogPattern(m_conversionPattern).fieldNames();
    const bool fieldScope = m_fieldFilterEnabledCheckBox && m_fieldFilterEnabledCheckBox->isChecked();

    for (int t = 0; t < ui->tabWidget->count(); ++t) {
        auto* lv = qobject_cast<LogViewWidget*>(ui->tabWidget->widget(t));
        if (!lv || !lv->model())
            continue;
        FilterRuleSet rules = lv->model()->filterRules();
        if (!rules.isActive())
            continue; // у документа нет фильтров — нечего перепривязывать
        rules.bindFields(fieldNames, fieldScope);
        lv->model()->setFilterRules(rules);
    }
}

void MainWindow::updateFilterPanelFieldNames()
{
    if (!m_filterPanel)
        return;
    const bool fieldScope = m_fieldFilterEnabledCheckBox && m_fieldFilterEnabledCheckBox->isChecked();
    m_filterPanel->setFieldNames(LogPattern(m_conversionPattern).fieldNames(), fieldScope);
}

void MainWindow::updateFilterInputsFromModel()
{
    // Текстовые фильтры и маркеры пер-вкладочные и применяются явно
    // (Apply / автоприменение маркеров), поэтому при смене вкладки
    // синхронизируется только фильтр по времени.
    LogViewWidget *currentView = qobject_cast<LogViewWidget *>(ui->tabWidget->currentWidget());

    if (currentView && currentView->model())
    {
        LogModel *model = currentView->model();
        QDateTime modelStartTime = model->startTimeFilter();
        QDateTime modelEndTime = model->endTimeFilter();

        if (modelStartTime.isValid())
        {
            m_timeFilterFrom->setDateTime(modelStartTime);
        }
        else
        {
            m_timeFilterFrom->clear();
        }
        if (modelEndTime.isValid())
        {
            m_timeFilterTo->setDateTime(modelEndTime);
        }
        else
        {
            m_timeFilterTo->clear();
        }
        m_applyTimeFilterButton->setChecked(modelStartTime.isValid() && modelEndTime.isValid());
    }
    else
    {
        m_timeFilterFrom->setDateTime(QDateTime::currentDateTime().addDays(-1).date().startOfDay());
        m_timeFilterTo->setDateTime(QDateTime::currentDateTime().date().endOfDay());
    }
}

void MainWindow::handleModelFiltered()
{
    if (m_activeLogView && m_activeLogView->model())
    {
        int totalRows = m_activeLogView->model()->rowCount();
        handleTotalRowCountChanged(totalRows);
    }
    // Любая перефильтрация (Apply/Reset из панелей, зум таймлайна, уровни)
    // могла изменить состояние фильтров — обновить кнопки-индикаторы.
    updateFilterStatusButtons();
    // Видимый набор активной вкладки изменился — в режиме поиска пересобрать
    // результаты (мы ищем над filteredEntries активной модели).
    scheduleSearchRefresh();
}

void MainWindow::onScanDirectoryClicked()
{
    QString dirPath = QFileDialog::getExistingDirectory(this, tr("Select Directory to Scan"),
        m_lastScanDir.isEmpty() ? QDir::homePath() : m_lastScanDir,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!dirPath.isEmpty()) {
        m_lastScanDir = dirPath;
        m_dirScanner->setFileExtensions(AppSettings::instance().scanExtensions());
        m_dirScanner->setConversionPattern(m_conversionPattern);
        m_dirScannerPanel->scanDirectory(dirPath);
    }
}

void MainWindow::onOpenSelectedDirectoryFiles(const QStringList &filePaths)
{
    if (filePaths.isEmpty())
        return;

    LogViewWidget *view = nullptr;
    int tabIndex = -1;

    if (ui->tabWidget->count() == 0 ||
        ((view = qobject_cast<LogViewWidget *>(ui->tabWidget->currentWidget())) && view && view->fileCount() > 0))
    {
        view = createLogViewWidget();
        tabIndex = ui->tabWidget->addTab(view, tr("Logs"));
        ui->tabWidget->setCurrentIndex(tabIndex);
    }
    else
    {
        view = qobject_cast<LogViewWidget *>(ui->tabWidget->currentWidget());
        if (!view)
        {
            view = createLogViewWidget();
            tabIndex = ui->tabWidget->addTab(view, tr("Logs"));
            ui->tabWidget->setCurrentIndex(tabIndex);
        }
        else
        {
            if (m_activeLogView != view)
                connectToLogView(view);
        }
    }

    if (!view)
        return;

    for (const QString &fPath : filePaths)
    {
        view->addLogFile(fPath);
    }

    int currentTabIndex = ui->tabWidget->indexOf(view);
    if (currentTabIndex == -1)
        currentTabIndex = ui->tabWidget->currentIndex();

    int fileCountInView = view->fileCount();
    QString tabText;
    QString tabToolTip;

    if (fileCountInView == 1)
    {
        // Use the path from filePaths instead of loadedFiles().first() to avoid
        // crashing if the async load hasn't populated the model yet.
        QString loadedPath = filePaths.first();
        tabText = QFileInfo(loadedPath).fileName();
        tabToolTip = loadedPath;
    }
    else if (fileCountInView > 1)
    {
        tabText = tr("Logs (%1)").arg(fileCountInView);
        QStringList loadedFilePaths;
        for (const auto &lf : view->loadedFiles())
        {
            loadedFilePaths << lf->filePath;
        }
        tabToolTip = loadedFilePaths.join("\n");
    }
    else
    {
        tabText = tr("Logs");
        tabToolTip = "";
    }
    ui->tabWidget->setTabText(currentTabIndex, tabText);
    ui->tabWidget->setTabToolTip(currentTabIndex, tabToolTip);
}

void MainWindow::onConfigureScanExtensionsClicked()
{
    // Delegate to the full Settings dialog — opens on the General tab
    // where the user can add/remove file extensions properly.
    SettingsDialog dlg(this);
    dlg.exec();
    // If the user applied changes, AppSettings::settingsChanged() was already
    // emitted and the scanner will use the new extensions on the next scan.
}

void MainWindow::onSettingsTriggered()
{
    SettingsDialog dlg(this);
    dlg.exec();
}

void MainWindow::toggleTextFilterDock()
{
    if (ui->textFilterDockWidget)
    {
        ui->textFilterDockWidget->setVisible(!ui->textFilterDockWidget->isVisible());
    }
}

void MainWindow::toggleDirectoryScannerDock()
{
    if (ui->directoryScannerDockWidget)
    {
        ui->directoryScannerDockWidget->setVisible(!ui->directoryScannerDockWidget->isVisible());
    }
}

void MainWindow::toggleTimeFilterDock()
{
    if (ui->timeFilterDockWidget)
    {
        ui->timeFilterDockWidget->setVisible(!ui->timeFilterDockWidget->isVisible());
    }
}

static const int MaxRecentFiles = 5;

void MainWindow::addToRecentFiles(const QString& filePath)
{
    m_recentFiles.removeAll(filePath);
    m_recentFiles.prepend(filePath);
    while (m_recentFiles.size() > MaxRecentFiles)
        m_recentFiles.removeLast();
    updateRecentFilesMenu();
}

void MainWindow::updateRecentFilesMenu()
{
    ui->menuRecentFiles->clear();
    if (m_recentFiles.isEmpty()) {
        QAction* empty = ui->menuRecentFiles->addAction(tr("(No recent files)"));
        empty->setEnabled(false);
        return;
    }
    for (int i = 0; i < m_recentFiles.size(); ++i) {
        const QString filePath = m_recentFiles.at(i);
        QString label = QStringLiteral("&%1  %2").arg(i + 1).arg(QFileInfo(filePath).fileName());
        QAction* a = ui->menuRecentFiles->addAction(label);
        a->setToolTip(filePath);
        connect(a, &QAction::triggered, this, [this, filePath]() {
            openRecentFile(filePath);
        });
    }
}

void MainWindow::openFilesFromCommandLine(const QStringList& paths)
{
    for (const QString& path : paths) {
        const QFileInfo fi(path);
        if (fi.isFile())
            openRecentFile(fi.absoluteFilePath());
    }
}

void MainWindow::openStdinStream()
{
    if (m_stdinSpooler)
        return;
    m_stdinSpooler = new StdinSpooler(this);
    if (!m_stdinSpooler->startSpooling()) {
        QMessageBox::warning(this, tr("stdin"),
            tr("Could not create a spool file for standard input."));
        m_stdinSpooler->deleteLater();
        m_stdinSpooler = nullptr;
        return;
    }

    const QString path = m_stdinSpooler->spoolFilePath();
    openRecentFile(path);

    auto* lv = qobject_cast<LogViewWidget*>(ui->tabWidget->currentWidget());
    if (!lv)
        return;

    // Живой поток: авто-догрузка + автопрокрутка к новым строкам.
    lv->setAutoReload(true);
    updateAutoReloadTimer();
    if (lv->view())
        lv->view()->setFollowTail(true);

    const int idx = ui->tabWidget->indexOf(lv);
    if (idx >= 0) {
        ui->tabWidget->setTabText(idx, tr("stdin"));
        ui->tabWidget->setTabToolTip(idx,
            tr("Standard input (spooled to %1)").arg(path));
    }

    // Пинки от спулера — сверх обычного поллинга, чтобы хвост подтягивался
    // живо (спулер сигналит не чаще ~3 раз в секунду).
    connect(m_stdinSpooler, &StdinSpooler::bytesAppended, lv,
            [lv](qint64) { lv->reloadChangedFiles(); });
}

void MainWindow::openRecentFile(const QString& filePath)
{
    if (!QFileInfo::exists(filePath)) {
        QMessageBox::warning(this, tr("File Not Found"),
            tr("The file '%1' no longer exists.").arg(filePath));
        m_recentFiles.removeAll(filePath);
        updateRecentFilesMenu();
        return;
    }

    LogViewWidget* view = nullptr;
    if (ui->tabWidget->count() == 0 ||
        ((view = qobject_cast<LogViewWidget*>(ui->tabWidget->currentWidget())) && view && view->fileCount() > 0))
    {
        view = createLogViewWidget();
        int tabIndex = ui->tabWidget->addTab(view, tr("Logs"));
        ui->tabWidget->setCurrentIndex(tabIndex);
    }
    else
    {
        view = qobject_cast<LogViewWidget*>(ui->tabWidget->currentWidget());
        if (!view) {
            view = createLogViewWidget();
            int tabIndex = ui->tabWidget->addTab(view, tr("Logs"));
            ui->tabWidget->setCurrentIndex(tabIndex);
        } else if (m_activeLogView != view) {
            connectToLogView(view);
        }
    }

    if (!view)
        return;

    view->addLogFile(filePath);

    int currentTabIndex = ui->tabWidget->indexOf(view);
    if (currentTabIndex == -1)
        currentTabIndex = ui->tabWidget->currentIndex();

    ui->tabWidget->setTabText(currentTabIndex, QFileInfo(filePath).fileName());
    ui->tabWidget->setTabToolTip(currentTabIndex, filePath);

    m_lastOpenDir = QFileInfo(filePath).absolutePath();
    addToRecentFiles(filePath);
}

// Search slot implementations
void MainWindow::onSearchEnterPressed()
{
    onSearchNextTriggered(); // Enter behaves like "Find Next"
}

void MainWindow::onSearchNextTriggered()
{
    if (m_activeLogView && m_searchLineEdit)
    {
        QString searchTerm = m_searchLineEdit->text();
        if (!searchTerm.isEmpty())
        {
            // For now, search is case-insensitive. This could be a checkbox in the UI later.
            m_activeLogView->searchTextNext(searchTerm, false /*caseSensitive*/);
        }
    }
}

void MainWindow::onSearchPreviousTriggered()
{
    if (m_activeLogView && m_searchLineEdit)
    {
        QString searchTerm = m_searchLineEdit->text();
        if (!searchTerm.isEmpty())
        {
            // For now, search is case-insensitive.
            m_activeLogView->searchTextPrevious(searchTerm, false /*caseSensitive*/);
        }
    }
}

// ---------------------------------------------------------------------------
void MainWindow::onReloadFileTriggered()
{
    // Manual one-shot reload of the active tab (F5 or left-click on the button).
    if (!m_activeLogView)
        return;
    m_activeLogView->reloadChangedFiles();
}

// ---------------------------------------------------------------------------
void MainWindow::onAutoReloadTimerTick()
{
    // Reload every tab that has per-tab auto-reload enabled.
    for (int i = 0; i < ui->tabWidget->count(); ++i) {
        auto* lvw = qobject_cast<LogViewWidget*>(ui->tabWidget->widget(i));
        if (lvw && lvw->autoReload())
            lvw->reloadChangedFiles();
    }
}

// ---------------------------------------------------------------------------
void MainWindow::onToggleTabAutoReload()
{
    if (m_activeLogView)
        setTabAutoReload(m_activeLogView, !m_activeLogView->autoReload());
}

// ---------------------------------------------------------------------------
void MainWindow::updateAutoReloadTimer()
{
    bool anyActive = false;
    for (int i = 0; i < ui->tabWidget->count(); ++i) {
        auto* lvw = qobject_cast<LogViewWidget*>(ui->tabWidget->widget(i));
        if (lvw && lvw->autoReload()) {
            anyActive = true;
            break;
        }
    }
    if (anyActive)
        m_autoReloadTimer->start();
    else
        m_autoReloadTimer->stop();
}

// ---------------------------------------------------------------------------
// Single atomic entry-point for changing a tab's auto-reload state.
// Always call this instead of touching setAutoReload/setChecked/updateAutoReloadTimer
// individually, so every caller stays in sync automatically.
void MainWindow::setTabAutoReload(LogViewWidget* view, bool enabled)
{
    if (!view) return;
    view->setAutoReload(enabled);
    if (view == m_activeLogView)
        syncReloadButton();
    updateAutoReloadTimer();
}

// ---------------------------------------------------------------------------
// Syncs the toolbar button's checked state to the currently active tab.
void MainWindow::syncReloadButton()
{
    if (m_reloadButton)
        m_reloadButton->setChecked(m_activeLogView && m_activeLogView->autoReload());
}

// ---------------------------------------------------------------------------
void MainWindow::applyAutoReloadSettings()
{
    // Interval is always global (from Settings). Per-tab toggle controls participation.
    const int intervalMs = AppSettings::instance().autoReloadIntervalSecs() * 1000;
    m_autoReloadTimer->setInterval(intervalMs);
    updateAutoReloadTimer();
}

// ---------------------------------------------------------------------------
bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == m_reloadButton) {
        const auto type = event->type();

        // Prevent the toolbar context menu from appearing on right-click over our
        // button: ContextMenuEvent would otherwise bubble up to the QToolBar parent.
        if (type == QEvent::ContextMenu)
            return true;

        if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                if (type == QEvent::MouseButtonPress) {
                    m_reloadButton->setDown(true);
                } else {
                    m_reloadButton->setDown(false);
                    // If auto-reload is on, left-click exits that state first.
                    if (m_activeLogView && m_activeLogView->autoReload())
                        setTabAutoReload(m_activeLogView, false);
                    onReloadFileTriggered();
                }
                return true;
            } else if (me->button() == Qt::RightButton) {
                // Right-click = toggle per-tab auto-reload.
                if (type == QEvent::MouseButtonRelease)
                    onToggleTabAutoReload();
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(obj, event);
}
