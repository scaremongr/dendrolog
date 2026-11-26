#include "fieldschemacontroller.h"

#include "documentstate.h"
#include "fieldreextraction.h"
#include "filterruleset.h"
#include "logmodel.h"
#include "logpattern.h"
#include "logviewwidget.h"
#include "sessioncontroller.h"

#include <QSettings>

#include <algorithm>
#include <memory>

FieldSchemaController::FieldSchemaController(QObject* parent)
    : QObject(parent)
{
}

FieldSchemaController::~FieldSchemaController()
{
    // Воркеры не должны пережить записи, которые читают; сигналов владельцу,
    // которого уже разбирают, — нет.
    for (FieldReextraction* job : std::as_const(m_jobs)) {
        job->cancel();
        delete job;
    }
    m_jobs.clear();
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
    m_defaults = FieldSettings();
    m_defaults.extraction = s.value("fieldFilterEnabled", false).toBool();
    m_defaults.visibleFields = s.value("fieldVisibleNames").toStringList();
    const int legacyMask = s.value("fieldVisibilityMask", -1).toInt();

    m_defaults.schema = s.value("conversionPattern").toString();
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
            if (m_defaults.schema == replaced.first)
                m_defaults.schema = replaced.second;
        }
        m_list = SchemaStore::loadAll(order);
    }
    // Имя схемы по умолчанию — её пункт в каталоге (первый с тем же
    // определением); нет пункта (или схемы) — без имени.
    const int defaultIndex = indexOf(m_defaults);
    m_defaults.schemaName = defaultIndex >= 0 ? m_list[defaultIndex].name : QString();

    // Выбор колонок версий до имён — битовая маска по индексам схемы.
    const QStringList defaultFieldNames = LogPattern(m_defaults.schema).fieldNames();
    if (m_defaults.visibleFields.isEmpty() && legacyMask >= 0) {
        for (int i = 0; i < defaultFieldNames.size() && i < 31; ++i) {
            if (legacyMask & (1 << i))
                m_defaults.visibleFields.append(defaultFieldNames[i]);
        }
    }
    m_defaults.visibleFields = keptColumns(m_defaults.visibleFields, defaultFieldNames);

    emit catalogueChanged();
    emit fieldsChanged();
}

void FieldSchemaController::save(QSettings& s) const
{
    // Схема по умолчанию — последняя выбранная в доке (F-5).
    s.setValue("fieldFilterEnabled", m_defaults.extraction);
    s.setValue("fieldVisibleNames", m_defaults.visibleFields);
    s.setValue("conversionPattern", m_defaults.schema);
    // Schemas themselves live as files in <configDir>/patterns/ (see
    // SchemaStore); the INI only remembers their display order, keyed by name.
    s.setValue("schemaOrder", listOrder());
}

int FieldSchemaController::indexOf(const FieldSettings& settings) const
{
    if (settings.schema.isEmpty())
        return -1;
    // Пункт с тем же именем и определением, иначе первый с тем же определением.
    int first = -1;
    for (int i = 0; i < m_list.size(); ++i) {
        if (m_list[i].schema != settings.schema)
            continue;
        if (!settings.schemaName.isEmpty() && m_list[i].name == settings.schemaName)
            return i;
        if (first < 0)
            first = i;
    }
    return first;
}

void FieldSchemaController::reload(const QString& extraName)
{
    QStringList order = listOrder();
    if (!extraName.isEmpty())
        order << extraName;
    m_list = SchemaStore::loadAll(order);
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
        FieldSettings settings = tabSettings();
        settings.schema = chosen;
        settings.schemaName = chosenIndex >= 0 && chosenIndex < m_list.size()
                && m_list[chosenIndex].schema == chosen
            ? m_list[chosenIndex].name : QString();
        if (const LogFilePtr file = targetLogFile()) {
            applyToFile(m_current, file, settings);
            return syncError;
        }
        chooseShown(settings, /*clearFiles=*/true);
        return syncError;
    }
    // Без «Use Selected» схема вкладки остаётся и уже применена; её пункт —
    // по определению (правка в редакторе могла её изменить — тогда она
    // действует без пункта).
    emit catalogueChanged();
    return syncError;
}

// =============================================================================
// Вкладка дока и её схема
// =============================================================================

