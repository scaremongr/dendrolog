// ============================================================================
// field_schema — FieldSchemaController (M-2) без окна: каталог схем и
// состояние дока Log Fields.
//
//   • каталог: миграция списков старых версий из ini в файлы, встроенные
//     схемы, активная схема по строке, «Use Selected» и правка в редакторе,
//     сохранённая схема вкладки (своя в каталоге по определению, «(from
//     session)» у сессии, у памяти файла — не добавляется);
//   • колонки: выбор по именам переживает смену схемы и перезапуск (ini),
//     маска старых версий;
//   • схема на вкладку (F-5, PR 1) на обоих бэкендах: выбор в доке меняет
//     только вкладку дока, соседняя не трогается (колонки и правило по
//     полю), док следует за вкладкой, новая вкладка — схема по умолчанию
//     (последняя выбранная), «Apply Schema to All Tabs», правила вкладки
//     перепривязываются к её новой схеме, поля резидентных записей — новой
//     схемы; ключевая схема (F-4) — тем же путём;
//   • фоновое переизвлечение (> 4000 записей): фильтр, запущенный за время
//     расчёта, остановлен до установки полей (инвариант 4 — на одну
//     вкладку: читатели остановлены у неё, соседняя не тронута), затем
//     перефильтрация.
// ============================================================================

#include "appsettings.h"
#include "builtinsamples.h"
#include "schemapick.h"
#include "documentstate.h"
#include "fieldschemacontroller.h"
#include "filterruleset.h"
#include "logmodel.h"
#include "linetemplate.h"
#include "schemaconverter.h"
#include "v3schema.h"
#include "logviewwidget.h"
#include "LogListView.h"
#include "schemastore.h"
#include "testdocuments.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QSemaphore>

#include <algorithm>
#include <cstdio>
#include <memory>

using namespace testdocs;

static int failures = 0;
#define CHECK(condition, label) do { \
    if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL: %s (line %d)\n", qPrintable(QString(label)), __LINE__); } \
} while (0)

// ---------------------------------------------------------------------------
// Фикстуры
// ---------------------------------------------------------------------------

// Схема фикстур (Timestamp, Thread, Level, Message) с другими именами колонок.
static QString schemaNamed(const QString& level, const QString& message)
{
    return LineTemplateSchema::definition(QStringLiteral("{Timestamp:time} [{Thread}] {%1:level} {?- ?}{%2}")
                                              .arg(level, message));
}

// Метка и остаток строки — ни одной колонки фикстурной схемы.
static QString schemaRest()
{
    return LineTemplateSchema::definition(QStringLiteral("{When:time} {Rest}"));
}

static const char* levelOf(int i)
{
    return i % 7 == 3 ? "ERROR" : (i % 5 == 1 ? "WARN" : "INFO");
}

static QByteArray logBytes(int records)
{
    QByteArray bytes;
    bytes.reserve(records * 60);
    for (int i = 0; i < records; ++i) {
        const int second = i % 86400;
        bytes += "2026-03-05 " + QByteArray::number(second / 3600).rightJustified(2, '0') + ':'
               + QByteArray::number(second / 60 % 60).rightJustified(2, '0') + ':'
               + QByteArray::number(second % 60).rightJustified(2, '0') + ".000 [w] "
               + levelOf(i) + " - line " + QByteArray::number(i) + '\n';
    }
    return bytes;
}

static int errorCount(int records)
{
    int n = 0;
    for (int i = 0; i < records; ++i)
        n += QByteArray(levelOf(i)) == "ERROR" ? 1 : 0;
    return n;
}

static QByteArray jsonBytes(int records)
{
    QByteArray bytes;
    for (int i = 0; i < records; ++i) {
        bytes += R"({"time":"2026-03-05T10:00:)" + QByteArray::number(i % 60).rightJustified(2, '0')
               + R"(Z","level":")" + levelOf(i) + R"(","msg":"json )" + QByteArray::number(i)
               + "\"}\n";
    }
    return bytes;
}

static bool settled(LogViewWidget& tab)
{
    const bool ok = waitFor([&] { return !tab.isLoading() && !tab.isPositionPending(); }, 60000)
        && settle(*tab.model());
    waitFor([&] { return !tab.view()->hasPendingLayout(); }, 5000);
    return ok;
}

static FilterRuleSet levelRule(const QString& field, const QString& text)
{
    FilterRule rule;
    rule.text = text;
    rule.fieldName = field;
    FilterRuleSet rules;
    rules.rules = { rule };
    return rules;
}

static int indexOf(const SchemaStore::SchemaList& list, const QString& name)
{
    for (int i = 0; i < list.size(); ++i)
        if (list[i].name == name)
            return i;
    return -1;
}

// Показанный текст каждой строки — одно из слов уровня (выбрана только
// колонка уровня).
static bool showsOnlyLevels(LogModel& model)
{
    if (model.rowCount() == 0)
        return false;
    for (int row = 0; row < model.rowCount(); ++row) {
        const QString text = model.data(model.index(row), Qt::DisplayRole).toString().trimmed();
        if (text != QLatin1String("INFO") && text != QLatin1String("WARN")
            && text != QLatin1String("ERROR"))
            return false;
    }
    return true;
}

// Свой каталог настроек на каждую проверку: ini и patterns/.
struct ConfigDir {
    QTemporaryDir dir;
    ConfigDir() { AppSettings::overrideConfigDirForTesting(dir.path()); }
    QString ini() const { return dir.filePath(QStringLiteral("DendroLog.ini")); }
};

// ---------------------------------------------------------------------------
// Каталог
// ---------------------------------------------------------------------------

