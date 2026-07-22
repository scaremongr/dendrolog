#ifndef LOGTABBAR_H
#define LOGTABBAR_H

#include <QTabBar>
#include <QPoint>

// Кастомный QTabBar для вкладок LogViewWidget: добавляет то, чего нет в
// стандартном баре и что ожидаешь от «взрослого» редактора.
//
//  * Средняя кнопка мыши по вкладке — закрыть её (эмитит tabCloseRequested,
//    который QTabWidget пробрасывает наружу как обычный запрос закрытия).
//  * Контекстное меню по заголовку — сигнал tabContextMenuRequested; само меню
//    строит MainWindow (у него есть контекст: файлы вкладки, соседи и т.п.).
//  * Drag & drop заголовков: перетаскивание между вкладками переупорядочивает
//    их (moveTab, QTabWidget синхронизирует страницы через tabMoved), а бросок
//    на центр другой вкладки эмитит mergeTabsRequested — MainWindow сливает
//    документы источника в приёмник.
//
// Собственный разбор мыши (а не встроенный setMovable) нужен именно чтобы в
// одном жесте различать «переставить» и «объединить»: во время drag рисуется
// либо линия-вставка (reorder), либо подсветка целевой вкладки (merge).
class LogTabBar : public QTabBar
{
    Q_OBJECT
public:
    explicit LogTabBar(QWidget* parent = nullptr);

signals:
    // Правый клик по вкладке index; globalPos — где показать меню.
    void tabContextMenuRequested(int index, const QPoint& globalPos);
    // Бросок вкладки fromIndex на центр вкладки toIndex — запрос объединения.
    void mergeTabsRequested(int fromIndex, int toIndex);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    // Пересчитать цель drag под курсором: заполняет m_mergeTarget ИЛИ
    // m_insertIndex (второй — слот вставки при переупорядочивании).
    void updateDropTarget(const QPoint& pos);
    void resetDragState();

    QPoint m_pressPos;              // точка нажатия ЛКМ (для порога старта drag)
    int    m_pressIndex   = -1;     // вкладка под нажатием ЛКМ
    int    m_middleIndex  = -1;     // вкладка под нажатием средней кнопки
    bool   m_dragging     = false;  // порог перетаскивания пройден
    int    m_mergeTarget  = -1;     // цель объединения (подсветка), либо -1
    int    m_insertIndex  = -1;     // слот вставки при переупорядочивании, либо -1
};

#endif // LOGTABBAR_H
