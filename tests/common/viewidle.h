#pragma once

// Ожидание покоя представлений по СОСТОЯНИЮ (P-6) — для тестов из виджетов.
// Ядро ожидания (модель не фильтрует, отложенные события доставлены) —
// testdocs::waitIdle / settleIdle в testdocuments.h; здесь — то, что видно
// только у view: отложенный пересчёт после resize, уточнение высот после
// отрисовки, пересчёт левых колонок (LogListView::hasPendingLayout).

#include "LogListView.h"
#include "testdocuments.h"

#include <QApplication>

namespace testdocs {

// Хоть один живой LogListView ещё не доделал отложенную раскладку. Все, а не
// один: у вкладки, панели и сценария их бывает несколько.
inline bool viewsBusy()
{
    const QWidgetList widgets = QApplication::allWidgets();
    for (QWidget* w : widgets)
        if (const auto* view = qobject_cast<const LogListView*>(w); view && view->hasPendingLayout())
            return true;
    return false;
}

// Модель не фильтрует, отложенное доставлено, view молчат. Вместо пауз
// pump(N) и окон тишины settle(): под нагрузкой пауза ничего не
// гарантировала, а без неё стоила десятки секунд.
inline bool settleViews(LogModel& model, const std::function<bool()>& busy = {})
{
    return settleIdle(model, [&] { return viewsBusy() || (busy && busy()); });
}

// То же без модели: после действия с view (выбор строки, клавиша, прокрутка).
inline bool idleViews()
{
    return waitIdle(viewsBusy);
}

} // namespace testdocs
