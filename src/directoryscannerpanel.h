#ifndef DIRECTORYSCANNERPANEL_H
#define DIRECTORYSCANNERPANEL_H

#include <QWidget>
#include <QDateTime>

#include "directoryscanner.h"
#include "sourceunpacker.h"

#include <memory>

class CardFrame;
class SpoolDir;
class SpoolItem;

class QTreeWidget;
class QLineEdit;
class QToolButton;
class QPushButton;
class QCheckBox;
class QDateTimeEdit;
class QLabel;
class QProgressBar;
class QShowEvent;

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
//
// Архив (F-2, шаги 4–5, docs/design/F-2.md «Архивы с набором файлов»): zip,
// tar, `.tar.gz`, `.tgz` извлекается во временный каталог
// (SourceUnpacker::unpackArchive, формат — по содержимому; только имена
// логов по расширениям сканера и их .gz), каталог показывается как обычная
// папка с подписью архива и кнопкой «назад» к прежнему корню (папке до
// первого архива). Каталог регистрируется в ArchiveRegistry: файлы оттуда
// открываются вкладкой как члены архива. Возврат, другой архив или скан папки
// отпускают каталог (удаляется в фоне). Ошибка архива — statusMessage,
// показанное остаётся.
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

    // ---- Архив (F-2, шаг 4) -------------------------------------------------
    // Извлечь архив (zip, tar) и показать его; идёт в фоне (прогресс — в
    // подписи пути).
    void openArchive(const QString& archivePath);
    // Показанный архив (пусто — показана папка или ничего).
    QString archivePath() const { return m_archivePath; }
    bool isExtractingArchive() const { return !m_pendingArchive.isEmpty(); }
    // Вернуться к папке, показанной до архива (кнопка «назад»); пока идёт
    // извлечение — отменить его (показанное остаётся).
    void returnFromArchive();
    // Корень временных файлов (по умолчанию AppSettings::spoolRoot; тесты —
    // свой каталог).
    void setSpoolRoot(const QString& root);

    ~DirectoryScannerPanel() override;

signals:
    // Пользователь нажал «Scan Directory…» — MainWindow покажет диалог выбора.
    void scanRequested();
    // Пользователь нажал «Exts…» — MainWindow откроет настройки расширений.
    void configureExtensionsRequested();
    // Сообщение для строки статуса окна (архив не открылся, имена пропущены).
    void statusMessage(const QString& message);

protected:
    // Пока док скрыт, авто-обновление только копит «есть изменения»; при
    // появлении панели подтягиваем их сразу, не дожидаясь очередного тика.
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void buildUi();
    void onApplyClicked();
    void onResetClicked();
    void onDateBoundsChanged(const QDateTime& earliest, const QDateTime& latest);
    void onContentProgress(qint64 bytesDone, qint64 bytesTotal, int filesDone, int filesTotal);
    void onContentFinished(int matched, int total);
    void onRescanFinished(int added, int removed, int updated);
    void setDateEditorsEnabled(bool on);
    void showPath(const QString& path);
    void showCurrentPath();
    void releaseArchive();
    void cancelExtraction();
    void onArchiveExtracted(const SourceUnpacker::Result& result);

    // Кнопка ⟳ несёт три состояния обновления (вручную / авто / отключено);
    // они же живут в AppSettings, чтобы переживать перезапуск.
    void showRefreshMenu(const QPoint& globalPos);
    void setRefreshMode(DirectoryScanner::RefreshMode mode);
    void promptRefreshInterval();
    void updateRefreshButton();

    DirectoryScanner* m_scanner = nullptr;
    QTreeWidget*      m_tree = nullptr;

    // Header card controls
    CardFrame*    m_card = nullptr;
    QToolButton*  m_scanButton = nullptr;
    QToolButton*  m_refreshButton = nullptr;
    QToolButton*  m_extsButton = nullptr;
    QToolButton*  m_settingsToggle = nullptr;  // ⚙ collapse/expand the filter area
    QToolButton*  m_backButton = nullptr;      // ← из архива к прежней папке (F-2)
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

    // Архив (F-2, шаг 4).
    QString m_spoolRoot;
    std::shared_ptr<SpoolDir> m_spoolDir;
    SourceUnpacker* m_unpacker = nullptr;
    quint64 m_job = 0;
    QString m_pendingArchive;                    // извлекается
    std::shared_ptr<SpoolItem> m_pendingItem;    // его каталог
    QString m_archivePath;                       // показан
    std::shared_ptr<SpoolItem> m_archiveItem;    // каталог показанного архива
    QString m_folderBeforeArchive;               // куда ведёт «назад»
};

#endif // DIRECTORYSCANNERPANEL_H
