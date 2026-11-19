#ifndef SESSIONREPORTDIALOG_H
#define SESSIONREPORTDIALOG_H

#include <QDialog>
#include <QPointer>

#include <functional>

class QLabel;
class QPushButton;
class QTreeWidget;
class SessionController;

// ============================================================================
// Отчёт о восстановлении сессии (R-5): немодальное окно со списком «вкладка,
// файл, что случилось» из SessionController::report() и действием «Locate
// File…» для пропавшего файла (принимается путь, где источник цел).
// Список обновляется по SessionController::reportChanged. Выбор файла —
// QFileDialog; тест подменяет его setFileChooser.
// ============================================================================
class SessionReportDialog : public QDialog {
    Q_OBJECT
public:
    explicit SessionReportDialog(SessionController* controller, QWidget* parent = nullptr);

    // Выбор файла для «Locate File…»: по сохранённому пути; пусто — отмена.
    using FileChooser = std::function<QString(QWidget* parent, const QString& savedPath)>;
    void setFileChooser(FileChooser chooser) { m_chooser = std::move(chooser); }

    void refresh();

private:
    void locateSelected();
    void updateButtons();

    QPointer<SessionController> m_controller;
    QTreeWidget* m_list = nullptr;
    QPushButton* m_locate = nullptr;
    QLabel* m_message = nullptr;
    FileChooser m_chooser;
};

#endif // SESSIONREPORTDIALOG_H