static void checkMigrationAndBuiltins()
{
    ConfigDir config;
    // Список прежних версий — схемы версии 3 (блоки): перевод в шаблоны
    // (F-17, шаг 9) тем же конвертером, что каталог.
    const QString legacy = v3schema::fixtureDefinition();
    const QString converted = SchemaConverter::upgradeDefinition(legacy);
    {
        QSettings s(config.ini(), QSettings::IniFormat);
        s.beginGroup(QStringLiteral("View"));
        s.setValue(QStringLiteral("patternNames"), QStringList{QStringLiteral("Old")});
        s.setValue(QStringLiteral("patternValues"), QStringList{legacy});
        s.setValue(QStringLiteral("conversionPattern"), legacy);
        s.setValue(QStringLiteral("fieldFilterEnabled"), true);
        // Маска версий до имён: колонки 2 и 3 (Level, Message).
        s.setValue(QStringLiteral("fieldVisibilityMask"), (1 << 2) | (1 << 3));
        s.endGroup();
        // Пресеты прежнего редактора — один раз схемами каталога (F-17, шаг 9).
        s.beginGroup(QStringLiteral("PatternPresets"));
        s.setValue(QStringLiteral("names"), QStringList{QStringLiteral("My preset"), QStringLiteral("Same as Old")});
        s.setValue(QStringLiteral("values"), QStringList{v3schema::fixtureDefinition(QStringLiteral("Severity")), legacy});
        s.endGroup();
    }
    FieldSchemaController c;
    int catalogueSignals = 0;
    QObject::connect(&c, &FieldSchemaController::catalogueChanged, [&] { ++catalogueSignals; });
    {
        QSettings s(config.ini(), QSettings::IniFormat);
        s.beginGroup(QStringLiteral("View"));
        c.load(s);
        CHECK(!s.contains(QStringLiteral("patternNames")) && !s.contains(QStringLiteral("patternValues")),
              "migration removes the INI-stored schema list");
        s.endGroup();
    }
    CHECK(catalogueSignals >= 1, "load announces the catalogue");
    {
        QSettings s(config.ini(), QSettings::IniFormat);
        CHECK(!s.childGroups().contains(QStringLiteral("PatternPresets")), "the presets group is gone from the INI");
    }
    {
        const int preset = indexOf(c.schemas(), QStringLiteral("My preset"));
        CHECK(preset >= 0 && FieldLayout::isTemplateDefinition(c.schemas()[preset].schema)
                  && c.schemas()[preset].schema
                         == SchemaConverter::upgradeDefinition(v3schema::fixtureDefinition(QStringLiteral("Severity"))),
              "a user preset becomes a schema of the catalogue (a line template)");
        CHECK(indexOf(c.schemas(), QStringLiteral("Same as Old")) < 0,
              "a preset equal to a schema of the catalogue is not added twice");
    }
    const int old = indexOf(c.schemas(), QStringLiteral("Old"));
    CHECK(old >= 0, "migrated schema is in the catalogue");
    CHECK(QFileInfo::exists(QDir(SchemaStore::patternsDir()).filePath(QStringLiteral("Old.json"))),
          "migrated schema is written as a file");
    CHECK(indexOf(c.schemas(), QStringLiteral("Logback / Log4j")) >= 0
              && indexOf(c.schemas(), QStringLiteral("JSON Lines")) < 0
              && indexOf(c.schemas(), QStringLiteral("logfmt")) < 0,
          "built-in schemas are installed; JSON Lines and logfmt are line formats, not schemas");
    CHECK(FieldLayout::isTemplateDefinition(converted) && old >= 0 && c.schemas()[old].schema == converted,
          "the migrated schema is a line template");
    CHECK(QFileInfo::exists(QDir(SchemaStore::patternsDir()).filePath(QStringLiteral("converted-from-v3/Old.json"))),
          "the original of the migrated schema is kept in converted-from-v3/");
    CHECK(c.activeIndex() == old && c.activeSchema() == converted, "active schema found by its text");
    CHECK(c.activeSchemaName() == QLatin1String("Old"), "active schema name for sessions");
    CHECK(c.showFields() && c.fieldDisplayEnabled(), "Filter blocks restored");
    CHECK(c.visibleFieldNames() == (QStringList{QStringLiteral("Level"), QStringLiteral("Message")}),
          QStringLiteral("legacy mask becomes names: %1").arg(c.visibleFieldNames().join(',')));
    CHECK(c.visibleFieldIndexes() == (QVector<int>{2, 3}), "visible indexes follow the names");

    // Второй запуск: каталог уже в файлах, миграции нет, порядок — из ini.
    {
        QSettings s(config.ini(), QSettings::IniFormat);
        s.beginGroup(QStringLiteral("View"));
        c.save(s);
        s.endGroup();
    }
    FieldSchemaController again;
    {
        QSettings s(config.ini(), QSettings::IniFormat);
        s.beginGroup(QStringLiteral("View"));
        again.load(s);
        s.endGroup();
    }
    QStringList names, namesAgain;
    for (const auto& e : c.schemas())
        names << e.name;
    for (const auto& e : again.schemas())
        namesAgain << e.name;
    CHECK(names == namesAgain, "catalogue order survives a restart");
    CHECK(again.activeIndex() == c.activeIndex() && again.activeSchema() == converted,
          "active schema survives a restart");
    CHECK(again.visibleFieldNames() == c.visibleFieldNames() && again.showFields(),
          "column choice and Filter blocks survive a restart");
}

