#include "fieldschemacontroller.h"

#include "appsettings.h"
#include "documentstate.h"
#include "lineformat.h"
#include "patternheuristics.h"
#include "filterruleset.h"
#include "logmodel.h"
#include "schemaconverter.h"
#include "logviewwidget.h"
#include "sessioncontroller.h"

#include <QFutureWatcher>
#include <QSettings>
#include <QTimer>
#include <QtConcurrent>

#include <algorithm>
#include <numeric>
#include <memory>

// Подбор при открытии на вкладку (F-17, шаг 8).
struct FieldSchemaController::PickState {
    SchemaFitJob* job = nullptr;
    bool started = false;        // первые строки вкладки пришли
    bool running = false;        // задание идёт
    bool pick = false;           // решить, когда меры придут
    bool userRequested = false;  // «Detect for This Tab»
    int retries = 0;             // перезапуски после reset модели
    int catalogueCount = 0;
    int defaultIndex = -1;
    int nameIndex = -1;
    bool nameNone = false;
    QStringList definitions;     // по мере: определение схемы
    QStringList names;           // по мере: имя
    QStringList catalogue;       // определения каталога на момент расчёта
    QVector<SchemaFit::Result> results;
    bool hasResults = false;
    // «Detect for This Tab»: схема вкладки на момент просьбы — выбор
    // пользователя после неё главнее итога.
    QString requestedSchema;
    FieldSettings::Origin requestedOrigin = FieldSettings::Origin::Default;
};

static constexpr int kPickRetries = 5;

// Колонки схемы по определению — блочной, ключевой или шаблона (F-17).
static QStringList schemaColumns(const QString& definition)
{
    if (definition.isEmpty())
        return {};
    FieldLayout layout;
    layout.schemaFor(definition);
    return layout.columns;
}

FieldSchemaController::FieldSchemaController(QObject* parent)
    : QObject(parent)
{
}

FieldSchemaController::~FieldSchemaController() = default;

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
    m_defaults.showFields = s.value("fieldFilterEnabled", false).toBool();
    m_defaults.visibleFields = s.value("fieldVisibleNames").toStringList();
    const int legacyMask = s.value("fieldVisibilityMask", -1).toInt();

    m_defaults.schema = s.value("conversionPattern").toString();
    // Ключевая схема прежних версий (JSON Lines, logfmt) — с F-17, шаг 7
    // формат строк файла: умолчание — без схемы.
    if (SchemaConverter::isRetiredKeyedDefinition(m_defaults.schema))
        m_defaults.schema.clear();
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
    for (const auto& replaced : builtins.replaced) {
        if (m_defaults.schema == replaced.first)
            m_defaults.schema = replaced.second;
    }
    // Каталог версии 3 — шаблонами (F-17, шаг 9): после встроенных, чтобы
    // нетронутая встроенная копия обновилась шаблоном по манифесту, а не
    // переводом; оригиналы — в converted-from-v3/. Повторный запуск ничего
    // не делает. Пресеты редактора из ini — один раз в каталог.
    const SchemaStore::MigrationReport migration = SchemaStore::migrateToTemplates();
    m_conversionFailures = migration.failed;
    const bool presetsMoved = migratePresets(s);
    if (builtins.changed() || !migration.converted.isEmpty() || presetsMoved)
        m_list = SchemaStore::loadAll(order);
    // Схема по умолчанию прежнего формата — тем же конвертером, что каталог.
    m_defaults.schema = SchemaConverter::upgradeDefinition(m_defaults.schema);
    // Имя схемы по умолчанию — её пункт в каталоге (первый с тем же
    // определением); нет пункта (или схемы) — без имени.
    const int defaultIndex = indexOf(m_defaults);
    m_defaults.schemaName = defaultIndex >= 0 ? m_list[defaultIndex].name : QString();

    // Выбор колонок версий до имён — битовая маска по индексам схемы.
    const QStringList defaultFieldNames = schemaColumns(m_defaults.schema);
    if (m_defaults.visibleFields.isEmpty() && legacyMask >= 0) {
        for (int i = 0; i < defaultFieldNames.size() && i < 31; ++i) {
            if (legacyMask & (1 << i))
                m_defaults.visibleFields.append(defaultFieldNames[i]);
        }
    }
    m_defaults.visibleFields = keptColumns(m_defaults.visibleFields, defaultFieldNames);
    m_defaults.origin = FieldSettings::Origin::Default;

    // Таблица имён подбора при открытии (F-17, шаг 8) — группа рядом с [View].
    {
        const QString group = s.group();
        while (!s.group().isEmpty())
            s.endGroup();
        s.beginGroup(QStringLiteral("SchemaByName"));
        m_nameTable = SchemaPick::NameTable::fromJson(s.value(QStringLiteral("entries")).toString());
        s.endGroup();
        for (const QString& part : group.split(QLatin1Char('/'), Qt::SkipEmptyParts))
            s.beginGroup(part);
    }

    emit catalogueChanged();
    emit fieldsChanged();
}