void FieldSchemaController::setCurrentView(LogViewWidget* view)
{
    if (m_current == view)
        return;
    m_current = view;
    m_targetFile.clear();
    emit catalogueChanged();
    emit fieldsChanged();
}

FieldSettings FieldSchemaController::tabSettings() const
{
    return m_current ? m_current->fieldSettings() : m_defaults;
}

FieldSettings FieldSchemaController::shownSettings() const
{
    FieldSettings shown = tabSettings();
    if (const LogFilePtr file = targetLogFile()) {
        const FieldSettings own = m_current->fileSchema(file);
        shown.schema = own.schema;
        shown.schemaName = own.schemaName;
    }
    return shown;
}

LogFilePtr FieldSchemaController::targetLogFile() const
{
    if (!m_current || m_targetFile.isEmpty())
        return nullptr;
    for (const LogFilePtr& f : m_current->loadedFiles())
        if (f && f->filePath == m_targetFile)
            return f;
    return nullptr;
}

void FieldSchemaController::setTargetFile(const QString& filePath)
{
    if (m_targetFile == filePath)
        return;
    m_targetFile = filePath;
    emit catalogueChanged();
    emit fieldsChanged();
}

QString FieldSchemaController::targetFile() const
{
    return targetLogFile() ? m_targetFile : QString();
}

bool FieldSchemaController::isMixed() const
{
    return m_current && !targetLogFile() && m_current->hasFileSchemas();
}

QString FieldSchemaController::activeSchemaName() const
{
    const FieldSettings tab = tabSettings();
    return SessionController::schemaNameFor(m_list, indexOf(tab), tab.schema);
}

QStringList FieldSchemaController::fieldNames() const
{
    return m_current ? m_current->parserFieldNames() : LogPattern(m_defaults.schema).fieldNames();
}

QVector<int> FieldSchemaController::visibleFieldIndexes() const
{
    if (m_current)
        return m_current->visibleFieldIndexes();
    const QStringList names = fieldNames();
    QVector<int> indexes;
    indexes.reserve(names.size());
    for (int i = 0; i < names.size(); ++i)
        if (m_defaults.visibleFields.contains(names[i]))
            indexes.push_back(i);
    return indexes;
}

QStringList FieldSchemaController::keptColumns(const QStringList& visible, const QStringList& fieldNames)
{
    QStringList kept;
    for (const QString& name : fieldNames)
        if (visible.contains(name))
            kept << name;
    return kept.isEmpty() ? fieldNames : kept;
}

void FieldSchemaController::chooseShown(const FieldSettings& settings, bool clearFiles)
{
    FieldSettings chosen = settings;
    if (chosen.schema.isEmpty())
        chosen.schemaName.clear();
    if (m_current) {
        const QHash<QString, FieldSettings> none;
        // шлёт catalogueChanged / fieldsChanged
        applyToView(m_current, chosen, clearFiles ? &none : nullptr);
        m_defaults = m_current ? m_current->fieldSettings() : chosen;
        return;
    }
    // Вкладок нет — только схема по умолчанию.
    if (chosen.schema != m_defaults.schema)
        chosen.visibleFields = keptColumns(chosen.visibleFields, LogPattern(chosen.schema).fieldNames());
    m_defaults = chosen;
    emit catalogueChanged();
    emit fieldsChanged();
}

void FieldSchemaController::selectSchema(int index)
{
    FieldSettings settings = tabSettings();
    if (index >= 0 && index < m_list.size()) {
        settings.schema = m_list[index].schema;
        settings.schemaName = m_list[index].name;
    } else {
        settings.schema.clear();
        settings.schemaName.clear();
    }
    // «Applies to» — файл: своя схема файла, схемой по умолчанию не становится.
    if (const LogFilePtr file = targetLogFile()) {
        applyToFile(m_current, file, settings);
        return;
    }
    // Все файлы: схема вкладки, свои схемы файлов снимаются.
    chooseShown(settings, /*clearFiles=*/true);
}

void FieldSchemaController::applyToFile(LogViewWidget* view, const LogFilePtr& file,
                                        const FieldSettings& ref)
{
    if (!view || !file)
        return;
    QHash<QString, FieldSettings> files = view->fileSchemas();
    FieldSettings own;
    own.schema = ref.schema;
    own.schemaName = ref.hasSchema() ? ref.schemaName : QString();
    files.insert(file->filePath, own);
    applyToView(view, view->fieldSettings(), &files);
}

