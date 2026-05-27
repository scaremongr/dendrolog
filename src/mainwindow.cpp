#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "logviewwidget.h"
#include "conversionpatterndialog.h"
#include "directoryscanner.h"

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
#include <QInputDialog>
#include <QMenu>
#include <QApplication>
#include <QToolTip>
#include <QFormLayout>
#include <QSettings>
#include <QCloseEvent>
#include <QMessageBox>
#include <QComboBox>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow), m_progressBar(nullptr), m_statusLabel(nullptr), m_activeLogView(nullptr), m_lineInfoLabel(nullptr), m_timeFilterFrom(nullptr), m_timeFilterTo(nullptr), m_applyTimeFilterButton(nullptr), m_resetTimeFilterButton(nullptr), m_textFilterContainerWidget(nullptr), m_textFilterLayout(nullptr), m_textFilterGlobalCaseSensitiveCheckBox(nullptr), m_addTextFilterButton(nullptr), m_applyAllTextFiltersButton(nullptr)
{
    ui->setupUi(this);

    m_scanFileExtensions << "log" << "txt";

    setupStatusBar();
    setupTimeFilterDockContents();
    setupTextFilterDockContents();
    setupDirectoryScanner();
    setupFieldVisibilityDock();

    // Connect search actions
    connect(ui->searchLineEdit, &QLineEdit::returnPressed, this, &MainWindow::onSearchEnterPressed);
    connect(ui->actionSearchNext, &QAction::triggered, this, &MainWindow::onSearchNextTriggered);
    connect(ui->actionSearchPrevious, &QAction::triggered, this, &MainWindow::onSearchPreviousTriggered);

    // Replace static dock-toggle actions with QDockWidget::toggleViewAction()
    // so checkmarks automatically reflect actual dock visibility
    {
        auto setupDockToggle = [](QMenu* menu, QAction* staticAction, QDockWidget* dock, const QString& text) {
            QAction* a = dock->toggleViewAction();
            a->setText(text);
            menu->insertAction(staticAction, a);
            menu->removeAction(staticAction);
        };
        setupDockToggle(ui->menuView, ui->actionToggle_Text_Filters_Panel,
                        ui->textFilterDockWidget, tr("Text Filters Panel"));
        setupDockToggle(ui->menuView, ui->actionToggle_Directory_Scanner_Panel,
                        ui->directoryScannerDockWidget, tr("Directory Scanner Panel"));
        setupDockToggle(ui->menuView, ui->actionToggle_Time_Filter_Panel,
                        ui->timeFilterDockWidget, tr("Time Filter Panel"));
        setupDockToggle(ui->menuView, ui->actionToggle_Field_Visibility_Panel,
                        ui->fieldVisibilityDockWidget, tr("Log Fields Panel"));
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
            if (ui->tabWidget->count() == 0) {
                 updateStatusBarDefaultText();
            } });
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
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
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

    s.beginGroup("Scanner");
    s.setValue("extensions", m_scanFileExtensions);
    s.endGroup();

    s.beginGroup("View");
    s.setValue("wordWrap", ui->actionWordWrap->isChecked());
    {
        uint8_t mask = 0;
        for (int i = 0; i < LogFieldCount; ++i)
            if (m_fieldCheckBoxes[i] && m_fieldCheckBoxes[i]->isChecked())
                mask |= static_cast<uint8_t>(1u << i);
        s.setValue("fieldVisibilityMask", static_cast<int>(mask));
    }
    s.setValue("conversionPattern", m_conversionPattern);
    {
        QStringList names, values;
        for (const auto& e : m_patternList) {
            names  << e.first;
            values << e.second;
        }
        s.setValue("patternNames",  names);
        s.setValue("patternValues", values);
    }
    s.endGroup();

    s.beginGroup("RecentFiles");
    s.setValue("files", m_recentFiles);
    s.endGroup();
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

    s.beginGroup("Scanner");
    QStringList exts = s.value("extensions").toStringList();
    if (!exts.isEmpty())
        m_scanFileExtensions = exts;
    s.endGroup();

    s.beginGroup("View");
    ui->actionWordWrap->setChecked(s.value("wordWrap", true).toBool());
    {
        const int savedMask = s.value("fieldVisibilityMask", static_cast<int>(LogFieldAllMask)).toInt();
        const uint8_t mask  = static_cast<uint8_t>(savedMask);
        for (int i = 0; i < LogFieldCount; ++i)
            if (m_fieldCheckBoxes[i])
                m_fieldCheckBoxes[i]->setChecked((mask >> i) & 1u);
        // Sync master toggle state after restoring individual checkboxes
        if (m_allFieldsCheckBox) {
            int cnt = 0;
            for (int i = 0; i < LogFieldCount; ++i)
                cnt += (mask >> i) & 1u;
            m_allFieldsCheckBox->blockSignals(true);
            if (cnt == 0)                  m_allFieldsCheckBox->setCheckState(Qt::Unchecked);
            else if (cnt == LogFieldCount) m_allFieldsCheckBox->setCheckState(Qt::Checked);
            else                           m_allFieldsCheckBox->setCheckState(Qt::PartiallyChecked);
            m_allFieldsCheckBox->blockSignals(false);
        }
    }
    m_conversionPattern = s.value("conversionPattern").toString();
    {
        const QStringList names  = s.value("patternNames").toStringList();
        const QStringList values = s.value("patternValues").toStringList();
        m_patternList.clear();
        const int cnt = qMin(names.size(), values.size());
        m_patternList.reserve(cnt);
        for (int i = 0; i < cnt; ++i)
            m_patternList.append({names[i], values[i]});
        // Backwards compatibility: migrate old single-string list
        if (m_patternList.isEmpty()) {
            const QStringList old = s.value("conversionPatternList").toStringList();
            for (const auto& p : old)
                if (!p.trimmed().isEmpty())
                    m_patternList.append({p.trimmed(), p.trimmed()});
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
    s.endGroup();

    s.beginGroup("RecentFiles");
    m_recentFiles = s.value("files").toStringList();
    s.endGroup();

    updateRecentFilesMenu();
}

// =============================================================================
// Field-visibility dock
// =============================================================================

static const char* fieldDisplayName(int i)
{
    switch (static_cast<LogField>(i)) {
    case LogField::Timestamp:  return "Timestamp  (%d)";
    case LogField::ThreadId:   return "Thread ID  (%t)";
    case LogField::LoggerName: return "Logger Name  (%c)";
    case LogField::Level:      return "Level  (%p)";
    case LogField::Message:    return "Message  (%m)";
    case LogField::Ndc:        return "Context / NDC  (%x)";
    case LogField::SourceFile: return "Source File  (%F)";
    case LogField::SourceLine: return "Source Line  (%L)";
    default:                   return "Unknown";
    }
}

void MainWindow::setupFieldVisibilityDock()
{
    QWidget* contents = ui->fieldVisibilityContentsWidget;
    if (!contents) return;

    auto* rootLayout = new QVBoxLayout(contents);
    rootLayout->setContentsMargins(4, 4, 4, 4);
    rootLayout->setSpacing(2);

    // ---- Section: visible fields ----
    auto* fieldsLabel = new QLabel(tr("<b>Show fields:</b>"), contents);
    rootLayout->addWidget(fieldsLabel);

    // "All fields" tri-state master toggle
    m_allFieldsCheckBox = new QCheckBox(tr("(all fields)"), contents);
    m_allFieldsCheckBox->setTristate(true);
    m_allFieldsCheckBox->setCheckState(Qt::Checked);
    connect(m_allFieldsCheckBox, &QCheckBox::stateChanged, this, [this](int state) {
        if (static_cast<Qt::CheckState>(state) == Qt::PartiallyChecked) return;
        const bool checkAll = (state == Qt::Checked);
        for (int i = 0; i < LogFieldCount; ++i) {
            if (m_fieldCheckBoxes[i]) {
                m_fieldCheckBoxes[i]->blockSignals(true);
                m_fieldCheckBoxes[i]->setChecked(checkAll);
                m_fieldCheckBoxes[i]->blockSignals(false);
            }
        }
        applyFieldVisibilityToAllViews();
    });
    rootLayout->addWidget(m_allFieldsCheckBox);

    for (int i = 0; i < LogFieldCount; ++i) {
        auto* cb = new QCheckBox(tr(fieldDisplayName(i)), contents);
        cb->setChecked(true); // default: all visible
        m_fieldCheckBoxes[i] = cb;
        connect(cb, &QCheckBox::toggled, this, &MainWindow::onFieldVisibilityChanged);
        rootLayout->addWidget(cb);
    }

    rootLayout->addSpacing(4);

    // ---- Section: conversion pattern ----
    auto* patternLabel = new QLabel(tr("<b>Conversion pattern:</b>"), contents);
    patternLabel->setToolTip(tr(
        "Log4cxx / Log4j ConversionPattern - e.g.:\n"
        "> %d [%-10t] %-20c  %-8p %m ~~ %x {%F:%L}%n\n\n"
        "Use 'Manage...' to add, edit and delete named patterns.\n"
        "'Apply to all tabs' re-extracts fields in every open tab."));
    rootLayout->addWidget(patternLabel);

    // Non-editable combo: shows pattern display names from m_patternList
    m_conversionPatternCombo = new QComboBox(contents);
    m_conversionPatternCombo->setEditable(false);
    m_conversionPatternCombo->setToolTip(patternLabel->toolTip());
    connect(m_conversionPatternCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onPatternComboChanged);
    rootLayout->addWidget(m_conversionPatternCombo);

    auto* btnRow = new QHBoxLayout();
    btnRow->setSpacing(4);
    auto* applyPatternBtn = new QPushButton(tr("Apply to all tabs"), contents);
    applyPatternBtn->setToolTip(tr(
        "Re-apply field extraction to all already-loaded entries."));
    connect(applyPatternBtn, &QPushButton::clicked,
            this, &MainWindow::onConversionPatternApply);
    auto* manageBtn = new QPushButton(tr("Manage..."), contents);
    manageBtn->setToolTip(tr("Add, edit and delete named conversion patterns."));
    connect(manageBtn, &QPushButton::clicked,
            this, &MainWindow::onManagePatterns);
    btnRow->addWidget(applyPatternBtn, 1);
    btnRow->addWidget(manageBtn);
    rootLayout->addLayout(btnRow);

    rootLayout->addStretch();

    contents->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding);
    ui->fieldVisibilityDockWidget->setSizePolicy(QSizePolicy::Preferred,
                                                  QSizePolicy::MinimumExpanding);
}

