#include "mainwindow.h"
#include "filememorystore.h"
#include "scrollbarmarks.h"
#include "bookmarkpanelwidget.h"
#include "quicksearchfield.h"
#include "ui_mainwindow.h"
#include "logviewwidget.h"
#include "encodingindicator.h"
#include "logtabwidget.h"
#include "conversionpatterndialog.h"
#include "gotodialog.h"
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
#include "cardframe.h"
#include "apptheme.h"
#include "updatechecker.h"
#include "stdinspooler.h"
#include "openrouting.h"
#include "viewexport.h"
#include "searchresultscontroller.h"
#include "searchresultsnavigator.h"
#include "fieldreextraction.h"
#include "welcomewidget.h"
#include "compactstyle.h"
#include "searchpanelwidget.h"

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
#include "sessioncontroller.h"
#include "sessionfile.h"
#include "sessionreportdialog.h"
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
#include <QTextBrowser>
#include <QDialog>
#include <QDesktopServices>
#include <QProcess>
#include <QClipboard>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QLocale>
#include <QUrl>
#include <QDate>
#include <QScrollArea>
#include <QSignalBlocker>

#include <algorithm>
#include <limits>

// Recolouring a monochrome SVG resource so the glyph stays visible on both
// light and dark palettes lives in CardFrame, next to the other shared chrome
// helpers — the scanner panel needs the same treatment for its buttons.
static QIcon tintedIcon(const QString& resourcePath, const QColor& color)
{
    return CardFrame::tintedIcon(resourcePath, color);
}

// Подсказка действия без сочетания клавиш: applyShortcuts() дописывает к
// первой её строке текущее сочетание, так что после переназначения клавиш
// подсказка не врёт.
static const char* const kBaseToolTipProperty = "dendroBaseToolTip";

static void setBaseToolTip(QAction* action, const QString& tip)
{
    action->setProperty(kBaseToolTipProperty, tip);
    action->setToolTip(tip);
}

// Цветная точка кнопки уровня: цвет уровня узнаётся раньше, чем подпись.
static QIcon levelDotIcon(const QColor& color)
{
    QPixmap pm(20, 20);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color.darker(135), 1.5));
    p.setBrush(color);
    p.drawEllipse(QRectF(2.5, 2.5, 15.0, 15.0));
    return QIcon(pm);
}

// Шрифт панели результатов поиска — на пункт меньше, чем в основном view:
// это вспомогательный список (обычно 2–5 строк высотой), и при крупном
// основном шрифте он съедал бы половину окна. Отдельной настройки нет
// намеренно — размер всегда следует за настройкой шрифта лога.
// Schema combo item: the description (if any) is its tooltip.
static void addSchemaComboItem(QComboBox* combo, const SchemaStore::SchemaEntry& entry)
{
    combo->addItem(entry.name);
    const QString description = entry.description();
    if (!description.isEmpty())
        combo->setItemData(combo->count() - 1, description, Qt::ToolTipRole);
}

static QFont searchResultsFont()
{
    QFont f(AppSettings::instance().fontFamily());
    f.setPointSize(qMax(6, AppSettings::instance().fontSize() - 1));
    f.setWeight(QFont::Medium);
    return f;
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow), m_progressBar(nullptr), m_statusLabel(nullptr), m_activeLogView(nullptr), m_lineInfoLabel(nullptr), m_timeFilterFrom(nullptr), m_timeFilterTo(nullptr), m_applyTimeFilterButton(nullptr), m_resetTimeFilterButton(nullptr)
{
    ui->setupUi(this);

    // Load persistent application preferences before anything else so that
    // setup methods (e.g. setupDirectoryScanner) can read correct values.
    AppSettings::instance().load();
    ShortcutManager::instance().load();
    m_fileMemory = std::make_unique<FileMemoryStore>(AppSettings::fileMemoryDir());

    setupStatusBar();
    setupTimeFilterDockContents();
    setupTextFilterDockContents();
    setupRowMarkerDock();
    setupTimelineDock();
    setupSearchDock();
    setupEntryDetailsDock();
    setupStatisticsDock();
    setupBookmarksDock();
    setupDirectoryScanner();
    setupFieldVisibilityDock();
    // После setupFieldVisibilityDock (нужен m_fieldFilterEnabledCheckBox) и
    // ДО loadSettings — restoreState() должен знать тулбар по objectName.
    setupFilterStatusToolbar();

    // Лог вплотную к тулбару и докам: у вкладок своя рамка, а поля
    // центральной раскладки давали лишние ~10 px пустоты до панелей.
    ui->centralwidget->layout()->setContentsMargins(0, 0, 0, 0);

    // Стартовый экран — в той же раскладке центрального виджета, что и
    // вкладки; виден, пока их нет (updateWelcomeVisibility).
    m_welcome = new WelcomeWidget(ui->centralwidget);
    ui->centralwidget->layout()->addWidget(m_welcome);
    connect(m_welcome, &WelcomeWidget::openFilesRequested,
            this, &MainWindow::on_actionOpen_triggered);
    connect(m_welcome, &WelcomeWidget::scanDirectoryRequested,
            this, &MainWindow::onScanDirectoryClicked);
    connect(m_welcome, &WelcomeWidget::recentFileRequested,
            this, [this](const QString& path) { openRecentFile(path); });
    connect(m_welcome, &WelcomeWidget::clearRecentRequested,
            this, &MainWindow::clearRecentFiles);

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

        // Панель результатов поиска — свой (уменьшенный) шрифт; раньше она
        // оставалась со шрифтом, снятым при её создании.
        if (m_searchResultsView) {
            m_searchResultsView->setFont(searchResultsFont());
            m_searchResultsView->viewport()->update();
        }

        // Re-apply auto-reload timer whenever settings change.
        applyAutoReloadSettings();

        // Цвета уровней могли смениться в Settings → Colors.
        refreshToolIcons();
        updateLogLevelFilterButtons();
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

    // --- Toolbars -------------------------------------------------------------
    // File (открыть, обновление, follow-tail, перенос) | Find | Log Levels |
    // Filters. Иконки — монохромные SVG в цвет текста палитры (refreshToolIcons,
    // заново при смене темы); подсказки дополняются текущими сочетаниями
    // клавиш в applyShortcuts().
    // Иконки File/Find — размера тулбара из CompactStyle; у уровней — точка.
    ui->levelToolBar->setIconSize(QSize(10, 10));
    ui->levelToolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    setBaseToolTip(ui->actionOpen, tr("Open log file(s) in a new tab"));
    setBaseToolTip(ui->actionReloadFile,
        tr("Reload: read the lines appended to the files of this tab\n"
           "Right-click: toggle auto-reload for this tab"));
    setBaseToolTip(ui->actionAutoReload,
        tr("Auto-reload this tab: pick up new lines by itself\n"
           "(the check interval is set in Settings → General)"));
    setBaseToolTip(ui->actionFollowTail,
        tr("Follow tail: keep the newest lines in view\n"
           "Scrolling up turns it off"));
    setBaseToolTip(ui->actionWordWrap, tr("Word wrap"));
    setBaseToolTip(ui->actionSearchPrevious, tr("Find previous"));
    setBaseToolTip(ui->actionSearchNext, tr("Find next"));
    {
        const QString levelTip = tr("Show only %1 lines. Several levels can be combined;\n"
                                    "with none selected, every level is shown.");
        const QList<QPair<QAction*, QString>> levels {
            { ui->actionFatal, QStringLiteral("Fatal") }, { ui->actionError, QStringLiteral("Error") },
            { ui->actionWarn,  QStringLiteral("Warn")  }, { ui->actionInfo,  QStringLiteral("Info")  },
            { ui->actionDebug, QStringLiteral("Debug") }, { ui->actionTrace, QStringLiteral("Trace") },
        };
        for (const auto& [action, name] : levels)
            setBaseToolTip(action, levelTip.arg(name));
    }

    // Reload — разовое дочитывание; авто-обновление — отдельный тогл рядом.
    connect(ui->actionReloadFile, &QAction::triggered, this, &MainWindow::onReloadFileTriggered);
    connect(ui->actionAutoReload, &QAction::triggered, this, [this](bool on) {
        if (m_activeLogView)
            setTabAutoReload(m_activeLogView, on);
        else
            syncReloadButton(); // вкладки нет — тогл не залипает
    });
    // Прежний жест сохранён: правый клик по Reload переключает авто-обновление.
    if (QToolButton* btn = qobject_cast<QToolButton*>(ui->fileToolBar->widgetForAction(ui->actionReloadFile))) {
        m_reloadButton = btn;
        m_reloadButton->installEventFilter(this);
    }

    // --- Follow-tail (автопрокрутка к концу растущего лога) ---
    m_followTailAction = ui->actionFollowTail;
    connect(m_followTailAction, &QAction::toggled, this, [this](bool on) {
        if (m_activeLogView && m_activeLogView->view()
            && m_activeLogView->view()->followTail() != on)
            m_activeLogView->view()->setFollowTail(on);
    });

    connect(ui->actionScanDirectory, &QAction::triggered, this, &MainWindow::onScanDirectoryClicked);
    connect(ui->actionCloseTab, &QAction::triggered, this, [this]() {
        if (ui->tabWidget->count() > 0)
            closeTab(ui->tabWidget->currentIndex());
    });
    connect(ui->actionExit, &QAction::triggered, this, &QWidget::close);
    // Сессии (R-5): файл *.dlsession — вкладки целиком, схема и колонки.
    m_sessionController = new SessionController(this);
    QAction* openSessionAction = new QAction(tr("Open Session..."), this);
    openSessionAction->setToolTip(tr("Open the tabs of a saved session next to the open ones"));
    connect(openSessionAction, &QAction::triggered, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, tr("Open Session"), m_lastOpenDir,
            tr("DendroLog sessions (*.dlsession);;All files (*)"));
        if (!path.isEmpty())
            openSessionFile(path);
    });
    QAction* saveSession = new QAction(tr("Save Session As..."), this);
    saveSession->setToolTip(tr("Save all tabs with their filters, bookmarks and positions to a file"));
    connect(saveSession, &QAction::triggered, this, &MainWindow::saveSessionAs);
    ui->menuFile->insertActions(ui->actionCloseTab, { openSessionAction, saveSession });
    ui->menuFile->insertSeparator(ui->actionCloseTab);
    connect(m_sessionController, &SessionController::reportChanged, this, [this]() {
        if (m_sessionReportLink && m_sessionController->report().isEmpty())
            m_sessionReportLink->hide(); // вкладки с проблемами закрыты
    });
    // Сессия окна для контроллера: вкладки по порядку ряда, схема и колонки.
    m_sessionController->setWindow({
        [this]() { return logViews(); },
        [this]() { return qobject_cast<LogViewWidget*>(ui->tabWidget->currentWidget()); },
        [this]() {
            SessionController::WindowSettings w;
            w.schema = m_conversionPattern;
            w.schemaName = SessionController::schemaNameFor(m_patternList,
                m_conversionPatternCombo ? m_conversionPatternCombo->currentIndex() : -1, m_conversionPattern);
            w.fieldExtraction = m_fieldFilterEnabledCheckBox && m_fieldFilterEnabledCheckBox->isChecked();
            w.visibleFields = selectedVisibleFieldNames();
            return w;
        } });
    // Ctrl+Shift+T — вернуть закрытую вкладку; прошлый сеанс — ссылкой на
    // стартовом экране (последняя сессия читается в startUp).
    QAction* reopenTab = new QAction(tr("Reopen Closed Tab"), this);
    reopenTab->setEnabled(false);
    connect(reopenTab, &QAction::triggered, this, &MainWindow::reopenClosedTab);
    connect(m_sessionController, &SessionController::closedTabsChanged, reopenTab,
            [reopenTab](int count) { reopenTab->setEnabled(count > 0); });
    ui->menuFile->insertAction(ui->actionCloseTab, reopenTab);
    m_shortcutActions.insert(QStringLiteral("reopenTab"), reopenTab);
    connect(m_sessionController, &SessionController::lastSessionAvailabilityChanged,
            m_welcome, &WelcomeWidget::setRestoreSessionAvailable);
    connect(m_welcome, &WelcomeWidget::restoreSessionRequested, this, [this]() {
        QString error;
        if (const std::optional<SessionState> session = m_sessionController->takeLastSession(&error))
            openSession(*session, m_sessionController->lastSessionFile(), SessionController::Origin::LastSession);
        else
            statusBar()->showMessage(tr("Could not restore the previous session: %1").arg(error), 10000);
    });
    connect(ui->actionZoomIn,  &QAction::triggered, this, [this]() { changeFontSize(+1); });
    connect(ui->actionZoomOut, &QAction::triggered, this, [this]() { changeFontSize(-1); });
    connect(ui->actionZoomReset, &QAction::triggered, this, [this]() { changeFontSize(0); });
    connect(ui->actionShowAllLevels, &QAction::triggered, this, &MainWindow::showAllLevels);
    connect(ui->actionResetAllFilters, &QAction::triggered,
            this, &MainWindow::resetAllFiltersOnActiveView);
    ui->menuRecentFiles->setToolTipsVisible(true);

    // "Tools" menu: field schemas and Settings.
    QMenu* menuTools = menuBar()->addMenu(tr("&Tools"));
    QAction* schemasAction = menuTools->addAction(tr("Field &Schemas..."));
    connect(schemasAction, &QAction::triggered, this, &MainWindow::onManagePatterns);
    menuTools->addSeparator();
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
    // Placeholder (с текущим сочетанием клавиш) ставит applyShortcuts().
    m_searchLineEdit = new QuickSearchField(ui->findToolBar);
    m_searchLineEdit->setObjectName("searchLineEdit");
    m_searchLineEdit->setMinimumWidth(160);
    m_searchLineEdit->setMaximumWidth(280);
    m_searchLineEdit->setClearButtonEnabled(true);
    ui->findToolBar->insertWidget(ui->actionSearchPrevious, m_searchLineEdit);

    // Переключатели запроса — «Aa» (регистр), «ab» (слово целиком), «.*»
    // (регекс) — рядом с полем; состояние и история живут между запусками.
    {
        AppSettings& settings = AppSettings::instance();
        QuickSearchField* field = m_searchLineEdit;
        field->matchCaseAction()->setChecked(settings.quickSearchMatchCase());
        field->wholeWordAction()->setChecked(settings.quickSearchWholeWord());
        field->regexAction()->setChecked(settings.quickSearchRegex());
        field->setHistory(settings.quickSearchHistory());
        const struct { QAction* action; const char* id; } toggles[] = {
            {field->matchCaseAction(), "findMatchCase"},
            {field->wholeWordAction(), "findWholeWord"},
            {field->regexAction(), "findRegex"},
        };
        for (const auto& t : toggles) {
            setBaseToolTip(t.action, t.action->toolTip());
            ui->findToolBar->insertAction(ui->actionSearchPrevious, t.action);
            m_shortcutActions.insert(QString::fromLatin1(t.id), t.action);
        }
        // В ini — сразу: не теряется при аварии, а у второго окна
        // (--new-instance) пропадает только то, что оно перепишет само.
        connect(field, &QuickSearchField::optionsChanged, this, [field]() {
            AppSettings& s = AppSettings::instance();
            s.setQuickSearchMatchCase(field->matchCaseAction()->isChecked());
            s.setQuickSearchWholeWord(field->wholeWordAction()->isChecked());
            s.setQuickSearchRegex(field->regexAction()->isChecked());
            s.saveQuickSearch();
        });
        connect(field, &QuickSearchField::historyChanged, this, [](const QStringList& entries) {
            AppSettings::instance().setQuickSearchHistory(entries);
            AppSettings::instance().saveQuickSearch();
        });
        // Запрос из истории — сразу поиск вперёд: повтор без набора.
        connect(field, &QuickSearchField::historyActivated, this,
                [this]() { onSearchNextTriggered(); });
    }

    // «All» — тот же текст, но списком всех строк в панели Search: поле
    // тулбара прыгает по совпадениям, панель их перечисляет.
    m_findAllAction = new QAction(tr("All"), this);
    setBaseToolTip(m_findAllAction,
        tr("Find all: list every line with this text in the Search panel\n"
           "(the log is not filtered; match case, whole word and regex carry over)"));
    ui->findToolBar->addAction(m_findAllAction);
    connect(m_findAllAction, &QAction::triggered, this, [this]() {
        TextQuery query = m_searchLineEdit->query();
        // Пустое поле — просто открыть панель; невалидный регекс — не идём.
        if (!query.isEmpty() && !takeQuickSearchQuery(&query))
            return;
        findAllInSearchPanel(query);
    });
    m_shortcutActions.insert(QStringLiteral("findAll"), m_findAllAction);
    // Подложка «не найдено» живёт до правки запроса или флагов — это делает
    // само поле.

    // Connect search actions
    connect(m_searchLineEdit, &QLineEdit::returnPressed, this, &MainWindow::onSearchEnterPressed);
    connect(ui->actionSearchNext, &QAction::triggered, this, &MainWindow::onSearchNextTriggered);
    connect(ui->actionSearchPrevious, &QAction::triggered, this, &MainWindow::onSearchPreviousTriggered);

    // Быстрый поиск идёт в фоне; Esc в поле поиска его отменяет.
    QAction* cancelSearchAction = new QAction(tr("Cancel Search"), m_searchLineEdit);
    cancelSearchAction->setShortcut(QKeySequence(Qt::Key_Escape));
    cancelSearchAction->setShortcutContext(Qt::WidgetShortcut);
    static_cast<QWidget*>(m_searchLineEdit)->addAction(cancelSearchAction);
    connect(cancelSearchAction, &QAction::triggered, this, [this]() {
        if (m_activeLogView && m_activeLogView->isQuickSearchRunning()) {
            m_activeLogView->cancelQuickSearch();
            m_quickSearchStatusShown = false;
            m_statusLabel->setText(tr("Search cancelled."));
        }
    });

    // Quick-search focus action (Ctrl+F by default): just move focus to the
    // search field and pre-select its text for an immediate new query.
    QAction* focusSearchAction = new QAction(tr("Find"), this);
    connect(focusSearchAction, &QAction::triggered, this, [this]() {
        m_searchLineEdit->setFocus(Qt::ShortcutFocusReason);
        m_searchLineEdit->selectAll();
    });
    addAction(focusSearchAction);   // window-level shortcut
    m_shortcutActions.insert(QStringLiteral("focusSearch"), focusSearchAction);

    // Go to Line / Time (Ctrl+G): в меню View, перед группой режимов вида.
    m_goToAction = new QAction(tr("Go to Line / Time…"), this);
    connect(m_goToAction, &QAction::triggered, this, &MainWindow::showGoToDialog);
    ui->menuView->insertAction(ui->actionWordWrap, m_goToAction);
    m_shortcutActions.insert(QStringLiteral("goTo"), m_goToAction);

    // История переходов вкладки (R-7): Alt+← / Alt+→, рядом с Go to.
    // Доступность — по истории активной вкладки (updateNavHistoryActions).
    {
        const auto addHistoryAction = [this](const QString& text, const QString& shortcutId,
                                             bool forward) {
            auto* action = new QAction(text, this);
            connect(action, &QAction::triggered, this, [this, forward]() {
                if (!m_activeLogView)
                    return;
                if (!m_activeLogView->navigateHistory(forward))
                    m_statusLabel->setText(forward ? tr("No later place in navigation history")
                                                   : tr("No earlier place in navigation history"));
                updateNavHistoryActions();
            });
            ui->menuView->insertAction(ui->actionWordWrap, action);
            m_shortcutActions.insert(shortcutId, action);
            return action;
        };
        m_navBackAction = addHistoryAction(tr("Navigate Back"), QStringLiteral("navigateBack"), false);
        m_navForwardAction = addHistoryAction(tr("Navigate Forward"), QStringLiteral("navigateForward"), true);
        updateNavHistoryActions();
    }
    ui->menuView->insertSeparator(ui->actionWordWrap);

    // Следующая / предыдущая ошибка (F8 / Shift+F8) и предупреждение
    // (F7 / Shift+F7) среди показанных строк активной вкладки.
    {
        const LogLevelMask errors = levelBit(LogLevel::Error) | levelBit(LogLevel::Fatal);
        const LogLevelMask warnings = levelBit(LogLevel::Warn);
        const auto addLevelJump = [this](const QString& text, const QString& shortcutId,
                                         LogLevelMask levels, bool forward) {
            QAction* action = new QAction(text, this);
            connect(action, &QAction::triggered, this, [this, levels, forward]() {
                if (m_activeLogView)
                    m_activeLogView->goToLevel(levels, forward);
            });
            ui->menuView->insertAction(ui->actionWordWrap, action);
            m_shortcutActions.insert(shortcutId, action);
            m_levelJumpActions.append(action);
        };
        addLevelJump(tr("Next Error"), QStringLiteral("nextError"), errors, true);
        addLevelJump(tr("Previous Error"), QStringLiteral("prevError"), errors, false);
        addLevelJump(tr("Next Warning"), QStringLiteral("nextWarning"), warnings, true);
        addLevelJump(tr("Previous Warning"), QStringLiteral("prevWarning"), warnings, false);
        ui->menuView->insertSeparator(ui->actionWordWrap);
    }

    // Закладки (R-2): поставить / снять на выбранной строке (Ctrl+F2),
    // следующая / предыдущая видимая (F2 / Shift+F2).
    {
        QAction* toggle = new QAction(tr("Add / Remove Bookmark"), this);
        connect(toggle, &QAction::triggered, this, [this]() {
            if (m_activeLogView && !m_activeLogView->toggleBookmarkAtSelection())
                m_statusLabel->setText(tr("Select a line to bookmark it"));
        });
        ui->menuView->insertAction(ui->actionWordWrap, toggle);
        m_shortcutActions.insert(QStringLiteral("toggleBookmark"), toggle);
        const auto addBookmarkJump = [this](const QString& text, const QString& id, bool forward) {
            QAction* action = new QAction(text, this);
            connect(action, &QAction::triggered, this, [this, forward]() {
                if (m_activeLogView)
                    m_activeLogView->goToBookmark(forward);
            });
            ui->menuView->insertAction(ui->actionWordWrap, action);
            m_shortcutActions.insert(id, action);
        };
        addBookmarkJump(tr("Next Bookmark"), QStringLiteral("nextBookmark"), true);
        addBookmarkJump(tr("Previous Bookmark"), QStringLiteral("prevBookmark"), false);
        ui->menuView->insertSeparator(ui->actionWordWrap);
    }

    // Переход к строкам, дописанным ниже экрана («↓ N new lines» — та же
    // команда, что щелчок по плашке вкладки).
    {
        QAction* action = new QAction(tr("Go to New Lines"), this);
        connect(action, &QAction::triggered, this, [this]() {
            if (m_activeLogView)
                m_activeLogView->view()->goToNewLines();
        });
        ui->menuView->insertAction(ui->actionWordWrap, action);
        ui->menuView->insertSeparator(ui->actionWordWrap);
        m_shortcutActions.insert(QStringLiteral("goToNewLines"), action);
    }

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
        // Крестик вкладки и средняя кнопка мыши (LogTabBar эмитит тот же сигнал)
        // идут через общий closeTab().
        connect(ui->tabWidget, &QTabWidget::tabCloseRequested,
                this, [this](int index) { closeTab(index); });
        connect(ui->tabWidget, &QTabWidget::currentChanged,
                this, &MainWindow::onCurrentTabChanged);
        // Правый клик по заголовку — контекстное меню; бросок вкладки на вкладку
        // — объединение документов.
        connect(ui->tabWidget, &LogTabWidget::tabContextMenuRequested,
                this, &MainWindow::onTabContextMenu);
        connect(ui->tabWidget, &LogTabWidget::mergeTabsRequested,
                this, &MainWindow::mergeTabs);
        // Нет вкладок — стартовый экран вместо пустой рамки.
        connect(ui->tabWidget, &LogTabWidget::tabCountChanged,
                this, &MainWindow::updateWelcomeVisibility);
        updateWelcomeVisibility();

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
    refreshToolIcons();

    // Configurable keyboard shortcuts: map the remaining actions and assign the
    // current sequences. Re-apply automatically when the user edits them.
    registerShortcutActions();
    applyShortcuts();
    connect(&ShortcutManager::instance(), &ShortcutManager::shortcutsChanged,
            this, &MainWindow::applyShortcuts);
    m_constructed = true;
}

