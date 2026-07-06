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
#include "schemastore.h"
#include "apptheme.h"

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
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QFile>
#include <QIcon>
#include <QPixmap>
#include <QPainter>
#include <QKeySequence>
#include <QSvgRenderer>

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
    setupDirectoryScanner();
    setupFieldVisibilityDock();

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

    // "Tools" menu with Settings action.
    QMenu* menuTools = menuBar()->addMenu(tr("&Tools"));
    QAction* settingsAction = menuTools->addAction(tr("Settings..."));
    connect(settingsAction, &QAction::triggered, this, &MainWindow::onSettingsTriggered);
    m_shortcutActions.insert(QStringLiteral("settings"), settingsAction);

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
    return QApplication::applicationDirPath() + "/LogViewer.ini";
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
    // Schemas themselves live as files in <exe>/patterns/ (see SchemaStore);
    // the INI only remembers their display order, keyed by name.
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
        const QJsonDocument doc(m_filterPanel->ruleSet().toJson());
        s.setValue("textFilterRules", QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
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

    s.beginGroup("Filters");
    if (m_filterPanel) {
        const QByteArray rulesJson = s.value("textFilterRules").toString().toUtf8();
        if (!rulesJson.isEmpty()) {
            const QJsonDocument doc = QJsonDocument::fromJson(rulesJson);
            if (doc.isObject())
                m_filterPanel->setRuleSet(FilterRuleSet::fromJson(doc.object()));
        }
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

    // Offer first lines of the active log as live-preview samples.
    QStringList sampleLines;
    if (auto* lv = qobject_cast<LogViewWidget*>(ui->tabWidget->currentWidget())) {
        if (lv->model()) {
            const auto& entries = lv->model()->allEntries();
            for (const auto& entry : entries) {
                if (!entry || entry->message.trimmed().isEmpty())
                    continue;
                sampleLines.append(entry->message);
                if (sampleLines.size() >= 8)
                    break;
            }
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
    // wait for the worker threads to actually stop touching entry->fields.
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

        if (lv->model()) {
            for (const auto& entry : lv->model()->allEntries()) {
                if (!entry)
                    continue;
                if (doExtract)
                    entries.append(entry);
                else
                    entry->fields = LogEntryFields();
            }
        }
    }

    // Small batches (or "no extraction") complete instantly on the GUI
    // thread — no worker, no progress bar, no display flicker.
    constexpr int kAsyncThreshold = 4000;
    if (!doExtract || entries.size() < kAsyncThreshold) {
        for (const auto& entry : entries)
            entry->fields = pattern->extractFields(entry->message);
        finishPatternApplication();
        return;
    }

    // Large batch: re-extract in the background and show the same status-bar
    // progress as file loading. While it runs we turn OFF field display so
    // formatDisplayMessage never reads entry->fields concurrently with the
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

    // Воркеры ниже перезаписывают entry->fields, а фоновая перефильтрация
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
            entry->fields = pattern->extractFields(entry->message);
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

    if (logView->view())
        connect(logView->view(), &LogListView::timeFilterBoundRequested,
                this, &MainWindow::onTimeFilterBoundRequested);

    if (m_activeLogView && m_activeLogView->model() && m_activeLogView->view())
    {
        int totalRows = m_activeLogView->model()->rowCount();
        QModelIndex currentModelIndex = m_activeLogView->view()->currentIndex();
        int currentRow = currentModelIndex.isValid() ? currentModelIndex.row() : -1;
        updateLineInfoLabel(currentRow, totalRows);
    }
    syncReloadButton();
    updateFilterInputsFromModel();
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
    }
    updateStatusBarDefaultText();
    updateLogLevelFilterButtons();
    updateFilterInputsFromModel();
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
    
    // Update tab text to reflect the loaded file
    for (int i = 0; i < ui->tabWidget->count(); ++i) {
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

void MainWindow::onApplyAllTextFiltersClicked()
{
    applyTextFiltersToActiveView();
}

void MainWindow::onResetTextFiltersClicked()
{
    // Снять фильтры и подсветку с активного документа; правила в панели
    // остаются и могут быть применены заново.
    if (!m_activeLogView)
        return;
    if (m_activeLogView->model())
        m_activeLogView->model()->setFilterRules(FilterRuleSet{});
    if (m_activeLogView->view())
        m_activeLogView->view()->setTextHighlightPatterns({});
}

void MainWindow::onApplyRowMarkersClicked()
{
    applyRowMarkersToActiveView();
}

void MainWindow::onResetRowMarkersClicked()
{
    // Снять окраску маркеров с активного документа; маркеры в панели
    // остаются и могут быть применены заново.
    if (m_activeLogView && m_activeLogView->model())
        m_activeLogView->model()->setRowMarkers({});
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
    if (m_activeLogView->view())
        m_activeLogView->view()->setTextHighlightPatterns(rules.highlightPatterns());
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