void FieldSchemaController::setExtractionEnabled(bool enabled)
{
    FieldSettings settings = tabSettings();
    if (settings.extraction == enabled)
        return;
    // Включает/выключает извлечение полей, переизвлекает поля уже загруженных
    // записей и перепривязывает колоночные правила (галочка «Filter blocks»
    // управляет строгой фильтрацией по колонкам).
    settings.extraction = enabled;
    chooseShown(settings);
}

void FieldSchemaController::setVisibleFieldNames(const QStringList& names)
{
    FieldSettings settings = tabSettings();
    settings.visibleFields.clear();
    for (const QString& name : fieldNames())
        if (names.contains(name))
            settings.visibleFields << name;
    chooseShown(settings);
}

void FieldSchemaController::bindRules(const LogViewWidget* view, FilterRuleSet& rules)
{
    if (view)
        rules.bindFields(view->parserFieldNames(), view->fieldSettings().extraction);
}

std::optional<FieldSettings> FieldSchemaController::resolveSaved(const FieldSettings& saved,
                                                                 bool addMissing, QString* error)
{
    FieldSettings resolved = saved;
    if (!saved.hasSchema()) {
        resolved.schemaName.clear();
        return resolved;
    }
    // Каталог схем — источник правды, и его могли изменить с прошлой загрузки
    // (sync ниже удалил бы файлы, которых нет в устаревшем списке). Список
    // перечитан — комбобокс дока перестраивается: его индексы — индексы списка.
    reload(QString());
    emit catalogueChanged();
    int index = indexOf(saved); // то же имя и определение — без сравнения JSON
    if (index < 0 || m_list[index].name != saved.schemaName) {
        const SessionController::SchemaChoice choice =
            SessionController::chooseSchema(m_list, saved.schemaName, saved.schema);
        if (choice.added) {
            if (!addMissing)
                return std::nullopt;
            // Нет в каталоге — добавить под именем с пометкой.
            m_list.append(choice.newEntry);
            QString syncError;
            if (SchemaStore::sync(m_list, &syncError))
                syncError.clear();
            if (error)
                *error = syncError;
            reload(choice.newEntry.name);
            index = -1;
            for (int i = 0; i < m_list.size(); ++i)
                if (m_list[i].name == choice.newEntry.name)
                    index = i;
            emit catalogueChanged();
        } else {
            index = choice.index;
        }
    }
    // Не записалась в каталог — схема всё равно действует, без пункта в списке.
    if (index >= 0) {
        resolved.schema = m_list[index].schema;
        resolved.schemaName = m_list[index].name;
    } else {
        resolved.schemaName.clear();
    }
    return resolved;
}

void FieldSchemaController::onSavedFileSchema(LogViewWidget* view, const QString& sourceKey,
                                              const FieldSettings& saved, bool fromMemory)
{
    if (!view)
        return;
    FieldSettings ref;
    ref.schemaName = saved.schemaName;
    ref.schema = saved.schema;
    QString error;
    // Как у схемы вкладки: сессия добавляет схему не из каталога, память
    // файла — нет (файл остаётся со схемой вкладки).
    const std::optional<FieldSettings> resolved = resolveSaved(ref, !fromMemory, &error);
    if (!resolved) {
        emit statusMessage(tr("Remembered schema %1 is not in the catalogue; the file uses the "
                              "tab's schema.")
                               .arg(saved.schemaName.isEmpty() ? tr("(unnamed)") : saved.schemaName));
        return;
    }
    if (!error.isEmpty())
        emit statusMessage(tr("Could not save the session schema: %1").arg(error));
    // Файл уже читается — смена схемы (переизвлечение, инвариант 4); ещё без
    // строк (не открыт, сжатый ждёт распаковки) — схема ждёт его и только
    // растит раскладку.
    const LogFilePtr file = view->fileForSourceKey(sourceKey);
    if (file && view->fileHasRows(file)) {
        applyToFile(view, file, *resolved);
        return;
    }
    view->setPendingFileSchema(sourceKey, *resolved);
}