// ---------------------------------------------------------------------------
void MainWindow::registerShortcutActions()
{
    // Dock-toggle actions and the Settings action are inserted into
    // m_shortcutActions where they are created; register the static ones here.
    m_shortcutActions.insert(QStringLiteral("open"),       ui->actionOpen);
    m_shortcutActions.insert(QStringLiteral("saveAs"),     ui->actionSaveAs);
    m_shortcutActions.insert(QStringLiteral("closeTab"),   ui->actionCloseTab);
    m_shortcutActions.insert(QStringLiteral("quit"),       ui->actionExit);
    m_shortcutActions.insert(QStringLiteral("reload"),     ui->actionReloadFile);
    m_shortcutActions.insert(QStringLiteral("autoReload"), ui->actionAutoReload);
    m_shortcutActions.insert(QStringLiteral("followTail"), ui->actionFollowTail);
    m_shortcutActions.insert(QStringLiteral("searchNext"), ui->actionSearchNext);
    m_shortcutActions.insert(QStringLiteral("searchPrev"), ui->actionSearchPrevious);
    m_shortcutActions.insert(QStringLiteral("wordWrap"),   ui->actionWordWrap);
    m_shortcutActions.insert(QStringLiteral("zoomIn"),     ui->actionZoomIn);
    m_shortcutActions.insert(QStringLiteral("zoomOut"),    ui->actionZoomOut);
    m_shortcutActions.insert(QStringLiteral("zoomReset"),  ui->actionZoomReset);
    m_shortcutActions.insert(QStringLiteral("levelFatal"), ui->actionFatal);
    m_shortcutActions.insert(QStringLiteral("levelError"), ui->actionError);
    m_shortcutActions.insert(QStringLiteral("levelWarn"),  ui->actionWarn);
    m_shortcutActions.insert(QStringLiteral("levelInfo"),  ui->actionInfo);
    m_shortcutActions.insert(QStringLiteral("levelDebug"), ui->actionDebug);
    m_shortcutActions.insert(QStringLiteral("levelTrace"), ui->actionTrace);
    m_shortcutActions.insert(QStringLiteral("levelAll"),   ui->actionShowAllLevels);
    m_shortcutActions.insert(QStringLiteral("resetFilters"), ui->actionResetAllFilters);
}

// ---------------------------------------------------------------------------
void MainWindow::applyShortcuts()
{
    const ShortcutManager& mgr = ShortcutManager::instance();
    for (const auto& cmd : mgr.commands()) {
        if (QAction* a = m_shortcutActions.value(cmd.id, nullptr))
            a->setShortcut(mgr.sequence(cmd.id));
    }

    // Подсказки кнопок — с текущими сочетаниями (дописываются к первой строке).
    for (QAction* a : findChildren<QAction*>()) {
        const QVariant base = a->property(kBaseToolTipProperty);
        if (!base.isValid())
            continue;
        QString tip = base.toString();
        if (!a->shortcut().isEmpty()) {
            const QString keys = QStringLiteral(" (%1)")
                .arg(a->shortcut().toString(QKeySequence::NativeText));
            const int eol = tip.indexOf(QLatin1Char('\n'));
            tip.insert(eol < 0 ? tip.size() : eol, keys);
        }
        a->setToolTip(tip);
    }

    const QKeySequence findSeq = mgr.sequence(QStringLiteral("focusSearch"));
    if (m_searchLineEdit)
    {
        m_searchLineEdit->setPlaceholderText(findSeq.isEmpty()
            ? tr("Find in log")
            : tr("Find in log (%1)").arg(findSeq.toString(QKeySequence::NativeText)));
        const auto keys = [&mgr](const char* id) {
            return mgr.sequence(QString::fromLatin1(id)).toString(QKeySequence::NativeText);
        };
        QString tip = tr("Find in log: Enter — next");
        if (!keys("searchNext").isEmpty())
            tip += QStringLiteral(" (%1)").arg(keys("searchNext"));
        if (!keys("searchPrev").isEmpty())
            tip += tr(", %1 — previous").arg(keys("searchPrev"));
        tip += QLatin1Char('\n') + tr("Down arrow — recent searches");
        m_searchLineEdit->setBaseToolTip(tip);
    }
    if (m_welcome)
        m_welcome->setShortcutHints(mgr.sequence(QStringLiteral("open")), findSeq);
}

// ---------------------------------------------------------------------------
void MainWindow::refreshToolIcons()
{
    const QColor glyph = palette().color(QPalette::ButtonText);
    ui->actionOpen->setIcon(tintedIcon(QStringLiteral(":/icons/open.svg"), glyph));
    ui->actionReloadFile->setIcon(tintedIcon(QStringLiteral(":/icons/reload.svg"), glyph));
    ui->actionAutoReload->setIcon(tintedIcon(QStringLiteral(":/icons/autoreload.svg"), glyph));
    ui->actionFollowTail->setIcon(tintedIcon(QStringLiteral(":/icons/followtail.svg"), glyph));
    ui->actionWordWrap->setIcon(tintedIcon(QStringLiteral(":/icons/wordwrap.svg"), glyph));
    ui->actionSearchPrevious->setIcon(tintedIcon(QStringLiteral(":/icons/chevron-up.svg"), glyph));
    ui->actionSearchNext->setIcon(tintedIcon(QStringLiteral(":/icons/chevron-down.svg"), glyph));
    if (m_searchLineEdit) {
        m_searchLineEdit->historyAction()->setIcon(
            tintedIcon(QStringLiteral(":/icons/chevron-down.svg"), glyph));
        m_searchLineEdit->refreshStyle();
    }

    const AppTheme& theme = AppTheme::instance();
    ui->actionFatal->setIcon(levelDotIcon(theme.forLevel(LogLevel::Fatal)));
    ui->actionError->setIcon(levelDotIcon(theme.forLevel(LogLevel::Error)));
    ui->actionWarn->setIcon(levelDotIcon(theme.forLevel(LogLevel::Warn)));
    ui->actionInfo->setIcon(levelDotIcon(theme.forLevel(LogLevel::Info)));
    ui->actionDebug->setIcon(levelDotIcon(theme.forLevel(LogLevel::Debug)));
    ui->actionTrace->setIcon(levelDotIcon(theme.forLevel(LogLevel::Trace)));

    // Значок авто-обновления на вкладках — того же цвета, что и глифы.
    for (int i = 0; i < ui->tabWidget->count(); ++i)
        updateTabLabel(qobject_cast<LogViewWidget*>(ui->tabWidget->widget(i)));
}

