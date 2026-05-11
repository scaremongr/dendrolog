#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "logviewwidget.h"

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

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow), m_progressBar(nullptr), m_statusLabel(nullptr), m_activeLogView(nullptr), m_lineInfoLabel(nullptr), m_timeFilterFrom(nullptr), m_timeFilterTo(nullptr), m_applyTimeFilterButton(nullptr), m_resetTimeFilterButton(nullptr), m_textFilterContainerWidget(nullptr), m_textFilterLayout(nullptr), m_textFilterGlobalCaseSensitiveCheckBox(nullptr), m_addTextFilterButton(nullptr), m_applyAllTextFiltersButton(nullptr), m_statParser(nullptr)
{
    ui->setupUi(this);

    m_scanFileExtensions << "log" << "txt";

    setupStatusBar();
    setupTimeFilterDockContents();
    setupTextFilterDockContents();
    setupDirectoryScanner();

    // Connect search actions
    connect(ui->searchLineEdit, &QLineEdit::returnPressed, this, &MainWindow::onSearchEnterPressed);
    connect(ui->actionSearchNext, &QAction::triggered, this, &MainWindow::onSearchNextTriggered);
    connect(ui->actionSearchPrevious, &QAction::triggered, this, &MainWindow::onSearchPreviousTriggered);

    m_statParser = new LogParser(this);
    connect(&m_fileStatWatcher, &QFutureWatcher<QPair<LogParser::FileStats, QString>>::finished,
            this, &MainWindow::handleFileStatProcessed);

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
}