bool FieldSchemaController::migratePresets(QSettings& s)
{
    // Пресеты прежнего редактора ([PatternPresets] names / values — блочные
    // схемы) — схемами каталога (F-17, шаг 9: пресеты слились со схемами).
    // Группа — рядом с [View], которую открыл вызывающий.
    const QString group = s.group();
    while (!s.group().isEmpty())
        s.endGroup();
    s.beginGroup(QStringLiteral("PatternPresets"));
    const QStringList names = s.value(QStringLiteral("names")).toStringList();
    const QStringList values = s.value(QStringLiteral("values")).toStringList();
    s.endGroup();
    bool moved = false;
    if (!names.isEmpty() || !values.isEmpty()) {
        SchemaStore::SchemaList list = SchemaStore::loadAll();
        QSet<QString> taken;
        for (const auto& e : std::as_const(list))
            taken.insert(e.name);
        const int count = int(qMin(names.size(), values.size()));
        for (int i = 0; i < count; ++i) {
            const QString definition = SchemaConverter::upgradeDefinition(values[i]);
            if (definition.isEmpty() || !FieldLayout::isTemplateDefinition(definition))
                continue;
            bool same = false;
            for (const auto& e : std::as_const(list))
                same = same || e.schema == definition;
            if (same)
                continue;   // такая схема в каталоге уже есть
            const QString base = names[i].trimmed().isEmpty() ? QStringLiteral("Preset") : names[i].trimmed();
            QString name = base;
            for (int n = 2; taken.contains(name); ++n)
                name = QStringLiteral("%1 (%2)").arg(base).arg(n);
            taken.insert(name);
            list.append({name, definition, {}, {}});
            moved = true;
        }
        if (moved && !SchemaStore::sync(list))
            moved = false;   // ini не трогаем: попытка в следующий раз
        else
            s.remove(QStringLiteral("PatternPresets"));
    }
    for (const QString& part : group.split(QLatin1Char('/'), Qt::SkipEmptyParts))
        s.beginGroup(part);
    return moved;
}

void FieldSchemaController::save(QSettings& s) const
{
    // Схема по умолчанию — последняя выбранная в доке (F-5).
    s.setValue("fieldFilterEnabled", m_defaults.showFields);
    s.setValue("fieldVisibleNames", m_defaults.visibleFields);
    s.setValue("conversionPattern", m_defaults.schema);
    // Schemas themselves live as files in <configDir>/patterns/ (see
    // SchemaStore); the INI only remembers their display order, keyed by name.
    s.setValue("schemaOrder", listOrder());
    // Таблица имён (F-17, шаг 8): [SchemaByName] entries.
    const QString group = s.group();
    while (!s.group().isEmpty())
        s.endGroup();
    s.beginGroup(QStringLiteral("SchemaByName"));
    s.setValue(QStringLiteral("entries"), m_nameTable.toJson());
    s.endGroup();
    for (const QString& part : group.split(QLatin1Char('/'), Qt::SkipEmptyParts))
        s.beginGroup(part);
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
    refreshCoverage();
}