// ---------------------------------------------------------------------------
void MainWindow::updateWelcomeVisibility()
{
    const bool empty = ui->tabWidget->count() == 0;
    ui->tabWidget->setVisible(!empty);
    if (m_welcome)
        m_welcome->setVisible(empty);
    // Команды над вкладкой без вкладок ничего не делали бы — гасим их.
    for (QAction* a : { ui->actionCloseTab, ui->actionReloadFile, ui->actionFollowTail,
                        ui->actionSearchNext, ui->actionSearchPrevious, m_findAllAction,
                        m_goToAction,
                        ui->actionShowAllLevels, ui->actionResetAllFilters,
                        ui->actionFatal, ui->actionError, ui->actionWarn,
                        ui->actionInfo, ui->actionDebug, ui->actionTrace })
        a->setEnabled(!empty);
    for (QAction* a : std::as_const(m_levelJumpActions))
        a->setEnabled(!empty);
    updateNavHistoryActions();
}

// ---------------------------------------------------------------------------
void MainWindow::updateNavHistoryActions()
{
    if (!m_navBackAction || !m_navForwardAction)
        return;
    const NavHistory* history = m_activeLogView ? &m_activeLogView->navHistory() : nullptr;
    m_navBackAction->setEnabled(history && history->canGoBack());
    m_navForwardAction->setEnabled(history && history->canGoForward());
}

// ---------------------------------------------------------------------------
void MainWindow::changeFontSize(int steps)
{
    AppSettings& settings = AppSettings::instance();
    const int size = steps == 0
        ? AppSettings::kDefaultFontSize
        : qBound(AppSettings::kMinFontSize, settings.fontSize() + steps,
                 AppSettings::kMaxFontSize);
    // settingsChanged перешрифтует все вкладки и панель результатов.
    settings.setFontSize(size);
    m_statusLabel->setText(tr("Font size: %1 pt").arg(size));
}

// ---------------------------------------------------------------------------
void MainWindow::showAllLevels()
{
    LogModel* model = m_activeLogView ? m_activeLogView->model() : nullptr;
    if (!model)
        return;
    if (!model->logLevelFilter().isEmpty())
        model->setLogLevelFilter({});
    updateLogLevelFilterButtons();
}

