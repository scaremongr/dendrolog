#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QPair>
#include <QHash>
#include <QStringList>
#include <QVector>
#include <QFutureWatcher>
#include <memory>
#include "logentry.h"
#include "logfile.h"

// Forward declarations for Qt classes used in members or method signatures
class QProgressBar;
class QLabel;
class LogViewWidget;
class QDateTimeEdit;
class QLineEdit;
class QCheckBox;
class QPushButton;
class QVBoxLayout;
class QDockWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QDir;
class QMenu;
class QComboBox;
class QInputDialog; // For scan extensions
class ConversionPatternDialog;
class DirectoryScanner;
class DirectoryScannerPanel;
class QCloseEvent;
class QTimer;
class QToolButton;
class QWidget;
class FilterPanelWidget;
class MarkerPanelWidget;
class LogModel;
class LogPattern;

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; } // Forward declaration for the UI class
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    // Slots connected by objectName in .ui file (or via connectSlotsByName)
    void on_actionOpen_triggered();
    void on_actionSaveAs_triggered();
    void on_actionFatal_toggled(bool checked);
    void on_actionError_toggled(bool checked);
    void on_actionWarn_toggled(bool checked);
    void on_actionInfo_toggled(bool checked);
    void on_actionDebug_toggled(bool checked);
    void on_actionTrace_toggled(bool checked);
    void on_actionWordWrap_toggled(bool checked);
    void onScanDirectoryClicked(); // Connected from ui->scanDirectoryButton
    void onConfigureScanExtensionsClicked(); // Connected from ui->configureScanExtensionsButton
    void toggleTextFilterDock(); // Connected from ui->actionToggle_Text_Filters_Panel
    void toggleDirectoryScannerDock(); // Connected from ui->actionToggle_Directory_Scanner_Panel
    void toggleTimeFilterDock(); // New slot for the time filter dock

    // Slots connected manually
    void onCurrentTabChanged(int index);
    void handleFileParsingStarted(const LogFilePtr& logFile);
    void handleFileParsingProgress(const LogFilePtr& logFile, int progressPercentage);
    void handleFileParsingFinished(const LogFilePtr& logFile, int totalEntries);
    void handleFileParsingFailed(const LogFilePtr& logFile);
    void handleTotalRowCountChanged(int totalRows);
    void updateLineInfoLabel(int currentRow, int totalRows);
    void onApplyTimeFilterClicked();
    void onResetTimeFilterClicked();
    // Подстановка таймстампа (из контекстного меню LogView) в поле фильтра по времени.
    void onTimeFilterBoundRequested(const QDateTime& dt, bool isStart);
    void onApplyAllTextFiltersClicked(); // Apply из конструктора фильтров (активная вкладка)
    void onResetTextFiltersClicked();    // Reset — снять фильтры с активной вкладки
    void onApplyRowMarkersClicked();     // Apply из панели маркеров (активная вкладка)
    void onResetRowMarkersClicked();     // Reset — снять маркеры с активной вкладки
    void updateFilterInputsFromModel();
    void handleModelFiltered();
    void onOpenSelectedDirectoryFiles(const QStringList& filePaths);
    void openRecentFile(const QString& filePath);
    void onSettingsTriggered();

    // Search related slots
    void onSearchNextTriggered();
    void onSearchPreviousTriggered();
    void onSearchEnterPressed();

    // Reload slot (manual button + auto-timer)
    void onReloadFileTriggered();
    void onAutoReloadTimerTick();   // Timer tick: reloads every tab that has auto-reload on
    void onToggleTabAutoReload();   // Right-click on reload button: toggle current tab's auto-reload

    // Field-visibility dock slots
    void onFieldVisibilityChanged();
    void onConversionPatternApply();
    void onPatternComboChanged(int index);
    void onManagePatterns();

