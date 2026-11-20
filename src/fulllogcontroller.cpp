#include "fulllogcontroller.h"

#include "LogListView.h"
#include "logmodel.h"

#include <utility>

FullLogController::FullLogController(LogModel& model, LogListView& view, QObject* parent)
    : QObject(parent)
    , m_model(model)
    , m_view(view)
{
    connect(&m_model, &LogModel::filterSettingsChanged, this, &FullLogController::onFilterSettingsChanged);
    // Фоновый фильтр закончился (и любая другая перефильтрация): позиция,
    // ждущая видимого набора, ставится сейчас.
    connect(&m_model, &LogModel::modelFiltered, this, [this]() { tryPlace(); });
}

bool FullLogController::canShow() const
{
    return !isActive() && m_model.hasFilters();
}

FullLogController::Place FullLogController::placeOfRow(int row, bool selected) const
{
    Place p;
    if (row < 0 || row >= m_model.rowCount())
        return p;
    p.key = m_model.keyAt(row);
    p.selected = selected;
    const QRect rect = m_view.visualRect(m_model.index(row, 0));
    // Строка вне экрана — по центру, как её поставил бы goToRow.
    if (rect.bottom() < 0 || rect.top() >= m_view.viewport()->height())
        p.centered = true;
    else
        p.offset = rect.top();
    return p;
}

FullLogController::Place FullLogController::topPlace() const
{
    // Верхняя строка экрана (видна хотя бы частично) и её смещение.
    Place p;
    if (m_model.rowCount() <= 0)
        return p;
    const QModelIndex top = m_view.indexAt(QPoint(1, 0));
    const int row = top.isValid() ? top.row() : 0;
    p.key = m_model.keyAt(row);
    p.offset = m_view.visualRect(m_model.index(row, 0)).top();
    return p;
}

FullLogController::Place FullLogController::currentPlace(bool deferHiddenSelection) const
{
    // Позиция ещё не поставлена, а пользователь вид не трогал: экран
    // полуготовый (фильтр в фоне), правда — то, что будет поставлено.
    if (m_pending.armed && m_view.userActionCount() == m_pending.userActions)
        return m_pending.primary.isValid() ? m_pending.primary : m_pending.fallback;
    const int selected = m_view.userSelectedRow();
    if (selected >= 0)
        return placeOfRow(selected, true);
    if (deferHiddenSelection && m_view.selectedRecordId() >= 0)
        return {}; // выбор скрыт фильтром — view вернёт его сам
    return topPlace();
}

void FullLogController::setModelFilters(const Filters& filters)
{
    const bool was = std::exchange(m_settingFilters, true);
    m_model.setFilters(filters.levels, filters.from, filters.to, filters.rules, filters.hiddenFiles);
    m_settingFilters = was;
}

bool FullLogController::show(int row)
{
    if (!canShow())
        return false;
    // Возврат ещё не поставил позицию (нажали снова до конца фильтра) —
    // исходная строка та же, что была.
    const bool returnPending = m_pending.armed && m_view.userActionCount() == m_pending.userActions;
    const Place returnOrigin = returnPending ? m_pending.fallback : Place();

    Place anchor = row >= 0 ? placeOfRow(row, true) : currentPlace(/*deferHiddenSelection=*/true);
    Place origin = returnOrigin.isValid() ? returnOrigin
                 : anchor.isValid()       ? anchor
                                          : topPlace();

    Filters suspended;
    suspended.levels = m_model.logLevelFilter();
    suspended.from = m_model.startTimeFilter();
    suspended.to = m_model.endTimeFilter();
    suspended.rules = m_model.filterRules();
    suspended.hiddenFiles = m_model.hiddenFiles();
    m_suspended = suspended;
    m_origin = origin;
    // Строку выбрал пользователь — он хочет видеть её, а не хвост: как у
    // goToRow, follow-tail снимается (иначе позиция уступила бы ему).
    if (anchor.selected && m_view.followTail())
        m_view.setFollowTail(false);
    m_pending = {};
    m_pending.primary = anchor;
    m_pending.userActions = m_view.userActionCount();
    m_pending.armed = anchor.isValid();

    setModelFilters({});
    emit activeChanged(true);
    tryPlace();
    return true;
}

