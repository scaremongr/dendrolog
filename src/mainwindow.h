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
#include "viewexport.h"
#include "LogListView.h"
#include "filterruleset.h"

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
class TimelineHistogramWidget;
class EntryDetailsPanel;
class StatisticsPanel;
class LogModel;
class LogListView;
class LogPattern;
class SearchResultsController;
class FieldReextraction;
class QModelIndex;
class QDialog;
class QDragEnterEvent;
class QDropEvent;
class UpdateChecker;
class QPoint;
class WelcomeWidget;
class SearchPanelWidget;

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; } // Forward declaration for the UI class
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    // Открыть файлы, переданные в командной строке (DendroLog.exe a.log b.log).
    void openFilesFromCommandLine(const QStringList& paths);

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
    // Возврат фокуса на окно — повод подтянуть хвост вкладок с авто-обновлением.
    void changeEvent(QEvent* event) override;
    // Перетаскивание лог-файлов из проводника прямо в окно.
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

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
    // Клик по таймлайн-гистограмме: переход к моменту (или к ближайшей
    // ошибке, если клик пришёлся в дорожку Warn/Error/Fatal).
    void onTimelineTimeClicked(const QDateTime& time, bool preferErrors);
    // Протяжка по таймлайну: применить выделенный интервал к фильтру по
    // времени (поля дока Time Filter синхронизируются, док не поднимается).
    void onTimelineRangeSelected(const QDateTime& from, const QDateTime& to);
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
    void onQuickSearchProgress(const QString& term, int percent);
    void onQuickSearchFinished(const QString& term, bool found);
    // Пункт контекстного меню лога над выделенным текстом: найти его,
    // добавить правило фильтра или маркер и применить к активной вкладке.
    void onViewTextAction(LogListView::TextAction action, const QString& text);
    // Панель Search: Search/Enter — запустить поиск по активной вкладке,
    // Clear — очистить выдачу (правила остаются).
    void onSearchRequested();
    void onSearchCleared();
    // «Find All»: запрос из одного правила «Contains text» в панели Search
    // и поиск; пустой text — просто открыть панель с курсором в правиле.
    void findAllInSearchPanel(const QString& text, bool caseSensitive);
    void onViewExportFinished(const ViewExport::Result& result);
    // Save View As of the active tab into fileName, in the background.
    void startViewExport(const QString& fileName);

    // Reload slot (manual button + auto-timer)
    void onReloadFileTriggered();
    void onAutoReloadTimerTick();   // Timer tick: reloads every tab that has auto-reload on
    void onToggleTabAutoReload();   // Right-click on reload button: toggle current tab's auto-reload

    // Field-visibility dock slots
    void onFieldVisibilityChanged();
    void onConversionPatternApply();
    void onPatternComboChanged(int index);
    void onManagePatterns();

public:
    // Приём потокового ввода (`program | DendroLog -`): спулит stdin во
    // временный файл и открывает его вкладкой-растущим логом с follow-tail.
    void openStdinStream();

