#ifndef LOGTABWIDGET_H
#define LOGTABWIDGET_H

#include <QTabWidget>
#include <QPoint>

class LogTabBar;

// Тонкая обёртка над QTabWidget: единственная её задача — установить кастомный
// LogTabBar (QTabWidget::setTabBar защищён, поэтому нужен подкласс) и пробросить
// его сигналы наружу. Всё поведение вкладок — в LogTabBar, вся реакция на него
// (закрытие, объединение, контекстное меню) — в MainWindow.
class LogTabWidget : public QTabWidget
{
    Q_OBJECT
public:
    explicit LogTabWidget(QWidget* parent = nullptr);

signals:
    void tabContextMenuRequested(int index, const QPoint& globalPos);
    void mergeTabsRequested(int fromIndex, int toIndex);
    // Вкладка добавлена или убрана (в т.ч. закрыта последняя) — окно
    // переключает стартовый экран и заглушку «нет вкладок».
    void tabCountChanged(int count);

protected:
    void tabInserted(int index) override;
    void tabRemoved(int index) override;

private:
    LogTabBar* m_bar = nullptr;
};

#endif // LOGTABWIDGET_H
