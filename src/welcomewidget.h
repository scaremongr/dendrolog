#ifndef WELCOMEWIDGET_H
#define WELCOMEWIDGET_H

#include <QKeySequence>
#include <QStringList>
#include <QWidget>

class QLabel;
class QPushButton;
class QVBoxLayout;

// ============================================================================
// WelcomeWidget — стартовый экран центральной области, пока нет ни одной
// вкладки. Вместо пустой рамки QTabWidget подсказывает, с чего начать:
// открыть файлы, просканировать каталог, бросить файлы в окно, открыть
// недавний файл одной ссылкой.
//
// Сам ничего не открывает — только сигналит MainWindow. Бросок файлов
// обрабатывает окно: виджет drop не принимает, и Qt передаёт событие
// ближайшему родителю, который его ждёт.
// ============================================================================
class WelcomeWidget : public QWidget {
    Q_OBJECT
public:
    explicit WelcomeWidget(QWidget* parent = nullptr);

    // Недавние файлы, новые первыми; пустой список прячет секцию.
    void setRecentFiles(const QStringList& paths);
    // Подсказки с текущими сочетаниями клавиш (они настраиваются).
    void setShortcutHints(const QKeySequence& open, const QKeySequence& find);

signals:
    void openFilesRequested();
    void scanDirectoryRequested();
    void recentFileRequested(const QString& path);
    void clearRecentRequested();

protected:
    void changeEvent(QEvent* event) override;

private:
    void rebuildRecent();
    void updateHints();
    void applyColors();

    QLabel*      m_subtitle = nullptr;
    QLabel*      m_dropHint = nullptr;
    QPushButton* m_openButton = nullptr;
    QPushButton* m_scanButton = nullptr;
    QWidget*     m_recentSection = nullptr;
    QLabel*      m_recentHeader = nullptr;
    QLabel*      m_clearLink = nullptr;
    QVBoxLayout* m_recentList = nullptr;
    QLabel*      m_hints = nullptr;

    QStringList  m_recentFiles;
    QKeySequence m_openSeq;
    QKeySequence m_findSeq;
};

#endif // WELCOMEWIDGET_H
