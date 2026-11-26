#include "fieldschemacontroller.h"

#include "documentstate.h"
#include "fieldreextraction.h"
#include "filterruleset.h"
#include "logmodel.h"
#include "logpattern.h"
#include "logviewwidget.h"
#include "sessioncontroller.h"

#include <QSettings>

#include <memory>

FieldSchemaController::FieldSchemaController(QObject* parent)
    : QObject(parent)
{
}

FieldSchemaController::~FieldSchemaController()
{
    // Воркеры не должны пережить записи, которые читают; сигналов владельцу,
    // которого уже разбирают, — нет.
    if (m_reextraction) {
        m_reextraction->cancel();
        delete m_reextraction;
        m_reextraction = nullptr;
    }
}

QList<LogViewWidget*> FieldSchemaController::views() const
{
    return m_window.views ? m_window.views() : QList<LogViewWidget*>();
}

QStringList FieldSchemaController::listOrder() const
{
    QStringList order;
    order.reserve(m_list.size());
    for (const auto& e : m_list)
        order << e.name;
    return order;
}

// =============================================================================
// Каталог
// =============================================================================

void FieldSchemaController::load(QSettings& s)
{
    m_extraction = s.value("fieldFilterEnabled", false).toBool();
    m_visible = s.value("fieldVisibleNames").toStringList();
    const int legacyMask = s.value("fieldVisibilityMask", -1).toInt();

    m_active = s.value("conversionPattern").toString();
    // Schemas are loaded from <configDir>/patterns/*.json; the INI keeps only
    // their display order, by name.
    const QStringList order = s.value("schemaOrder").toStringList();
    m_list = SchemaStore::loadAll(order);

    // One-time migration of the old INI-stored schema list into files.
    if (m_list.isEmpty()) {
        const QStringList names  = s.value("patternNames").toStringList();
        const QStringList values = s.value("patternValues").toStringList();
        const int cnt = qMin(names.size(), values.size());
        m_list.reserve(cnt);
        for (int i = 0; i < cnt; ++i)
            m_list.append({names[i], values[i], {}, {}});
        // Even older single-string list.
        if (m_list.isEmpty()) {
            const QStringList old = s.value("conversionPatternList").toStringList();
            for (const auto& p : old)
                if (!p.trimmed().isEmpty())
                    m_list.append({p.trimmed(), p.trimmed(), {}, {}});
        }
        if (!m_list.isEmpty()) {
            SchemaStore::sync(m_list);
            s.remove("patternNames");
            s.remove("patternValues");
            s.remove("conversionPatternList");
        }
    }
    // Built-in schemas (Logback, nginx, syslog, JSON Lines, …) — after the
    // migration above, which only runs on an empty folder. Never overwrites
    // the user's files; a corrected built-in replaces only an untouched copy,
    // and the active schema follows it.
    const BuiltinSchemas::InstallReport builtins = SchemaStore::installBuiltins();
    if (builtins.changed()) {
        for (const auto& replaced : builtins.replaced) {
            if (m_active == replaced.first)
                m_active = replaced.second;
        }
        m_list = SchemaStore::loadAll(order);
    }
    // No active schema (the default — extraction off) shows as none.
    m_activeIndex = -1;
    for (int i = 0; i < m_list.size(); ++i) {
        if (m_list[i].schema == m_active) {
            m_activeIndex = i;
            break;
        }
    }

    // Выбор колонок версий до имён — битовая маска по индексам схемы.
    if (m_visible.isEmpty() && legacyMask >= 0) {
        const QStringList legacyFieldNames = LogPattern(m_active).fieldNames();
        for (int i = 0; i < legacyFieldNames.size() && i < 31; ++i) {
            if (legacyMask & (1 << i))
                m_visible.append(legacyFieldNames[i]);
        }
    }

    emit catalogueChanged();
    setFieldNames(LogPattern(m_active).fieldNames());
    applyFieldVisibility();
}