static void checkColumnChoice()
{
    ConfigDir config;
    const QString a = schemaNamed(QStringLiteral("Level"), QStringLiteral("Message"));
    const QString b = schemaNamed(QStringLiteral("Severity"), QStringLiteral("Message"));
    SchemaStore::sync({ {QStringLiteral("A"), a, {}, {}}, {QStringLiteral("B"), b, {}, {}},
                        {QStringLiteral("C"), schemaRest(), {}, {}} });
    FieldSchemaController c;
    {
        QSettings s(config.ini(), QSettings::IniFormat);
        s.beginGroup(QStringLiteral("View"));
        s.setValue(QStringLiteral("schemaOrder"),
                   QStringList{QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")});
        c.load(s);
        s.endGroup();
    }
    CHECK(c.activeIndex() == -1 && c.activeSchema().isEmpty() && c.fieldNames().isEmpty(),
          "no active schema by default");
    CHECK(!c.showFields() && !c.fieldDisplayEnabled(), "Filter blocks off by default");
    int fieldSignals = 0;
    QObject::connect(&c, &FieldSchemaController::fieldsChanged, [&] { ++fieldSignals; });

    c.selectSchema(indexOf(c.schemas(), QStringLiteral("A")));
    CHECK(c.activeSchema() == a && c.fieldNames().size() == 4, "schema A selected");
    CHECK(c.visibleFieldNames() == c.fieldNames(), "new schema without a choice shows every column");
    CHECK(fieldSignals > 0, "fields announced");
    c.setVisibleFieldNames({QStringLiteral("Level"), QStringLiteral("Message"), QStringLiteral("Nope")});
    CHECK(c.visibleFieldNames() == (QStringList{QStringLiteral("Level"), QStringLiteral("Message")}),
          "choice keeps only columns of the schema");
    c.selectSchema(indexOf(c.schemas(), QStringLiteral("B")));
    CHECK(c.visibleFieldNames() == QStringList{QStringLiteral("Message")},
          QStringLiteral("choice survives a schema switch by name: %1").arg(c.visibleFieldNames().join(',')));
    c.selectSchema(indexOf(c.schemas(), QStringLiteral("C")));
    CHECK(c.visibleFieldNames() == (QStringList{QStringLiteral("When"), QStringLiteral("Rest")}),
          "no chosen column in the schema: all columns");
    c.setVisibleFieldNames({});
    CHECK(c.visibleFieldNames().isEmpty() && c.visibleFieldIndexes().isEmpty(), "everything unchecked");
    c.selectSchema(-1);
    CHECK(c.activeIndex() == -1 && c.activeSchema().isEmpty() && c.fieldNames().isEmpty(),
          "schema cleared");

    // Перезапуск: ini → новый контроллер (без вкладок выбор — схема по умолчанию).
    c.setShowFields(true);
    c.selectSchema(indexOf(c.schemas(), QStringLiteral("A")));
    c.setVisibleFieldNames({QStringLiteral("Thread")});
    {
        QSettings s(config.ini(), QSettings::IniFormat);
        s.beginGroup(QStringLiteral("View"));
        c.save(s);
        s.endGroup();
    }
    FieldSchemaController restarted;
    {
        QSettings s(config.ini(), QSettings::IniFormat);
        s.beginGroup(QStringLiteral("View"));
        restarted.load(s);
        s.endGroup();
    }
    CHECK(restarted.activeSchema() == a && restarted.showFields()
              && restarted.visibleFieldNames() == QStringList{QStringLiteral("Thread")},
          "column choice survives a restart");

    // Редактор схем: «Use Selected» — новая активная; без него активная,
    // которой больше нет в списке, действует без пункта.
    SchemaStore::SchemaList edited = c.schemas();
    const int bIndex = indexOf(edited, QStringLiteral("B"));
    QString error = c.replaceCatalogue(edited, edited[bIndex].schema, bIndex);
    CHECK(error.isEmpty() && c.activeIndex() == bIndex && c.activeSchema() == b, "Use Selected");
    edited.removeAt(bIndex);
    error = c.replaceCatalogue(edited, QString(), -1);
    CHECK(error.isEmpty() && c.activeIndex() == -1 && c.activeSchema() == b,
          "removed active schema stays in effect without an entry");
    CHECK(!QFileInfo::exists(QDir(SchemaStore::patternsDir()).filePath(QStringLiteral("B.json"))),
          "removed schema file is deleted");
    c.reloadCatalogue();
    CHECK(c.schemas().size() == edited.size() && c.activeIndex() == -1, "reload keeps the list");
}

static void checkSavedSchema()
{
    ConfigDir config;
    const QString a = schemaNamed(QStringLiteral("Level"), QStringLiteral("Message"));
    const QString foreign = schemaNamed(QStringLiteral("Lvl"), QStringLiteral("Text"));
    SchemaStore::sync({ {QStringLiteral("A"), a, {}, {}} });
    FieldSchemaController c;
    {
        QSettings s(config.ini(), QSettings::IniFormat);
        s.beginGroup(QStringLiteral("View"));
        c.load(s);
        s.endGroup();
    }
    FieldSettings none;
    none.showFields = true;
    auto resolved = c.resolveSaved(none, true);
    CHECK(resolved && !resolved->hasSchema() && resolved->showFields, "saved tab without a schema");

    FieldSettings known;
    known.schemaName = QStringLiteral("Other name");
    known.schema = a;
    known.showFields = true;
    known.visibleFields = {QStringLiteral("Message")};
    QString error;
    resolved = c.resolveSaved(known, true, &error);
    CHECK(resolved && error.isEmpty() && resolved->schemaName == QLatin1String("A")
              && resolved->schema == a,
          "same definition — the catalogue entry, whatever its name");
    CHECK(resolved && resolved->showFields && resolved->visibleFields == QStringList{QStringLiteral("Message")},
          "saved Filter blocks and columns");
    resolved = c.resolveSaved(known, false);
    CHECK(resolved && resolved->schemaName == QLatin1String("A"), "file memory finds it as well");

    FieldSettings unknown;
    unknown.schemaName = QStringLiteral("Mine");
    unknown.schema = foreign;
    const int before = c.schemas().size();
    CHECK(!c.resolveSaved(unknown, false) && c.schemas().size() == before,
          "file memory does not add a missing schema to the catalogue");
    resolved = c.resolveSaved(unknown, true, &error);
    CHECK(resolved && error.isEmpty() && c.schemas().size() == before + 1
              && resolved->schemaName.contains(QLatin1String("from session"))
              && resolved->schema == foreign,
          "missing schema is added as «(from session)»");
    // Та же схема второй вкладки — та же запись каталога.
    resolved = c.resolveSaved(unknown, true, &error);
    CHECK(resolved && c.schemas().size() == before + 1
              && resolved->schemaName.contains(QLatin1String("from session")),
          "same definition twice — one catalogue entry");
}

// ---------------------------------------------------------------------------
// Вкладки
// ---------------------------------------------------------------------------

static void checkViews(const QDir& dir, bool indexed)
{
    const QString label = indexed ? QStringLiteral("indexed") : QStringLiteral("resident");
    ConfigDir config;
    AppSettings::instance().setIndexedThresholdMB(indexed ? 0 : 512);
    const QString a = schemaNamed(QStringLiteral("Level"), QStringLiteral("Message"));
    const QString b = schemaNamed(QStringLiteral("Severity"), QStringLiteral("Message"));
    SchemaStore::sync({ {QStringLiteral("A"), a, {}, {}}, {QStringLiteral("B"), b, {}, {}} });

    const int records = 300;
    const QString plain = writeFile(dir, QStringLiteral("plain-%1.log").arg(label), logBytes(records));
    const QString json = writeFile(dir, QStringLiteral("json-%1.jsonl").arg(label), jsonBytes(records));

    std::vector<std::unique_ptr<LogViewWidget>> tabs;
    FieldSchemaController c;
    int rebound = 0, applied = 0;
    c.setWindow({
        [&]() {
            QList<LogViewWidget*> list;
            for (const auto& t : tabs)
                list << t.get();
            return list;
        } });
    QObject::connect(&c, &FieldSchemaController::filtersRebound, [&](LogViewWidget*) { ++rebound; });
    QObject::connect(&c, &FieldSchemaController::schemaApplied, [&](LogViewWidget*) { ++applied; });
    {
        QSettings s(config.ini(), QSettings::IniFormat);
        s.beginGroup(QStringLiteral("View"));
        c.load(s);
        s.endGroup();
    }
    const auto newTab = [&](const QString& path) {
        tabs.push_back(std::make_unique<LogViewWidget>());
        LogViewWidget* tab = tabs.back().get();
        c.configureView(tab);
        tab->addLogFile(path);
        CHECK(settled(*tab), label + ": tab settles");
        return tab;
    };
    LogViewWidget* first = newTab(plain);
    LogViewWidget* second = newTab(plain);

    // Док — у первой вкладки: выбор меняет только её.
    c.setCurrentView(first);
    c.setShowFields(true); // без схемы колонок нет — показ выключен
    CHECK(!first->model()->fieldDisplayFilterEnabled(), label + ": no schema, no column display");
    c.selectSchema(indexOf(c.schemas(), QStringLiteral("A")));
    c.setVisibleFieldNames({QStringLiteral("Level")});
    CHECK(settled(*first), label + ": settles after the switch");
    CHECK(first->parserPattern() == a && first->fieldSettings().schemaName == QLatin1String("A"),
          label + ": schema switch reaches the dock's tab");
    CHECK(first->model()->fieldDisplayFilterEnabled()
              && first->model()->visibleFieldIndexes() == QVector<int>{2},
          label + ": column choice reaches the dock's tab");
    CHECK(showsOnlyLevels(*first->model()), label + ": rows show the chosen column");
    CHECK(second->parserPattern().isEmpty() && !second->model()->fieldDisplayFilterEnabled()
              && !second->fieldSettings().showFields,
          label + ": the other tab keeps its schema");
    CHECK(applied >= 1, label + ": application announced");

    // Правило по колонке на первой вкладке переживает смену схемы второй.
    FilterRuleSet rules = levelRule(QStringLiteral("Level"), QStringLiteral("ERROR"));
    FieldSchemaController::bindRules(first, rules);
    CHECK(!rules.fieldMissing(0), label + ": rule bound to a column of the tab's schema");
    first->model()->setFilterRules(rules);
    CHECK(settled(*first) && first->model()->rowCount() == errorCount(records),
          QStringLiteral("%1: rule on Level filters (%2)").arg(label).arg(first->model()->rowCount()));
    c.setCurrentView(second);
    CHECK(c.activeSchema().isEmpty() && c.activeIndex() == -1 && !c.showFields(),
          label + ": the dock follows the tab");
    rebound = 0;
    c.setShowFields(true);
    c.selectSchema(indexOf(c.schemas(), QStringLiteral("B")));
    CHECK(settled(*second) && second->parserPattern() == b, label + ": schema B on the second tab");
    CHECK(rebound == 0, label + ": the tab without rules is not rebound");
    CHECK(first->parserPattern() == a && first->model()->rowCount() == errorCount(records)
              && first->model()->visibleFieldIndexes() == QVector<int>{2}
              && showsOnlyLevels(*first->model()),
          label + ": the neighbour's columns and field filter are untouched");
    FilterRuleSet onB = levelRule(QStringLiteral("Level"), QStringLiteral("ERROR"));
    FieldSchemaController::bindRules(second, onB);
    CHECK(onB.fieldMissing(0), label + ": rule on a column the tab's schema lacks is neutral");

    c.setCurrentView(first);
    CHECK(c.activeSchema() == a && c.visibleFieldNames() == QStringList{QStringLiteral("Level")},
          label + ": back on the first tab — its schema and columns");
    rebound = 0;
    c.selectSchema(indexOf(c.schemas(), QStringLiteral("B")));
    CHECK(settled(*first) && rebound == 1, label + ": the tab with rules is rebound");
    CHECK(first->model()->rowCount() == records,
          label + ": rule on a column the new schema lacks is neutral");
    c.selectSchema(indexOf(c.schemas(), QStringLiteral("A")));
    CHECK(settled(*first) && first->model()->rowCount() == errorCount(records),
          label + ": rule bound again after switching back");

    // A → B: у B нет Level — выбраны все колонки B; B → A: те из них, что есть в A.
    CHECK(c.visibleFieldNames() == (QStringList{QStringLiteral("Timestamp"), QStringLiteral("Thread"),
                                                QStringLiteral("Message")}),
          label + ": column choice follows the names through B and back");
    c.setVisibleFieldNames({QStringLiteral("Level")});
    // Новая вкладка — схема по умолчанию: последняя выбранная в доке.
    LogViewWidget* third = newTab(plain);
    CHECK(third->parserPattern() == a && third->model()->fieldDisplayFilterEnabled()
              && showsOnlyLevels(*third->model()),
          label + ": new tab takes the default schema");

    // «Apply Schema to All Tabs»: схема дока на всех вкладках.
    c.applyToAllViews();
    for (const auto& t : tabs) {
        CHECK(settled(*t) && t->parserPattern() == a && t->model()->fieldDisplayFilterEnabled()
                  && t->model()->visibleFieldIndexes() == QVector<int>{2},
              label + ": Apply Schema to All Tabs reaches every tab");
    }

    // JSON-файл (F-17, шаг 7): поля — ключи формата строк файла, при любой
    // схеме вкладки; колонки ролей — те же имена (Level).
    LogViewWidget* keyed = newTab(json);
    c.setCurrentView(keyed);
    c.setVisibleFieldNames({QStringLiteral("Level")});
    CHECK(settled(*keyed), label + ": keyed tab settles");
    CHECK(!keyed->loadedFiles().isEmpty()
              && keyed->lineFormat(keyed->loadedFiles().first()).kind == LineFormat::Kind::Json
              && dynamic_cast<const KeyedExtractor*>(
                     keyed->model()->fileSchemaFor(keyed->loadedFiles().first()->filePath)->extractor.get()),
          label + ": the JSON file reads its keys");
    FilterRuleSet keyedRules = levelRule(QStringLiteral("Level"), QStringLiteral("ERROR"));
    FieldSchemaController::bindRules(keyed, keyedRules);
    keyed->model()->setFilterRules(keyedRules);
    CHECK(settled(*keyed) && keyed->model()->rowCount() == errorCount(records),
          QStringLiteral("%1: rule on a keyed column (%2)").arg(label).arg(keyed->model()->rowCount()));
    CHECK(showsOnlyLevels(*keyed->model()), label + ": keyed column shown decoded");

    // «Show selected fields only» выключена — только показ: правило по полю
    // ищет в поле как прежде (F-17, шаг 3).
    c.setShowFields(false);
    CHECK(settled(*keyed), label + ": settles after Show selected fields off");
    CHECK(!keyed->model()->fieldDisplayFilterEnabled(), label + ": column display off on the tab");
    CHECK(keyed->model()->rowCount() == errorCount(records),
          label + ": the rule on a column still filters with the display off");
    CHECK(first->model()->fieldDisplayFilterEnabled(), label + ": the neighbour keeps its columns");

    // Редактор схем без «Use Selected»: схема вкладки уже применена — заново
    // не применяется (вкладка не переизвлекается, поиск не сбрасывается).
    c.setCurrentView(first);
    applied = 0;
    c.replaceCatalogue(c.schemas(), QString(), -1);
    CHECK(applied == 0 && c.activeSchema() == a, label + ": editor without a choice keeps the schema");

    // Сохранённая схема вкладки (applyState → savedFieldSettingsFound).
    LogViewWidget* restored = newTab(plain);
    TabState state = first->captureState();
    CHECK(state.view.fields && state.view.fields->schema == a && state.view.fields->showFields
              && state.view.fields->visibleFields == QStringList{QStringLiteral("Level")},
          label + ": captureState takes the tab's schema");
    state.sources.clear();
    state.bookmarks.clear();
    state.view.textRules = {};
    restored->applyState(state);
    CHECK(settled(*restored) && restored->parserPattern() == a
              && restored->model()->visibleFieldIndexes() == QVector<int>{2},
          label + ": applyState sets the saved schema of the tab");
}

// Смена схемы на большой резидентной вкладке (F-17, шаг 3): поля — функция
// текста и схемы, поэтому смена — O(1): ни reset, ни прохода по записям,
// вызовов извлекателя — только у фильтра по полю (не больше строк, где есть
// образец) и у показа видимых строк. Соседняя вкладка не тронута.
static void checkLazySchemaSwitch(const QDir& dir)
{
    ConfigDir config;
    AppSettings::instance().setIndexedThresholdMB(512);
    const QString a = schemaNamed(QStringLiteral("Level"), QStringLiteral("Message"));
    const QString b = schemaNamed(QStringLiteral("Level"), QStringLiteral("Text"));
    SchemaStore::sync({ {QStringLiteral("A"), a, {}, {}}, {QStringLiteral("B"), b, {}, {}} });

    // Больше 100 000 записей — резидентный фильтр уходит в пул.
    const int records = 120000;
    const QString path = writeFile(dir, QStringLiteral("big.log"), logBytes(records));
    auto tab = std::make_unique<LogViewWidget>();
    auto neighbour = std::make_unique<LogViewWidget>();
    FieldSchemaController c;
    c.setWindow({ [&]() { return QList<LogViewWidget*>{tab.get(), neighbour.get()}; } });
    {
        QSettings s(config.ini(), QSettings::IniFormat);
        s.beginGroup(QStringLiteral("View"));
        c.load(s);
        s.endGroup();
    }
    c.configureView(tab.get());
    c.configureView(neighbour.get());
    tab->addLogFile(path);
    neighbour->addLogFile(writeFile(dir, QStringLiteral("small.log"), logBytes(200)));
    CHECK(settled(*tab) && tab->model()->rowCount() == records, "big tab loads");
    CHECK(settled(*neighbour), "neighbour tab loads");
    CHECK(!tab->model()->isIndexedBackend(), "big tab is resident");
    c.setCurrentView(tab.get());

    LogModel& model = *tab->model();
    int resets = 0;
    QObject::connect(&model, &QAbstractItemModel::modelReset, [&] { ++resets; });
    model.fieldSource().resetStats();
    c.setShowFields(true);
    c.selectSchema(indexOf(c.schemas(), QStringLiteral("A")));
    CHECK(settled(*tab), "settles after the switch");
    CHECK(resets == 0, QStringLiteral("schema switch: no model reset (%1)").arg(resets));
    CHECK(model.fieldSource().stats().scanExtracts == 0,
          "schema switch: no pass over the entries");
    CHECK(model.fieldDisplayFilterEnabled(), "column display on at once");

    // Правило по полю: вызовов extract в воркере — не больше строк, где есть
    // образец (предфильтр по строке), и ровно один на запись.
    FilterRuleSet rules = levelRule(QStringLiteral("Level"), QStringLiteral("ERROR"));
    FieldSchemaController::bindRules(tab.get(), rules);
    model.fieldSource().resetStats();
    model.setFilterRules(rules);
    CHECK(model.isFiltering(), "field filter runs in the pool");
    CHECK(settled(*tab) && model.rowCount() == errorCount(records),
          QStringLiteral("field filter (%1)").arg(model.rowCount()));
    const qint64 extracts = model.fieldSource().stats().scanExtracts;
    CHECK(extracts <= errorCount(records),
          QStringLiteral("field filter: extract calls %1 <= lines with the text %2")
              .arg(extracts).arg(errorCount(records)));

    // Смена схемы с правилом по полю — перефильтрация, без reset записей.
    resets = 0;
    c.selectSchema(indexOf(c.schemas(), QStringLiteral("B")));
    CHECK(settled(*tab) && model.rowCount() == errorCount(records) && tab->parserPattern() == b,
          "rule re-applied with the new schema");
    c.setVisibleFieldNames({QStringLiteral("Level")});
    CHECK(settled(*tab) && model.fieldDisplayFilterEnabled()
              && model.visibleFieldIndexes() == QVector<int>{2},
          "column display with the chosen column");
    bool levels = true;
    for (int row = 0; row < 50 && row < model.rowCount(); ++row)
        levels = levels && model.data(model.index(row), Qt::DisplayRole)
                               .toString().trimmed() == QLatin1String("ERROR");
    CHECK(levels, "rows show fields of the new schema");
    CHECK(neighbour->parserPattern().isEmpty() && !neighbour->model()->fieldDisplayFilterEnabled(),
          "the neighbour tab keeps its schema");
}


// ---------------------------------------------------------------------------
// Подбор при открытии (F-17, шаг 8)
// ---------------------------------------------------------------------------

// Мера с заданным покрытием и специфичностью (rules — без файлов).
static SchemaFit::Result fitResult(int candidates, int full, int partial, int fieldsPerLine = 3)
{
    SchemaFit::Result r;
    r.valid = true;
    r.examined = candidates;
    r.candidates = candidates;
    r.full = full;
    r.partial = partial;
    r.missed = candidates - full - partial;
    r.fieldsOnCandidates = qint64(full + partial) * fieldsPerLine;
    r.keyedRoles = full + partial;
    return r;
}

static void checkPickRules()
{
    using namespace SchemaPick;
    CHECK(nameKey(QStringLiteral("App-2026-11-26.log.3.gz")) == QLatin1String("app-#-#-#.log"),
          "name key: case, rotation tail, .gz, digits");
    CHECK(nameKey(QStringLiteral("app.log.1")) == nameKey(QStringLiteral("APP.log")),
          "name key: rotation tail dropped");
    CHECK(nameKey(QStringLiteral("server12.log")) == QLatin1String("server#.log"), "name key: digits");
    CHECK(nameKey(QStringLiteral("app.log-20260101")) == QLatin1String("app.log"), "name key: dated tail");

    const SchemaFit::Result fits = fitResult(100, 98, 0);       // подходит
    const SchemaFit::Result doubtful = fitResult(100, 60, 0);   // 0,60
    const SchemaFit::Result poor = fitResult(100, 30, 0);       // 0,30
    Input in;
    in.results = { poor, fits, doubtful };
    in.catalogueCount = 3;
    CHECK(choose(in).source == Source::Content && choose(in).index == 1, "content: the fitting schema");
    in.nameIndex = 2;
    CHECK(choose(in).source == Source::Name && choose(in).index == 2,
          "name beats content when it parses at least half");
    in.nameIndex = 0;
    CHECK(choose(in).source == Source::Content, "name below 0.50 — content");
    in.nameNone = true;
    CHECK(choose(in).source == Source::NameNone && choose(in).index == -1,
          "\"no schema\" for the name is not contested");
    in = Input();
    in.results = { poor, doubtful };
    in.catalogueCount = 2;
    CHECK(choose(in).source == Source::None, "nothing fits — no schema");
    in.defaultIndex = 1;
    CHECK(choose(in).source == Source::Default && choose(in).index == 1, "default — at least half");
    in.defaultIndex = 0;
    CHECK(choose(in).source == Source::None, "default below 0.50 — no schema");
    CHECK(fitsFirst({ poor, fits, doubtful, fits }) == QVector<int>({1, 3, 0, 2}),
          "combo order: fitting first");

    NameTable table;
    for (int i = 0; i < kMaxNames + 20; ++i)
        table.remember(QStringLiteral("k%1").arg(i), QStringLiteral("S%1").arg(i));
    CHECK(table.size() == kMaxNames && !table.lookup(QStringLiteral("k0"))
              && table.lookup(QStringLiteral("k219")) == QStringLiteral("S219"),
          "name table: at most 200, the oldest goes");
    table.remember(QStringLiteral("none"), QString());
    const NameTable back = NameTable::fromJson(table.toJson());
    CHECK(back.size() == kMaxNames && back.lookup(QStringLiteral("none")) == QString()
              && back.lookup(QStringLiteral("k100")) == QStringLiteral("S100"),
          "name table: JSON round trip, \"no schema\" kept");
}

static QByteArray corpusBytes(const builtinsamples::SchemaCase& sc, int repeat = 4)
{
    QByteArray bytes;
    for (int r = 0; r < repeat; ++r)
        for (const builtinsamples::Sample& sample : sc.samples)
            bytes += sample.line.toUtf8() + '\n';
    return bytes;
}

static QByteArray unknownBytes()
{
    // Ни меток, ни уровней, ни устойчивых полей.
    QByteArray bytes;
    const char* words[] = { "alpha", "beta", "gamma", "delta", "omega", "kappa" };
    for (int i = 0; i < 60; ++i)
        bytes += QByteArray(words[i % 6]) + ' ' + words[(i * 7) % 6] + " and " + words[(i * 5 + 1) % 6]
            + " went " + (i % 3 ? "home" : "out") + '\n';
    return bytes;
}

static void checkAutoPick(const QDir& dir, bool indexed)
{
    const QString label = QStringLiteral("auto pick, %1").arg(indexed ? "indexed" : "resident");
    ConfigDir config;
    AppSettings::instance().setIndexedThresholdMB(indexed ? 0 : 512);
    AppSettings::instance().setAutoPickSchema(true);
    std::vector<std::unique_ptr<LogViewWidget>> tabs;
    FieldSchemaController c;
    c.setWindow({ [&]() {
        QList<LogViewWidget*> list;
        for (const auto& t : tabs)
            list << t.get();
        return list;
    } });
    SchemaFit::Budget unlimited;
    unlimited.maxMs = 0;    // санитайзер: без предела времени
    c.setFitBudget(unlimited);
    {
        QSettings s(config.ini(), QSettings::IniFormat);
        s.beginGroup(QStringLiteral("View"));
        c.load(s);
        s.endGroup();
    }
    QHash<QString, QString> nameOfFile;
    for (const auto& e : c.schemas())
        nameOfFile.insert(e.fileName, e.name);
    CHECK(nameOfFile.size() >= 9, label + ": built-in schemas installed");

    struct Opened {
        LogViewWidget* tab = nullptr;
        int passes = 0;
        int jobsAtFirstRows = -1;
    };
    // Открыть вкладку и дождаться мер подбора.
    const auto open = [&](const QString& path, const std::function<void(LogViewWidget*)>& before = {}) {
        auto opened = std::make_shared<Opened>();
        tabs.push_back(std::make_unique<LogViewWidget>());
        LogViewWidget* tab = tabs.back().get();
        opened->tab = tab;
        c.configureView(tab);
        QObject::connect(tab, &LogViewWidget::fileParsingStarted, tab, [opened]() { ++opened->passes; });
        const auto firstRows = [&c, opened]() {
            if (opened->jobsAtFirstRows < 0)
                opened->jobsAtFirstRows = c.pickJobsStarted();
        };
        QObject::connect(tab->model(), &QAbstractItemModel::rowsInserted, tab, firstRows);
        QObject::connect(tab->model(), &QAbstractItemModel::modelReset, tab, [opened, firstRows, tab]() {
            if (tab->model()->rowCount() > 0)
                firstRows();
        });
        if (before)
            before(tab);
        tab->addLogFile(path);
        CHECK(settled(*tab), label + ": tab settles");
        waitFor([&] { return !c.isPicking(tab) && c.coverageOf(tab); }, 30000);
        settled(*tab);
        return opened;
    };

    // Корпус встроенных форматов: схема подставлена, проход один, первая
    // порция — до задания.
    for (const builtinsamples::SchemaCase& sc : builtinsamples::cases()) {
        const QString file = sc.file.startsWith(QLatin1String("syslog-")) ? QStringLiteral("syslog.json") : sc.file;
        const QString expected = nameOfFile.value(file);
        const QString path = writeFile(dir, QStringLiteral("pick-%1-%2.log").arg(label.right(3), sc.file), corpusBytes(sc));
        const int jobsBefore = c.pickJobsStarted();
        const auto o = open(path);
        const FieldSettings got = o->tab->fieldSettings();
        CHECK(got.schemaName == expected && got.origin == FieldSettings::Origin::Auto,
              QStringLiteral("%1: %2 — picked \"%3\", expected \"%4\"").arg(label, sc.file, got.schemaName, expected));
        CHECK(o->passes == 1, QStringLiteral("%1: %2 — one loader pass (%3)").arg(label, sc.file).arg(o->passes));
        CHECK(o->jobsAtFirstRows == jobsBefore && c.pickJobsStarted() == jobsBefore + 1,
              QStringLiteral("%1: %2 — the first rows come before the one pick job").arg(label, sc.file));
        CHECK(o->tab->model()->rowCount() > 0, label + ": rows are shown");
    }
    const builtinsamples::SchemaCase logback = builtinsamples::cases().constFirst();
    const QString logbackName = nameOfFile.value(logback.file);

    // Незнакомый формат — без схемы.
    {
        const auto o = open(writeFile(dir, QStringLiteral("unknown-%1.txt").arg(indexed), unknownBytes()));
        CHECK(!o->tab->fieldSettings().hasSchema() && c.pickJobsStarted() > 0,
              label + ": unknown format — no schema");
    }

    // Сохранённое состояние главнее содержимого: Python logging на строках Logback.
    {
        const QString python = nameOfFile.value(QStringLiteral("python-logging.json"));
        const auto o = open(writeFile(dir, QStringLiteral("state-%1.log").arg(indexed), corpusBytes(logback)),
                            [&](LogViewWidget* tab) {
                                TabState state;
                                FieldSettings fields;
                                fields.schemaName = python;
                                fields.schema = c.schemas()[indexOf(c.schemas(), python)].schema;
                                fields.origin = FieldSettings::Origin::User;
                                state.view.fields = fields;
                                tab->applyState(state, LogViewWidget::StateFilters::Apply);
                            });
        CHECK(o->tab->fieldSettings().schemaName == python,
              QStringLiteral("%1: saved state beats content (%2)").arg(label, o->tab->fieldSettings().schemaName));
    }

    // Выбор в доке запоминается для имени файла и главнее содержимого:
    // nginx error — строки, которые Syslog тоже разбирает (схема по имени
    // ≥ 0,50), а по содержимому выигрывает nginx error.
    const QVector<builtinsamples::SchemaCase> allCases = builtinsamples::cases();
    const builtinsamples::SchemaCase* nginx = nullptr;
    for (const auto& sc : allCases)
        if (sc.file == QLatin1String("nginx-error.json"))
            nginx = &sc;
    const QString syslog = nameOfFile.value(QStringLiteral("syslog.json"));
    const QString nginxName = nameOfFile.value(QStringLiteral("nginx-error.json"));
    if (nginx) {
        const auto first = open(writeFile(dir, QStringLiteral("edge-%1-1.log").arg(indexed), corpusBytes(*nginx)));
        CHECK(first->tab->fieldSettings().schemaName == nginxName, label + ": nginx error by content");
        c.setCurrentView(first->tab);
        c.selectSchema(indexOf(c.schemas(), syslog));
        CHECK(first->tab->fieldSettings().origin == FieldSettings::Origin::User, label + ": dock choice — user");
        const auto second = open(writeFile(dir, QStringLiteral("edge-%1-2.log").arg(indexed), corpusBytes(*nginx)));
        CHECK(second->tab->fieldSettings().schemaName == syslog
                  && second->tab->fieldSettings().origin == FieldSettings::Origin::Auto,
              QStringLiteral("%1: the name beats content (%2)").arg(label, second->tab->fieldSettings().schemaName));
        c.setCurrentView(nullptr);
    }

    // «No schema» не оспаривается: ни по имени, ни на вкладке.
    {
        const auto first = open(writeFile(dir, QStringLiteral("quiet-%1-1.log").arg(indexed), corpusBytes(logback)));
        CHECK(first->tab->fieldSettings().schemaName == logbackName, label + ": Logback by content");
        c.setCurrentView(first->tab);
        c.selectSchema(-1);
        CHECK(!first->tab->fieldSettings().hasSchema()
                  && first->tab->fieldSettings().origin == FieldSettings::Origin::User,
              label + ": \"No schema\" chosen");
        c.setCurrentView(nullptr);
        const auto second = open(writeFile(dir, QStringLiteral("quiet-%1-2.log").arg(indexed), corpusBytes(logback)));
        CHECK(!second->tab->fieldSettings().hasSchema(),
              label + ": \"No schema\" for the name is not contested");
        CHECK(!first->tab->fieldSettings().hasSchema(), label + ": the tab keeps \"No schema\"");
        // Сохранённое состояние вкладки несёт выбор и не оспаривается.
        const TabState state = first->tab->captureState();
        CHECK(state.view.fields && state.view.fields->origin == FieldSettings::Origin::User,
              label + ": the state carries the user's choice");
    }

    // Таблица имён переживает перезапуск (ini).
    {
        QSettings s(config.ini(), QSettings::IniFormat);
        s.beginGroup(QStringLiteral("View"));
        c.save(s);
        s.endGroup();
        FieldSchemaController again;
        s.beginGroup(QStringLiteral("View"));
        again.load(s);
        s.endGroup();
        CHECK(again.nameTable().lookup(SchemaPick::nameKey(QStringLiteral("quiet-0-7.log"))) == QString()
                  && again.nameTable().lookup(SchemaPick::nameKey(QStringLiteral("edge-0-9.log"))) == syslog,
              label + ": the name table is kept in the ini");
    }

    // «Detect for This Tab» — подбор заново и при выборе пользователя.
    if (!tabs.empty()) {
        LogViewWidget* tab = nullptr;
        for (const auto& t : tabs)
            if (t->loadedFiles().size() == 1 && t->loadedFiles().first()->displayName.startsWith(QLatin1String("state-")))
                tab = t.get();
        if (tab) {
            c.detectForView(tab);
            waitFor([&] { return !c.isPicking(tab); }, 30000);
            CHECK(tab->fieldSettings().schemaName == logbackName
                      && tab->fieldSettings().origin == FieldSettings::Origin::Auto,
                  label + ": Detect for This Tab picks by content");
        }
    }

    // Настройка выключена — заданий нет.
    {
        AppSettings::instance().setAutoPickSchema(false);
        const int jobsBefore = c.pickJobsStarted();
        tabs.push_back(std::make_unique<LogViewWidget>());
        LogViewWidget* tab = tabs.back().get();
        c.configureView(tab);
        tab->addLogFile(writeFile(dir, QStringLiteral("off-%1.log").arg(indexed), corpusBytes(logback)));
        CHECK(settled(*tab), label + ": tab settles with the setting off");
        pump(100);
        CHECK(c.pickJobsStarted() == jobsBefore && !tab->fieldSettings().hasSchema(),
              label + ": setting off — no jobs, no schema");
        AppSettings::instance().setAutoPickSchema(true);
    }
    tabs.clear();
    AppSettings::instance().setAutoPickSchema(false);
}


// Самое долгое событие GUI-потока (как в field_source).
class TimingApplication : public QApplication {
public:
    using QApplication::QApplication;
    bool notify(QObject* receiver, QEvent* event) override
    {
        if (!timing)
            return QApplication::notify(receiver, event);
        QElapsedTimer t;
        t.start();
        const bool result = QApplication::notify(receiver, event);
        const qint64 ms = t.elapsed();
        if (ms > maxMs) {
            maxMs = ms;
            maxWhat = QString::fromLatin1(receiver->metaObject()->className()) + QLatin1Char('/')
                + QString::number(int(event->type()));
        }
        return result;
    }
    bool timing = false;
    qint64 maxMs = 0;
    QString maxWhat;
};

// Режим замера (F-17, шаг 8): открытие файла с подбором и без — время до
// первых строк, до конца загрузки, до итога подбора и самое долгое событие
// GUI-потока. Каталог — встроенные схемы.
static int runBench(TimingApplication& app, const QString& file)
{
    for (const bool indexed : {false, true}) {
        for (const bool pick : {false, true}) {
            for (int round = 0; round < 3; ++round) {
                ConfigDir config;
                AppSettings::instance().setIndexedThresholdMB(indexed ? 0 : 512);
                AppSettings::instance().setAutoPickSchema(pick);
                FieldSchemaController c;
                {
                    QSettings s(config.ini(), QSettings::IniFormat);
                    s.beginGroup(QStringLiteral("View"));
                    c.load(s);
                    s.endGroup();
                }
                auto tab = std::make_unique<LogViewWidget>();
                c.configureView(tab.get());
                QElapsedTimer clock;
                qint64 firstRowsMs = -1;
                QObject::connect(tab->model(), &QAbstractItemModel::rowsInserted, tab.get(), [&]() {
                    if (firstRowsMs < 0)
                        firstRowsMs = clock.elapsed();
                });
                QObject::connect(tab->model(), &QAbstractItemModel::modelReset, tab.get(), [&]() {
                    if (firstRowsMs < 0 && tab->model()->rowCount() > 0)
                        firstRowsMs = clock.elapsed();
                });
                app.maxMs = 0;
                app.maxWhat.clear();
                app.timing = true;
                clock.start();
                tab->addLogFile(file);
                waitFor([&] { return !tab->isLoading(); }, 600000);
                const qint64 loadedMs = clock.elapsed();
                if (pick)
                    waitFor([&] { return c.pickJobsStarted() > 0 && !c.isPicking(tab.get()); }, 600000);
                const qint64 pickedMs = clock.elapsed();
                settle(*tab->model());
                app.timing = false;
                std::printf("%s pick=%s: first rows %lld ms, loaded %lld ms, picked %lld ms (%s), "
                            "longest GUI event %lld ms (%s)\n",
                            indexed ? "indexed " : "resident", pick ? "on " : "off",
                            static_cast<long long>(firstRowsMs), static_cast<long long>(loadedMs),
                            static_cast<long long>(pickedMs),
                            qPrintable(tab->fieldSettings().schemaName.isEmpty()
                                           ? QStringLiteral("no schema") : tab->fieldSettings().schemaName),
                            static_cast<long long>(app.maxMs), qPrintable(app.maxWhat));
                std::fflush(stdout);
            }
        }
    }
    return 0;
}

// Строки файла пришли, пока шло задание подбора (S-33). Задание снимает
// снапшот на старте; если строк файла в нём ещё нет, итог пуст, а порции,
// пришедшие за время задания, подбор не запускали (вкладка уже «начала»).
// Так на слитой индексной вкладке под нагрузкой схема не подбиралась вовсе
// (`logview_lifecycle`, «merged tab — the schema is picked»). Детерминированно:
// глобальный пул занят «шлагбаумом» — задание стоит в очереди, а строки
// текстового файла (рядом с JSON-файлом, у которого строки уже есть)
// приходят дозаписью до того, как оно снимет итог.
static void checkPickRowsDuringJob(const QDir& dir)
{
    const QString label = QStringLiteral("auto pick, rows during the job");
    ConfigDir config;
    AppSettings::instance().setIndexedThresholdMB(512);
    AppSettings::instance().setAutoPickSchema(true);
    std::unique_ptr<LogViewWidget> tab;
    FieldSchemaController c;
    c.setWindow({ [&]() { return tab ? QList<LogViewWidget*>{tab.get()} : QList<LogViewWidget*>{}; } });
    SchemaFit::Budget unlimited;
    unlimited.maxMs = 0;
    c.setFitBudget(unlimited);
    {
        QSettings s(config.ini(), QSettings::IniFormat);
        s.beginGroup(QStringLiteral("View"));
        c.load(s);
        s.endGroup();
    }
    QString logbackName;
    const builtinsamples::SchemaCase logback = builtinsamples::cases().constFirst();
    for (const auto& e : c.schemas())
        if (e.fileName == logback.file)
            logbackName = e.name;

    const QString json = writeFile(dir, QStringLiteral("during-job.jsonl"), jsonBytes(40));
    const QString plain = writeFile(dir, QStringLiteral("during-job.log"), QByteArray());

    // Шлагбаум: все потоки глобального пула ждут, задание подбора — в очереди.
    QThreadPool* pool = QThreadPool::globalInstance();
    const int threads = pool->maxThreadCount();
    QSemaphore gate;
    QAtomicInt blocked = 0;
    for (int i = 0; i < threads; ++i)
        pool->start([&gate, &blocked]() { blocked.fetchAndAddOrdered(1); gate.acquire(); });
    CHECK(waitFor([&] { return blocked.loadAcquire() == threads; }, 30000), label + ": the pool is blocked");

    tab = std::make_unique<LogViewWidget>();
    c.configureView(tab.get());
    tab->addLogFile(json);
    tab->addLogFile(plain);
    CHECK(waitFor([&] { return !tab->isLoading() && c.isPicking(tab.get()); }, 30000),
          label + ": the pick job waits in the pool");
    const int jobsBefore = c.pickJobsStarted();
    CHECK(testdocs::appendToFile(plain, corpusBytes(logback)), label + ": append the text lines");
    tab->reloadChangedFiles(/*force=*/true);
    CHECK(waitFor([&] {
              for (const LogFilePtr& f : tab->loadedFiles())
                  if (f->filePath == plain && tab->fileHasRows(f))
                      return !tab->isLoading();
              return false;
          }, 30000),
          label + ": the text lines arrive while the job waits");
    CHECK(c.isPicking(tab.get()) && c.pickJobsStarted() == jobsBefore,
          label + ": the rows did not start another job");

    gate.release(threads);
    waitFor([&] { return !c.isPicking(tab.get()) && c.coverageOf(tab.get()); }, 30000);
    CHECK(tab->fieldSettings().schemaName == logbackName
              && tab->fieldSettings().origin == FieldSettings::Origin::Auto,
          QStringLiteral("%1: the schema is picked by the rows that came during the job: \"%2\"")
              .arg(label, tab->fieldSettings().schemaName));
    pool->waitForDone();
    tab.reset();
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    TimingApplication app(argc, argv);
    if (argc > 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--bench"))
        return runBench(app, QString::fromLocal8Bit(argv[2]));

    // Подбор при открытии (F-17, шаг 8) включает только checkAutoPick.
    AppSettings::instance().setAutoPickSchema(false);
    checkMigrationAndBuiltins();
    checkColumnChoice();
    checkSavedSchema();

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "нет временного каталога\n");
        return 2;
    }
    const QDir dir(tmp.path());
    checkViews(dir, false);
    checkViews(dir, true);
    checkLazySchemaSwitch(dir);
    checkPickRules();
    checkAutoPick(dir, false);
    checkAutoPick(dir, true);
    checkPickRowsDuringJob(dir);

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("field_schema: OK\n");
    return 0;
}
