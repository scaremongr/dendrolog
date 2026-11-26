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
#include "documentstate.h"
#include "fieldschemacontroller.h"
#include "filterruleset.h"
#include "logmodel.h"
#include "logpattern.h"
#include "logviewwidget.h"
#include "LogListView.h"
#include "schemastore.h"
#include "testdocuments.h"

#include <QApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

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
    PatternDefinition def;
    def.blocks = {
        patternBlock(PatternBlock::MatchKind::Timestamp, QStringLiteral("Timestamp")),
        patternBlock(PatternBlock::MatchKind::TextUntilSeparator, QStringLiteral("Thread"),
                     QStringLiteral("["), QStringLiteral("]")),
        patternBlock(PatternBlock::MatchKind::Level, level, QString(), QString(),
                     QStringLiteral("-")),
        patternBlock(PatternBlock::MatchKind::Remainder, message),
    };
    return LogPattern::serializeDefinition(def);
}

// Метка и остаток строки — ни одной колонки фикстурной схемы.
static QString schemaRest()
{
    PatternDefinition def;
    def.blocks = {
        patternBlock(PatternBlock::MatchKind::Timestamp, QStringLiteral("When")),
        patternBlock(PatternBlock::MatchKind::Remainder, QStringLiteral("Rest")),
    };
    return LogPattern::serializeDefinition(def);
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
    const QString legacy = schemaNamed(QStringLiteral("Level"), QStringLiteral("Message"));
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
    const int old = indexOf(c.schemas(), QStringLiteral("Old"));
    CHECK(old >= 0, "migrated schema is in the catalogue");
    CHECK(QFileInfo::exists(QDir(SchemaStore::patternsDir()).filePath(QStringLiteral("Old.json"))),
          "migrated schema is written as a file");
    CHECK(indexOf(c.schemas(), QStringLiteral("JSON Lines")) >= 0
              && indexOf(c.schemas(), QStringLiteral("logfmt")) >= 0,
          "built-in schemas are installed, keyed ones included");
    CHECK(c.activeIndex() == old && c.activeSchema() == legacy, "active schema found by its text");
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
    CHECK(again.activeIndex() == c.activeIndex() && again.activeSchema() == legacy,
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

    // Ключевая схема (F-4) — тем же путём.
    LogViewWidget* keyed = newTab(json);
    c.setCurrentView(keyed);
    c.selectSchema(indexOf(c.schemas(), QStringLiteral("JSON Lines")));
    c.setVisibleFieldNames({QStringLiteral("Level")});
    CHECK(settled(*keyed), label + ": keyed tab settles");
    const QString jsonLines = c.activeSchema();
    CHECK(LogPattern(jsonLines).isKeyed() && keyed->parserPattern() == jsonLines,
          label + ": keyed schema reaches the tab");
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

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);

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

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("field_schema: OK\n");
    return 0;
}