QString FieldSchemaController::replaceCatalogue(const SchemaStore::SchemaList& list,
                                                const QString& chosen, int chosenIndex)
{
    m_list = list;
    // Persist the (possibly edited) list back to the patterns folder.
    QString syncError;
    if (SchemaStore::sync(m_list, &syncError))
        syncError.clear();
    // Меры вкладки дока — для нового каталога (после применения ниже).
    QTimer::singleShot(0, this, [this]() { refreshCoverage(); });

    if (!chosen.isEmpty()) {
        FieldSettings settings = tabSettings();
        settings.origin = FieldSettings::Origin::User;
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
    if (const LogFilePtr keyed = keyedFile())
        sampleKeys(keyed);
    emit catalogueChanged();
    emit fieldsChanged();
    emit keysChanged();
    refreshCoverage();
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
    if (const LogFilePtr keyed = keyedFile())
        sampleKeys(keyed);
    emit catalogueChanged();
    emit fieldsChanged();
    emit keysChanged();
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
    return m_current ? m_current->parserFieldNames() : schemaColumns(m_defaults.schema);
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
        // Схема по умолчанию — последняя ВЫБРАННАЯ (F-17, шаг 8): подобранная
        // или сохранённая схема вкладки ею не становится от смены показа.
        const FieldSettings tab = m_current ? m_current->fieldSettings() : chosen;
        if (tab.origin == FieldSettings::Origin::User || tab.schema == m_defaults.schema) {
            m_defaults = tab;
        } else {
            m_defaults.showFields = tab.showFields;
        }
        m_defaults.origin = FieldSettings::Origin::Default;
        return;
    }
    // Вкладок нет — только схема по умолчанию.
    if (chosen.schema != m_defaults.schema)
        chosen.visibleFields = keptColumns(chosen.visibleFields, schemaColumns(chosen.schema));
    m_defaults = chosen;
    m_defaults.origin = FieldSettings::Origin::Default;
    emit catalogueChanged();
    emit fieldsChanged();
}