void FieldSchemaController::save(QSettings& s) const
{
    s.setValue("fieldFilterEnabled", m_extraction);
    s.setValue("fieldVisibleNames", m_visible);
    s.setValue("conversionPattern", m_active);
    // Schemas themselves live as files in <configDir>/patterns/ (see
    // SchemaStore); the INI only remembers their display order, keyed by name.
    s.setValue("schemaOrder", listOrder());
}

void FieldSchemaController::reload(const QString& extraName)
{
    QStringList order = listOrder();
    if (!extraName.isEmpty())
        order << extraName;
    const int oldIndex = m_activeIndex;
    const QString oldName = oldIndex >= 0 && oldIndex < m_list.size() ? m_list[oldIndex].name
                                                                      : QString();
    m_list = SchemaStore::loadAll(order);
    // Пункт активной схемы — тот же по имени и схеме, иначе первый с той же
    // схемой; нет такого — схема действует без пункта.
    m_activeIndex = -1;
    for (int i = 0; i < m_list.size(); ++i) {
        if (m_list[i].schema != m_active)
            continue;
        if (m_activeIndex < 0 || (!oldName.isEmpty() && m_list[i].name == oldName))
            m_activeIndex = i;
    }
    if (m_active.isEmpty())
        m_activeIndex = -1;
}

void FieldSchemaController::reloadCatalogue()
{
    reload(QString());
    emit catalogueChanged();
}

QString FieldSchemaController::replaceCatalogue(const SchemaStore::SchemaList& list,
                                                const QString& chosen, int chosenIndex)
{
    m_list = list;
    // Persist the (possibly edited) list back to the patterns folder.
    QString syncError;
    if (SchemaStore::sync(m_list, &syncError))
        syncError.clear();

    if (!chosen.isEmpty()) {
        m_active = chosen;
        m_activeIndex = chosenIndex >= 0 && chosenIndex < m_list.size() ? chosenIndex : -1;
        emit catalogueChanged();
        applyToAllViews();
        return syncError;
    }
    // Без «Use Selected» активная схема остаётся и уже применена; её пункт —
    // первый с той же схемой (правка в редакторе могла её изменить — тогда
    // она действует без пункта).
    m_activeIndex = -1;
    for (int i = 0; i < m_list.size(); ++i) {
        if (m_list[i].schema == m_active) {
            m_activeIndex = i;
            break;
        }
    }
    emit catalogueChanged();
    return syncError;
}

// =============================================================================
// Активная схема
// =============================================================================

QString FieldSchemaController::activeSchemaName() const
{
    return SessionController::schemaNameFor(m_list, m_activeIndex, m_active);
}

void FieldSchemaController::selectSchema(int index)
{
    if (index >= 0 && index < m_list.size()) {
        m_activeIndex = index;
        m_active = m_list[index].schema;
    } else {
        m_activeIndex = -1;
        m_active.clear();
    }
    // Combo selection change must immediately rebind parser schema and field list.
    applyToAllViews();
}

QString FieldSchemaController::applySessionSchema(const SessionState& session)
{
    // Сессия без схемы схему окна не трогает: открытые вкладки не теряют поля.
    if (!session.hasSchema())
        return {};
    // Каталог схем — источник правды, и его могли изменить с прошлой загрузки
    // (sync ниже удалил бы файлы, которых нет в устаревшем списке).
    reload(QString());
    const SessionController::SchemaChoice choice =
        SessionController::chooseSchema(m_list, session.schemaName, session.schema);
    int index = choice.index;
    QString error;
    if (choice.added) { // нет в каталоге — добавить под именем с пометкой
        m_list.append(choice.newEntry);
        if (SchemaStore::sync(m_list, &error))
            error.clear();
        reload(choice.newEntry.name);
        index = -1;
        for (int i = 0; i < m_list.size(); ++i)
            if (m_list[i].name == choice.newEntry.name)
                index = i;
    }
    // Не записалась в каталог — схема всё равно действует, без пункта в списке.
    m_active = index >= 0 ? m_list[index].schema : session.schema;
    m_activeIndex = index;
    m_extraction = session.fieldExtraction;
    m_visible = session.visibleFields;
    emit catalogueChanged();
    applyToAllViews();
    return error;
}