void FieldSchemaController::onLayoutGrown(LogViewWidget* view)
{
    // Файл со своей схемой дописал колонки вкладки: поля загруженных записей
    // те же, показ колонок и привязка правил — по новой раскладке.
    if (!view)
        return;
    applyFieldVisibility(view);
    if (view->model()) {
        FilterRuleSet rules = view->model()->filterRules();
        if (rules.isActive()) {
            bindRules(view, rules);
            view->model()->setFilterRules(rules);
            emit filtersRebound(view);
        }
    }
    if (view == m_current) {
        emit catalogueChanged();
        emit fieldsChanged();
    }
    emit schemaApplied(view);
}

void FieldSchemaController::onSavedFieldSettings(LogViewWidget* view, const FieldSettings& saved,
                                                 bool fromMemory)
{
    if (!view)
        return;
    QString error;
    // Память файла каталог не засоряет: нет такой схемы — схема по умолчанию.
    const std::optional<FieldSettings> resolved = resolveSaved(saved, !fromMemory, &error);
    if (!resolved) {
        emit statusMessage(tr("Remembered schema %1 is not in the catalogue; the tab uses the "
                              "default schema.")
                               .arg(saved.schemaName.isEmpty() ? tr("(unnamed)") : saved.schemaName));
        return;
    }
    if (!error.isEmpty())
        emit statusMessage(tr("Could not save the session schema: %1").arg(error));
    // Пустой выбор колонок в снимке — «все колонки» (как у сессии 0.6 и ini),
    // какой бы ни была схема вкладки до применения.
    FieldSettings settings = *resolved;
    settings.visibleFields = keptColumns(settings.visibleFields, LogPattern(settings.schema).fieldNames());
    applyToView(view, settings);
}

// =============================================================================
// Вкладки
// =============================================================================

QList<LogViewWidget*> FieldSchemaController::views() const
{
    return m_window.views ? m_window.views() : QList<LogViewWidget*>();
}

void FieldSchemaController::configureView(LogViewWidget* view)
{
    if (!view)
        return;
    view->setFieldSettings(m_defaults);
    applyFieldVisibility(view);
    connect(view, &LogViewWidget::savedFieldSettingsFound, this,
            [this, view](const FieldSettings& saved, bool fromMemory) {
                onSavedFieldSettings(view, saved, fromMemory);
            });
    connect(view, &LogViewWidget::savedFileSchemaFound, this,
            [this, view](const QString& sourceKey, const FieldSettings& saved, bool fromMemory) {
                onSavedFileSchema(view, sourceKey, saved, fromMemory);
            });
    connect(view, &LogViewWidget::fieldLayoutGrown, this, [this, view]() { onLayoutGrown(view); });
    // Файлы вкладки дока изменились — список «Applies to».
    if (view->model())
        connect(view->model(), &LogModel::fileSetChanged, this, [this, view]() {
            if (view == m_current)
                emit catalogueChanged();
        });
    // Вкладка закрыта посреди расчёта: воркеры держат записи сами
    // (shared_ptr), расчёт просто отменяется.
    connect(view, &QObject::destroyed, this, [this, view]() { cancelReextraction(view); });
}

void FieldSchemaController::applyFieldVisibility(LogViewWidget* view)
{
    if (!view || !view->model())
        return;
    // Посреди фонового расчёта колонки новой схемы легли бы на поля старой
    // (и Save View As читал бы их во время замены) — показ включит конец
    // расчёта.
    const bool enabled = view->fieldDisplayEnabled() && !isReextracting(view);
    view->model()->setFieldDisplaySelection(enabled, enabled ? view->visibleFieldIndexes()
                                                             : QVector<int>());
}

bool FieldSchemaController::isReextracting(const LogViewWidget* view) const
{
    return view ? m_jobs.contains(view) : !m_jobs.isEmpty();
}

void FieldSchemaController::cancelReextraction(const LogViewWidget* view)
{
    FieldReextraction* job = m_jobs.take(view);
    if (!job)
        return;
    m_progress.remove(job);
    // Workers stop and are waited for; entries keep their old fields intact.
    job->cancel();
    job->deleteLater();
    emit reextractionFinished();
}

void FieldSchemaController::cancelReextraction()
{
    const QList<const LogViewWidget*> keys = m_jobs.keys();
    for (const LogViewWidget* view : keys)
        cancelReextraction(view);
}