private:
    Ui::MainWindow *ui; // Pointer to the UI class generated from mainwindow.ui

    // Widgets not fully managed by .ui (e.g. added to statusbar or dynamic content)
    QProgressBar* m_progressBar;
    QLabel* m_statusLabel;
    QLabel* m_lineInfoLabel;
    QLineEdit* m_searchLineEdit = nullptr;

    // Time filter widgets (added to ui->timeFilterToolBar)
    QDateTimeEdit* m_timeFilterFrom;
    QDateTimeEdit* m_timeFilterTo;
    QPushButton* m_applyTimeFilterButton;
    QPushButton* m_resetTimeFilterButton; // Added

    // Конструктор текстовых фильтров (содержимое textFilterDockWidget).
    // Вся логика динамического списка правил инкапсулирована в виджете.
    FilterPanelWidget* m_filterPanel = nullptr;

    // Недеструктивные row-маркеры (отдельный док, создаётся в коде).
    MarkerPanelWidget* m_markerPanel = nullptr;
    QDockWidget* m_markerDockWidget = nullptr;

    // Directory Scanner related members
    QString m_lastOpenDir;
    QString m_lastScanDir;
    QStringList m_recentFiles;

    // Other members
    LogViewWidget*     m_activeLogView;
    DirectoryScanner*  m_dirScanner = nullptr;        // owned by m_dirScannerPanel
    DirectoryScannerPanel* m_dirScannerPanel = nullptr;

    // Field-visibility dock widgets
    QVector<QCheckBox*> m_fieldCheckBoxes;
    QCheckBox* m_fieldFilterEnabledCheckBox = nullptr; // master on/off toggle
    QCheckBox* m_allFieldsCheckBox    = nullptr;
    QWidget* m_fieldFilterControlsWidget = nullptr;
    QVBoxLayout* m_fieldCheckboxLayout = nullptr;
    QComboBox* m_conversionPatternCombo = nullptr;
    QString    m_conversionPattern;             // Global pattern (for next file opens)
    using PatternEntry = QPair<QString, QString>; // (display name, pattern string)
    QList<PatternEntry> m_patternList;
    QStringList m_savedVisibleFieldNames;

    // Background field re-extraction (schema switch). The worker only reads
    // entry messages and writes entry->fields; the view's field display is
    // disabled for the duration so nothing reads fields concurrently.
    QFutureWatcher<void>*              m_fieldWatcher = nullptr;
    std::shared_ptr<LogPattern>        m_pendingFieldPattern;
    QVector<std::shared_ptr<LogEntry>> m_pendingFieldEntries; // kept alive while map() runs

    // Settings persistence
    void saveSettings();
    void loadSettings();
    void addToRecentFiles(const QString& filePath);
    void updateRecentFilesMenu();

    // Auto-reload support
    QTimer*       m_autoReloadTimer = nullptr;
    QToolButton*  m_reloadButton    = nullptr;  // toolbar button (icon + checkable)
    void applyAutoReloadSettings();
    void updateAutoReloadTimer();   // start/stop timer based on active per-tab flags
    // Atomic operation: update the flag on a tab, sync the button, update the timer.
    void setTabAutoReload(LogViewWidget* view, bool enabled);
    // Sync button checked-state to whatever m_activeLogView says (or false if none).
    void syncReloadButton();

    // Configurable keyboard shortcuts.
    // Maps a ShortcutManager command id to the QAction it drives, so that
    // applyShortcuts() can (re)assign sequences whenever the user edits them.
    QHash<QString, QAction*> m_shortcutActions;
    void registerShortcutActions();   // populate m_shortcutActions
    void applyShortcuts();            // assign sequences from ShortcutManager

    // Builds the file dialog filter string ("Log files (*.log *.txt);;All files (*)")
    // from the shared scan-extension list so Open and Save use the same set.
    QString logFileDialogFilter() const;

    // Setup methods
    void setupStatusBar();
    void setupTimeFilterDockContents(); // New method to set up the new dock
    void setupTextFilterDockContents();
    void setupRowMarkerDock();          // Док Row Highlighters
    void setupDirectoryScanner();
    void setupFieldVisibilityDock();    // New: Log Fields panel

    // Helper methods
    void connectToLogView(LogViewWidget* logView);
    void disconnectFromLogView(LogViewWidget* logView);
    void updateStatusBarDefaultText();
    void updateLogLevelFilterButtons();
    void setFilterLogLvl(LogLevel level, bool add);

    // Factory: creates a LogViewWidget with current global settings pre-applied
    LogViewWidget* createLogViewWidget();
    // Apply current field-visibility mask to every open LogViewWidget's model
    void applyFieldVisibilityToAllViews();
    // Apply current pattern to every open LogViewWidget's parser
    void applyPatternToAllViews();
    // Final display/filter refresh after fields have been (re)extracted.
    void finishPatternApplication();
    // Called when the background field re-extraction finishes.
    void onFieldExtractionFinished();
    // Abort any in-flight background field re-extraction (and wait for its
    // worker threads to stop) before mutating entries again.
    void cancelFieldExtraction();
    // Применить набор правил из конструктора фильтров (+ inline-подсветку
    // совпадений) к АКТИВНОЙ вкладке. Остальные документы не трогаются —
    // у каждой вкладки свой применённый набор.
    void applyTextFiltersToActiveView();
    // Применить row-маркеры к активной вкладке.
    void applyRowMarkersToActiveView();
    // Перепривязать УЖЕ применённые правила каждой вкладки к новой схеме
    // полей (вызывается при смене схемы / галочки "Filter blocks").
    void rebindFiltersOnAllViews();
    // Синхронизировать список колонок конструктора фильтров с активной
    // схемой Log Fields и состоянием галочки "Filter blocks".
    void updateFilterPanelFieldNames();
    void rebuildFieldVisibilityControls(const QStringList& fieldNames);
    QVector<int> selectedVisibleFieldIndexes() const;
    QStringList selectedVisibleFieldNames() const;

};

#endif // MAINWINDOW_H
