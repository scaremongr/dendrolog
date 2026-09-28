#include "searchresultscontroller.h"
#include "logmodel.h"

#include <QTimer>

SearchResultsController::SearchResultsController(QObject* parent)
    : QObject(parent)
    , m_results(new LogModel(this))
    , m_refreshTimer(new QTimer(this))
{
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(kRefreshDelayMs);
    connect(m_refreshTimer, &QTimer::timeout, this, [this]() {
        if (m_active && m_stale && m_live)
            runFullSearch();
    });

    connect(m_results, &LogModel::modelFiltered, this, [this](int) { updateCountStatus(); });
    connect(m_results, &LogModel::filterProgress, this, [this](int progress) {
        if (m_active && progress < 100)
            setStatus(tr("Searching visible rows of the active tab... %1%").arg(progress));
    });
    m_status = tr("No search active.");
}

SearchResultsController::~SearchResultsController()
{
    disconnectSource();
}

void SearchResultsController::setSource(LogModel* source)
{
    if (source == m_source)
        return;
    disconnectSource();
    clear();
    m_source = source;
    if (!source)
        return;

    m_sourceConnections << connect(source, &QAbstractItemModel::rowsInserted, this,
        [this](const QModelIndex&, int first, int last) { onSourceRowsInserted(first, last); });
    m_sourceConnections << connect(source, &QAbstractItemModel::dataChanged, this,
        [this](const QModelIndex& topLeft, const QModelIndex& bottomRight, const QList<int>& roles) {
            onSourceDataChanged(topLeft.row(), bottomRight.row(), roles);
        });
    // Всё, что меняет видимый набор иначе, чем дописыванием, — полный поиск.
    m_sourceConnections << connect(source, &QAbstractItemModel::modelReset, this,
        [this]() { requestFullSearch(); });
    m_sourceConnections << connect(source, &QAbstractItemModel::layoutChanged, this,
        [this]() { requestFullSearch(); });
    m_sourceConnections << connect(source, &QAbstractItemModel::rowsRemoved, this,
        [this](const QModelIndex&, int, int) { requestFullSearch(); });
    // К моменту destroyed QPointer уже пуст — setSource(nullptr) тут не годится.
    m_sourceConnections << connect(source, &QObject::destroyed, this, [this]() {
        disconnectSource();
        clear();
    });
}

void SearchResultsController::disconnectSource()
{
    for (const QMetaObject::Connection& connection : std::as_const(m_sourceConnections))
        disconnect(connection);
    m_sourceConnections.clear();
}

void SearchResultsController::search(const FilterRuleSet& rules)
{
    m_rules = rules;
    // Пустой запрос в режиме поиска — нет результатов, а не «пропустить всё»,
    // как трактует пустой набор фильтр.
    if (!m_source || !rules.isActive()) {
        clear();
        return;
    }
    // Все правила испорчены (например, неверный регекс): иначе набор из одних
    // непригодных правил пропустил бы в результаты весь лог.
    if (rules.usableRuleCount() == 0) {
        clear();
        setStatus(tr("Nothing to search: every rule is unusable "
                     "(an invalid regular expression or a column missing from the schema)."));
        return;
    }
    m_active = true;
    runFullSearch();
}

void SearchResultsController::clear()
{
    m_active = false;
    m_stale = false;
    m_refreshTimer->stop();
    m_results->clear();
    setStatus(tr("No search active."));
}

void SearchResultsController::setLive(bool live)
{
    m_live = live;
    if (m_live && m_active && m_stale)
        m_refreshTimer->start();
}

void SearchResultsController::requestFullSearch()
{
    if (!m_active)
        return;
    m_stale = true;
    if (m_live)
        m_refreshTimer->start();
}

void SearchResultsController::runFullSearch()
{
    m_refreshTimer->stop();
    m_stale = false;
    if (!m_active || !m_source)
        return;
    setStatus(tr("Searching visible rows of the active tab..."));
    m_results->searchVisible(*m_source, m_rules);
}

void SearchResultsController::setStatus(const QString& text)
{
    if (text == m_status)
        return;
    m_status = text;
    emit statusChanged(text);
}

void SearchResultsController::updateCountStatus()
{
    // Промежуточное число (резидентный батч посреди фонового поиска) за
    // итог не выдаём: до конца поиска статус остаётся «Searching».
    if (!m_active || m_results->isFiltering())
        return;
    setStatus(tr("%n match(es) in the visible rows of the active tab", "",
                 m_results->rowCount()));
}

void SearchResultsController::mirrorFieldDisplay()
{
    m_results->setAvailableFields(m_source->availableFields());
    m_results->setFieldDisplaySelection(m_source->fieldDisplayFilterEnabled(),
                                        m_source->visibleFieldIndexes());
}

void SearchResultsController::onSourceRowsInserted(int first, int last)
{
    if (!m_active || m_stale || !m_source)
        return;
    if (!m_results->appendSearchRows(*m_source, first, last))
        requestFullSearch();
}

void SearchResultsController::onSourceDataChanged(int first, int last, const QList<int>& roles)
{
    if (!m_active || m_stale || !m_source)
        return;
    if (roles.isEmpty()) {
        // Изменился текст строки: одна — переоцениваем, больше — ищем заново.
        if (first != last || !m_results->refreshSearchRow(*m_source, first))
            requestFullSearch();
        return;
    }
    if (roles.contains(Qt::DisplayRole))
        mirrorFieldDisplay();
    // Остальные роли (IsNewRole, маркеры, плашки файлов) выдачу не меняют.
}