void FieldSchemaController::applyToAllViews()
{
    // Схема вкладки дока (не цели «Applies to») на все вкладки и все их
    // файлы: свои схемы файлов снимаются.
    const FieldSettings shown = tabSettings();
    const QHash<QString, FieldSettings> none;
    const QList<LogViewWidget*> tabs = views();
    for (LogViewWidget* lv : tabs)
        if (lv)
            applyToView(lv, shown, &none);
    m_targetFile.clear();
    m_defaults = shown;
    emit catalogueChanged();
    emit fieldsChanged();
}

// Определения своих схем файлов (без имён) — то, что решает поля записей.
static QHash<QString, QString> definitionsOf(const QHash<QString, FieldSettings>& files,
                                             const QString& tabSchema)
{
    QHash<QString, QString> out;
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        if (it->schema != tabSchema)
            out.insert(it.key(), it->schema);
    return out;
}

// Колонки вкладки при схеме tabSchema и своих схемах файлов (объединение по
// имени в порядке файлов — как LogViewWidget::rebuildFieldLayout).
static QStringList columnsFor(const LogViewWidget* view, const QString& tabSchema,
                              const QHash<QString, QString>& files)
{
    FieldLayout layout;
    layout.schemaFor(tabSchema);
    for (const LogFilePtr& f : view->loadedFiles()) {
        const auto it = f ? files.constFind(f->filePath) : files.constEnd();
        if (it != files.constEnd() && !it->isEmpty())
            layout.schemaFor(*it);
    }
    return layout.columns;
}

void FieldSchemaController::applyToView(LogViewWidget* view, const FieldSettings& requested,
                                        const QHash<QString, FieldSettings>* files)
{
    if (!view)
        return;
    const FieldSettings before = view->fieldSettings();
    FieldSettings settings = requested;
    if (settings.schema.isEmpty())
        settings.schemaName.clear();
    const QHash<QString, FieldSettings> newFiles = files ? *files : view->fileSchemas();
    const QHash<QString, QString> beforeDefs = definitionsOf(view->fileSchemas(), before.schema);
    const QHash<QString, QString> newDefs = definitionsOf(newFiles, settings.schema);
    const bool schemasChanged = settings.schema != before.schema || newDefs != beforeDefs;
    const QStringList oldNames = view->parserFieldNames();
    const QStringList newNames = schemasChanged ? columnsFor(view, settings.schema, newDefs)
                                                : oldNames;
    // Новая схема: выбранные имена, которые в ней есть, иначе все колонки;
    // та же — выбор как есть (снятые все галочки — пустой выбор). Сменились
    // только схемы файлов при всех выбранных колонках — новые тоже видны.
    const bool allShown = !oldNames.isEmpty()
        && std::all_of(oldNames.cbegin(), oldNames.cend(),
                       [&settings](const QString& n) { return settings.visibleFields.contains(n); });
    if (schemasChanged && settings.schema == before.schema && allShown) {
        settings.visibleFields = newNames;
    } else if (schemasChanged) {
        settings.visibleFields = keptColumns(settings.visibleFields, newNames);
    } else {
        QStringList kept;
        for (const QString& name : newNames)
            if (settings.visibleFields.contains(name))
                kept << name;
        settings.visibleFields = kept;
    }

    const bool reextract = schemasChanged
        || (settings.extraction && !newNames.isEmpty()) != view->fieldDisplayEnabled();
    if (!reextract) {
        // Те же схема и флаг: поля записей не меняются — только выбор колонок
        // (и имена пунктов каталога).
        view->setFieldSettings(settings, newFiles);
        applyFieldVisibility(view);
        if (view == m_current) {
            emit catalogueChanged();
            emit fieldsChanged();
        }
        return;
    }

    // Новая схема вкладки отменяет её расчёт в полёте.
    cancelReextraction(view);
    // Индексный бэкенд: спаны полей не хранятся — извлечение идёт по
    // требованию новым паттерном (его передаст setFieldSettings).
    QVector<std::shared_ptr<LogEntry>> entries;
    if (view->model() && !view->model()->isIndexedBackend()) {
        for (const auto& entry : view->model()->residentEntriesForFieldMutation())
            if (entry)
                entries.append(entry);
    }
    // Читатели полей вне вкладки (выдача Search, Save View As) и её фильтр —
    // до синхронной мутации ниже (инвариант 4). Мутировать нечего (индексная
    // вкладка, вкладка из снимка до загрузки) — чужие читатели не трогаются:
    // их полей смена не касается. finishApplication() перефильтрует
    // отменённое, окно повторит поиск по вкладке.
    if (!entries.isEmpty() && m_window.stopFieldReaders)
        m_window.stopFieldReaders(view, true);
    if (view->model())
        view->model()->cancelPendingFilter(true);

    // Point the tab's parsers at the new schema up-front so newly loaded
    // lines are parsed correctly even while the back-fill runs.
    view->setFieldSettings(settings, newFiles);
    applyFieldVisibility(view);
    if (view == m_current) {
        emit catalogueChanged();
        emit fieldsChanged();
    }

    // Схема записи — схема её файла у модели вкладки (F-5): setFieldSettings
    // уже поставил их; nullptr — извлечение выключено, поля снимаются.
    const FileSchemaSet schemas = view->model() ? view->model()->fileSchemas() : FileSchemaSet();
    const bool doExtract = view->fieldDisplayEnabled()
        && (schemas.fallback || !schemas.byPath.isEmpty());
    if (!doExtract) {
        for (const auto& entry : entries)
            entry->setFields(LogEntryFields());
        entries.clear();
    }

    // Small batches (or "no extraction") complete instantly on the GUI
    // thread — no worker, no progress bar, no display flicker.
    constexpr int kAsyncThreshold = 4000;
    if (!doExtract || entries.size() < kAsyncThreshold) {
        for (const auto& entry : entries) {
            const FileSchema* schema = schemas.forFile(entry->sourceFile().get());
            entry->setFields((schema && schema->isValid()) ? schema->extractFields(entry->message())
                                                           : LogEntryFields());
        }
        finishApplication(view);
        return;
    }

    // Large batch: re-extract in the background. Workers do not touch the
    // entries, so filters started meanwhile read the old fields safely. Field
    // display of the tab is OFF for the duration (applyFieldVisibility): the
    // column list already describes the new schema, while the entries still
    // carry spans of the old one.
    auto* job = new FieldReextraction(std::move(entries), schemas, this);
    m_jobs.insert(view, job);
    applyFieldVisibility(view);
    // Прогресс окна — сумма расчётов («Apply Schema to All Tabs» запускает
    // по одному на вкладку).
    connect(job, &FieldReextraction::progressValueChanged, this, [this, job](int value) {
        m_progress.insert(job, value);
        int sum = 0;
        for (int v : std::as_const(m_progress))
            sum += v;
        emit reextractionProgress(sum);
    });
    QPointer<LogViewWidget> guard(view);
    connect(job, &FieldReextraction::finished, this, [this, guard, job]() {
        if (guard && m_jobs.value(guard) == job)
            onReextractionFinished(guard);
    });
    int total = 0;
    for (const FieldReextraction* running : std::as_const(m_jobs))
        total += running->entryCount();
    emit reextractionStarted(total);
    job->start();
}