// =============================================================================
// «Filter blocks» и колонки
// =============================================================================

void FieldSchemaController::setExtractionEnabled(bool enabled)
{
    if (enabled == m_extraction)
        return;
    m_extraction = enabled;
    // Включает/выключает извлечение полей, переизвлекает поля уже загруженных
    // записей и перепривязывает колоночные правила (галочка «Filter blocks»
    // управляет строгой фильтрацией по колонкам).
    applyToAllViews();
}

QVector<int> FieldSchemaController::visibleFieldIndexes() const
{
    QVector<int> indexes;
    indexes.reserve(m_fieldNames.size());
    for (int i = 0; i < m_fieldNames.size(); ++i) {
        if (m_visible.contains(m_fieldNames[i]))
            indexes.push_back(i);
    }
    return indexes;
}

void FieldSchemaController::setVisibleFieldNames(const QStringList& names)
{
    m_visible.clear();
    for (const QString& name : m_fieldNames)
        if (names.contains(name))
            m_visible << name;
    emit fieldsChanged();
    applyFieldVisibility();
}

void FieldSchemaController::setFieldNames(const QStringList& names)
{
    m_fieldNames = names;
    // Выбранные имена, которые есть в новой схеме; ни одного (или выбор
    // пуст) — все колонки.
    QStringList kept;
    for (const QString& name : m_fieldNames)
        if (m_visible.contains(name))
            kept << name;
    m_visible = kept.isEmpty() ? m_fieldNames : kept;
    emit fieldsChanged();
}

void FieldSchemaController::bindRules(FilterRuleSet& rules) const
{
    rules.bindFields(m_fieldNames, m_extraction);
}

void FieldSchemaController::applyFieldVisibility()
{
    // Посреди фонового расчёта колонки новой схемы легли бы на поля старой
    // (и Save View As читал бы их во время замены) — показ включит конец
    // расчёта.
    const bool enabled = fieldDisplayEnabled() && !m_reextraction;
    const QVector<int> indexes = enabled ? visibleFieldIndexes() : QVector<int>();
    for (LogViewWidget* lv : views()) {
        if (lv && lv->model())
            lv->model()->setFieldDisplaySelection(enabled, indexes);
    }
}

// =============================================================================
// Вкладки
// =============================================================================

void FieldSchemaController::configureView(LogViewWidget* view) const
{
    if (!view)
        return;
    view->setParserPattern(m_active);
    view->setExtractionEnabled(fieldDisplayEnabled());
    if (view->model())
        view->model()->setFieldDisplaySelection(fieldDisplayEnabled(), visibleFieldIndexes());
}

void FieldSchemaController::cancelReextraction()
{
    if (!m_reextraction)
        return;
    // Workers stop and are waited for; entries keep their old fields intact.
    m_reextraction->cancel();
    m_reextraction->deleteLater();
    m_reextraction = nullptr;
    emit reextractionFinished();
}

void FieldSchemaController::cancelFilterJobs()
{
    for (LogViewWidget* lv : views()) {
        if (lv && lv->model())
            lv->model()->cancelPendingFilter(true);
    }
}