// Compute the current mask from checkbox states
static LogModel::FieldVisibilityMask computeVisibilityMask(
    const std::array<QCheckBox*, LogFieldCount>& boxes)
{
    LogModel::FieldVisibilityMask mask = 0;
    for (int i = 0; i < LogFieldCount; ++i)
        if (boxes[i] && boxes[i]->isChecked())
            mask |= static_cast<LogModel::FieldVisibilityMask>(1u << i);
    return mask;
}

void MainWindow::onFieldVisibilityChanged()
{
    // Keep the master "all fields" checkbox in sync with the individual boxes
    if (m_allFieldsCheckBox) {
        int checkedCount = 0;
        for (int i = 0; i < LogFieldCount; ++i)
            if (m_fieldCheckBoxes[i] && m_fieldCheckBoxes[i]->isChecked())
                ++checkedCount;
        m_allFieldsCheckBox->blockSignals(true);
        if (checkedCount == 0)
            m_allFieldsCheckBox->setCheckState(Qt::Unchecked);
        else if (checkedCount == LogFieldCount)
            m_allFieldsCheckBox->setCheckState(Qt::Checked);
        else
            m_allFieldsCheckBox->setCheckState(Qt::PartiallyChecked);
        m_allFieldsCheckBox->blockSignals(false);
    }
    applyFieldVisibilityToAllViews();
}