void FieldSchemaController::onReextractionFinished(LogViewWidget* view)
{
    FieldReextraction* job = m_jobs.value(view);
    if (!job)
        return;
    // Readers started while the fields were being computed (the tab's filter:
    // Apply, level buttons, appended lines; Save View As to CSV / JSON Lines)
    // read entry->fields(): stop them before the new fields go in;
    // finishApplication() re-applies the filter. Search is not started then.
    if (view->model())
        view->model()->cancelPendingFilter(true);
    if (m_window.stopFieldReaders)
        m_window.stopFieldReaders(view, false);
    job->apply();
    m_jobs.remove(view);
    m_progress.remove(job);
    job->deleteLater();
    emit reextractionFinished();
    finishApplication(view);
}

void FieldSchemaController::finishApplication(LogViewWidget* view)
{
    if (view->model()) {
        view->model()->refreshDisplay();
        // Фильтр-джоб, отменённый на время переизвлечения полей, мог
        // оставить список строк со старыми настройками — доводим.
        view->model()->reapplyFilterIfStale();
    }

    // Re-enables field display with the tab's selection (it was turned off
    // during a background re-extraction).
    applyFieldVisibility(view);

    // Вкладка сохраняет СВОЙ применённый набор правил, но колоночные привязки
    // пересчитываются под новую схему (вкладке без фильтров правила не
    // навязываются).
    if (view->model()) {
        FilterRuleSet rules = view->model()->filterRules();
        if (rules.isActive()) {
            bindRules(view, rules);
            view->model()->setFilterRules(rules);
            emit filtersRebound(view);
        }
    }
    emit schemaApplied(view);
}