MainWindow::~MainWindow()
{
    delete ui;
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
    QStringList headers;
    headers << tr("Name") << tr("Total Entries") << tr("From") << tr("To")
            << tr("Warns") << tr("Errors") << tr("Fatals") << tr("Size (Bytes)");
    ui->directoryScanResultsTree->setHeaderLabels(headers);
    ui->directoryScanResultsTree->setColumnCount(headers.size());

    ui->directoryScanResultsTree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    ui->directoryScanResultsTree->header()->resizeSection(0, 180);

    ui->directoryScanResultsTree->header()->setStretchLastSection(false);
    for (int i = 1; i < headers.size(); ++i)
    {
        ui->directoryScanResultsTree->header()->setSectionResizeMode(i, QHeaderView::ResizeToContents);
    }

    ui->directoryScanResultsTree->setIndentation(15);
    ui->directoryScanResultsTree->setItemsExpandable(true);

    ui->directoryScanResultsTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ui->directoryScanResultsTree->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(ui->directoryScanResultsTree, &QTreeWidget::itemDoubleClicked,
            this, &MainWindow::onDirectoryTreeItemDoubleClicked);
    connect(ui->directoryScanResultsTree, &QTreeWidget::customContextMenuRequested,
            this, &MainWindow::onDirectoryTreeContextMenuRequested);
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
        QString(),
        tr("Log files (*.log *.txt);;All files (*)"));

    if (files.isEmpty())
        return;

    LogViewWidget *view = nullptr;
    int tabIndex = -1;
    if (ui->tabWidget->count() == 0 ||
        ((view = qobject_cast<LogViewWidget *>(ui->tabWidget->currentWidget())) && view && view->fileCount() > 0))
    {
        view = new LogViewWidget(this);
        tabIndex = ui->tabWidget->addTab(view, tr("Logs"));
        ui->tabWidget->setCurrentIndex(tabIndex);
    }
    else
    {
        view = qobject_cast<LogViewWidget *>(ui->tabWidget->currentWidget());
        if (!view)
        {
            view = new LogViewWidget(this);
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
            const auto logFile = view->loadedFiles().first(); // не ссылка — loadedFiles() возвращает временный список
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
                                                        QDir::homePath(),
                                                        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (!dirPath.isEmpty())
    {
        ui->selectedDirectoryPathLabel->setText(tr("Selected directory: %1").arg(dirPath));
        populateDirectoryScanResults(dirPath);
    }
}

void MainWindow::populateDirectoryScanResults(const QString &directoryPath)
{
    ui->directoryScanResultsTree->clear();
    m_filePathToTreeItemMap.clear();
    m_filesToStatQueue.clear();

    QDir rootDir(directoryPath);
    if (!rootDir.exists())
    {
        ui->selectedDirectoryPathLabel->setText(tr("Selected directory: %1 (Does not exist)").arg(directoryPath));
        return;
    }
    ui->selectedDirectoryPathLabel->setText(tr("Scanning: %1").arg(directoryPath));

    qint64 dummySizeAccumulator = 0;
    scanDirectoryRecursive(rootDir, ui->directoryScanResultsTree->invisibleRootItem(), dummySizeAccumulator);

    ui->directoryScanResultsTree->expandAll();

    startNextFileStatScan();
}

void MainWindow::scanDirectoryRecursive(QDir &currentDir, QTreeWidgetItem *parentItem, qint64 &totalSizeAccumulator)
{
    currentDir.setFilter(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
    currentDir.setSorting(QDir::Name | QDir::DirsFirst);

    QFileInfoList entries = currentDir.entryInfoList();

    for (const QFileInfo &entryInfo : entries)
    {
        if (entryInfo.isDir())
        {
            QTreeWidgetItem *dirItem = new QTreeWidgetItem(parentItem);
            dirItem->setText(0, entryInfo.fileName());
            dirItem->setToolTip(0, entryInfo.filePath()); // Tooltip for directory
            for (int i = 1; i <= 6; ++i)
            {
                dirItem->setText(i, "N/A");
            }
            qint64 directorySize = 0;
            auto subdir = QDir(entryInfo.filePath());
            scanDirectoryRecursive(subdir, dirItem, directorySize);
            dirItem->setText(7, QLocale().toString(directorySize));
            totalSizeAccumulator += directorySize;
        }
        else if (entryInfo.isFile())
        {
            QString suffixLower = entryInfo.suffix().toLower();
            bool extensionMatch = false;
            if (m_scanFileExtensions.isEmpty())
            {
                extensionMatch = (suffixLower == "log" || suffixLower == "txt");
            }
            else
            {
                for (const QString &configuredExt : m_scanFileExtensions)
                {
                    if (suffixLower == configuredExt)
                    {
                        extensionMatch = true;
                        break;
                    }
                }
            }

            if (extensionMatch)
            {
                QTreeWidgetItem *fileItem = new QTreeWidgetItem(parentItem);
                fileItem->setText(0, entryInfo.fileName());
                // Initial tooltip: just the file path. Will be enhanced later.
                fileItem->setToolTip(0, entryInfo.filePath());
                fileItem->setData(0, Qt::UserRole, entryInfo.filePath());

                qint64 fileSize = entryInfo.size();
                fileItem->setText(7, QLocale().toString(fileSize));
                totalSizeAccumulator += fileSize;

                m_filePathToTreeItemMap.insert(entryInfo.filePath(), fileItem);
                m_filesToStatQueue.append(entryInfo.filePath());

                fileItem->setText(1, tr("Queued..."));
                for (int i = 2; i <= 6; ++i)
                    fileItem->setText(i, "-");
            }
        }
    }
}

void MainWindow::startNextFileStatScan()
{
    if (m_fileStatWatcher.isRunning() || m_filesToStatQueue.isEmpty())
    {
        return;
    }

    QString filePathToProcess = m_filesToStatQueue.takeFirst();
    QTreeWidgetItem *item = m_filePathToTreeItemMap.value(filePathToProcess);
    if (item)
    {
        for (int i = 1; i <= 6; ++i)
        {
            item->setText(i, tr("Scanning..."));
        }
    }

    QFuture<QPair<LogParser::FileStats, QString>> future = QtConcurrent::run([this, filePathToProcess]()
                                                                             {
        LogParser::FileStats stats = m_statParser->analyzeFileForStats(filePathToProcess);
        return qMakePair(stats, filePathToProcess); });
    m_fileStatWatcher.setFuture(future);
}

void MainWindow::handleFileStatProcessed()
{
    if (!m_fileStatWatcher.future().isValid())
    {
        qWarning() << "Future is not valid in handleFileStatProcessed";
        startNextFileStatScan();
        return;
    }

    QPair<LogParser::FileStats, QString> resultPair = m_fileStatWatcher.result();
    const LogParser::FileStats &stats = resultPair.first;
    const QString &filePath = resultPair.second;

    QTreeWidgetItem *item = m_filePathToTreeItemMap.value(filePath);
    if (item)
    {
        QString generalStyle = "style='font-size: 9pt;'";
        QString boldStyle = "style='font-size: 9pt; font-weight: bold;'";

        // Determine if the tooltip background is likely light or dark.
        // Tooltips typically follow the application style. Assume light background for now if not certain.
        // QToolTip::palette().color(QPalette::ToolTipBase) // This is more accurate for tooltip bg
        // QToolTip::palette().color(QPalette::ToolTipText) // This is more accurate for tooltip text
        // For simplicity, let's assume tooltip text color is similar to general app text color for this heuristic.
        bool tooltipTextIsLight = QToolTip::palette().color(QPalette::ToolTipText).lightnessF() > 0.5;

        QColor fatalColorBase = LogModel::defaultColorForLevel(LogLevel::Fatal);
        QColor errorColorBase = LogModel::defaultColorForLevel(LogLevel::Error);
        QColor warnColorBase = LogModel::defaultColorForLevel(LogLevel::Warn);

        LogModel *currentModel = nullptr;
        if (m_activeLogView)
            currentModel = m_activeLogView->model();
        if (currentModel)
        {
            fatalColorBase = currentModel->getLogLevelColor(LogLevel::Fatal);
            errorColorBase = currentModel->getLogLevelColor(LogLevel::Error);
            warnColorBase = currentModel->getLogLevelColor(LogLevel::Warn);
        }

        // Adapt color for text based on tooltip's text color (light on dark, dark on light)
        auto adaptColorForText = [&](const QColor &base)
        {
            if (tooltipTextIsLight)
            {                             // Light text on dark tooltip background
                return base.lighter(120); // Make color lighter to show on dark bg
            }
            else
            {                            // Dark text on light tooltip background
                return base.darker(150); // Make color darker to show on light bg
            }
        };

        QString fatalColorText = adaptColorForText(fatalColorBase).name();
        QString errorColorText = adaptColorForText(errorColorBase).name();
        QString warnColorText = adaptColorForText(warnColorBase).name();

        QString tooltipText = QString("<table %1>").arg(generalStyle);
        tooltipText += QString("<tr><td %1>File:</td><td>%2</td></tr>").arg(boldStyle).arg(QFileInfo(filePath).fileName());
        tooltipText += QString("<tr><td %1>Path:</td><td>%2</td></tr>").arg(boldStyle).arg(filePath);

        if (stats.parseSuccess)
        {
            item->setText(1, QString::number(stats.totalEntries));
            item->setText(2, stats.firstEntryTimestamp.isValid() ? stats.firstEntryTimestamp.toString("yyyy-MM-dd HH:mm:ss") : "N/A");
            item->setText(3, stats.lastEntryTimestamp.isValid() ? stats.lastEntryTimestamp.toString("yyyy-MM-dd HH:mm:ss") : "N/A");
            item->setText(4, QString::number(stats.warnCount));
            item->setText(5, QString::number(stats.errorCount));
            item->setText(6, QString::number(stats.fatalCount));

            tooltipText += QString("<tr><td %1>Total Entries:</td><td>%2</td></tr>").arg(boldStyle).arg(stats.totalEntries);
            tooltipText += QString("<tr><td %1>First Entry:</td><td>%2</td></tr>").arg(boldStyle).arg(stats.firstEntryTimestamp.isValid() ? stats.firstEntryTimestamp.toString("yyyy-MM-dd HH:mm:ss") : "N/A");
            tooltipText += QString("<tr><td %1>Last Entry:</td><td>%2</td></tr>").arg(boldStyle).arg(stats.lastEntryTimestamp.isValid() ? stats.lastEntryTimestamp.toString("yyyy-MM-dd HH:mm:ss") : "N/A");
            tooltipText += QString("<tr><td %1 style='color: %2;'>Warnings:</td><td style='color: %2;'>%3</td></tr>").arg(boldStyle).arg(warnColorText).arg(stats.warnCount);
            tooltipText += QString("<tr><td %1 style='color: %2;'>Errors:</td><td style='color: %2;'>%3</td></tr>").arg(boldStyle).arg(errorColorText).arg(stats.errorCount);
            tooltipText += QString("<tr><td %1 style='color: %2;'>Fatals:</td><td style='color: %2;'>%3</td></tr>").arg(boldStyle).arg(fatalColorText).arg(stats.fatalCount);
        }
        else
        {
            for (int i = 1; i <= 6; ++i)
            {
                item->setText(i, tr("Parse Failed"));
            }
            tooltipText += QString("<tr><td %1 colspan='2'>Parse Failed</td></tr>").arg(boldStyle);
        }
        tooltipText += QString("<tr><td %1>Size:</td><td>%2 Bytes</td></tr>").arg(boldStyle).arg(QLocale().toString(QFileInfo(filePath).size()));
        tooltipText += "</table>";
        item->setToolTip(0, tooltipText);
    }
    else
    {
        qWarning() << "Could not find tree item for path:" << filePath;
    }

    startNextFileStatScan();
}

void MainWindow::onDirectoryTreeItemDoubleClicked(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    if (!item || item->childCount() > 0)
        return;

    QString filePath = item->data(0, Qt::UserRole).toString();
    if (filePath.isEmpty())
    {
        filePath = item->toolTip(0);
    }

    if (!filePath.isEmpty() && QFile::exists(filePath))
    {
        LogViewWidget *view = nullptr;
        int tabIndex = -1;

        if (ui->tabWidget->count() == 0 ||
            ((view = qobject_cast<LogViewWidget *>(ui->tabWidget->currentWidget())) && view && view->fileCount() > 0))
        {
            view = new LogViewWidget(this);
            tabIndex = ui->tabWidget->addTab(view, QFileInfo(filePath).fileName());
            ui->tabWidget->setCurrentIndex(tabIndex);
        }
        else
        {
            view = qobject_cast<LogViewWidget *>(ui->tabWidget->currentWidget());
            if (!view)
            {
                view = new LogViewWidget(this);
                tabIndex = ui->tabWidget->addTab(view, QFileInfo(filePath).fileName());
                ui->tabWidget->setCurrentIndex(tabIndex);
            }
            else
            {
                if (m_activeLogView != view)
                    connectToLogView(view);
            }
        }

        if (view)
        {
            view->addLogFile(filePath);
            int currentTabIndex = ui->tabWidget->indexOf(view);
            if (currentTabIndex != -1)
            {
                ui->tabWidget->setTabText(currentTabIndex, QFileInfo(filePath).fileName());
                ui->tabWidget->setTabToolTip(currentTabIndex, filePath);
            }
        }
    }
}

void MainWindow::onDirectoryTreeContextMenuRequested(const QPoint &pos)
{
    QList<QTreeWidgetItem *> selectedItems = ui->directoryScanResultsTree->selectedItems();
    if (selectedItems.isEmpty())
    {
        return;
    }

    QStringList filesToOpen;
    for (QTreeWidgetItem *item : selectedItems)
    {
        if (item && item->childCount() == 0)
        {
            QString filePath = item->data(0, Qt::UserRole).toString();
            if (filePath.isEmpty())
                filePath = item->toolTip(0);
            if (!filePath.isEmpty() && QFile::exists(filePath))
            {
                filesToOpen.append(filePath);
            }
        }
    }

    if (filesToOpen.isEmpty())
    {
        return;
    }

    QMenu contextMenu(this);
    QAction *openAction = contextMenu.addAction(tr("Open Selected Files (%1)").arg(filesToOpen.count()));

    connect(openAction, &QAction::triggered, this, [this, filesToOpen]()
            { this->onOpenSelectedDirectoryFiles(filesToOpen); });

    contextMenu.exec(ui->directoryScanResultsTree->viewport()->mapToGlobal(pos));
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
        view = new LogViewWidget(this);
        tabIndex = ui->tabWidget->addTab(view, tr("Logs"));
        ui->tabWidget->setCurrentIndex(tabIndex);
    }
    else
    {
        view = qobject_cast<LogViewWidget *>(ui->tabWidget->currentWidget());
        if (!view)
        {
            view = new LogViewWidget(this);
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

    if (fileCountInView == 1 && !view->loadedFiles().isEmpty())
    {
        const auto &logFile = view->loadedFiles().first();
        tabText = logFile->shortName();
        tabToolTip = logFile->filePath;
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