void MainWindow::applyFieldVisibilityToAllViews()
{
    const auto mask = computeVisibilityMask(m_fieldCheckBoxes);
    for (int t = 0; t < ui->tabWidget->count(); ++t) {
        auto* lv = qobject_cast<LogViewWidget*>(ui->tabWidget->widget(t));
        if (lv && lv->model())
            lv->model()->setVisibleFields(mask);
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
}

void MainWindow::onManagePatterns()
{
    ConversionPatternDialog dlg(m_patternList, this);
    if (dlg.exec() != QDialog::Accepted) return;

    m_patternList = dlg.resultPatterns();

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


void MainWindow::applyPatternToAllViews()
{
    for (int t = 0; t < ui->tabWidget->count(); ++t) {
        auto* lv = qobject_cast<LogViewWidget*>(ui->tabWidget->widget(t));
        if (!lv) continue;
        lv->setParserPattern(m_conversionPattern);

        // Re-extract fields for all already-loaded entries so the display
        // updates immediately without requiring a file re-open.
        if (lv->model()) {
            const LogPattern pat(m_conversionPattern);
            if (pat.isValid()) {
                const auto& entries = lv->model()->allEntries();
                for (const auto& entry : entries) {
                    if (entry)
                        entry->fields = pat.extractFields(entry->message);
                }
            }
            // Notify the view to repaint with updated field data.
            // refreshDisplay() always emits dataChanged (unlike setVisibleFields which
            // returns early when the mask hasn't changed).
            lv->model()->refreshDisplay();
        }
    }
}

LogViewWidget* MainWindow::createLogViewWidget()
{
    auto* view = new LogViewWidget(this);
    view->setParserPattern(m_conversionPattern);
    if (view->model())
        view->model()->setVisibleFields(computeVisibilityMask(m_fieldCheckBoxes));
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
    m_textFilterContainerWidget = ui->textFilterContentsWidget;
    if (!m_textFilterContainerWidget)
    {
        qWarning("textFilterContentsWidget not found in UI. Text filter dock will be empty.");
        m_textFilterContainerWidget = new QWidget(ui->textFilterDockWidget);
        ui->textFilterDockWidget->setWidget(m_textFilterContainerWidget);
    }

    m_textFilterLayout = new QVBoxLayout(m_textFilterContainerWidget);
    m_textFilterLayout->setContentsMargins(5, 5, 5, 5);
    m_textFilterLayout->setSpacing(5);

    QHBoxLayout *controlsLayout = new QHBoxLayout();
    m_addTextFilterButton = new QPushButton(tr("+"), this);
    m_addTextFilterButton->setToolTip(tr("Add another text filter term (OR logic)"));
    connect(m_addTextFilterButton, &QPushButton::clicked, this, &MainWindow::onAddTextFilterInputClicked);
    controlsLayout->addWidget(m_addTextFilterButton);

    m_applyAllTextFiltersButton = new QPushButton(tr("Apply Text Filters"), this);
    connect(m_applyAllTextFiltersButton, &QPushButton::clicked, this, &MainWindow::onApplyAllTextFiltersClicked);
    controlsLayout->addWidget(m_applyAllTextFiltersButton);

    m_textFilterGlobalCaseSensitiveCheckBox = new QCheckBox(tr("Case Sensitive"), this);
    controlsLayout->addWidget(m_textFilterGlobalCaseSensitiveCheckBox);
    controlsLayout->addStretch();
    m_textFilterLayout->addLayout(controlsLayout);

    addNewTextFilterInput();

    // This stretch pushes newly added filter rows upwards and ensures that the layout
    // doesn't try to grow unnecessarily beyond its content when the dock widget itself resizes.
    m_textFilterLayout->addStretch(1);
    m_textFilterContainerWidget->adjustSize(); // Update the container's size hint

    // Allow the dock widget to expand vertically as its content grows.
    // This replaces the previous QSizePolicy::Fixed.
    ui->textFilterDockWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding);

    // No explicit setFixedHeight calls are needed here, the layout should manage it.
}

void MainWindow::setupDirectoryScanner()
{
    m_dirScanner = new DirectoryScanner(ui->directoryScanResultsTree, this);
    m_dirScanner->setFileExtensions(m_scanFileExtensions);
    m_dirScanner->setConversionPattern(m_conversionPattern);
    connect(m_dirScanner, &DirectoryScanner::fileActivated,
            this, [this](const QString& path) {
        onOpenSelectedDirectoryFiles({path});
    });
    connect(m_dirScanner, &DirectoryScanner::filesActivated,
            this, &MainWindow::onOpenSelectedDirectoryFiles);
}

void MainWindow::addNewTextFilterInput(const QString &initialText)
{
    QHBoxLayout *rowLayout = new QHBoxLayout();
    QLineEdit *textInput = new QLineEdit(this);
    textInput->setPlaceholderText(tr("Enter filter text..."));
    if (!initialText.isEmpty())
    {
        textInput->setText(initialText);
    }
    connect(textInput, &QLineEdit::returnPressed, this, &MainWindow::onApplyAllTextFiltersClicked);

    QPushButton *removeButton = new QPushButton(tr("-"), this);
    removeButton->setToolTip(tr("Remove this filter term"));

    rowLayout->addWidget(textInput);
    rowLayout->addWidget(removeButton);

    m_textFilterLayout->insertLayout(m_textFilterLayout->count() - 1, rowLayout);

    m_textFilterInputs.append(textInput);
    m_textFilterRemoveButtons.append(removeButton);

    connect(removeButton, &QPushButton::clicked, this, &MainWindow::onRemoveTextFilterInputClicked);

    if (m_textFilterInputs.count() == 1)
    {
        m_textFilterRemoveButtons.first()->setEnabled(false);
    }
    else
    {
        for (QPushButton *btn : m_textFilterRemoveButtons)
        {
            btn->setEnabled(true);
        }
    }
}

void MainWindow::onRemoveTextFilterInputClicked()
{
    QPushButton *B = qobject_cast<QPushButton *>(sender());
    if (!B)
        return;

    int indexToRemove = -1;
    for (int i = 0; i < m_textFilterRemoveButtons.count(); ++i)
    {
        if (m_textFilterRemoveButtons.at(i) == B)
        {
            indexToRemove = i;
            break;
        }
    }

    if (indexToRemove != -1)
    {
        QLineEdit *lineEdit = m_textFilterInputs.takeAt(indexToRemove);
        QPushButton *removeButton = m_textFilterRemoveButtons.takeAt(indexToRemove);

        for (int i = 0; i < m_textFilterLayout->count(); ++i)
        {
            QLayoutItem *item = m_textFilterLayout->itemAt(i);
            if (item && item->layout())
            {
                QHBoxLayout *row = static_cast<QHBoxLayout *>(item->layout());
                bool found = false;
                for (int j = 0; j < row->count(); ++j)
                {
                    QWidget *widget = row->itemAt(j)->widget();
                    if (widget == lineEdit || widget == removeButton)
                    {
                        found = true;
                        break;
                    }
                }
                if (found)
                {
                    while (QLayoutItem *childItem = row->takeAt(0))
                    {
                        if (childItem->widget())
                        {
                            delete childItem->widget();
                        }
                        delete childItem;
                    }
                    m_textFilterLayout->removeItem(row);
                    delete row;
                    break;
                }
            }
        }

        if (m_textFilterInputs.count() == 1 && !m_textFilterRemoveButtons.isEmpty())
        {
            m_textFilterRemoveButtons.first()->setEnabled(false);
        }
    }
}

void MainWindow::clearTextFilterInputs()
{
    while (!m_textFilterInputs.isEmpty())
    {
        QLineEdit *lineEdit = m_textFilterInputs.takeFirst();
        QPushButton *removeButton = m_textFilterRemoveButtons.takeFirst();

        for (int i = 0; i < m_textFilterLayout->count(); ++i)
        {
            QLayoutItem *item = m_textFilterLayout->itemAt(i);
            if (item && item->layout())
            {
                QHBoxLayout *row = static_cast<QHBoxLayout *>(item->layout());
                bool found = false;
                for (int j = 0; j < row->count(); ++j)
                {
                    QWidget *widget = row->itemAt(j)->widget();
                    if (widget == lineEdit || widget == removeButton)
                    {
                        found = true;
                        break;
                    }
                }
                if (found)
                {
                    while (QLayoutItem *childItem = row->takeAt(0))
                    {
                        if (childItem->widget())
                        {
                            delete childItem->widget();
                        }
                        delete childItem;
                    }
                    m_textFilterLayout->removeItem(row);
                    delete row;
                    break;
                }
            }
        }
    }
    m_textFilterInputs.clear();
    m_textFilterRemoveButtons.clear();
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

    if (m_activeLogView && m_activeLogView->model() && m_activeLogView->view())
    {
        int totalRows = m_activeLogView->model()->rowCount();
        QModelIndex currentModelIndex = m_activeLogView->view()->currentIndex();
        int currentRow = currentModelIndex.isValid() ? currentModelIndex.row() : -1;
        updateLineInfoLabel(currentRow, totalRows);
    }
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
        tr("Log files (*.log *.txt);;All files (*)"));

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
        QColor baseColor = LogModel::defaultColorForLevel(level);

        if (model)
        {
            active = model->logLevelFilter().contains(level);
            if (active)
                baseColor = model->getLogLevelColor(level);
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

void MainWindow::onApplyAllTextFiltersClicked()
{
    if (m_activeLogView && m_activeLogView->model())
    {
        QVector<LogModel::MessageFilterRule> rules;
        for (QLineEdit *lineEdit : m_textFilterInputs)
        {
            QString searchText = lineEdit->text();
            if (!searchText.isEmpty())
            {
                LogModel::MessageFilterRule rule;
                rule.substring = searchText;
                rule.caseSensitivity = m_textFilterGlobalCaseSensitiveCheckBox->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive;
                rules.append(rule);
            }
        }
        m_activeLogView->model()->setMessageFilterRules(rules);
    }
}

void MainWindow::updateFilterInputsFromModel()
{
    LogViewWidget *currentView = qobject_cast<LogViewWidget *>(ui->tabWidget->currentWidget());
    clearTextFilterInputs();

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

        QVector<LogModel::MessageFilterRule> rules = model->messageFilterRules();
        if (!rules.isEmpty())
        {
            for (const auto &rule : rules)
            {
                addNewTextFilterInput(rule.substring);
            }
            if (m_textFilterGlobalCaseSensitiveCheckBox)
                m_textFilterGlobalCaseSensitiveCheckBox->setChecked(rules.first().caseSensitivity == Qt::CaseSensitive);
        }
        else
        {
            addNewTextFilterInput();
            if (m_textFilterGlobalCaseSensitiveCheckBox)
                m_textFilterGlobalCaseSensitiveCheckBox->setChecked(false);
        }
    }
    else
    {
        m_timeFilterFrom->setDateTime(QDateTime::currentDateTime().addDays(-1).date().startOfDay());
        m_timeFilterTo->setDateTime(QDateTime::currentDateTime().date().endOfDay());
        addNewTextFilterInput();
        if (m_textFilterGlobalCaseSensitiveCheckBox)
            m_textFilterGlobalCaseSensitiveCheckBox->setChecked(false);
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

void MainWindow::onAddTextFilterInputClicked()
{
    addNewTextFilterInput();
}

void MainWindow::onScanDirectoryClicked()
{
    QString dirPath = QFileDialog::getExistingDirectory(this, tr("Select Directory to Scan"),
        m_lastScanDir.isEmpty() ? QDir::homePath() : m_lastScanDir,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!dirPath.isEmpty()) {
        m_lastScanDir = dirPath;
        ui->selectedDirectoryPathLabel->setText(tr("Directory: %1").arg(dirPath));
        m_dirScanner->setFileExtensions(m_scanFileExtensions);
        m_dirScanner->setConversionPattern(m_conversionPattern);
        m_dirScanner->scan(dirPath);
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
    bool ok;
    QString currentExtensions = m_scanFileExtensions.join(',');
    QString text = QInputDialog::getText(this, tr("Configure Scan Extensions"),
                                         tr("Enter file extensions (comma-separated, e.g., log,txt,data):"),
                                         QLineEdit::Normal,
                                         currentExtensions, &ok);
    if (ok && !text.isEmpty())
    {
        m_scanFileExtensions = text.split(',', Qt::SkipEmptyParts);
        for (QString &ext : m_scanFileExtensions)
        {
            ext = ext.trimmed().toLower(); // Normalize: lowercase and trimmed
        }
        // If a directory is currently displayed, you might want to offer to re-scan it.
        // For now, new extensions apply to the next scan.
        QString currentPath = ui->selectedDirectoryPathLabel->text();
        if (QDir(currentPath).exists())
        {   // A crude check if a path is loaded
            // Consider re-populating or prompting user to re-scan.
            // populateDirectoryScanResults(currentPath); // Example: auto-rescan
        }
    }
    else if (ok && text.isEmpty())
    {                                 // User entered empty string, effectively clearing custom extensions
        m_scanFileExtensions.clear(); // Cleared, so scanDirectoryRecursive might use defaults
    }
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
    if (m_activeLogView && ui->searchLineEdit)
    {
        QString searchTerm = ui->searchLineEdit->text();
        if (!searchTerm.isEmpty())
        {
            // For now, search is case-insensitive. This could be a checkbox in the UI later.
            m_activeLogView->searchTextNext(searchTerm, false /*caseSensitive*/);
        }
    }
}

void MainWindow::onSearchPreviousTriggered()
{
    if (m_activeLogView && ui->searchLineEdit)
    {
        QString searchTerm = ui->searchLineEdit->text();
        if (!searchTerm.isEmpty())
        {
            // For now, search is case-insensitive.
            m_activeLogView->searchTextPrevious(searchTerm, false /*caseSensitive*/);
        }
    }
}