private:
    Ui::MainWindow *ui; // Pointer to the UI class generated from mainwindow.ui
    // Конструктор отработал: до этого указатели ui-> ещё не заполнены, а
    // changeEvent (смена палитры) может прийти уже во время setupUi.
    bool m_constructed = false;

    // Env-gated хук верификации (DENDRO_BASELINE_DUMP=<каталог>): фиксированная
    // последовательность фильтров над активной моделью с дампом результатов в
    // файлы и выходом. Байтовый базлайн для рефакторингов хранилища; в обычных
    // запусках недостижим. Вызывается из handleFileParsingFinished.
    void maybeStartBaselineDump();

    // Widgets not fully managed by .ui (e.g. added to statusbar or dynamic content)
    QProgressBar* m_progressBar;
    QLabel* m_statusLabel;
    QLabel* m_lineInfoLabel;
    QLineEdit* m_searchLineEdit = nullptr;
    QAction*   m_matchCaseAction = nullptr;   // «Aa» — учитывать регистр
    // Стартовый экран вместо пустой рамки, пока нет ни одной вкладки.
    WelcomeWidget* m_welcome = nullptr;
    // Строка статуса показывает прогресс быстрого поиска — вернуть её по итогу.
    bool m_quickSearchStatusShown = false;
    // Фоновый Save View As и кнопка его отмены в строке статуса.
    ViewExportJob* m_exportJob = nullptr;
    QToolButton* m_cancelExportButton = nullptr;
    QString m_exportCancelReason; // почему экспорт отменён не пользователем

    // Time filter widgets (added to ui->timeFilterToolBar)
    QDateTimeEdit* m_timeFilterFrom;
    QDateTimeEdit* m_timeFilterTo;
    QPushButton* m_applyTimeFilterButton;
    QPushButton* m_resetTimeFilterButton; // Added
    QLabel* m_timeFilterInfoLabel = nullptr;  // диапазон лога и состояние фильтра
    QLabel* m_timeFilterErrorLabel = nullptr; // «From позже To» и т.п.
    // Поля From/To правил пользователь: пока нет — они следуют за вкладкой
    // (применённый фильтр либо весь диапазон её лога).
    bool m_timeFilterEdited = false;
    bool m_seedingTimeFilter = false;         // программная установка полей

    // Конструктор текстовых фильтров (содержимое textFilterDockWidget).
    // Вся логика динамического списка правил инкапсулирована в виджете.
    FilterPanelWidget* m_filterPanel = nullptr;

    // Недеструктивные row-маркеры (отдельный док, создаётся в коде).
    MarkerPanelWidget* m_markerPanel = nullptr;
    QDockWidget* m_markerDockWidget = nullptr;

    // Таймлайн-гистограмма плотности записей (нижний док, создаётся в коде).
    TimelineHistogramWidget* m_timelinePanel = nullptr;
    QDockWidget* m_timelineDockWidget = nullptr;

    // Панель деталей записи (док, создаётся в коде): выбранная строка целиком —
    // метаданные, извлечённые поля, полный текст логической записи и найденные
    // в нём JSON-фрагменты в отформатированном виде.
    EntryDetailsPanel* m_detailsPanel = nullptr;
    QDockWidget* m_detailsDockWidget = nullptr;

    // Панель статистики по документу (док, создаётся в коде): сводка, уровни,
    // топ повторяющихся сообщений, темп записей и эвристики-аномалии.
    StatisticsPanel* m_statsPanel = nullptr;
    QDockWidget* m_statsDockWidget = nullptr;

    // Панель Search (нижний док, создаётся в коде): запрос (FilterPanelWidget
    // в роли Search) и результаты в одной панели (SearchPanelWidget). Модель
    // результатов и её живое обновление вслед за активной вкладкой —
    // SearchResultsController; здесь view, подпись и навигация: клик прыгает
    // в основном view.
    QDockWidget*       m_searchDockWidget = nullptr;
    SearchPanelWidget* m_searchPanel = nullptr;
    FilterPanelWidget* m_searchQuery = nullptr;   // запрос панели Search
    LogListView* m_searchResultsView = nullptr;
    SearchResultsController* m_searchController = nullptr;
    QLabel*      m_searchResultsStatusLabel = nullptr;
    // Пользователь запустил поиск (Search/Enter/Find All) и не очистил его:
    // тогда выдача следует за сменой вкладки и появлением панели.
    bool         m_searchRequested = false;
    // onSearchRequested сам показывает панель и ищет: её visibilityChanged
    // в этот момент поиск не запускает.
    bool         m_showingSearchDock = false;
    // Правила идущего поиска (привязанные к полям) — для подсветки в логе.
    FilterRuleSet m_searchRules;
    // «Find All» в тулбаре поиска.
    QAction*     m_findAllAction = nullptr;
    // Подавляет авто-прыжок в основном view, когда выбор в панели результатов
    // меняется программно (reset модели при пересборке результатов).
    bool         m_suppressResultNavigation = false;
    // Состояние загрузки активной вкладки → панель статистики (не считать по
    // недогруженному документу). Рвётся в disconnectFromLogView.
    QMetaObject::Connection m_statsLoadingConn;

    // Follow-tail: тогл в тулбаре + синхронизация с view активной вкладки.
    QAction* m_followTailAction = nullptr;
    QMetaObject::Connection m_followTailConn;

    // Спулер потокового ввода (`DendroLog -`); nullptr, если stdin не читаем.
    class StdinSpooler* m_stdinSpooler = nullptr;

    // Тулбар «Filters»: по кнопке-индикатору на каждый вид фильтра.
    // Кнопка зажата = фильтр применён (Time/Text/Markers — к активной
    // вкладке, Fields — глобально). Отжать = Reset этого фильтра,
    // зажать = Apply текущих настроек его панели. Состояние синхронизирует
    // updateFilterStatusButtons().
    QAction* m_timeFilterStatusAction = nullptr;
    QAction* m_textFilterStatusAction = nullptr;
    QAction* m_fieldFilterStatusAction = nullptr;
    QAction* m_markerStatusAction = nullptr;

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

    // Background field re-extraction (schema switch). Workers never write to
    // the entries: new fields are applied on the GUI thread once it finishes,
    // with background filters stopped (see FieldReextraction).
    FieldReextraction* m_fieldExtraction = nullptr;

    // Settings persistence
    void saveSettings();
    void loadSettings();
    void addToRecentFiles(const QString& filePath);
    void updateRecentFilesMenu();

    // Auto-reload support
    QTimer*       m_autoReloadTimer = nullptr;
    QToolButton*  m_reloadButton    = nullptr;  // toolbar button (icon + checkable)
    void applyAutoReloadSettings();
    // Монохромные иконки тулбаров в цвет текста текущей палитры и цветные
    // точки кнопок уровней (при смене темы — заново).
    void refreshToolIcons();
    // Стартовый экран виден, пока нет ни одной вкладки.
    void updateWelcomeVisibility();
    void clearRecentFiles();
    // Размер шрифта лога: steps > 0 крупнее, < 0 мельче, 0 — по умолчанию.
    void changeFontSize(int steps);
    // Снять фильтр уровней / все фильтры (уровни, время, текст) с активной вкладки.
    void showAllLevels();
    void resetAllFiltersOnActiveView();
    // Поля From/To дока Time Filter: применённый фильтр активной вкладки
    // либо весь диапазон её лога (а не «вчера–сегодня»).
    void seedTimeFilterEditors();
    void updateTimeFilterInfo();
    // Подсветка поля быстрого поиска «не найдено».
    void setQuickSearchNotFound(bool notFound);
    // Подпись списка результатов панели Search: статус контроллера + как
    // начать поиск.
    void updateSearchResultsStatus();
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

    // ---- Help menu -------------------------------------------------------
    // Встроенная справка (docs/help_*.md из ресурсов), About и проверка
    // обновлений. Тихая проверка запускается при старте не чаще раза в
    // неделю; ручная (из меню) всегда показывает результат диалогом.
    void setupHelpMenu();
    void showHelp();
    void showAbout();
    void checkForUpdates(bool interactive);
    QDialog*       m_helpDialog = nullptr;
    UpdateChecker* m_updateChecker = nullptr;
    bool           m_updateCheckInteractive = false;

    // Дефолтная раскладка панелей: правые — одной вкладочной группой,
    // таймлайн и результаты поиска — снизу во всю ширину, всё скрыто.
    // Применяется при первом запуске (нет сохранённого состояния окна)
    // и по команде View → Reset Panel Layout.
    void applyDefaultPanelLayout();

    // Setup methods
    void setupStatusBar();
    void setupTimeFilterDockContents(); // New method to set up the new dock
    void setupTextFilterDockContents();
    void setupRowMarkerDock();          // Док Row Highlighters
    void setupTimelineDock();           // Док Timeline (гистограмма по времени)
    void setupSearchDock();             // Док Search: запрос + результаты
    void setupEntryDetailsDock();       // Док Entry Details (текущая запись целиком)
    void setupStatisticsDock();         // Док Statistics (сводка по документу)
    void setupFilterStatusToolbar();    // Тулбар «Filters» (индикаторы-кнопки фильтров)
    void setupDirectoryScanner();
    void setupFieldVisibilityDock();    // New: Log Fields panel

    // Helper methods
    void connectToLogView(LogViewWidget* logView);
    void disconnectFromLogView(LogViewWidget* logView);
    void updateStatusBarDefaultText();
    void updateLogLevelFilterButtons();
    void updateFilterStatusButtons();
    void setFilterLogLvl(LogLevel level, bool add);

    // Factory: creates a LogViewWidget with current global settings pre-applied
    LogViewWidget* createLogViewWidget();

    // ---- Управление вкладками ------------------------------------------------
    // Закрыть одну вкладку: отсоединить её view, снять со стека, удалить и
    // обновить таймер авто-перезагрузки / статусбар. Общий путь для крестика,
    // средней кнопки мыши и пунктов контекстного меню.
    void closeTab(int index);
    void closeOtherTabs(int keepIndex);   // закрыть все, кроме keepIndex
    void closeTabsToRight(int index);     // закрыть вкладки правее index
    void closeAllTabs();
    // Слить файлы вкладки fromIndex в toIndex и закрыть источник (D&D-объединение).
    void mergeTabs(int fromIndex, int toIndex);
    // Слить в keepIndex файлы всех остальных вкладок и закрыть их.
    void mergeAllTabsInto(int keepIndex);
    // Показать файл вкладки в системном файловом менеджере (Explorer /select).
    void revealTabFile(int index);
    // Скопировать в буфер обмена полный путь(и) файлов вкладки.
    void copyTabPath(int index);
    // Пересчитать заголовок и подсказку вкладки по её текущему списку файлов.
    void updateTabLabel(LogViewWidget* view);
    // Построить и показать контекстное меню заголовка вкладки index.
    void onTabContextMenu(int index, const QPoint& globalPos);
    // Apply current field-visibility mask to every open LogViewWidget's model
    void applyFieldVisibilityToAllViews();
    // Apply current pattern to every open LogViewWidget's parser
    void applyPatternToAllViews();
    // Final display/filter refresh after fields have been (re)extracted.
    void finishPatternApplication();
    // Called when the background field re-extraction finishes.
    void onFieldExtractionFinished();
    // Abort any in-flight background field re-extraction and wait for its
    // workers; entries keep their old fields.
    void cancelFieldExtraction();
    // Stop (and wait for) every tab's background filter: they read
    // entry->fields(), which is about to change (invariant 4).
    void cancelFilterJobsOnAllViews();
    // Применить набор правил из конструктора фильтров (+ inline-подсветку
    // совпадений) к АКТИВНОЙ вкладке. Остальные документы не трогаются —
    // у каждой вкладки свой применённый набор.
    void applyTextFiltersToActiveView();
    // ---- Панель Search (неразрушающий поиск) -------------------------------
    // Поиск правилами панели Search по видимому набору активной вкладки и
    // подсветка совпадений. Без запущенного пользователем поиска
    // (m_searchRequested) — ничего не делает.
    void runSearchIntoResults();
    // Опустошить выдачу (модель + подпись). Запрос и флаг m_searchRequested
    // не трогает: так останавливают воркеры перед сменой схемы полей.
    void clearSearchResults();
    // Подсветка совпадений в основном view вкладки: правила её фильтра
    // (галочка Highlight панели Text Filters) плюс, у активной вкладки,
    // идущий поиск (галочка Highlight панели Search).
    void refreshMainViewHighlights(LogViewWidget* view);
    // Клик/навигация по строке в панели результатов → прыжок в основном view.
    void onSearchResultActivated(const QModelIndex& current);
    // Панель Search видима: только тогда выдача держится актуальной полными
    // перезапусками поиска (см. SearchResultsController).
    bool searchResultsLive() const;
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
