#ifndef DIRECTORYSCANNERPANEL_H
#define DIRECTORYSCANNERPANEL_H

#include <QWidget>
#include <QDateTime>

class DirectoryScanner;
class CardFrame;

class QTreeWidget;
class QLineEdit;
class QToolButton;
class QPushButton;
class QCheckBox;
class QDateTimeEdit;
class QLabel;
class QProgressBar;

// ============================================================================
// DirectoryScannerPanel — содержимое дока «Directory Scanner».
//
// Объединяет в едином виджете:
//   • закруглённую карточку-заголовок (CardFrame) с управляющими элементами
//     в стиле панели Text Filters — кнопки сканирования, фильтр по содержимому
//     (текст / регэксп / регистр) и фильтр по диапазону дат;
//   • дерево результатов (QTreeWidget), которым управляет внутренний
//     DirectoryScanner (ленивое наполнение, параллельный разбор файлов).
//
// Наружу отдаёт только высокоуровневые сигналы; о внутренних layout'ах и
// контролах MainWindow ничего не знает (тот же подход, что у FilterPanelWidget).
// ============================================================================

class DirectoryScannerPanel : public QWidget
{
    Q_OBJECT
public:
    explicit DirectoryScannerPanel(QWidget* parent = nullptr);

    // Доступ к контроллеру: MainWindow задаёт ему расширения / conversion
    // pattern и подключается к fileActivated / filesActivated.
    DirectoryScanner* scanner() const { return m_scanner; }

    // Обновляет подпись пути, сбрасывает UI фильтров и запускает скан.
    void scanDirectory(const QString& path);

signals:
    // Пользователь нажал «Scan Directory…» — MainWindow покажет диалог выбора.
    void scanRequested();
    // Пользователь нажал «Exts…» — MainWindow откроет настройки расширений.
    void configureExtensionsRequested();

private:
    void buildUi();
    void onApplyClicked();
    void onResetClicked();
    void onDateBoundsChanged(const QDateTime& earliest, const QDateTime& latest);
    void onContentProgress(int done, int total);
    void onContentFinished(int matched, int total);
    void setDateEditorsEnabled(bool on);

    DirectoryScanner* m_scanner = nullptr;
    QTreeWidget*      m_tree = nullptr;

    // Header card controls
    CardFrame*    m_card = nullptr;
    QToolButton*  m_scanButton = nullptr;
    QToolButton*  m_extsButton = nullptr;
    QToolButton*  m_settingsToggle = nullptr;  // ⚙ collapse/expand the filter area
    QLabel*       m_pathLabel = nullptr;

    QWidget*      m_filterArea = nullptr;      // collapsible filter controls
    QLineEdit*    m_contentEdit = nullptr;
    QToolButton*  m_regexButton = nullptr;
    QToolButton*  m_caseButton = nullptr;

    QCheckBox*    m_dateEnable = nullptr;
    QDateTimeEdit* m_dateFrom = nullptr;
    QDateTimeEdit* m_dateTo = nullptr;
    QToolButton*  m_dateResetButton = nullptr;

    QPushButton*  m_applyButton = nullptr;
    QPushButton*  m_resetButton = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel*       m_statusLabel = nullptr;

    // True once the user manually edits a date editor, so auto-seeding from the
    // scan bounds stops overwriting their choice.
    bool m_userTouchedDates = false;
    // Guards the dateTimeChanged handlers while we set editors programmatically.
    bool m_seeding = false;
};

#endif // DIRECTORYSCANNERPANEL_H
