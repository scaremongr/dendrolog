#ifndef FULLLOGCONTROLLER_H
#define FULLLOGCONTROLLER_H

#include "filterruleset.h"
#include "logentry.h"
#include "recordid.h"

#include <QDateTime>
#include <QObject>
#include <QSet>

#include <functional>
#include <optional>

class LogModel;
class LogListView;

// ============================================================================
// FullLogController — «Show in Full Log» и возврат одним действием (R-3).
//
// Лог отфильтрован (уровни, время, текстовые правила), пользователь стоит на
// строке и хочет увидеть, что было вокруг неё в полном логе, а потом
// вернуться к отфильтрованному виду. Контроллер одной вкладки: модель + view,
// без окна (владелец — LogViewWidget; тесты строят его на голой паре
// модель / view).
//
// Показ: фильтры модели, скрывающие строки, снимаются и запоминаются
// (приостановлены); маркеры строк остаются. Модель при этом честно без
// фильтров — индикаторы окна показывают действующее состояние вкладки.
// Выбранная пользователем строка остаётся выбранной и на том же месте
// экрана; строки не выбрано — на месте остаётся верхняя строка экрана.
// Выбор, скрытый фильтром (якорь инварианта 10 есть, а строки нет), view
// возвращает сам — по центру, как после любого reset.
//
// Возврат — ровно прежние фильтры. Позиция:
//   • выбранная сейчас строка проходит фильтр — она остаётся выбранной на
//     своём месте экрана (восстановление места — не переход);
//   • не проходит (стоим на строке контекста) или ничего не выбрано —
//     строка, с которой уходили, на том месте экрана, где она была перед
//     уходом. Если её выбирал пользователь, а уходим со строки контекста, —
//     это переход NavKind::FullLog: Alt+← ведёт к ближайшей видимой к строке
//     контекста.
//
// Позиция ставится, когда фильтр закончился (у индексного бэкенда и у
// большого резидентного он фоновый), и уступает пользователю, успевшему
// выбрать строку или прокрутить (userActionCount), и follow-tail. Показ с
// выбранной строкой follow-tail снимает (как goToRow): пользователь просил
// эту строку; возврат включённый follow-tail уважает — вид у хвоста.
// Повторное нажатие до конца фильтра берёт ещё не поставленную позицию, а не
// полуготовый экран.
//
// Правка фильтра мимо контроллера (LogModel::filterSettingsChanged: кнопки
// уровней, Apply / Reset, таймлайн, «Restore Filters», applyState) показ
// заканчивает: новый фильтр — правда, приостановленные отбрасываются.
//
// Только GUI-поток. Данные не трогает: всё через сеттеры фильтров модели и
// LogListView::setViewAnchor / goToRowAt (инвариант 10).
// ============================================================================
class FullLogController : public QObject {
    Q_OBJECT
public:
    struct Filters {
        QSet<LogLevel> levels;
        QDateTime from, to;
        FilterRuleSet rules;
    };

    FullLogController(LogModel& model, LogListView& view, QObject* parent = nullptr);

    // Полный лог показан, фильтры приостановлены.
    bool isActive() const { return m_suspended.has_value(); }
    // Есть что показывать: показа нет, и фильтры модели скрывают строки.
    bool canShow() const;
    // Приостановленные фильтры (nullopt — показа нет): их снимает
    // captureState вкладки вместо пустых фильтров модели.
    const std::optional<Filters>& suspendedFilters() const { return m_suspended; }

    // Показать полный лог. row >= 0 — строка под курсором (контекстное меню):
    // она становится выбором пользователя на своём месте экрана. false —
    // показ уже идёт или фильтров, скрывающих строки, нет.
    bool show(int row = -1);
    // Показать полный лог со строкой key (скрыта фильтром: переход к
    // закладке) — выбранной, по центру экрана.
    bool showRecord(const RecordKey& key);
    // Вернуть приостановленные фильтры; false — показа нет.
    bool returnToFiltered();
    // show / returnToFiltered по состоянию.
    bool toggle(int row = -1);
    // Закончить показ, оставив полный лог: приостановленные фильтры
    // отбрасываются (Reset всех фильтров вкладки).
    void discard();

    // Позиция ждёт конца фильтра.
    bool isPlacementPending() const { return m_pending.armed; }

    // Перепривязка текстовых правил к схеме перед возвратом: схема могла
    // смениться, пока фильтры были приостановлены.
    void setRuleBinder(std::function<void(FilterRuleSet&)> binder) { m_binder = std::move(binder); }

signals:
    void activeChanged(bool active);

private:
    struct Place {
        RecordKey key;
        int offset = 0;          // верх строки от верха экрана
        bool selected = false;   // выбор пользователя (иначе — верх экрана)
        bool centered = false;   // по центру экрана (offset не задан)
        bool isValid() const { return key.isValid(); }
    };
    struct Pending {
        Place primary;           // поставить, если строка видна
        Place fallback;          // иначе — эту (возврат: строка, с которой ушли)
        quint64 userActions = 0;
        bool armed = false;
    };

    // Место вида сейчас: ещё не поставленная позиция (пользователь вид не
    // трогал), иначе выбранная строка. deferHiddenSelection — выбор скрыт
    // фильтром: место пустое, view вернёт выбор сам.
    Place currentPlace(bool deferHiddenSelection) const;
    Place topPlace() const;
    Place placeOfRow(int row, bool selected) const;
    void setModelFilters(const Filters& filters);
    void tryPlace();
    void place(int row, const Place& p, bool selected);
    void onFilterSettingsChanged();

    LogModel& m_model;
    LogListView& m_view;
    std::optional<Filters> m_suspended;
    Place m_origin;              // откуда ушли в полный лог
    Pending m_pending;
    bool m_settingFilters = false;
    std::function<void(FilterRuleSet&)> m_binder;
};

#endif // FULLLOGCONTROLLER_H