void FieldSchemaController::selectSchema(int index)
{
    FieldSettings settings = tabSettings();
    // Выбор пользователя (F-17, шаг 8): подбор его больше не оспаривает, имя
    // файла запоминает его (и «No schema»).
    settings.origin = FieldSettings::Origin::User;
    if (index >= 0 && index < m_list.size()) {
        settings.schema = m_list[index].schema;
        settings.schemaName = m_list[index].name;
    } else {
        settings.schema.clear();
        settings.schemaName.clear();
    }
    rememberName(settings);
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

void FieldSchemaController::setShowFields(bool enabled)
{
    FieldSettings settings = tabSettings();
    if (settings.showFields == enabled)
        return;
    // Только показ выбранных колонок (F-17, шаг 3): поля и правила по полю
    // от галочки не зависят.
    settings.showFields = enabled;
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
        rules.bindFields(view->parserFieldNames());
}

std::optional<FieldSettings> FieldSchemaController::resolveSaved(const FieldSettings& saved,
                                                                 bool addMissing, QString* error)
{
    FieldSettings resolved = saved;
    // Ключевая схема прежних версий (F-4) — с F-17, шаг 7 формат строк файла:
    // сохранённая с ней вкладка — без схемы (JSON-файл читается своим форматом).
    if (SchemaConverter::isRetiredKeyedDefinition(saved.schema))
        resolved.schema.clear();
    // Происхождение — как сохранено (снимок до F-17, шаг 8 со схемой читается
    // как User: FieldSettings::fromJson).
    if (!resolved.hasSchema()) {
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
    // Файл уже читается — смена схемы (applyToFile); ещё без
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
    settings.visibleFields = keptColumns(settings.visibleFields, schemaColumns(settings.schema));
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
    FieldSettings defaults = m_defaults;
    defaults.origin = FieldSettings::Origin::Default;
    view->setFieldSettings(defaults);
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
    // Подбор при открытии (F-17, шаг 8): после первых строк. Вставка и
    // reset (слитая индексная вкладка) — проверка O(1), дальше ничего.
    // QPointer: модель — дочерний объект вкладки и умирает после неё.
    if (LogModel* model = view->model()) {
        const QPointer<LogViewWidget> guard(view);
        connect(model, &QAbstractItemModel::rowsInserted, this, [this, guard]() {
            if (guard)
                onRowsArrived(guard);
        });
        connect(model, &QAbstractItemModel::modelReset, this, [this, guard]() {
            if (guard)
                onRowsArrived(guard);
        });
    }
    // Формат строк файла решён или выбран — ключи JSON-файла в доке.
    connect(view, &LogViewWidget::lineFormatChanged, this, [this, view](const LogFilePtr&) {
        if (view != m_current)
            return;
        if (const LogFilePtr keyed = keyedFile())
            sampleKeys(keyed);
        emit keysChanged();
        emit catalogueChanged();
    });
    connect(view, &LogViewWidget::loadingChanged, this, [this, view](bool) {
        if (view == m_current && keyedFile())
            emit keysChanged();
    });
    connect(view, &QObject::destroyed, this, [this, view]() {
        const auto it = m_picks.find(view);
        if (it == m_picks.end())
            return;
        if ((*it)->job) {
            (*it)->job->cancel();
            (*it)->job->deleteLater();
        }
        m_picks.erase(it);
    });
}

void FieldSchemaController::applyFieldVisibility(LogViewWidget* view)
{
    if (!view || !view->model())
        return;
    const bool enabled = view->fieldDisplayEnabled();
    view->model()->setFieldDisplaySelection(enabled, enabled ? view->visibleFieldIndexes()
                                                             : QVector<int>());
}

void FieldSchemaController::applyToAllViews()
{
    // Схема вкладки дока (не цели «Applies to») на все вкладки и все их
    // файлы: свои схемы файлов снимаются.
    FieldSettings shown = tabSettings();
    shown.origin = FieldSettings::Origin::User;   // выбор пользователя
    const QHash<QString, FieldSettings> none;
    const QList<LogViewWidget*> tabs = views();
    for (LogViewWidget* lv : tabs)
        if (lv)
            applyToView(lv, shown, &none);
    m_targetFile.clear();
    m_defaults = shown;
    m_defaults.origin = FieldSettings::Origin::Default;
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

    // Поля — функция текста и схемы (F-17, шаг 3): смена схемы — O(1) на
    // вкладке (FieldSource модели: поколение, кэш пуст), записи не
    // трогаются, читателей останавливать не нужно. Схема в модели — до
    // перепривязки правил (их перефильтрация берёт её снимок).
    view->setFieldSettings(settings, newFiles);
    applyFieldVisibility(view);
    if (view == m_current) {
        emit catalogueChanged();
        emit fieldsChanged();
    }
    if (schemasChanged)
        finishApplication(view);
}

void FieldSchemaController::finishApplication(LogViewWidget* view)
{
    if (view->model())
        view->model()->refreshDisplay();

    // Вкладка сохраняет СВОЙ применённый набор правил, но колоночные привязки
    // пересчитываются под новую схему, и правила по полю — перефильтрация
    // (правила только по всей строке от схемы не зависят: вкладке без
    // правил по полю ничего не перефильтровывается).
    if (view->model()) {
        FilterRuleSet rules = view->model()->filterRules();
        const bool byField = std::any_of(rules.rules.cbegin(), rules.rules.cend(),
            [](const FilterRule& r) { return r.isActive() && !r.fieldName.isEmpty(); });
        if (rules.isActive() && byField) {
            bindRules(view, rules);
            view->model()->setFilterRules(rules);
            emit filtersRebound(view);
        }
    }
    emit schemaApplied(view);
}

// =============================================================================
// Подбор при открытии и покрытие (F-17, шаг 8)
// =============================================================================


QStringList FieldSchemaController::catalogueDefinitions() const
{
    QStringList defs;
    defs.reserve(m_list.size());
    for (const auto& e : m_list)
        defs << e.schema;
    return defs;
}

FieldSchemaController::PickState& FieldSchemaController::pickState(LogViewWidget* view)
{
    auto it = m_picks.find(view);
    if (it == m_picks.end())
        it = m_picks.insert(view, std::make_shared<PickState>());
    return **it;
}

void FieldSchemaController::onRowsArrived(LogViewWidget* view)
{
    if (!view || !view->model() || view->model()->rowCount() == 0)
        return;
    PickState& st = pickState(view);
    if (st.started)
        return;
    st.started = true;
    if (!AppSettings::instance().autoPickSchema())
        return;   // настройка выключена — заданий нет
    // После обработчика порции: первая порция подбора не ждёт.
    QPointer<LogViewWidget> guard(view);
    QTimer::singleShot(0, this, [this, guard]() {
        if (guard)
            startPick(guard, /*pick=*/true, /*userRequested=*/false);
    });
}

void FieldSchemaController::startPick(LogViewWidget* view, bool pick, bool userRequested)
{
    if (!view || !view->model())
        return;
    // Файлы вкладки — все JSON / logfmt: поля — ключи формата строк, схеме
    // здесь делать нечего.
    // Строки и имя — первого файла вкладки в обычном тексте (у слитой вкладки
    // с JSON-файлом первым меры по его строкам сняли бы схему соседа).
    // Сначала — файл, чьи строки уже есть (слитая вкладка грузит файлы
    // порциями вперемешку).
    LogFilePtr plainFile;
    for (const bool needRows : {true, false}) {
        for (const LogFilePtr& f : view->loadedFiles())
            if (f && !view->lineFormat(f).isStructured() && (!needRows || view->fileHasRows(f))) {
                plainFile = f;
                break;
            }
        if (plainFile)
            break;
    }
    if (!plainFile) {
        if (userRequested)
            emit statusMessage(tr("The lines of this tab are JSON Lines or logfmt: their fields are "
                                  "the keys of the lines."));
        return;
    }
    PickState& st = pickState(view);
    st.pick = st.pick || pick;
    st.userRequested = st.userRequested || userRequested;
    if (userRequested) {
        st.requestedSchema = view->fieldSettings().schema;
        st.requestedOrigin = view->fieldSettings().origin;
    }
    // Прежние меры — другого каталога: до нового итога покрытия нет.
    st.results.clear();
    st.hasResults = false;
    st.definitions.clear();
    st.names.clear();
    QVector<FieldExtractorPtr> extractors;
    extractors.reserve(m_list.size() + 1);
    for (const auto& e : std::as_const(m_list)) {
        extractors << FieldLayout::extractorFor(e.schema);
        st.definitions << e.schema;
        st.names << e.name;
    }
    st.catalogueCount = int(m_list.size());
    st.catalogue = st.definitions;
    // Схема по умолчанию: её пункт каталога, иначе отдельной мерой.
    st.defaultIndex = indexOf(m_defaults);
    if (st.defaultIndex < 0 && m_defaults.hasSchema()) {
        st.defaultIndex = int(extractors.size());
        extractors << FieldLayout::extractorFor(m_defaults.schema);
        st.definitions << m_defaults.schema;
        st.names << m_defaults.schemaName;
    }
    // Имя файла: схема, которую пользователь выбирал для такого имени.
    st.nameIndex = -1;
    st.nameNone = false;
    {
        if (const std::optional<QString> remembered =
                m_nameTable.lookup(SchemaPick::nameKey(plainFile->displayName))) {
            if (remembered->isEmpty()) {
                st.nameNone = true;
            } else {
                for (int i = 0; i < st.catalogueCount; ++i)
                    if (st.names[i] == *remembered) {
                        st.nameIndex = i;
                        break;
                    }
            }
        }
    }
    if (!st.job) {
        st.job = new SchemaFitJob(this);
        st.job->setModel(view->model());
        QPointer<LogViewWidget> guard(view);
        connect(st.job, &SchemaFitJob::finished, this, [this, guard]() {
            if (guard)
                onPickFinished(guard);
        });
        connect(st.job, &SchemaFitJob::invalidated, this, [this, guard]() {
            // Строки снапшота ушли посреди расчёта (reset модели): заново,
            // не больше kPickRetries раз (слитая индексная вкладка грузится
            // с reset на порцию).
            if (!guard)
                return;
            PickState& s = pickState(guard);
            if (!s.running)
                return;
            s.running = false;
            if (s.retries >= kPickRetries) {
                // Сдались: подбор этой загрузки отменён, а не отложен.
                s.pick = false;
                s.userRequested = false;
                return;
            }
            ++s.retries;
            QTimer::singleShot(250, this, [this, guard]() {
                if (guard)
                    startPick(guard, false, false);
            });
        });
    }
    st.running = true;
    st.job->start(extractors, plainFile.get(), m_fitBudget);
    ++m_pickJobs;
}

void FieldSchemaController::onPickFinished(LogViewWidget* view)
{
    PickState& st = pickState(view);
    st.running = false;
    st.retries = 0;
    if (st.job->outcome().sampled == 0 && !st.userRequested) {
        // Строк файла в снапшоте ещё нет: решить по следующей порции.
        st.started = false;
        return;
    }
    st.results = st.job->outcome().results;
    st.hasResults = st.results.size() == st.definitions.size();
    if (st.hasResults && st.pick) {
        // Выбор пользователя и сохранённое состояние подбор не оспаривает;
        // «Detect for This Tab» — просьба пользователя, если после неё он
        // сам схему не выбрал.
        const FieldSettings now = view->fieldSettings();
        const bool asked = st.userRequested && now.schema == st.requestedSchema
            && now.origin == st.requestedOrigin;
        if (asked || (!st.userRequested && now.origin == FieldSettings::Origin::Default))
            applyPick(view, st);
    }
    st.pick = false;
    st.userRequested = false;
    if (view == m_current)
        emit catalogueChanged();
}

void FieldSchemaController::applyPick(LogViewWidget* view, PickState& st)
{
    SchemaPick::Input in;
    in.results = st.results;
    in.catalogueCount = st.catalogueCount;
    in.nameIndex = st.nameIndex;
    in.nameNone = st.nameNone;
    in.defaultIndex = st.defaultIndex;
    const SchemaPick::Choice choice = SchemaPick::choose(in);
    FieldSettings settings = view->fieldSettings();
    if (choice.index >= 0) {
        settings.schema = st.definitions[choice.index];
        settings.schemaName = st.names[choice.index];
        settings.origin = FieldSettings::Origin::Auto;
    } else {
        if (st.userRequested) {
            // Просили подобрать — ничего не подошло: схема остаётся, сказать.
            emit statusMessage(tr("No schema of the catalogue fits the lines of this tab."));
            return;
        }
        // Ни имя, ни содержимое, ни умолчание (покрытие < 0,50): без схемы.
        settings.schema.clear();
        settings.schemaName.clear();
        settings.origin = FieldSettings::Origin::Default;
    }
    applyToView(view, settings);
    if (choice.index >= 0)
        emit schemaPicked(view, settings.schemaName);
}

void FieldSchemaController::detectForView(LogViewWidget* view)
{
    if (!view)
        return;
    pickState(view).started = true;
    startPick(view, /*pick=*/true, /*userRequested=*/true);
}

bool FieldSchemaController::isPicking(const LogViewWidget* view) const
{
    const auto it = m_picks.constFind(view);
    return it != m_picks.cend() && (*it)->running;
}

void FieldSchemaController::refreshCoverage()
{
    // Меры вкладки дока — для нынешнего каталога: прежние (каталог
    // изменился) пересчитываются без подбора.
    if (!m_current || !AppSettings::instance().autoPickSchema())
        return;
    const auto it = m_picks.constFind(m_current.data());
    if (it == m_picks.cend() || !(*it)->started || (*it)->running)
        return;
    if ((*it)->hasResults && (*it)->catalogue == catalogueDefinitions())
        return;
    startPick(m_current, /*pick=*/false, /*userRequested=*/false);
}

const QVector<SchemaFit::Result>* FieldSchemaController::coverageOf(const LogViewWidget* view) const
{
    const auto it = m_picks.constFind(view);
    if (it == m_picks.cend() || !(*it)->hasResults || (*it)->catalogue != catalogueDefinitions())
        return nullptr;
    return &(*it)->results;
}

QVector<int> FieldSchemaController::comboOrder() const
{
    const QVector<SchemaFit::Result>* results = coverageOf(m_current);
    if (!results || targetLogFile()) {
        QVector<int> order(m_list.size());
        std::iota(order.begin(), order.end(), 0);
        return order;
    }
    return SchemaPick::fitsFirst(results->mid(0, m_list.size()));
}

QString FieldSchemaController::coverageText(int index) const
{
    const QVector<SchemaFit::Result>* results = coverageOf(m_current);
    if (!results || targetLogFile() || index < 0 || index >= m_list.size() || index >= results->size())
        return QString();
    const SchemaFit::Result& r = (*results)[index];
    if (!r.valid || r.candidates == 0)
        return QString();
    return SchemaFit::percent(r.coverage()) + QLatin1Char('%');
}

QString FieldSchemaController::coverageSummary() const
{
    const auto it = m_picks.constFind(m_current.data());
    if (targetLogFile() || it == m_picks.cend() || !(*it)->hasResults)
        return QString();
    const QString schema = shownSchema();
    if (schema.isEmpty())
        return QString();
    for (int i = 0; i < (*it)->definitions.size() && i < (*it)->results.size(); ++i)
        if ((*it)->definitions[i] == schema)
            return SchemaFit::summary((*it)->results[i]);
    return QString();
}

bool FieldSchemaController::shownIsAuto() const
{
    if (targetLogFile())
        return false;
    const FieldSettings tab = tabSettings();
    return m_current && tab.hasSchema() && tab.origin == FieldSettings::Origin::Auto
        && !m_current->hasFileSchemas();
}

void FieldSchemaController::rememberName(const FieldSettings& chosen)
{
    // Выбор в доке — для имени файла цели «Applies to» или файла вкладки из
    // одного файла (у слитой вкладки «все файлы» имя не одно).
    if (!m_current || (chosen.hasSchema() && chosen.schemaName.isEmpty()))
        return;
    LogFilePtr file = targetLogFile();
    if (!file) {
        const QList<LogFilePtr> files = m_current->loadedFiles();
        if (files.size() == 1)
            file = files.first();
    }
    if (!file)
        return;
    m_nameTable.remember(SchemaPick::nameKey(file->displayName),
                         chosen.hasSchema() ? chosen.schemaName : QString());
}

// =============================================================================
// Ключи файла JSON / logfmt (F-17, шаг 8)
// =============================================================================

LogFilePtr FieldSchemaController::keyedFile() const
{
    if (!m_current)
        return nullptr;
    LogFilePtr file = targetLogFile();
    if (!file) {
        const QList<LogFilePtr> files = m_current->loadedFiles();
        if (files.size() == 1)
            file = files.first();
    }
    return file && m_current->lineFormat(file).isStructured() ? file : nullptr;
}

QStringList FieldSchemaController::keyColumns() const
{
    const LogFilePtr file = keyedFile();
    return file ? m_current->lineFormat(file).keys.extraColumns : QStringList();
}

QStringList FieldSchemaController::availableKeys() const
{
    const LogFilePtr file = keyedFile();
    if (!file)
        return {};
    // Порядок не зависит от галочек (снятая не уходит из списка): ключи
    // начала файла, затем колонки, которых там нет, затем добавленные.
    QStringList keys = m_sampledKeys.value(file->filePath);
    const QStringList more = m_current->lineFormat(file).keys.extraColumns
        + m_addedKeys.value(file->filePath);
    for (const QString& k : more)
        if (!keys.contains(k))
            keys << k;
    return keys;
}

void FieldSchemaController::sampleKeys(const LogFilePtr& file)
{
    if (!file || !m_current || m_sampledKeys.contains(file->filePath)
        || m_keysPending.contains(file->filePath))
        return;
    // Ключи начала файла (≤ 256 КБ) — один раз на файл, в пуле.
    const QString path = file->filePath;
    KeyedDefinition keys = m_current->lineFormat(file).keys;
    keys.extraColumns.clear();   // все ключи, кроме ключей ролей, — и нынешние колонки
    m_keysPending.insert(path);
    auto* watcher = new QFutureWatcher<QStringList>(this);
    connect(watcher, &QFutureWatcher<QStringList>::finished, this, [this, watcher, path]() {
        m_sampledKeys.insert(path, watcher->result());
        m_keysPending.remove(path);
        watcher->deleteLater();
        emit keysChanged();
    });
    watcher->setFuture(QtConcurrent::run([path, keys]() {
        bool endOfFile = false;
        const QStringList lines = LineFormatDetector::sampleFile(path, &endOfFile);
        return PatternHeuristics::keyedSampleKeys(lines, keys);
    }));
}

void FieldSchemaController::setKeyColumns(const QStringList& keys)
{
    const LogFilePtr file = keyedFile();
    if (!file)
        return;
    if (m_current->isLoading())
        return;   // проход в полёте перечитал бы файл; галочки дока выключены
    LineFormat format = m_current->lineFormat(file);
    if (format.keys.extraColumns == keys)
        return;
    // Снятый ключ остаётся в списке дока.
    QStringList& kept = m_addedKeys[file->filePath];
    for (const QString& k : std::as_const(format.keys.extraColumns))
        if (!keys.contains(k) && !kept.contains(k))
            kept << k;
    format.keys.extraColumns = keys;
    // Выбор формата пользователем; колонки не входят в подпись чтения —
    // файл не перечитывается (LogViewWidget::setLineFormat).
    m_current->setLineFormat(file, format);
    emit keysChanged();
    emit fieldsChanged();
}

void FieldSchemaController::addKey(const QString& key)
{
    const QString k = key.trimmed();
    if (k.isEmpty())
        return;
    QStringList keys = keyColumns();
    if (keys.contains(k))
        return;
    keys << k;
    if (const LogFilePtr file = keyedFile()) {
        QStringList& added = m_addedKeys[file->filePath];
        if (!added.contains(k))
            added << k;
    }
    setKeyColumns(keys);
}