void FieldSchemaController::applyToAllViews()
{
    // A schema switch supersedes any re-extraction still in flight.
    cancelReextraction();
    // Читатели полей вне вкладок (выдача Search, Save View As) и фильтры
    // вкладок — до синхронной мутации ниже (инвариант 4). finishApplication()
    // перефильтрует отменённое, окно повторит поиск.
    if (m_window.stopFieldReaders)
        m_window.stopFieldReaders(true);
    cancelFilterJobs();

    auto pattern = std::make_shared<LogPattern>(m_active);
    setFieldNames(pattern->fieldNames());
    applyFieldVisibility();

    const bool filterEnabled = fieldDisplayEnabled();
    const bool doExtract = filterEnabled && pattern->isValid();

    // Point every view's parser at the new schema up-front so newly loaded
    // lines are parsed correctly even while the back-fill runs.
    QVector<std::shared_ptr<LogEntry>> entries;
    const QList<LogViewWidget*> tabs = views();
    for (LogViewWidget* lv : tabs) {
        if (!lv)
            continue;
        lv->setParserPattern(m_active);
        lv->setExtractionEnabled(filterEnabled);
        if (!lv->model())
            continue;
        // Индексный бэкенд: спаны полей не хранятся — извлечение идёт по
        // требованию новым паттерном (он уже передан через setParserPattern).
        if (lv->model()->isIndexedBackend())
            continue;
        for (const auto& entry : lv->model()->residentEntriesForFieldMutation()) {
            if (!entry)
                continue;
            if (doExtract)
                entries.append(entry);
            else
                entry->setFields(LogEntryFields());
        }
    }

    // Small batches (or "no extraction") complete instantly on the GUI
    // thread — no worker, no progress bar, no display flicker.
    constexpr int kAsyncThreshold = 4000;
    if (!doExtract || entries.size() < kAsyncThreshold) {
        for (const auto& entry : entries)
            entry->setFields(pattern->extractFields(entry->message()));
        finishApplication();
        return;
    }

    // Large batch: re-extract in the background. Workers do not touch the
    // entries, so filters started meanwhile read the old fields safely. Field
    // display is OFF for the duration (applyFieldVisibility): the column list
    // already describes the new schema, while the entries still carry spans
    // of the old one.
    m_reextraction = new FieldReextraction(std::move(entries), pattern, this);
    applyFieldVisibility();
    connect(m_reextraction, &FieldReextraction::progressValueChanged,
            this, &FieldSchemaController::reextractionProgress);
    connect(m_reextraction, &FieldReextraction::finished,
            this, &FieldSchemaController::onReextractionFinished);
    emit reextractionStarted(m_reextraction->entryCount());
    m_reextraction->start();
}

void FieldSchemaController::onReextractionFinished()
{
    if (!m_reextraction)
        return;
    // Readers started while the fields were being computed (filters: Apply,
    // level buttons, appended lines; Save View As to CSV / JSON Lines) read
    // entry->fields(): stop them before the new fields go in;
    // finishApplication() re-applies the filters. Search is not started then.
    cancelFilterJobs();
    if (m_window.stopFieldReaders)
        m_window.stopFieldReaders(false);
    m_reextraction->apply();
    m_reextraction->deleteLater();
    m_reextraction = nullptr;
    emit reextractionFinished();
    finishApplication();
}

void FieldSchemaController::finishApplication()
{
    const QList<LogViewWidget*> tabs = views();
    for (LogViewWidget* lv : tabs) {
        if (lv && lv->model()) {
            lv->model()->refreshDisplay();
            // Фильтр-джоб, отменённый на время переизвлечения полей, мог
            // оставить список строк со старыми настройками — доводим.
            lv->model()->reapplyFilterIfStale();
        }
    }

    // Re-enables field display with the active selection (it was turned off
    // during a background re-extraction).
    applyFieldVisibility();

    // Каждая вкладка сохраняет СВОЙ применённый набор правил, но колоночные
    // привязки пересчитываются под новую схему (вкладкам без фильтров правила
    // не навязываются).
    for (LogViewWidget* lv : tabs) {
        if (!lv || !lv->model())
            continue;
        FilterRuleSet rules = lv->model()->filterRules();
        if (!rules.isActive())
            continue;
        bindRules(rules);
        lv->model()->setFilterRules(rules);
        emit filtersRebound(lv);
    }
    emit schemaApplied();
}
