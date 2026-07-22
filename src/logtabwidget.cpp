#include "logtabwidget.h"
#include "logtabbar.h"

LogTabWidget::LogTabWidget(QWidget* parent)
    : QTabWidget(parent)
{
    m_bar = new LogTabBar(this);
    // setTabBar переносит на новый бар свойства (в т.ч. tabsClosable, которое
    // выставляется из .ui уже после конструктора) и подключает tabCloseRequested
    // к одноимённому сигналу QTabWidget — поэтому средняя кнопка мыши закрывает
    // вкладку через тот же путь, что и штатный крестик.
    setTabBar(m_bar);

    connect(m_bar, &LogTabBar::tabContextMenuRequested,
            this, &LogTabWidget::tabContextMenuRequested);
    connect(m_bar, &LogTabBar::mergeTabsRequested,
            this, &LogTabWidget::mergeTabsRequested);
}
