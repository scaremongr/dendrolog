#include "searchresultsnavigator.h"

#include "LogListView.h"
#include "logmodel.h"
#include "logviewwidget.h"

#include <QItemSelectionModel>

SearchResultsNavigator::SearchResultsNavigator(LogModel* results, LogListView* resultsView,
                                               QObject* parent)
    : QObject(parent)
    , m_results(results)
{
    // До setModel: глушим раньше, чем view начнёт восстанавливать выделение.
    connect(results, &QAbstractItemModel::modelAboutToBeReset,
            this, [this]() { ++m_suppressed; });
    resultsView->setModel(results);
    // После setModel: снимаем, когда обработчик reset самой view отработал.
    connect(results, &QAbstractItemModel::modelReset,
            this, [this]() { m_suppressed = qMax(0, m_suppressed - 1); });
    if (QItemSelectionModel* selection = resultsView->selectionModel())
        connect(selection, &QItemSelectionModel::currentRowChanged,
                this, [this](const QModelIndex& current, const QModelIndex&) { activate(current); });
}

void SearchResultsNavigator::activate(const QModelIndex& current)
{
    if (m_suppressed > 0 || !current.isValid() || !m_results || !m_target)
        return;
    LogViewWidget* tab = m_target();
    if (!tab || !tab->model() || !tab->view())
        return;
    const LogModel::EntryKey key = m_results->keyForRow(current.row());
    if (key.logicalEntryId < 0)
        return;
    // Фокус остаётся в выдаче: стрелками можно листать совпадения дальше.
    if (tab->goToEntry(key.logicalEntryId, key.sourceFile, NavKind::Search))
        ++m_navigations;
}