bool FullLogController::showRecord(const RecordKey& key)
{
    if (!key.isValid() || !canShow())
        return false;
    Place target;
    target.key = key;
    target.selected = true;
    target.centered = true;
    Filters suspended;
    suspended.levels = m_model.logLevelFilter();
    suspended.from = m_model.startTimeFilter();
    suspended.to = m_model.endTimeFilter();
    suspended.rules = m_model.filterRules();
    suspended.hiddenFiles = m_model.hiddenFiles();
    // Возврат — туда, где пользователь стоит сейчас (ближайшая видимая).
    const Place origin = currentPlace(false);
    m_suspended = suspended;
    m_origin = origin;
    if (m_view.followTail())
        m_view.setFollowTail(false);
    m_pending = {};
    m_pending.primary = target;
    m_pending.userActions = m_view.userActionCount();
    m_pending.armed = true;
    setModelFilters({});
    emit activeChanged(true);
    tryPlace();
    return true;
}

bool FullLogController::returnToFiltered()
{
    if (!isActive())
        return false;
    Place primary;
    if (m_pending.armed && m_view.userActionCount() == m_pending.userActions) {
        // Показ ещё не поставил позицию — вернуть то, что он ставил бы.
        primary = m_pending.primary;
    } else {
        const int selected = m_view.userSelectedRow();
        if (selected >= 0)
            primary = placeOfRow(selected, true);
    }
    Filters filters = *m_suspended;
    m_suspended.reset();
    if (m_binder)
        m_binder(filters.rules);
    m_pending = {};
    m_pending.primary = primary;
    m_pending.fallback = std::exchange(m_origin, {});
    m_pending.userActions = m_view.userActionCount();
    m_pending.armed = m_pending.primary.isValid() || m_pending.fallback.isValid();

    setModelFilters(filters);
    emit activeChanged(false);
    tryPlace();
    return true;
}

bool FullLogController::toggle(int row)
{
    return isActive() ? returnToFiltered() : show(row);
}

void FullLogController::discard()
{
    m_pending = {};
    m_origin = {};
    if (!m_suspended)
        return;
    m_suspended.reset();
    emit activeChanged(false);
}

void FullLogController::onFilterSettingsChanged()
{
    if (m_settingFilters)
        return;
    // Пользователь сам задал фильтр: он и правда, позицию не ставим.
    discard();
}

void FullLogController::place(int row, const Place& p, bool selected)
{
    int offset = p.offset;
    if (p.centered) {
        const int rowH = m_view.visualRect(m_model.index(row, 0)).height();
        offset = qMax(0, (m_view.viewport()->height() - rowH) / 2);
    }
    m_view.setViewAnchor({ row, offset, selected });
}

void FullLogController::tryPlace()
{
    if (!m_pending.armed)
        return;
    // Инвариант 10: пользователь успел выбрать строку или прокрутить, либо
    // вид идёт за хвостом — его выбор главнее.
    if (m_view.userActionCount() != m_pending.userActions || m_view.followTail()) {
        m_pending = {};
        return;
    }
    if (m_model.isFiltering())
        return; // видимый набор ещё строится
    const Pending p = std::exchange(m_pending, {});

    if (p.primary.isValid()) {
        const int row = m_model.rowForKey(p.primary.key);
        if (row >= 0) {
            place(row, p.primary, p.primary.selected);
            return;
        }
    }
    const Place& target = p.fallback.isValid() ? p.fallback : p.primary;
    if (!target.isValid())
        return;
    int row = m_model.rowForKey(target.key);
    const bool visible = row >= 0;
    if (!visible)
        row = m_model.nearestRowForKey(target.key);
    if (row < 0)
        return;
    if (visible && target.selected && &target == &p.fallback && p.primary.isValid()
        && p.primary.selected) {
        // Уходим со строки контекста к строке, с которой уходили в полный
        // лог: это переход (Alt+← — обратно к контексту, к ближайшей видимой).
        const int from = m_model.nearestRowForKey(p.primary.key);
        int offset = target.offset;
        if (target.centered) {
            const int rowH = m_view.visualRect(m_model.index(row, 0)).height();
            offset = qMax(0, (m_view.viewport()->height() - rowH) / 2);
        }
        m_view.goToRowAt(row, offset, NavKind::FullLog, from);
        return;
    }
    place(row, target, target.selected && visible);
}