// ---------------------------------------------------------------------------
void MainWindow::resetAllFiltersOnActiveView()
{
    LogModel* model = m_activeLogView ? m_activeLogView->model() : nullptr;
    if (!model)
        return;
    // Только включённое: каждый сброс — отдельная перефильтрация.
    if (!model->logLevelFilter().isEmpty())
        showAllLevels();
    if (model->startTimeFilter().isValid() || model->endTimeFilter().isValid())
        onResetTimeFilterClicked();
    if (model->filterRules().isActive())
        onResetTextFiltersClicked();
    updateFilterStatusButtons();
    m_statusLabel->setText(tr("Level, time and text filters of this tab are off."));
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
    // Make sure no re-extraction worker still reads entries we are about to
    // release (closeEvent usually handles this, but not every teardown path
    // goes through it).
    cancelFieldExtraction();
    delete ui;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // Stop any background field re-extraction before our members (which its
    // workers reference) are torn down.
    cancelFieldExtraction();
    saveSettings();
    // Последняя сессия (R-5) — только при штатном выходе и только первичным
    // окном; пустое окно прошлую не затирает (SessionController).
    m_sessionController->saveLastSession();
    // Память файлов открытых вкладок (R-4) — на диск до выхода.
    for (LogViewWidget* logView : logViews())
        logView->saveFileMemory();
    m_fileMemory->waitForWrites();
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
        if (url.isLocalFile())
            paths << url.toLocalFile();
    }
    // Файлы открываются, папка уходит в Directory Scanner — правило одно на
    // drop, командную строку и второй экземпляр (OpenRouting::plan).
    if (!OpenRouting::plan(paths).isEmpty()) {
        event->acceptProposedAction();
        // После выхода из обработчика: сообщение (битая сессия) не держит
        // перетаскивание источника (Explorer) открытым.
        QTimer::singleShot(0, this, [this, paths]() { openFilesFromCommandLine(paths); });
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
        m_bookmarkDockWidget,
        m_statsDockWidget,
        ui->directoryScannerDockWidget,
    };
    const QList<QDockWidget*> bottomDocks {
        m_timelineDockWidget,
        m_searchDockWidget,
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
    if (m_searchDockWidget) {
        addDockWidget(Qt::BottomDockWidgetArea, m_searchDockWidget);
        if (m_timelineDockWidget)
            splitDockWidget(m_timelineDockWidget, m_searchDockWidget,
                            Qt::Vertical);
    }
    if (m_detailsDockWidget) {
        addDockWidget(Qt::BottomDockWidgetArea, m_detailsDockWidget);
        if (m_searchDockWidget)
            tabifyDockWidget(m_searchDockWidget, m_detailsDockWidget);
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
            order << e.name;
        s.setValue("schemaOrder", order);
    }
    s.endGroup();

    s.beginGroup("Filters");
    if (m_filterPanel) {
        const QJsonDocument doc(m_filterPanel->profilesToJson());
        s.setValue("textFilterProfiles", QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
        s.remove("textFilterRules"); // legacy одиночный набор больше не пишем
        s.remove("textFilterMode");  // режим Filter/Search стал двумя панелями
        s.setValue("highlightInMainView", m_filterPanel->highlightInMainView());
    }
    if (m_searchQuery) {
        const QJsonDocument doc(m_searchQuery->profilesToJson());
        s.setValue("searchProfiles", QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
        s.setValue("searchHighlightInMainView", m_searchQuery->highlightInMainView());
    }
    if (m_markerPanel) {
        const QJsonDocument doc(QJsonObject{
            {QStringLiteral("markers"), highlightPatternsToJson(m_markerPanel->markers())}});
        s.setValue("rowMarkers", QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
    }
    s.endGroup();

    s.beginGroup("RecentFiles");
    s.setValue("files", m_recentFiles.paths());
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
                m_patternList.append({names[i], values[i], {}, {}});
            // Even older single-string list.
            if (m_patternList.isEmpty()) {
                const QStringList old = s.value("conversionPatternList").toStringList();
                for (const auto& p : old)
                    if (!p.trimmed().isEmpty())
                        m_patternList.append({p.trimmed(), p.trimmed(), {}, {}});
            }
            if (!m_patternList.isEmpty()) {
                SchemaStore::sync(m_patternList);
                s.remove("patternNames");
                s.remove("patternValues");
                s.remove("conversionPatternList");
            }
        }
        // Built-in schemas (Logback, nginx, syslog, …) — after the migration
        // above, which only runs on an empty folder. Never overwrites the
        // user's files; a corrected built-in replaces only an untouched copy,
        // and the active schema follows it.
        const BuiltinSchemas::InstallReport builtins = SchemaStore::installBuiltins();
        if (builtins.changed()) {
            for (const auto& replaced : builtins.replaced) {
                if (m_conversionPattern == replaced.first)
                    m_conversionPattern = replaced.second;
            }
            m_patternList = SchemaStore::loadAll(order);
        }
        if (m_conversionPatternCombo) {
            m_conversionPatternCombo->blockSignals(true);
            m_conversionPatternCombo->clear();
            for (const auto& e : m_patternList)
                addSchemaComboItem(m_conversionPatternCombo, e);
            // No active schema (the default — extraction off) must show as
            // none, not as the first schema that addItem() selects by itself.
            m_conversionPatternCombo->setCurrentIndex(-1);
            // Restore the active selection
            for (int i = 0; i < m_patternList.size(); ++i) {
                if (m_patternList[i].schema == m_conversionPattern) {
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
        // Галочка восстанавливается тихо (setHighlightInMainView блокирует
        // сигналы) — видимость самого дока восстановит restoreState.
        m_filterPanel->setHighlightInMainView(
            s.value("highlightInMainView", true).toBool());
    }
    if (m_searchQuery) {
        // У поиска свои профили. Версии с переключателем Filter/Search в
        // одной панели хранили один набор на оба режима — при первом запуске
        // он достаётся и поиску, чтобы сохранённые запросы не пропали.
        QByteArray profilesJson = s.value("searchProfiles").toString().toUtf8();
        if (profilesJson.isEmpty())
            profilesJson = s.value("textFilterProfiles").toString().toUtf8();
        if (!profilesJson.isEmpty()) {
            const QJsonDocument doc = QJsonDocument::fromJson(profilesJson);
            if (doc.isObject())
                m_searchQuery->profilesFromJson(doc.object());
        }
        m_searchQuery->setHighlightInMainView(
            s.value("searchHighlightInMainView", true).toBool());
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
    m_recentFiles.setPaths(s.value("files").toStringList());
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

    // Поля и промежутки — от CompactStyle, как у остальных панелей.
    auto* rootLayout = new QVBoxLayout(contents);

    // ---- Section: field schema (сначала — КАК разбирать строку) ----
    auto* patternLabel = new QLabel(tr("<b>Field schema:</b>"), contents);
    patternLabel->setToolTip(tr(
        "A schema is an ordered list of blocks.\n"
        "Each block has a name and a match rule: timestamp, level, integer, text until separator,\n"
        "greedy text, custom regex, or remainder of line.\n\n"
        "Use 'Manage...' to build and edit schemas."));
    rootLayout->addWidget(patternLabel);

    auto* schemaRow = new QHBoxLayout();
    m_conversionPatternCombo = new QComboBox(contents);
    m_conversionPatternCombo->setEditable(false);
    m_conversionPatternCombo->setToolTip(patternLabel->toolTip());
    m_conversionPatternCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_conversionPatternCombo->setMinimumContentsLength(8);
    m_conversionPatternCombo->setPlaceholderText(tr("(none)"));
    connect(m_conversionPatternCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onPatternComboChanged);
    schemaRow->addWidget(m_conversionPatternCombo, 1);

    auto* manageBtn = new QPushButton(tr("Manage..."), contents);
    manageBtn->setToolTip(tr("Create, edit and delete named field schemas."));
    connect(manageBtn, &QPushButton::clicked,
            this, &MainWindow::onManagePatterns);
    schemaRow->addWidget(manageBtn);
    rootLayout->addLayout(schemaRow);
    rootLayout->addSpacing(CompactStyle::kSpacing);

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
    filterControlsLayout->setContentsMargins(12, 0, 0, 0);
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

    // Схема на два десятка полей не должна растягивать док за край окна —
    // список полей прокручивается, остальное стоит на месте.
    auto* fieldsScroll = new QScrollArea(contents);
    fieldsScroll->setWidgetResizable(true);
    fieldsScroll->setFrameShape(QFrame::NoFrame);
    auto* fieldsHost = new QWidget(fieldsScroll);
    auto* fieldsHostLayout = new QVBoxLayout(fieldsHost);
    fieldsHostLayout->setContentsMargins(0, 0, 0, 0);
    fieldsHostLayout->addWidget(m_fieldFilterControlsWidget);
    fieldsHostLayout->addStretch(1);
    fieldsScroll->setWidget(fieldsHost);
    rootLayout->addWidget(fieldsScroll, 1);

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
        m_conversionPattern = m_patternList[index].schema;
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
            order << e.name;
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
        addSchemaComboItem(m_conversionPatternCombo, e);
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
            if (m_patternList[i].schema == m_conversionPattern) {
                m_conversionPatternCombo->setCurrentIndex(i);
                break;
            }
        }
    }
}


void MainWindow::cancelFieldExtraction()
{
    if (!m_fieldExtraction)
        return;
    // Workers stop and are waited for; entries keep their old fields intact.
    m_fieldExtraction->cancel();
    m_fieldExtraction->deleteLater();
    m_fieldExtraction = nullptr;
}

void MainWindow::cancelFilterJobsOnAllViews()
{
    for (int t = 0; t < ui->tabWidget->count(); ++t) {
        auto* lv = qobject_cast<LogViewWidget*>(ui->tabWidget->widget(t));
        if (lv && lv->model())
            lv->model()->cancelPendingFilter(true);
    }
}

void MainWindow::applyPatternToAllViews()
{
    // A schema switch supersedes any re-extraction still in flight.
    cancelFieldExtraction();
    // Resident filter workers read entry->fields(): each tab's own and the
    // Search Results one, which shares entries with its source (clear() waits
    // for it). Stop them all before the synchronous field mutation below
    // (invariant 4). finishPatternApplication() re-applies what was cancelled.
    clearSearchResults();
    cancelFilterJobsOnAllViews();
    // A background Save View As reads the fields of resident entries too.
    if (m_exportJob && m_exportJob->isRunning()) {
        m_exportCancelReason = tr("Save View As cancelled: the field schema changed. "
                                  "%1 was not changed.")
                                   .arg(QFileInfo(m_exportJob->destination()).fileName());
        m_exportJob->cancel(/*wait=*/true);
    }

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
    // progress as file loading. Workers do not touch the entries, so filters
    // started meanwhile read the old fields safely. Field display is OFF for
    // the duration: the column checkboxes already describe the new schema,
    // while the entries still carry spans of the old one.
    for (int t = 0; t < ui->tabWidget->count(); ++t) {
        auto* lv = qobject_cast<LogViewWidget*>(ui->tabWidget->widget(t));
        if (lv && lv->model())
            lv->model()->setFieldDisplaySelection(false, QVector<int>());
    }

    m_fieldExtraction = new FieldReextraction(std::move(entries), pattern, this);
    connect(m_fieldExtraction, &FieldReextraction::progressRangeChanged,
            m_progressBar, &QProgressBar::setRange);
    connect(m_fieldExtraction, &FieldReextraction::progressValueChanged,
            m_progressBar, &QProgressBar::setValue);
    connect(m_fieldExtraction, &FieldReextraction::finished,
            this, &MainWindow::onFieldExtractionFinished);

    m_statusLabel->setText(tr("Applying field schema…"));
    m_progressBar->setRange(0, m_fieldExtraction->entryCount());
    m_progressBar->setValue(0);
    m_progressBar->show();
    m_fieldExtraction->start();
}

void MainWindow::onFieldExtractionFinished()
{
    if (!m_fieldExtraction)
        return;
    // Filters started while the fields were being computed (Apply, level
    // buttons, appended lines) read entry->fields(): stop them before the new
    // fields go in; finishPatternApplication() re-applies them.
    cancelFilterJobsOnAllViews();
    m_fieldExtraction->apply();
    m_fieldExtraction->deleteLater();
    m_fieldExtraction = nullptr;

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
    // Поиск сброшен в начале применения схемы — повторяем его с правилами,
    // привязанными к новым полям, если панель результатов на виду (иначе
    // его запустит её появление).
    if (searchResultsLive())
        runSearchIntoResults();
}

LogViewWidget* MainWindow::createLogViewWidget()
{
    auto* view = new LogViewWidget(this);
    view->setParserPattern(m_conversionPattern);
    // Память файла (R-4): вкладка читает и пишет её сама; окну — сообщения
    // и флаг авто-обновления, который могла поставить память.
    view->setFileMemory(m_fileMemory.get());
    connect(view, &LogViewWidget::statusMessage, this,
            [this](const QString& text) { statusBar()->showMessage(text, 10000); });
    connect(view, &LogViewWidget::offeredFiltersChanged, this, [this, view](bool offered) {
        if (!offered && view == m_activeLogView) { // «Restore Filters»: кнопки уровней и поля времени
            updateLogLevelFilterButtons();
            seedTimeFilterEditors();
        }
    });
    connect(view, &LogViewWidget::autoReloadChanged, this, [this, view]() {
        if (view == m_activeLogView)
            syncReloadButton();
        updateAutoReloadTimer();
        updateTabLabel(view);
    });
    // Отметки поиска на скроллбаре (R-6): view сам решает, источник ли он.
    view->view()->scrollbarMarks()->setSearch(m_searchController);
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
    m_lineInfoLabel->setContentsMargins(4, 0, 12, 0);
    ui->statusbar->addWidget(m_lineInfoLabel);

    m_statusLabel = new QLabel(tr("Ready"), this);
    ui->statusbar->addWidget(m_statusLabel, 1);
    // Отчёт о восстановлении сессии: ссылка живёт, пока в отчёте что-то есть.
    m_sessionReportLink = new QLabel(this);
    m_sessionReportLink->setObjectName(QStringLiteral("sessionReportLink"));
    m_sessionReportLink->setTextFormat(Qt::RichText);
    m_sessionReportLink->hide();
    connect(m_sessionReportLink, &QLabel::linkActivated, this, [this]() {
        if (!m_sessionReport)
            m_sessionReport = new SessionReportDialog(m_sessionController, this);
        m_sessionReport->show();
        m_sessionReport->raise();
        m_sessionReport->activateWindow();
    });
    ui->statusbar->addPermanentWidget(m_sessionReportLink);
    updateLineInfoLabel(-1, 0);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    m_progressBar->setFixedWidth(200);
    m_progressBar->hide();
    ui->statusbar->addPermanentWidget(m_progressBar);
    m_encodingIndicator = new EncodingIndicator(this); // логика — во вкладке (F-1)
    ui->statusbar->addPermanentWidget(m_encodingIndicator);

    // Save View As пишет в фоне: прогресс — в строке статуса, рядом отмена.
    m_exportJob = new ViewExportJob(this);
    m_cancelExportButton = new QToolButton(this);
    m_cancelExportButton->setText(tr("Cancel Save"));
    m_cancelExportButton->setToolTip(tr("Stop saving the view; the destination file stays as it was"));
    m_cancelExportButton->hide();
    ui->statusbar->addPermanentWidget(m_cancelExportButton);
    connect(m_cancelExportButton, &QToolButton::clicked, this, [this]() { m_exportJob->cancel(); });
    connect(m_exportJob, &ViewExportJob::progress, this, [this](int percent) {
        m_statusLabel->setText(tr("Saving view to %1... %2%")
                                   .arg(QFileInfo(m_exportJob->destination()).fileName())
                                   .arg(percent));
    });
    connect(m_exportJob, &ViewExportJob::finished, this, &MainWindow::onViewExportFinished);
}

void MainWindow::setupTimeFilterDockContents()
{
    if (!ui->timeFilterContentsWidget)
    {
        qWarning("timeFilterContentsWidget not found in UI. Time filter dock cannot be populated.");
        return;
    }

    // Поля и промежутки — от CompactStyle, как у остальных панелей.
    QVBoxLayout *timeFilterMainLayout = new QVBoxLayout(ui->timeFilterContentsWidget);

    // Using QFormLayout for a nice label-field alignment
    QFormLayout *formLayout = new QFormLayout();
    formLayout->setLabelAlignment(Qt::AlignRight);

    // Секунды и миллисекунды видны и редактируемы: у логов это рабочая
    // точность, а формат локали по умолчанию обрезает время до минут.
    const QString timeFormat = QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz");
    m_timeFilterFrom = new QDateTimeEdit(this);
    m_timeFilterFrom->setCalendarPopup(true);
    m_timeFilterFrom->setDisplayFormat(timeFormat);
    m_timeFilterFrom->setToolTip(tr("Show lines at or after this moment.\n"
                                    "Left at the first line of the log, the range is open at the start."));
    formLayout->addRow(tr("From:"), m_timeFilterFrom);

    m_timeFilterTo = new QDateTimeEdit(this);
    m_timeFilterTo->setCalendarPopup(true);
    m_timeFilterTo->setDisplayFormat(timeFormat);
    m_timeFilterTo->setToolTip(tr("Show lines at or before this moment.\n"
                                  "Left at the last line of the log, the range is open at the end,\n"
                                  "so lines appended to a growing log keep showing up."));
    formLayout->addRow(tr("To:"), m_timeFilterTo);

    timeFilterMainLayout->addLayout(formLayout);

    // Правка полей пользователем отвязывает их от вкладки до Apply/Reset.
    const auto markEdited = [this]() {
        if (m_seedingTimeFilter)
            return;
        m_timeFilterEdited = true;
        m_timeFilterErrorLabel->hide();
    };
    connect(m_timeFilterFrom, &QDateTimeEdit::dateTimeChanged, this, markEdited);
    connect(m_timeFilterTo, &QDateTimeEdit::dateTimeChanged, this, markEdited);

    m_timeFilterErrorLabel = new QLabel(this);
    m_timeFilterErrorLabel->setWordWrap(true);
    m_timeFilterErrorLabel->setStyleSheet(QStringLiteral("color: %1;")
        .arg(AppTheme::instance().logError.darker(115).name()));
    m_timeFilterErrorLabel->hide();
    timeFilterMainLayout->addWidget(m_timeFilterErrorLabel);

    auto* wholeLogButton = new QPushButton(tr("Whole Log"), this);
    wholeLogButton->setToolTip(tr("Put the first and last timestamps of the log into From / To\n"
                                  "(nothing is applied until you press Apply)."));
    connect(wholeLogButton, &QPushButton::clicked, this, [this]() {
        LogModel* model = m_activeLogView ? m_activeLogView->model() : nullptr;
        if (!model)
            return;
        const auto range = model->fullTimeRange();
        if (!range.first.isValid())
            return;
        m_timeFilterFrom->setDateTime(range.first);
        m_timeFilterTo->setDateTime(range.second);
    });

    m_applyTimeFilterButton = new QPushButton(tr("Apply"), this);
    m_applyTimeFilterButton->setToolTip(tr("Show only the lines between From and To in the current tab"));
    connect(m_applyTimeFilterButton, &QPushButton::clicked, this, &MainWindow::onApplyTimeFilterClicked);

    m_resetTimeFilterButton = new QPushButton(tr("Reset"), this);
    m_resetTimeFilterButton->setToolTip(tr("Turn the time filter of the current tab off"));
    connect(m_resetTimeFilterButton, &QPushButton::clicked, this, &MainWindow::onResetTimeFilterClicked);

    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->addWidget(wholeLogButton);
    buttonLayout->addStretch(1);
    buttonLayout->addWidget(m_applyTimeFilterButton);
    buttonLayout->addWidget(m_resetTimeFilterButton);
    timeFilterMainLayout->addLayout(buttonLayout);

    // Диапазон лога и состояние фильтра — чтобы не гадать, что уже применено.
    m_timeFilterInfoLabel = new QLabel(this);
    m_timeFilterInfoLabel->setWordWrap(true);
    m_timeFilterInfoLabel->setTextFormat(Qt::RichText);
    timeFilterMainLayout->addWidget(m_timeFilterInfoLabel);

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

    m_filterPanel = new FilterPanelWidget(FilterPanelWidget::Mode::Filter, container);
    layout->addWidget(m_filterPanel);

    connect(m_filterPanel, &FilterPanelWidget::applyRequested,
            this, &MainWindow::onApplyAllTextFiltersClicked);
    connect(m_filterPanel, &FilterPanelWidget::resetRequested,
            this, &MainWindow::onResetTextFiltersClicked);
    connect(m_filterPanel, &FilterPanelWidget::highlightInMainViewChanged,
            this, [this]() { refreshMainViewHighlights(m_activeLogView); });

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

void MainWindow::setupSearchDock()
{
    m_searchDockWidget = new QDockWidget(tr("Search"), this);
    // objectName обязателен: saveState()/restoreState() узнают док по нему.
    // Имя прежней панели «Search Results»: её сохранённое место и видимость
    // переходят к этой панели.
    m_searchDockWidget->setObjectName(QStringLiteral("searchResultsDockWidget"));

    // Запрос и результаты в одной панели; раскладка сама подстраивается
    // под широкий (снизу) или узкий (сбоку) док.
    m_searchPanel = new SearchPanelWidget(m_searchDockWidget);
    m_searchQuery = m_searchPanel->query();
    m_searchResultsStatusLabel = m_searchPanel->statusLabel();

    connect(m_searchQuery, &FilterPanelWidget::applyRequested,
            this, &MainWindow::onSearchRequested);
    connect(m_searchQuery, &FilterPanelWidget::resetRequested,
            this, &MainWindow::onSearchCleared);
    connect(m_searchQuery, &FilterPanelWidget::highlightInMainViewChanged,
            this, [this]() { refreshMainViewHighlights(m_activeLogView); });

    // Вторая LogListView поверх отдельной LogModel: та же отрисовка/подсветка,
    // что и в основном view, но со своим (отфильтрованным) набором записей.
    m_searchController = new SearchResultsController(this);
    LogModel* resultsModel = m_searchController->model();
    // Выбор строки в результатах (мышь или клавиатура) → прыжок в основном
    // view; перестройка выдачи (reset, Suppress) — не выбор пользователя.
    m_searchResultsView = m_searchPanel->resultsView();
    m_searchNavigator = new SearchResultsNavigator(resultsModel, m_searchResultsView, this);
    m_searchNavigator->setTarget([this]() { return m_activeLogView; });

    // Тот же шрифт, что и у основных view, но на пункт мельче (searchResultsFont).
    m_searchResultsView->setFont(searchResultsFont());
    // Контекстное меню и Ctrl+колесо работают и здесь — над активной вкладкой.
    connect(m_searchResultsView, &LogListView::textActionRequested,
            this, &MainWindow::onViewTextAction);
    connect(m_searchResultsView, &LogListView::fontZoomRequested,
            this, &MainWindow::changeFontSize);
    connect(m_searchResultsView, &LogListView::timeFilterBoundRequested,
            this, &MainWindow::onTimeFilterBoundRequested);

    m_searchDockWidget->setWidget(m_searchPanel);
    addDockWidget(Qt::BottomDockWidgetArea, m_searchDockWidget);
    // Обычная панель: видимостью управляет пользователь (меню View, Find All).
    // Открыли панель с запущенным ранее поиском — сразу подтянуть актуальные
    // результаты: отставшую выдачу обновит сам контроллер, пустую — новый поиск.
    connect(m_searchDockWidget, &QDockWidget::visibilityChanged,
            this, [this](bool) {
        m_searchController->setLive(searchResultsLive());
        if (!m_showingSearchDock && searchResultsLive() && !m_searchController->isActive())
            runSearchIntoResults();
    });

    // Подпись: «ищем… N%», число совпадений или почему поиска нет (и как его
    // начать).
    connect(m_searchController, &SearchResultsController::statusChanged,
            this, [this]() { updateSearchResultsStatus(); });
    m_searchController->setLive(searchResultsLive());
    updateSearchResultsStatus();

    if (ui->menuView) {
        QAction* toggle = m_searchDockWidget->toggleViewAction();
        toggle->setText(tr("Search Panel"));
        // Сразу под Text Filters: две панели работы с текстом рядом.
        ui->menuView->insertAction(ui->actionToggle_Directory_Scanner_Panel, toggle);
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

void MainWindow::setupBookmarksDock()
{
    m_bookmarkDockWidget = new QDockWidget(tr("Bookmarks"), this);
    // objectName обязателен: saveState()/restoreState() узнают док по нему.
    m_bookmarkDockWidget->setObjectName(QStringLiteral("bookmarksDockWidget"));

    m_bookmarkPanel = new BookmarkPanelWidget(m_bookmarkDockWidget);
    m_bookmarkDockWidget->setWidget(m_bookmarkPanel);
    addDockWidget(Qt::RightDockWidgetArea, m_bookmarkDockWidget);
    // Панель по требованию: по умолчанию скрыта (View → Bookmarks Panel, Ctrl+F11).
    m_bookmarkDockWidget->hide();
    connect(m_bookmarkPanel, &BookmarkPanelWidget::statusMessage,
            this, [this](const QString& text) { m_statusLabel->setText(text); });

    if (ui->menuView) {
        QAction* toggle = m_bookmarkDockWidget->toggleViewAction();
        toggle->setText(tr("Bookmarks Panel"));
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
        m_shortcutActions.insert(QStringLiteral("panelBookmarks"), toggle);
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
                m_activeLogView->goToEntry(logicalEntryId, file.get(), NavKind::Statistics);
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
    // Новое имя (было filterStatusToolBar): сохранённая раскладка прежних
    // версий сжимала этот тулбар до одной кнопки и «»», восстанавливать её
    // незачем — тулбар встаёт на место по умолчанию, доки не трогаются.
    tb->setObjectName(QStringLiteral("filtersToolBar"));

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

    // Time (пер-вкладочный; граница может быть открытой)
    {
        const QDateTime from = model ? model->startTimeFilter() : QDateTime();
        const QDateTime to = model ? model->endTimeFilter() : QDateTime();
        const bool active = from.isValid() || to.isValid();
        const QString fmt = QStringLiteral("yyyy-MM-dd HH:mm:ss");
        QString range;
        if (from.isValid() && to.isValid())
            range = tr("%1 – %2").arg(from.toString(fmt), to.toString(fmt));
        else if (from.isValid())
            range = tr("from %1").arg(from.toString(fmt));
        else if (to.isValid())
            range = tr("until %1").arg(to.toString(fmt));
        m_timeFilterStatusAction->setEnabled(model != nullptr);
        m_timeFilterStatusAction->setChecked(active);
        m_timeFilterStatusAction->setToolTip(active
            ? tr("Time filter: %1\nClick to reset").arg(range)
            : tr("Time filter is off\nClick to apply the range from the Time Filter panel"));
        updateTimeFilterInfo();
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
    connect(logView, &LogViewWidget::quickSearchProgress, this, &MainWindow::onQuickSearchProgress);
    connect(logView, &LogViewWidget::quickSearchFinished, this, &MainWindow::onQuickSearchFinished);
    connect(logView, &LogViewWidget::levelJumpProgress, this, &MainWindow::onLevelJumpProgress);
    connect(logView, &LogViewWidget::levelJumpFinished, this, &MainWindow::onLevelJumpFinished);
    connect(logView, &LogViewWidget::bookmarkJumpFinished, this, &MainWindow::onBookmarkJumpFinished);
    connect(logView, &LogViewWidget::bookmarkNoteEditRequested, this, &MainWindow::editBookmarkNote);
    connect(logView, &LogViewWidget::navHistoryChanged, this, &MainWindow::updateNavHistoryActions);
    updateNavHistoryActions();
    if (m_bookmarkPanel)
        m_bookmarkPanel->setLogView(logView);

    if (logView->view()) {
        connect(logView->view(), &LogListView::timeFilterBoundRequested,
                this, &MainWindow::onTimeFilterBoundRequested);
        connect(logView->view(), &LogListView::textActionRequested,
                this, &MainWindow::onViewTextAction);
        connect(logView->view(), &LogListView::fontZoomRequested,
                this, &MainWindow::changeFontSize);

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

    // Панель статистики — тоже (сама пересоберётся, если видима). Состояние
    // загрузки ставим ДО модели: иначе setModel успел бы запустить сбор по
    // ещё не догруженному документу.
    if (m_statsPanel) {
        m_statsPanel->setLoading(logView->isLoading());
        m_statsPanel->setModel(logView->model());
        m_statsLoadingConn = connect(logView, &LogViewWidget::loadingChanged,
                                     this, [this](bool loading) {
            if (m_statsPanel)
                m_statsPanel->setLoading(loading);
        });
    }

    // Панель результатов следит за моделью активной вкладки: дописанные строки
    // проверяет инкрементально, перестройку — полным поиском заново.
    if (m_searchController) {
        SearchResultsNavigator::Suppress quiet(*m_searchNavigator);
        m_searchController->setSource(logView->model());
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

    // Смена вкладки при запущенном поиске: результаты старой вкладки указывали
    // бы на чужие записи — пересобираем под новую активную вкладку (если
    // панель видна; иначе — при её появлении).
    if (m_searchRequested && searchResultsLive())
        runSearchIntoResults();
    refreshMainViewHighlights(logView);
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
    disconnect(logView, &LogViewWidget::quickSearchProgress, this, &MainWindow::onQuickSearchProgress);
    disconnect(logView, &LogViewWidget::quickSearchFinished, this, &MainWindow::onQuickSearchFinished);
    disconnect(logView, &LogViewWidget::levelJumpProgress, this, &MainWindow::onLevelJumpProgress);
    disconnect(logView, &LogViewWidget::levelJumpFinished, this, &MainWindow::onLevelJumpFinished);
    disconnect(logView, &LogViewWidget::bookmarkJumpFinished, this, &MainWindow::onBookmarkJumpFinished);
    disconnect(logView, &LogViewWidget::bookmarkNoteEditRequested, this, &MainWindow::editBookmarkNote);
    disconnect(logView, &LogViewWidget::navHistoryChanged, this, &MainWindow::updateNavHistoryActions);
    // Поиск ушедшей вкладки больше некому показывать.
    logView->cancelQuickSearch();
    logView->cancelLevelJump();
    const bool ownLevelJumpStatus = m_levelJumpStatusShown
        && m_statusLabel->text() == m_levelJumpStatusText;
    m_levelJumpStatusShown = false;
    if (m_quickSearchStatusShown || ownLevelJumpStatus) {
        m_quickSearchStatusShown = false;
        m_statusLabel->setText(tr("Ready"));
    }

    if (logView->view()) {
        disconnect(logView->view(), &LogListView::timeFilterBoundRequested,
                   this, &MainWindow::onTimeFilterBoundRequested);
        disconnect(logView->view(), &LogListView::textActionRequested,
                   this, &MainWindow::onViewTextAction);
        disconnect(logView->view(), &LogListView::fontZoomRequested,
                   this, &MainWindow::changeFontSize);
    }

    disconnect(m_followTailConn);

    if (m_timelinePanel)
        m_timelinePanel->setModel(nullptr);

    disconnect(m_statsLoadingConn);
    if (m_statsPanel) {
        m_statsPanel->setModel(nullptr);
        m_statsPanel->setLoading(false); // состояние уходящей вкладки больше не наше
    }

    // Выдача уходящей вкладки указывала бы на чужие записи.
    if (m_searchController) {
        SearchResultsNavigator::Suppress quiet(*m_searchNavigator);
        m_searchController->setSource(nullptr);
    }

    if (m_activeLogView == logView)
    {
        m_activeLogView = nullptr;
    }
    // Подсветка поиска принадлежала активной вкладке — у ушедшей остаётся
    // только подсветка её собственного фильтра.
    refreshMainViewHighlights(logView);
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
        updateNavHistoryActions();
    }
    m_encodingIndicator->setTab(currentView);
    // Переключение на вкладку — тоже «фокус на view»: с авто-обновлением она
    // должна показывать актуальные данные сразу, а не со следующего тика.
    if (currentView && currentView->autoReload())
        currentView->reloadChangedFiles();
    updateStatusBarDefaultText();
    updateLogLevelFilterButtons();
    updateFilterInputsFromModel();
    updateFilterStatusButtons();
}

// =============================================================================
// Управление вкладками: закрытие, объединение, контекстное меню
// =============================================================================

void MainWindow::closeTab(int index, bool remember)
{
    QWidget* page = ui->tabWidget->widget(index);
    if (!page)
        return;
    if (auto* logView = qobject_cast<LogViewWidget*>(page)) {
        if (remember) // слитая в другую вкладка не закрыта — её файлы живы там
            m_sessionController->rememberClosedTab(logView, index); // Ctrl+Shift+T (R-5)
        logView->saveFileMemory(); // память файла (R-4)
        disconnectFromLogView(logView);
    }
    ui->tabWidget->removeTab(index);
    delete page;
    // Закрытая вкладка могла держать авто-перезагрузку — обновить таймер.
    updateAutoReloadTimer();
    if (ui->tabWidget->count() == 0)
        updateStatusBarDefaultText();
    syncReloadButton();
}

void MainWindow::closeOtherTabs(int keepIndex)
{
    QWidget* keep = ui->tabWidget->widget(keepIndex);
    if (!keep)
        return;
    // Идём справа налево: closeTab сдвигает индексы правее удаляемого.
    for (int i = ui->tabWidget->count() - 1; i >= 0; --i)
        if (ui->tabWidget->widget(i) != keep)
            closeTab(i);
}

void MainWindow::closeTabsToRight(int index)
{
    for (int i = ui->tabWidget->count() - 1; i > index; --i)
        closeTab(i);
}

void MainWindow::closeAllTabs()
{
    for (int i = ui->tabWidget->count() - 1; i >= 0; --i)
        closeTab(i);
}

void MainWindow::updateTabLabel(LogViewWidget* view)
{
    if (!view)
        return;
    const int idx = ui->tabWidget->indexOf(view);
    if (idx < 0)
        return;

    const auto files = view->loadedFiles();
    const int n = files.size();
    QString text;
    QString tip;
    const bool isStdinSpool = m_stdinSpooler && n == 1 && files.first()
        && LogViewWidget::samePath(files.first()->filePath,
                                   LogViewWidget::normalizedPath(m_stdinSpooler->spoolFilePath()));
    if (isStdinSpool) {
        // Файл спула — техническая деталь: вкладка остаётся «stdin».
        text = tr("stdin");
        tip  = tr("Standard input (spooled to %1)")
                   .arg(QDir::toNativeSeparators(files.first()->filePath));
    } else if (n == 1 && files.first()) {
        text = files.first()->shortName();
        tip  = QDir::toNativeSeparators(files.first()->filePath);
    } else if (n > 1) {
        // Имя первого файла узнаваемее безликого «Logs (3)».
        text = tr("%1 +%2").arg(files.first() ? files.first()->shortName() : tr("Logs"))
                           .arg(n - 1);
        QStringList paths;
        for (const auto& lf : files)
            if (lf)
                paths << QDir::toNativeSeparators(lf->filePath);
        tip = tr("%n file(s), merged by time:", nullptr, n)
              + QLatin1Char('\n') + paths.join(QLatin1Char('\n'));
    } else {
        text = tr("Logs");
    }
    if (view->autoReload())
        tip += QStringLiteral("\n\n") + tr("Auto-reload is on for this tab.");
    ui->tabWidget->setTabText(idx, text);
    ui->tabWidget->setTabToolTip(idx, tip);
    // Вкладки с авто-обновлением отмечены значком — видно, какие логи «живые».
    ui->tabWidget->setTabIcon(idx, view->autoReload()
        ? tintedIcon(QStringLiteral(":/icons/autoreload.svg"), AppTheme::instance().logInfo)
        : QIcon());
}

void MainWindow::mergeTabs(int fromIndex, int toIndex)
{
    if (fromIndex == toIndex)
        return;
    auto* src = qobject_cast<LogViewWidget*>(ui->tabWidget->widget(fromIndex));
    auto* dst = qobject_cast<LogViewWidget*>(ui->tabWidget->widget(toIndex));
    if (!src || !dst)
        return;

    // Файлы — в приёмник штатным addLogFile (бэкенд, дубли), с их кодировкой.
    dst->addFilesFrom(*src);

    updateTabLabel(dst);

    // addLogFile вкладок не двигает — индекс источника ещё валиден.
    closeTab(fromIndex, /*remember=*/false);

    // Индекс приёмника мог сдвинуться после закрытия источника.
    const int dstIdx = ui->tabWidget->indexOf(dst);
    if (dstIdx >= 0)
        ui->tabWidget->setCurrentIndex(dstIdx);
}

void MainWindow::mergeAllTabsInto(int keepIndex)
{
    auto* dst = qobject_cast<LogViewWidget*>(ui->tabWidget->widget(keepIndex));
    if (!dst)
        return;

    for (int i = 0; i < ui->tabWidget->count(); ++i)
        if (auto* v = qobject_cast<LogViewWidget*>(ui->tabWidget->widget(i)); v && i != keepIndex)
            dst->addFilesFrom(*v);

    updateTabLabel(dst);

    for (int i = ui->tabWidget->count() - 1; i >= 0; --i)
        if (ui->tabWidget->widget(i) != dst)
            closeTab(i, /*remember=*/false);

    const int dstIdx = ui->tabWidget->indexOf(dst);
    if (dstIdx >= 0)
        ui->tabWidget->setCurrentIndex(dstIdx);
}

void MainWindow::revealTabFile(int index)
{
    auto* view = qobject_cast<LogViewWidget*>(ui->tabWidget->widget(index));
    if (!view)
        return;
    const auto files = view->loadedFiles();
    if (files.isEmpty() || !files.first())
        return;
    const QString path = files.first()->filePath;

#ifdef Q_OS_WIN
    // Открыть Проводник с выделенным файлом.
    QProcess::startDetached(QStringLiteral("explorer.exe"),
        { QStringLiteral("/select,") + QDir::toNativeSeparators(path) });
#else
    // Прочие ОС — открыть содержащую папку.
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}

void MainWindow::copyTabPath(int index)
{
    auto* view = qobject_cast<LogViewWidget*>(ui->tabWidget->widget(index));
    if (!view)
        return;
    QStringList paths;
    for (const auto& lf : view->loadedFiles())
        if (lf)
            paths << QDir::toNativeSeparators(lf->filePath);
    if (paths.isEmpty())
        return;
    QApplication::clipboard()->setText(paths.join(QLatin1Char('\n')));
}

void MainWindow::onTabContextMenu(int index, const QPoint& globalPos)
{
    if (index < 0 || index >= ui->tabWidget->count())
        return;

    const int total = ui->tabWidget->count();
    auto* view = qobject_cast<LogViewWidget*>(ui->tabWidget->widget(index));
    const bool hasFile = view && !view->loadedFiles().isEmpty();

    QMenu menu(this);
    QAction* actClose       = menu.addAction(tr("Close"));
    QAction* actCloseOthers = menu.addAction(tr("Close Others"));
    QAction* actCloseRight  = menu.addAction(tr("Close Tabs to the Right"));
    QAction* actCloseAll    = menu.addAction(tr("Close All"));
    menu.addSeparator();
    QAction* actReveal      = menu.addAction(tr("Open Containing Folder"));
    QAction* actCopyPath    = menu.addAction(tr("Copy Full Path"));
    QAction* actMergeAll    = nullptr;
    if (total > 1) {
        menu.addSeparator();
        actMergeAll = menu.addAction(tr("Merge All Tabs Into This One"));
    }

    actCloseOthers->setEnabled(total > 1);
    actCloseRight->setEnabled(index < total - 1);
    actReveal->setEnabled(hasFile);
    actCopyPath->setEnabled(hasFile);

    QAction* chosen = menu.exec(globalPos);
    if (!chosen)
        return;
    if (chosen == actClose)                          closeTab(index);
    else if (chosen == actCloseOthers)               closeOtherTabs(index);
    else if (chosen == actCloseRight)                closeTabsToRight(index);
    else if (chosen == actCloseAll)                  closeAllTabs();
    else if (chosen == actReveal)                    revealTabFile(index);
    else if (chosen == actCopyPath)                  copyTabPath(index);
    else if (actMergeAll && chosen == actMergeAll)   mergeAllTabsInto(index);
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

    // Файл сессии, выбранный здесь, открывается как сессия (R-5).
    for (const QString& f : files)
        if (SessionFile::isSessionFile(f))
            openSessionFile(f);
    files.removeIf([](const QString& f) { return SessionFile::isSessionFile(f); });
    if (files.isEmpty())
        return;

    for (const QString& f : files)
        addToRecentFiles(f);

    LogViewWidget* view = tabForOpening();
    for (const QString &f : files)
        view->addLogFile(f);

    updateTabLabel(view);
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
    startViewExport(fileName);
}

void MainWindow::startViewExport(const QString& fileName)
{
    if (!m_activeLogView || !m_activeLogView->model())
        return;

    // Protect sources in every tab, including aliases of their filesystem paths.
    QStringList sourcePaths;
    for (int t = 0; t < ui->tabWidget->count(); ++t) {
        auto* lvw = qobject_cast<LogViewWidget*>(ui->tabWidget->widget(t));
        if (!lvw)
            continue;
        for (const auto& lf : lvw->loadedFiles()) {
            if (lf)
                sourcePaths.append(lf->filePath);
        }
    }

    // Save exactly what the view currently shows: the model's visible rows
    // (already filtered + merged across all documents in this tab) with the
    // display text, so the active Log Fields selection is honoured too. The
    // rows are snapshotted now and written in the background.
    if (!m_exportJob->start(*m_activeLogView->model(), fileName, sourcePaths)) {
        QMessageBox::information(this, tr("Save View As"),
            tr("The view is still being saved. Wait for it to finish or cancel it."));
        return;
    }
    ui->actionSaveAs->setEnabled(false);
    m_cancelExportButton->show();
    m_statusLabel->setText(tr("Saving view to %1...").arg(QFileInfo(fileName).fileName()));
}

void MainWindow::onViewExportFinished(const ViewExport::Result& result)
{
    ui->actionSaveAs->setEnabled(true);
    m_cancelExportButton->hide();
    const QString fileName = m_exportJob->destination();
    if (result.error == ViewExport::Error::Cancelled) {
        m_statusLabel->setText(m_exportCancelReason.isEmpty()
                                   ? tr("Save View As cancelled; %1 was not changed.")
                                         .arg(QFileInfo(fileName).fileName())
                                   : m_exportCancelReason);
        m_exportCancelReason.clear();
        return;
    }
    if (!result.ok()) {
        updateStatusBarDefaultText();
        if (result.error == ViewExport::Error::SourceFile) {
            QMessageBox::warning(this, tr("Save View As"),
                tr("This file is currently open. Please choose a different, new file."));
        } else {
            QMessageBox::warning(this, tr("Save View As"),
                tr("Could not save '%1'.\n%2").arg(fileName, result.detail));
        }
        return;
    }
    const int rows = m_exportJob->rowCount();

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

    // Update tab text to reflect the loaded file (updateTabLabel сам оставит
    // вкладке спула stdin её имя — файл там технический).
    for (int i = 0; i < ui->tabWidget->count(); ++i) {
        auto *view = qobject_cast<LogViewWidget*>(ui->tabWidget->widget(i));
        if (view && view->loadedFiles().contains(logFile)) {
            updateTabLabel(view);
            // Поля фильтра по времени, которых пользователь не касался,
            // следуют за диапазоном только что прочитанного лога.
            if (view == m_activeLogView && !m_timeFilterEdited)
                seedTimeFilterEditors();
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
    const auto* view = qobject_cast<LogViewWidget*>(sender()); // причина — у вкладки (UTF-16 и др.)
    m_statusLabel->setText(view ? view->loadFailureMessage(logFile) : tr("Failed to parse: %1").arg(logFile->shortName()));
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
    // «Line 12 of 5 000 (filtered from 116 384)»: позиция, видимые строки и,
    // если фильтры что-то скрыли, сколько строк в документе всего.
    LogModel* model = m_activeLogView ? m_activeLogView->model() : nullptr;
    const QLocale locale;
    const qint64 allLines = model ? model->totalLineCount() : 0;
    QString text;
    if (totalRows > 0 && currentRow >= 0)
        text = tr("Line %1 of %2").arg(locale.toString(currentRow + 1), locale.toString(totalRows));
    else if (totalRows > 0)
        text = tr("%1 lines").arg(locale.toString(totalRows));
    else if (allLines > 0)
        text = tr("No lines match the filters");
    else
        text = QStringLiteral("—");
    if (allLines > totalRows)
        text += QLatin1Char(' ') + tr("(filtered from %1)").arg(locale.toString(allLines));
    m_lineInfoLabel->setText(text);

    // Текущая запись в отфильтрованном списке активной вкладки (если есть).
    std::shared_ptr<LogEntry> currentEntry;
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
    for (LogViewWidget* v : logViews())
        v->view()->setWordWrap(checked);
}

void MainWindow::updateLogLevelFilterButtons()
{
    LogViewWidget *currentView = qobject_cast<LogViewWidget *>(ui->tabWidget->currentWidget());
    LogModel *model = nullptr;
    if (currentView)
    {
        model = currentView->model();
    }

    // Тёмная палитра — по фону (Base), а не по тексту.
    const QColor base = palette().color(QPalette::Base);
    const bool isDarkTheme = base.lightness() < 128;

    auto updateButtonState = [&](QAction *action, LogLevel level)
    {
        const bool active = model ? model->logLevelFilter().contains(level)
                                  : action->isChecked();
        // Синхронизация состояния, а не выбор пользователя: без сигналов, иначе
        // toggled снова дёрнул бы setLogLevelFilter и перефильтровал вкладку
        // (при смене вкладки — по разу на каждую включённую кнопку).
        {
            const QSignalBlocker blocker(action);
            action->setChecked(active);
        }

        QWidget *button = ui->levelToolBar->widgetForAction(action);
        if (!button)
            return;
        // Неактивная — плоская, с рамкой при наведении; активная — «чип»:
        // фон с оттенком уровня (светлая тема — пастельный dim-цвет уровня,
        // тёмная — цвет уровня, смешанный с фоном) и рамка цвета уровня.
        const QColor levelColor = AppTheme::instance().forLevel(level);
        const QString padding = QStringLiteral("padding: 0px 3px; border-radius: 3px;");
        QString styleSheet;
        if (active) {
            const QColor bg = isDarkTheme
                ? CardFrame::mixedColor(levelColor, base, 0.6)
                : AppTheme::instance().dimForLevel(level);
            styleSheet = QStringLiteral(
                "QToolButton { background-color: %1; color: %2; border: 1px solid %3; %4 }")
                    .arg(bg.name(), palette().color(QPalette::Text).name(),
                         levelColor.darker(isDarkTheme ? 100 : 125).name(), padding);
        } else {
            styleSheet = QStringLiteral(
                "QToolButton { background-color: transparent; border: 1px solid transparent; %1 }"
                "QToolButton:hover { border-color: %2; }")
                    .arg(padding, CardFrame::mutedBorderColor(palette()).name());
        }
        button->setStyleSheet(styleSheet);
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
    if (!m_activeLogView || !m_activeLogView->model())
        return;
    LogModel* model = m_activeLogView->model();

    QDateTime fromDateTime = m_timeFilterFrom->dateTime();
    QDateTime toDateTime = m_timeFilterTo->dateTime();
    if (!fromDateTime.isValid() || !toDateTime.isValid() || fromDateTime > toDateTime) {
        // Раньше неверный диапазон молча снимал фильтр — теперь объясняем,
        // а применённый фильтр вкладки не трогаем.
        m_timeFilterErrorLabel->setText(tr("“From” is later than “To” "
                                           "— nothing was applied."));
        m_timeFilterErrorLabel->show();
        return;
    }
    m_timeFilterErrorLabel->hide();

    // Граница на краю данных (или за ним) — не граница: такой край остаётся
    // открытым, и дописанные в растущий лог строки продолжают показываться.
    const auto fullRange = model->fullTimeRange();
    if (fullRange.first.isValid() && fromDateTime <= fullRange.first)
        fromDateTime = QDateTime();
    if (fullRange.second.isValid() && toDateTime >= fullRange.second)
        toDateTime = QDateTime();
    model->setTimeRangeFilter(fromDateTime, toDateTime);
    seedTimeFilterEditors();
}

void MainWindow::onResetTimeFilterClicked()
{
    if (m_activeLogView && m_activeLogView->model())
    {
        m_activeLogView->model()->setTimeRangeFilter(QDateTime(), QDateTime()); // Clear filter in model
    }
    // Поля возвращаются к диапазону лога — готовы к следующему Apply.
    seedTimeFilterEditors();
}

void MainWindow::seedTimeFilterEditors()
{
    if (!m_timeFilterFrom || !m_timeFilterTo)
        return;
    LogModel* model = m_activeLogView ? m_activeLogView->model() : nullptr;
    QDateTime from, to;
    if (model) {
        from = model->startTimeFilter();
        to = model->endTimeFilter();
        if (!from.isValid() || !to.isValid()) {
            const auto range = model->fullTimeRange();
            if (!from.isValid())
                from = range.first;
            if (!to.isValid())
                to = range.second;
        }
    }
    // Документа (или меток в нём) нет — прежний нейтральный диапазон.
    if (!from.isValid())
        from = QDateTime::currentDateTime().addDays(-1).date().startOfDay();
    if (!to.isValid())
        to = QDateTime::currentDateTime().date().endOfDay();

    m_seedingTimeFilter = true;
    m_timeFilterFrom->setDateTime(from);
    m_timeFilterTo->setDateTime(to);
    m_seedingTimeFilter = false;
    m_timeFilterEdited = false;
    if (m_timeFilterErrorLabel)
        m_timeFilterErrorLabel->hide();
    updateTimeFilterInfo();
}

void MainWindow::updateTimeFilterInfo()
{
    if (!m_timeFilterInfoLabel)
        return;
    LogModel* model = m_activeLogView ? m_activeLogView->model() : nullptr;
    const bool haveModel = model != nullptr;
    m_applyTimeFilterButton->setEnabled(haveModel);
    m_resetTimeFilterButton->setEnabled(haveModel);
    if (!haveModel) {
        m_timeFilterInfoLabel->setText(QString());
        return;
    }

    const QString fmt = QStringLiteral("yyyy-MM-dd HH:mm:ss");
    const QString muted = CardFrame::mutedTextColor(palette()).name();
    const auto range = model->fullTimeRange();
    const QString span = range.first.isValid()
        ? tr("The log spans %1 – %2.").arg(range.first.toString(fmt), range.second.toString(fmt))
        : tr("The log has no timestamps yet.");
    const QDateTime from = model->startTimeFilter();
    const QDateTime to = model->endTimeFilter();
    QString state;
    if (from.isValid() && to.isValid())
        state = tr("Filter on: %1 – %2.").arg(from.toString(fmt), to.toString(fmt));
    else if (from.isValid())
        state = tr("Filter on: from %1.").arg(from.toString(fmt));
    else if (to.isValid())
        state = tr("Filter on: until %1.").arg(to.toString(fmt));
    else
        state = tr("Filter off.");
    m_timeFilterInfoLabel->setText(QStringLiteral("<span style=\"color:%1;\">%2</span><br><b>%3</b>")
        .arg(muted, span.toHtmlEscaped(), state.toHtmlEscaped()));
}

void MainWindow::onTimeFilterBoundRequested(const QDateTime& dt, bool isStart)
{
    if (!dt.isValid())
        return;

    // Подставляем выбранный таймстамп в нужное поле и показываем панель фильтра
    // по времени, но НЕ применяем фильтр автоматически — пользователь сам решает,
    // когда нажать Apply (вторая граница по умолчанию — край лога, т.е. открыта).
    if (isStart)
        m_timeFilterFrom->setDateTime(dt);
    else
        m_timeFilterTo->setDateTime(dt);
    m_timeFilterEdited = true;

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

    // Общий путь переходов: строка — выбор пользователя, follow-tail снимается.
    m_activeLogView->view()->goToRow(row, NavKind::Timeline);
}

void MainWindow::onTimelineRangeSelected(const QDateTime& from, const QDateTime& to)
{
    if (!from.isValid() || !to.isValid() || from >= to
        || !m_activeLogView || !m_activeLogView->model())
        return;

    // Интервал покрывает весь файл (zoom-out «до упора») — честнее снять
    // фильтр по времени совсем, чем держать фильтр шире данных.
    const auto fullRange = m_activeLogView->model()->fullTimeRange();
    if (fullRange.first.isValid()
        && from <= fullRange.first && to >= fullRange.second)
    {
        m_activeLogView->model()->setTimeRangeFilter(QDateTime(), QDateTime());
    } else {
        // Фильтр применяется исходными границами, не значениями QDateTimeEdit:
        // редактор обрезает время до отображаемых секций, и записи на краях
        // интервала выпадали бы из выборки.
        m_activeLogView->model()->setTimeRangeFilter(from, to);
    }
    // Поля дока Time Filter показывают применённый интервал (сам док не
    // показываем и не поднимаем).
    seedTimeFilterEditors();
}

void MainWindow::onApplyAllTextFiltersClicked()
{
    applyTextFiltersToActiveView();
}

void MainWindow::onResetTextFiltersClicked()
{
    if (!m_activeLogView)
        return;

    // Снять фильтр (и его подсветку) с активного документа; правила в панели
    // остаются и могут быть применены заново. Поиск не трогаем — у него своя
    // панель и своя кнопка Clear.
    if (m_activeLogView->model())
        m_activeLogView->model()->setFilterRules(FilterRuleSet{});
    refreshMainViewHighlights(m_activeLogView);
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

    // Фильтрация пер-вкладочная: Apply действует только на текущий документ.
    // Остальные документы сохраняют свои (или никакие) фильтры.
    FilterRuleSet rules = m_filterPanel->ruleSet();
    const bool fieldScope = m_fieldFilterEnabledCheckBox && m_fieldFilterEnabledCheckBox->isChecked();
    rules.bindFields(LogPattern(m_conversionPattern).fieldNames(), fieldScope);

    if (m_activeLogView->model())
        m_activeLogView->model()->setFilterRules(rules);
    // Подсветка совпадений в основном view — по галочке Highlight.
    refreshMainViewHighlights(m_activeLogView);
}

void MainWindow::onSearchRequested()
{
    m_searchRequested = true;
    // Явный запуск поиска показывает панель, если её скрыли. Появление
    // панели само запустило бы поиск — здесь он идёт один раз, ниже.
    if (m_searchDockWidget && !m_searchDockWidget->isVisible()) {
        m_showingSearchDock = true;
        m_searchDockWidget->show();
        m_searchDockWidget->raise();
        m_showingSearchDock = false;
    }
    runSearchIntoResults();
}

void MainWindow::onSearchCleared()
{
    m_searchRequested = false;
    clearSearchResults();
}

void MainWindow::showGoToDialog()
{
    LogViewWidget* tab = m_activeLogView;
    if (!tab || !tab->model() || !tab->view())
        return;

    // Опорная строка — выбранная пользователем: текущую без выделения мог
    // назначить Qt при возврате фокуса (инвариант 10).
    const QModelIndex current = tab->view()->currentIndex();
    const int currentRow = tab->view()->userSelectedRow();

    GoToDialog dialog(tab->model(), tab->loadedFiles(), currentRow, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    // Вкладку могли закрыть, пока диалог был открыт (второй экземпляр, таймер).
    if (tab != m_activeLogView || dialog.target().row < 0)
        return;
    tab->view()->goToRow(dialog.target().row, NavKind::GoTo);
    tab->view()->setFocus(Qt::OtherFocusReason);
    // Точное попадание пояснений не требует, но прошлое пояснение убираем.
    m_statusLabel->setText(dialog.targetNote().isEmpty() ? tr("Ready") : dialog.targetNote());
}

void MainWindow::findAllInSearchPanel(const TextQuery& query)
{
    if (!m_searchQuery || !m_searchDockWidget)
        return;
    if (query.isEmpty()) {
        m_searchDockWidget->show();
        m_searchDockWidget->raise();
        m_searchQuery->focusFirstRule();
        return;
    }
    // Сначала новый запрос, потом показ панели и поиск — иначе появление
    // панели успело бы запустить прежний запрос.
    m_searchQuery->setSingleRule(query);
    onSearchRequested();
    m_searchDockWidget->raise();
}

void MainWindow::refreshMainViewHighlights(LogViewWidget* view)
{
    if (!view || !view->view())
        return;
    QVector<HighlightPattern> patterns;
    // Фильтр вкладки — по правилам, реально применённым к ней.
    if (m_filterPanel && m_filterPanel->highlightInMainView() && view->model())
        patterns += view->model()->filterRules().highlightPatterns();
    // Поиск идёт только по активной вкладке.
    if (view == m_activeLogView && m_searchQuery && m_searchQuery->highlightInMainView()
        && m_searchController && m_searchController->isActive())
        patterns += m_searchRules.highlightPatterns();
    view->view()->setTextHighlightPatterns(patterns);
}

void MainWindow::runSearchIntoResults()
{
    // Fields are being recomputed for the new schema: rules bound to it would
    // not match the entries' old spans. finishPatternApplication() searches.
    if (m_fieldExtraction)
        return;
    if (!m_searchRequested || !m_searchQuery || !m_searchController || !m_activeLogView
        || !m_activeLogView->model())
        return;

    // Поиск ведём над текущим ВИДИМЫМ набором активной вкладки (после Time/Level/
    // Fields-фильтров) — тогда любой результат гарантированно виден в main и клик
    // всегда попадает точно на строку. Пустой или испорченный запрос контроллер
    // превращает в пустую выдачу с объясняющей подписью.
    FilterRuleSet rules = m_searchQuery->ruleSet();
    const bool fieldScope = m_fieldFilterEnabledCheckBox && m_fieldFilterEnabledCheckBox->isChecked();
    rules.bindFields(LogPattern(m_conversionPattern).fieldNames(), fieldScope);
    m_searchRules = rules;

    // Подавляем авто-навигацию: reset модели результатов дёрнет currentRowChanged.
    {
        SearchResultsNavigator::Suppress quiet(*m_searchNavigator);
        m_searchController->setSource(m_activeLogView->model());
        m_searchController->search(rules);
    }

    // Подсветка совпадений: всегда в списке результатов; в основном view — по
    // галочке Highlight панели Search (вместе с подсветкой фильтра вкладки).
    m_searchResultsView->setTextHighlightPatterns(m_searchController->isActive()
        ? rules.highlightPatterns() : QVector<HighlightPattern>{});
    refreshMainViewHighlights(m_activeLogView);
}

void MainWindow::clearSearchResults()
{
    if (!m_searchController)
        return;
    // Reset модели дёрнет currentRowChanged — не даём ему прыгнуть в main.
    {
        SearchResultsNavigator::Suppress quiet(*m_searchNavigator);
        m_searchController->clear();
    }
    if (m_searchResultsView)
        m_searchResultsView->setTextHighlightPatterns({});
    refreshMainViewHighlights(m_activeLogView);
}

bool MainWindow::searchResultsLive() const
{
    return m_searchDockWidget && m_searchDockWidget->isVisible();
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
        refreshMainViewHighlights(lv);
    }
}

void MainWindow::updateFilterPanelFieldNames()
{
    const bool fieldScope = m_fieldFilterEnabledCheckBox && m_fieldFilterEnabledCheckBox->isChecked();
    const QStringList fieldNames = LogPattern(m_conversionPattern).fieldNames();
    if (m_filterPanel)
        m_filterPanel->setFieldNames(fieldNames, fieldScope);
    if (m_searchQuery)
        m_searchQuery->setFieldNames(fieldNames, fieldScope);
}

void MainWindow::updateFilterInputsFromModel()
{
    // Текстовые фильтры и маркеры пер-вкладочные и применяются явно
    // (Apply / автоприменение маркеров), поэтому при смене вкладки
    // синхронизируется только фильтр по времени: поля показывают применённый
    // интервал вкладки, а без него — весь диапазон её лога.
    seedTimeFilterEditors();
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
    // Панель результатов за видимым набором следит сама (контроллер поиска
    // слушает модель вкладки): здесь её не трогаем — modelFiltered приходит и
    // после каждой дописанной порции, и полный поиск заново на него свёл бы
    // на нет инкрементальное обновление.
}

void MainWindow::onScanDirectoryClicked()
{
    QString dirPath = QFileDialog::getExistingDirectory(this, tr("Select Directory to Scan"),
        m_lastScanDir.isEmpty() ? QDir::homePath() : m_lastScanDir,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!dirPath.isEmpty())
        scanDirectory(dirPath);
}

void MainWindow::scanDirectory(const QString& dirPath)
{
    m_lastScanDir = dirPath;
    m_dirScanner->setFileExtensions(AppSettings::instance().scanExtensions());
    m_dirScannerPanel->scanDirectory(dirPath);
    // Скан из меню, со стартового экрана или брошенной папки — результат
    // должен быть виден.
    ui->directoryScannerDockWidget->show();
    ui->directoryScannerDockWidget->raise();
}

void MainWindow::onOpenSelectedDirectoryFiles(const QStringList &filePaths)
{
    if (filePaths.isEmpty())
        return;

    LogViewWidget* view = tabForOpening();
    for (const QString &fPath : filePaths)
    {
        view->addLogFile(fPath);
    }

    updateTabLabel(view);
}

void MainWindow::onConfigureScanExtensionsClicked()
{
    // Delegate to the full Settings dialog — opens on the General tab
    // where the user can add/remove file extensions properly.
    SettingsDialog dlg(this);
    dlg.setFileMemory(m_fileMemory.get());
    dlg.exec();
    // If the user applied changes, AppSettings::settingsChanged() was already
    // emitted and the scanner will use the new extensions on the next scan.
}

void MainWindow::onSettingsTriggered()
{
    SettingsDialog dlg(this);
    dlg.setFileMemory(m_fileMemory.get());
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

void MainWindow::addToRecentFiles(const QString& filePath)
{
    // Спул stdin в недавние не попадает (RecentFiles::accepts): после выхода
    // временного файла нет, запись была бы мёртвой.
    if (m_recentFiles.add(filePath))
        updateRecentFilesMenu();
}

void MainWindow::updateRecentFilesMenu()
{
    const QStringList recent = m_recentFiles.paths();
    if (m_welcome)
        m_welcome->setRecentFiles(recent);
    ui->menuRecentFiles->clear();
    if (recent.isEmpty()) {
        QAction* empty = ui->menuRecentFiles->addAction(tr("(No recent files)"));
        empty->setEnabled(false);
        return;
    }
    for (int i = 0; i < recent.size(); ++i) {
        const QString filePath = recent.at(i);
        // Мнемоники 1…9 и 0 для десятого пункта.
        const QString number = i < 9 ? QStringLiteral("&%1").arg(i + 1)
                                     : QStringLiteral("1&0");
        QString label = QStringLiteral("%1  %2").arg(number, QFileInfo(filePath).fileName());
        QAction* a = ui->menuRecentFiles->addAction(label);
        a->setToolTip(QDir::toNativeSeparators(filePath));
        connect(a, &QAction::triggered, this, [this, filePath]() {
            openRecentFile(filePath);
        });
    }
    ui->menuRecentFiles->addSeparator();
    ui->menuRecentFiles->addAction(tr("Clear Recent Files"), this, &MainWindow::clearRecentFiles);
}

void MainWindow::clearRecentFiles()
{
    m_recentFiles.clear();
    updateRecentFilesMenu();
}

void MainWindow::openCommandLine(const OpenRouting::CommandLine& commandLine)
{
    const FileEncoding initial = OpenRouting::initialEncoding(commandLine);
    openFilesFromCommandLine(commandLine.paths, initial);
    if (commandLine.readStdin)
        openStdinStream(initial);
    if (!commandLine.error.isEmpty()) // неизвестная кодировка: файлы открыты с автоопределением
        QMessageBox::warning(this, tr("Command line"), commandLine.error);
}

void MainWindow::openFilesFromCommandLine(const QStringList& paths, const FileEncoding& initial)
{
    // Что куда идёт, решает OpenRouting::plan: файлы — вкладками, как раньше;
    // папка — в Directory Scanner (из нескольких — первая).
    const OpenRouting::Plan plan = OpenRouting::plan(paths);
    for (const QString& session : plan.sessions)
        openSessionFile(session);
    for (const QString& file : plan.files)
        openRecentFile(file, initial);
    if (plan.scanDirectory.isEmpty())
        return;
    scanDirectory(plan.scanDirectory);
    if (!plan.skippedDirectories.isEmpty()) {
        m_statusLabel->setText(
            tr("Scanning %1. The Directory Scanner shows one folder at a time: %n other folder(s) skipped.",
               nullptr, int(plan.skippedDirectories.size()))
                .arg(QDir::toNativeSeparators(plan.scanDirectory)));
    }
}

void MainWindow::startUp(const OpenRouting::CommandLine& commandLine, bool primary)
{
    // Последняя сессия (R-5) — только у первичного окна; включена настройка —
    // вкладки прошлого сеанса, затем файлы командной строки; выключена —
    // ссылка на стартовом экране.
    m_sessionController->setLastSessionFile(AppSettings::lastSessionFile(), primary);
    QString error;
    m_sessionController->startUp(AppSettings::instance().restoreSessionOnStartup(),
        [this](const SessionState& session, const QString& path) {
            openSession(session, path, SessionController::Origin::LastSession);
        },
        [this, &commandLine]() { openCommandLine(commandLine); }, &error);
    if (!error.isEmpty())
        statusBar()->showMessage(tr("Could not restore the previous session: %1").arg(error), 10000);
}

void MainWindow::openSessionFile(const QString& path)
{
    const SessionFile::LoadResult loaded = SessionFile::load(path);
    const QString name = QDir::toNativeSeparators(path);
    if (!loaded.state) {
        QMessageBox::warning(this, tr("Open Session"), loaded.newerVersion
            ? tr("%1 was saved by a newer version of DendroLog and cannot be opened here. "
                 "The file was not changed.").arg(name)
            : tr("%1 is not a DendroLog session or is damaged:\n%2").arg(name, loaded.error));
        return;
    }
    m_lastOpenDir = QFileInfo(path).absolutePath();
    openSession(*loaded.state, path, SessionController::Origin::SessionFile);
}

void MainWindow::openSession(const SessionState& session, const QString& path, SessionController::Origin origin)
{
    // Схема — до вкладок: правила вкладок привязываются к схеме при применении.
    applySessionSchema(session);
    const SessionController::OpenResult result =
        m_sessionController->open(session, path, [this]() {
            LogViewWidget* view = createLogViewWidget();
            ui->tabWidget->addTab(view, tr("Logs"));
            return view;
        });
    showSessionResult(result, origin);
}

void MainWindow::reopenClosedTab()
{
    const SessionController::OpenResult result =
        m_sessionController->reopenClosedTab([this](int position) {
            LogViewWidget* view = createLogViewWidget();
            ui->tabWidget->insertTab(qMin(position, ui->tabWidget->count()), view, tr("Logs"));
            return view;
        });
    showSessionResult(result, SessionController::Origin::ClosedTab);
}

void MainWindow::showSessionResult(const SessionController::OpenResult& result, SessionController::Origin origin)
{
    if (result.tabs.isEmpty() && origin == SessionController::Origin::ClosedTab)
        return;
    for (LogViewWidget* view : result.tabs)
        updateTabLabel(view);
    if (result.active)
        ui->tabWidget->setCurrentWidget(result.active);
    // Модальных окон нет: проблемы — ссылкой на отчёт в строке состояния.
    const QString summary = SessionController::summary(result, origin);
    if (result.hasProblems() || result.relocated) {
        m_sessionReportLink->setText(QStringLiteral("<a href=\"report\">%1</a>")
            .arg((summary + QLatin1Char(' ') + tr("Details…")).toHtmlEscaped()));
        m_sessionReportLink->show();
    } else {
        statusBar()->showMessage(summary, 10000);
    }
}

void MainWindow::saveSessionAs()
{
    const SessionState session = m_sessionController->captureWindow();
    if (session.tabs.isEmpty()) {
        QMessageBox::information(this, tr("Save Session As"),
            tr("There is nothing to save: no tab has files (standard input is not saved)."));
        return;
    }

    // defaultSuffix — до проверки «файл уже есть, заменить?» внутри диалога.
    QFileDialog dialog(this, tr("Save Session As"), m_lastOpenDir,
                       tr("DendroLog sessions (*.dlsession)"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix(QLatin1String(SessionState::kFileSuffix));
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
        return;
    const QString path = SessionFile::withSuffix(dialog.selectedFiles().first());
    QString error;
    if (!SessionFile::save(path, session, &error)) {
        QMessageBox::warning(this, tr("Save Session As"),
            tr("Could not save %1:\n%2").arg(QDir::toNativeSeparators(path), error));
        return;
    }
    statusBar()->showMessage(tr("Session saved to %1: %n tab(s).", nullptr, int(session.tabs.size()))
                                 .arg(QDir::toNativeSeparators(path)), 10000);
}

void MainWindow::applySessionSchema(const SessionState& session)
{
    // Сессия без схемы схему окна не трогает: открытые вкладки не теряют поля.
    if (!session.hasSchema())
        return;
    // Каталог схем — источник правды, и его могли изменить с прошлой загрузки
    // (sync ниже удалил бы файлы, которых нет в устаревшем списке).
    const auto reloadSchemas = [this](const QString& extraName) {
        QStringList order;
        for (const auto& e : m_patternList)
            order << e.name;
        if (!extraName.isEmpty())
            order << extraName;
        m_patternList = SchemaStore::loadAll(order);
        m_conversionPatternCombo->blockSignals(true);
        m_conversionPatternCombo->clear();
        for (const auto& e : m_patternList)
            addSchemaComboItem(m_conversionPatternCombo, e);
        m_conversionPatternCombo->blockSignals(false);
    };
    reloadSchemas(QString());
    const SessionController::SchemaChoice choice =
        SessionController::chooseSchema(m_patternList, session.schemaName, session.schema);
    int index = choice.index;
    if (choice.added) { // нет в каталоге — добавить под именем с пометкой
        m_patternList.append(choice.newEntry);
        QString error;
        if (!SchemaStore::sync(m_patternList, &error) && !error.isEmpty())
            statusBar()->showMessage(tr("Could not save the session schema: %1").arg(error), 10000);
        reloadSchemas(choice.newEntry.name);
        index = -1;
        for (int i = 0; i < m_patternList.size(); ++i)
            if (m_patternList[i].name == choice.newEntry.name)
                index = i;
    }
    // Не записалась в каталог — схема всё равно действует, без пункта в списке.
    m_conversionPattern = index >= 0 ? m_patternList[index].schema : session.schema;
    m_conversionPatternCombo->blockSignals(true);
    m_conversionPatternCombo->setCurrentIndex(index);
    m_conversionPatternCombo->blockSignals(false);
    if (m_fieldFilterEnabledCheckBox) {
        m_fieldFilterEnabledCheckBox->blockSignals(true);
        m_fieldFilterEnabledCheckBox->setChecked(session.fieldExtraction);
        m_fieldFilterEnabledCheckBox->blockSignals(false);
    }
    m_savedVisibleFieldNames = session.visibleFields;
    applyPatternToAllViews();
    updateFilterStatusButtons();
}

void MainWindow::openStdinStream(const FileEncoding& initial)
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
    openRecentFile(path, initial);

    auto* lv = qobject_cast<LogViewWidget*>(ui->tabWidget->currentWidget());
    if (!lv)
        return;

    // Живой поток: авто-догрузка + автопрокрутка к новым строкам.
    setTabAutoReload(lv, true);
    if (lv->view())
        lv->view()->setFollowTail(true);

    // Пинки от спулера — сверх обычного поллинга, чтобы хвост подтягивался
    // живо (спулер сигналит не чаще ~3 раз в секунду).
    connect(m_stdinSpooler, &StdinSpooler::bytesAppended, lv,
            [lv](qint64) { lv->reloadChangedFiles(); });
}

QList<LogViewWidget*> MainWindow::logViews() const
{
    QList<LogViewWidget*> views;
    for (int i = 0; i < ui->tabWidget->count(); ++i)
        if (auto* view = qobject_cast<LogViewWidget*>(ui->tabWidget->widget(i)))
            views << view;
    return views;
}

LogViewWidget* MainWindow::tabForOpening()
{
    // Текущая пустая вкладка занимается; вкладка из снимка (сессия,
    // Ctrl+Shift+T), даже без файлов, — нет: у неё свои фильтры и закладки.
    auto* view = qobject_cast<LogViewWidget*>(ui->tabWidget->currentWidget());
    if (view && view->isBlank()) {
        if (m_activeLogView != view)
            connectToLogView(view);
        return view;
    }
    view = createLogViewWidget();
    ui->tabWidget->setCurrentIndex(ui->tabWidget->addTab(view, tr("Logs")));
    return view;
}

void MainWindow::openRecentFile(const QString& filePath, const FileEncoding& initial)
{
    if (!QFileInfo::exists(filePath)) {
        QMessageBox::warning(this, tr("File Not Found"),
            tr("The file '%1' no longer exists.").arg(filePath));
        m_recentFiles.remove(filePath);
        updateRecentFilesMenu();
        return;
    }

    LogViewWidget* view = tabForOpening();
    view->addLogFile(filePath, initial);
    updateTabLabel(view);

    // Спул stdin — временный файл: ни в недавние, ни в «последнюю папку»
    // диалога Open он не идёт.
    if (RecentFiles::accepts(filePath))
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
    TextQuery query;
    if (m_activeLogView && takeQuickSearchQuery(&query))
        m_activeLogView->searchTextNext(query);
}

bool MainWindow::takeQuickSearchQuery(TextQuery* query)
{
    if (!m_searchLineEdit)
        return false;
    *query = m_searchLineEdit->query();
    if (query->isEmpty())
        return false;
    // Невалидный регекс: поле уже в красной рамке, причина — в строке статуса.
    const QString error = m_searchLineEdit->queryError();
    if (!error.isEmpty()) {
        m_statusLabel->setText(error);
        return false;
    }
    m_searchLineEdit->remember(query->text);
    return true;
}

void MainWindow::onQuickSearchProgress(const QString& term, int percent)
{
    // Приходит, только если скан длится дольше ~200 мс: короткий поиск
    // строку статуса не трогает.
    m_quickSearchStatusShown = true;
    m_statusLabel->setText(tr("Searching for \"%1\"... %2% (Esc to cancel)").arg(term).arg(percent));
}

void MainWindow::onQuickSearchFinished(const QString& term, bool found)
{
    if (!found) {
        m_statusLabel->setText(tr("\"%1\" not found").arg(term));
    } else if (m_quickSearchStatusShown) {
        m_statusLabel->setText(tr("Ready"));
    }
    m_quickSearchStatusShown = false;
    // «Не найдено» видно и в самом поле, а не только в строке статуса.
    // Только если поле всё ещё держит ТОТ запрос (текст и флаги): поиск,
    // запущенный до переключения флага, поле не красит.
    const auto* view = qobject_cast<const LogViewWidget*>(sender());
    if (m_searchLineEdit && view && view->quickSearchQuery() == m_searchLineEdit->query())
        m_searchLineEdit->setNotFound(!found);
}

void MainWindow::onLevelJumpProgress(LogLevelMask levels, bool forward, int percent)
{
    // Приходит, только если скан длится дольше ~200 мс (огромная вкладка).
    const bool warnings = levels == levelBit(LogLevel::Warn);
    m_levelJumpStatusShown = true;
    m_statusLabel->setText(
        (forward ? (warnings ? tr("Looking for the next warning... %1%")
                             : tr("Looking for the next error... %1%"))
                 : (warnings ? tr("Looking for the previous warning... %1%")
                             : tr("Looking for the previous error... %1%")))
            .arg(percent));
    m_levelJumpStatusText = m_statusLabel->text();
}

void MainWindow::onLevelJumpFinished(LogLevelMask levels, LogViewWidget::LevelJumpOutcome outcome)
{
    const bool warnings = levels == levelBit(LogLevel::Warn);
    QString text;
    switch (outcome) {
    case LogViewWidget::LevelJumpOutcome::Moved:
        break;
    case LogViewWidget::LevelJumpOutcome::Wrapped:
        text = warnings ? tr("No more warnings this way - continued from the other end")
                        : tr("No more errors this way - continued from the other end");
        break;
    case LogViewWidget::LevelJumpOutcome::OnlyCurrent:
        text = warnings ? tr("This is the only warning among the shown lines")
                        : tr("This is the only error among the shown lines");
        break;
    case LogViewWidget::LevelJumpOutcome::NotFound:
        text = warnings ? tr("No warnings among the shown lines")
                        : tr("No errors among the shown lines");
        break;
    case LogViewWidget::LevelJumpOutcome::Interrupted:
        text = tr("The shown lines changed during the search - press the key again");
        break;
    }
    showJumpStatus(text);
}

void MainWindow::showJumpStatus(const QString& text)
{
    // Удачный переход пояснений не требует, но своё прошлое пояснение убираем
    // (только своё: строку статуса с тех пор мог занять кто-то другой).
    if (!text.isEmpty())
        m_statusLabel->setText(text);
    else if (m_levelJumpStatusShown && m_statusLabel->text() == m_levelJumpStatusText)
        m_statusLabel->setText(tr("Ready"));
    m_levelJumpStatusShown = !text.isEmpty();
    m_levelJumpStatusText = text;
}

void MainWindow::onBookmarkJumpFinished(bool forward, LogViewWidget::LevelJumpOutcome outcome)
{
    Q_UNUSED(forward);
    QString text;
    switch (outcome) {
    case LogViewWidget::LevelJumpOutcome::Moved:
    case LogViewWidget::LevelJumpOutcome::Interrupted:
        break;
    case LogViewWidget::LevelJumpOutcome::Wrapped:
        text = tr("No more bookmarks this way - continued from the other end");
        break;
    case LogViewWidget::LevelJumpOutcome::OnlyCurrent:
        text = tr("This is the only bookmark among the shown lines");
        break;
    case LogViewWidget::LevelJumpOutcome::NotFound:
        text = tr("No bookmarks among the shown lines");
        break;
    }
    showJumpStatus(text);
}

void MainWindow::editBookmarkNote(int bookmarkId, bool created)
{
    if (!m_activeLogView)
        return;
    BookmarkStore& store = m_activeLogView->bookmarks();
    const Bookmark* bookmark = store.find(bookmarkId);
    if (!bookmark)
        return;
    bool ok = false;
    const QString note = QInputDialog::getText(
        this, tr("Bookmark Note"),
        tr("Note for line %1 of %2:").arg(bookmark->line)
            .arg(QFileInfo(bookmark->filePath).fileName()),
        QLineEdit::Normal, bookmark->note, &ok);
    if (ok)
        store.setNote(bookmarkId, note);
    else if (created)
        store.remove(bookmarkId); // «Add Bookmark with Note…» отменён целиком
}

void MainWindow::updateSearchResultsStatus()
{
    if (!m_searchResultsStatusLabel || !m_searchController)
        return;
    QString text = m_searchController->statusText();
    // Пустой список без объяснения выглядел сломанным: подсказываем, как
    // его наполнить.
    if (!m_searchController->isActive())
        text += QLatin1Char(' ') + tr("Type what to find into a rule and press Search "
                                      "or Enter. The log itself is not filtered.");
    m_searchResultsStatusLabel->setText(text);
}

void MainWindow::onViewTextAction(LogListView::TextAction action, const QString& text)
{
    if (text.isEmpty())
        return;
    switch (action) {
    case LogListView::TextAction::Find:
        if (m_searchLineEdit) {
            // Выделенный текст — литерал: «.*» выключаем, а не экранируем
            // текст (иначе в историю ушла бы экранированная строка).
            m_searchLineEdit->regexAction()->setChecked(false);
            m_searchLineEdit->setText(text);
            onSearchNextTriggered();
        }
        break;
    case LogListView::TextAction::FindAll:
        findAllInSearchPanel(TextQuery(text, Qt::CaseInsensitive));
        break;
    case LogListView::TextAction::FilterInclude:
    case LogListView::TextAction::FilterExclude: {
        LogModel* model = m_activeLogView ? m_activeLogView->model() : nullptr;
        if (!m_filterPanel || !model)
            return;
        const bool exclude = (action == LogListView::TextAction::FilterExclude);
        // Сужаем то, что вкладка показывает СЕЙЧАС. Если панель описывает
        // именно это (или обе пусты) — правило добавляется в панель и всё
        // применяется, как по Apply: видно, откуда фильтр. Иначе в панели
        // другие, не применённые к вкладке правила (например, профиль из
        // прошлого сеанса): их не применяем и не затираем — правило
        // добавляется только к фильтру вкладки.
        const FilterRuleSet panelRules = m_filterPanel->ruleSet();
        const FilterRuleSet applied = model->filterRules();
        const bool panelDescribesTab = panelRules == applied
            || (!panelRules.isActive() && !applied.isActive());
        if (panelDescribesTab) {
            m_filterPanel->addQuickRule(text, exclude);
            applyTextFiltersToActiveView();
            ui->textFilterDockWidget->show();
            ui->textFilterDockWidget->raise();
        } else {
            FilterRuleSet rules = applied;
            rules.rules.append(m_filterPanel->quickRule(text, exclude));
            const bool fieldScope = m_fieldFilterEnabledCheckBox
                                 && m_fieldFilterEnabledCheckBox->isChecked();
            rules.bindFields(LogPattern(m_conversionPattern).fieldNames(), fieldScope);
            model->setFilterRules(rules);
            refreshMainViewHighlights(m_activeLogView);
            m_statusLabel->setText(tr("Filter added to this tab. The Text Filters panel "
                                      "holds other rules and was left as is; "
                                      "Reset there turns this filter off."));
        }
        break;
    }
    case LogListView::TextAction::Highlight: {
        LogModel* model = m_activeLogView ? m_activeLogView->model() : nullptr;
        if (!m_markerPanel || !model)
            return;
        // Маркер всегда попадает в панель (ничего не затирает), а к вкладке —
        // вдобавок к уже применённым к ней маркерам.
        m_markerPanel->addQuickMarker(text);
        QVector<HighlightPattern> markers = model->rowMarkers();
        markers.append(m_markerPanel->markers().constLast());
        model->setRowMarkers(markers);
        updateFilterStatusButtons();
        m_markerDockWidget->show();
        m_markerDockWidget->raise();
        break;
    }
    }
}

void MainWindow::onSearchPreviousTriggered()
{
    TextQuery query;
    if (m_activeLogView && takeQuickSearchQuery(&query))
        m_activeLogView->searchTextPrevious(query);
}

// ---------------------------------------------------------------------------
void MainWindow::onReloadFileTriggered()
{
    // Manual one-shot reload of the active tab (F5 or left-click on the button).
    if (!m_activeLogView)
        return;
    m_activeLogView->reloadChangedFiles(/*force=*/true);
}

// ---------------------------------------------------------------------------
void MainWindow::onAutoReloadTimerTick()
{
    // Reload every tab that has per-tab auto-reload enabled.
    for (LogViewWidget* lvw : logViews())
        if (lvw->autoReload())
            lvw->reloadChangedFiles();
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
    const QList<LogViewWidget*> views = logViews();
    if (std::any_of(views.cbegin(), views.cend(), [](LogViewWidget* v) { return v->autoReload(); }))
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
    if (view) // таймер и значок «живой» вкладки обновляет autoReloadChanged
        view->setAutoReload(enabled);
    syncReloadButton(); // и кнопку, даже если флаг не изменился
}

// ---------------------------------------------------------------------------
// Syncs the toolbar button's checked state to the currently active tab.
void MainWindow::syncReloadButton()
{
    ui->actionAutoReload->setChecked(m_activeLogView && m_activeLogView->autoReload());
    ui->actionAutoReload->setEnabled(m_activeLogView != nullptr);
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
// Окно снова стало активным (развернули из трея/панели задач, переключились
// на него) — данные вкладок с авто-обновлением могли устареть, пока окно было
// не в фокусе. Подтягиваем хвост сразу, не дожидаясь очередного тика таймера.
void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    if (!event)
        return;
    const auto type = event->type();
    const bool activated = (type == QEvent::ActivationChange && isActiveWindow());
    const bool restored  = (type == QEvent::WindowStateChange && !isMinimized());
    if (activated || restored)
        onAutoReloadTimerTick();
    // Смена светлой/тёмной темы: иконки, стили кнопок уровней и фон поля
    // быстрого поиска «не найдено» считаются из палитры — перекрасить (поле —
    // в refreshToolIcons).
    if (type == QEvent::PaletteChange && m_constructed) {
        refreshToolIcons();
        updateLogLevelFilterButtons();
        updateTimeFilterInfo();
    }
}

// ---------------------------------------------------------------------------
bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    // Правый клик по Reload — прежний жест переключения авто-обновления
    // вкладки (рядом теперь есть и отдельный тогл). ContextMenu гасим, иначе
    // он всплыл бы в меню тулбара.
    if (obj == m_reloadButton && event->type() == QEvent::ContextMenu) {
        onToggleTabAutoReload();
        return true;
    }
    return QMainWindow::eventFilter(obj, event);
}
