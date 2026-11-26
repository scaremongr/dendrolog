// ============================================================================
// field_schema — FieldSchemaController (M-2) без окна: каталог схем и
// состояние дока Log Fields.
//
//   • каталог: миграция списков старых версий из ini в файлы, встроенные
//     схемы, активная схема по строке, «Use Selected» и правка в редакторе,
//     схема сессии (своя в каталоге или «(from session)»);
//   • колонки: выбор по именам переживает смену схемы и перезапуск (ini),
//     маска старых версий;
//   • вкладки на обоих бэкендах: смена схемы доходит до каждой вкладки (и
//     до новой), правила вкладки перепривязываются к новой схеме, поля
//     резидентных записей — новой схемы; ключевая схема (F-4) — тем же путём;
//   • фоновое переизвлечение (> 4000 записей): фильтр, запущенный за время
//     расчёта, остановлен до установки полей (инвариант 4), затем
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
    CHECK(c.extractionEnabled() && c.fieldDisplayEnabled(), "Filter blocks restored");
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
    CHECK(again.visibleFieldNames() == c.visibleFieldNames() && again.extractionEnabled(),
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
    CHECK(!c.extractionEnabled() && !c.fieldDisplayEnabled(), "Filter blocks off by default");
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

    FilterRuleSet rules = levelRule(QStringLiteral("Level"), QStringLiteral("ERROR"));
    c.selectSchema(indexOf(c.schemas(), QStringLiteral("A")));
    c.setExtractionEnabled(true);
    c.bindRules(rules);
    CHECK(!rules.fieldMissing(0), "rule bound to a column of the active schema");
    c.selectSchema(indexOf(c.schemas(), QStringLiteral("B")));
    c.bindRules(rules);
    CHECK(rules.fieldMissing(0), "rule on a column the schema lacks is neutral");

    // Перезапуск: ini → новый контроллер.
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
    CHECK(restarted.activeSchema() == a && restarted.extractionEnabled()
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

static void checkSessionSchema()
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
    SessionState none;
    CHECK(c.applySessionSchema(none).isEmpty() && c.activeSchema().isEmpty(),
          "session without a schema changes nothing");

    SessionState known;
    known.schemaName = QStringLiteral("Other name");
    known.schema = a;
    known.fieldExtraction = true;
    known.visibleFields = {QStringLiteral("Message")};
    CHECK(c.applySessionSchema(known).isEmpty(), "known session schema");
    CHECK(c.activeIndex() == indexOf(c.schemas(), QStringLiteral("A")) && c.activeSchema() == a,
          "same definition — the catalogue entry, whatever its name");
    CHECK(c.extractionEnabled() && c.visibleFieldNames() == QStringList{QStringLiteral("Message")},
          "session Filter blocks and columns");

    SessionState unknown;
    unknown.schemaName = QStringLiteral("Mine");
    unknown.schema = foreign;
    const int before = c.schemas().size();
    CHECK(c.applySessionSchema(unknown).isEmpty(), "unknown session schema");
    CHECK(c.schemas().size() == before + 1 && c.activeIndex() >= 0
              && c.schemas()[c.activeIndex()].name.contains(QLatin1String("from session"))
              && c.activeSchema() == foreign,
          "missing schema is added as «(from session)» and selected");
    CHECK(!c.extractionEnabled() && c.visibleFieldNames() == c.fieldNames(),
          "session columns: everything when none was saved");
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
    int starts = 0, rebound = 0, applied = 0;
    c.setWindow({
        [&]() {
            QList<LogViewWidget*> list;
            for (const auto& t : tabs)
                list << t.get();
            return list;
        },
        [&]() { ++starts; } });
    QObject::connect(&c, &FieldSchemaController::filtersRebound, [&](LogViewWidget*) { ++rebound; });
    QObject::connect(&c, &FieldSchemaController::schemaApplied, [&] { ++applied; });
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

    c.setExtractionEnabled(true); // без схемы колонок нет — показ выключен
    CHECK(!first->model()->fieldDisplayFilterEnabled(), label + ": no schema, no column display");
    c.selectSchema(indexOf(c.schemas(), QStringLiteral("A")));
    c.setVisibleFieldNames({QStringLiteral("Level")});
    for (LogViewWidget* t : {first, second}) {
        CHECK(settled(*t), label + ": settles after the switch");
        CHECK(t->parserPattern() == a, label + ": schema switch reaches every tab");
        CHECK(t->model()->fieldDisplayFilterEnabled()
                  && t->model()->visibleFieldIndexes() == QVector<int>{2},
              label + ": column choice reaches every tab");
        CHECK(showsOnlyLevels(*t->model()), label + ": rows show the chosen column");
    }
    CHECK(starts >= 1 && applied >= 1, label + ": readers stopped, application announced");

    // Правило по колонке на одной вкладке: после смены схемы — перепривязано.
    FilterRuleSet rules = levelRule(QStringLiteral("Level"), QStringLiteral("ERROR"));
    c.bindRules(rules);
    second->model()->setFilterRules(rules);
    CHECK(settled(*second) && second->model()->rowCount() == errorCount(records),
          QStringLiteral("%1: rule on Level filters (%2)").arg(label).arg(second->model()->rowCount()));
    rebound = 0;
    c.selectSchema(indexOf(c.schemas(), QStringLiteral("B")));
    CHECK(settled(*second) && rebound == 1, label + ": only the tab with rules is rebound");
    CHECK(second->model()->rowCount() == records,
          label + ": rule on a column the new schema lacks is neutral");
    CHECK(first->parserPattern() == b && second->parserPattern() == b, label + ": schema B on both tabs");
    c.selectSchema(indexOf(c.schemas(), QStringLiteral("A")));
    CHECK(settled(*second) && second->model()->rowCount() == errorCount(records),
          label + ": rule bound again after switching back");

    // A → B: у B нет Level — выбраны все колонки B; B → A: те из них, что есть в A.
    CHECK(c.visibleFieldNames() == (QStringList{QStringLiteral("Timestamp"), QStringLiteral("Thread"),
                                                QStringLiteral("Message")}),
          label + ": column choice follows the names through B and back");
    c.setVisibleFieldNames({QStringLiteral("Level")});
    // Новая вкладка — схема, флаг и колонки окна.
    LogViewWidget* third = newTab(plain);
    CHECK(third->parserPattern() == a && third->model()->fieldDisplayFilterEnabled()
              && showsOnlyLevels(*third->model()),
          label + ": new tab takes the window schema");

    // Ключевая схема (F-4) — тем же путём.
    LogViewWidget* keyed = newTab(json);
    c.selectSchema(indexOf(c.schemas(), QStringLiteral("JSON Lines")));
    c.setVisibleFieldNames({QStringLiteral("Level")});
    CHECK(settled(*keyed), label + ": keyed tab settles");
    const QString jsonLines = c.activeSchema();
    CHECK(LogPattern(jsonLines).isKeyed() && keyed->parserPattern() == jsonLines,
          label + ": keyed schema reaches the tab");
    FilterRuleSet keyedRules = levelRule(QStringLiteral("Level"), QStringLiteral("ERROR"));
    c.bindRules(keyedRules);
    keyed->model()->setFilterRules(keyedRules);
    CHECK(settled(*keyed) && keyed->model()->rowCount() == errorCount(records),
          QStringLiteral("%1: rule on a keyed column (%2)").arg(label).arg(keyed->model()->rowCount()));
    CHECK(showsOnlyLevels(*keyed->model()), label + ": keyed column shown decoded");

    // «Filter blocks» выключен — поля сняты, правила по колонкам — по строке.
    c.setExtractionEnabled(false);
    CHECK(settled(*keyed), label + ": settles after Filter blocks off");
    for (const auto& t : tabs)
        CHECK(!t->model()->fieldDisplayFilterEnabled(), label + ": column display off everywhere");
    CHECK(!c.isReextracting(), label + ": small tabs re-extract synchronously");

    // Редактор схем без «Use Selected»: активная уже применена — заново не
    // применяется (вкладки не переизвлекаются, поиск не сбрасывается).
    c.selectSchema(indexOf(c.schemas(), QStringLiteral("A")));
    applied = 0;
    starts = 0;
    c.replaceCatalogue(c.schemas(), QString(), -1);
    CHECK(applied == 0 && starts == 0 && c.activeSchema() == a, label + ": editor without a choice keeps the schema");
}

static void checkBackgroundReextraction(const QDir& dir)
{
    ConfigDir config;
    AppSettings::instance().setIndexedThresholdMB(512);
    const QString a = schemaNamed(QStringLiteral("Level"), QStringLiteral("Message"));
    const QString b = schemaNamed(QStringLiteral("Level"), QStringLiteral("Text"));
    SchemaStore::sync({ {QStringLiteral("A"), a, {}, {}}, {QStringLiteral("B"), b, {}, {}} });

    // Больше 100 000 записей — резидентный фильтр уходит в пул; больше
    // 4000 — переизвлечение тоже.
    const int records = 120000;
    const QString path = writeFile(dir, QStringLiteral("big.log"), logBytes(records));
    auto tab = std::make_unique<LogViewWidget>();
    FieldSchemaController c;
    int starts = 0;
    bool filteringAtApply = false;
    c.setWindow({
        [&]() { return QList<LogViewWidget*>{tab.get()}; },
        [&]() { ++starts; } });
    int startedEntries = 0, finished = 0;
    QObject::connect(&c, &FieldSchemaController::reextractionStarted, [&](int n) { startedEntries = n; });
    QObject::connect(&c, &FieldSchemaController::reextractionFinished, [&] {
        ++finished;
        // Поля уже стоят, перефильтрации ещё не было: фильтр, запущенный за
        // время расчёта, остановлен до установки полей (инвариант 4).
        filteringAtApply = filteringAtApply || tab->model()->isFiltering();
    });
    {
        QSettings s(config.ini(), QSettings::IniFormat);
        s.beginGroup(QStringLiteral("View"));
        c.load(s);
        s.endGroup();
    }
    c.configureView(tab.get());
    tab->addLogFile(path);
    CHECK(settled(*tab) && tab->model()->rowCount() == records, "big tab loads");
    CHECK(!tab->model()->isIndexedBackend(), "big tab is resident");

    c.setExtractionEnabled(true);
    c.selectSchema(indexOf(c.schemas(), QStringLiteral("A")));
    CHECK(c.isReextracting() && startedEntries == records, "large re-extraction runs in the background");
    CHECK(!tab->model()->fieldDisplayFilterEnabled(), "column display off while re-extracting");
    CHECK(waitFor([&] { return !c.isReextracting(); }, 120000) && finished == 1,
          "background re-extraction finishes");
    CHECK(starts >= 1, "readers outside the tabs are stopped first");

    finished = 0;
    c.selectSchema(indexOf(c.schemas(), QStringLiteral("B")));
    CHECK(c.isReextracting(), "second schema re-extracts in the background");
    // Выбор колонок посреди расчёта: колонки новой схемы не ложатся на поля
    // старой — показ остаётся выключенным до конца расчёта.
    c.setVisibleFieldNames({QStringLiteral("Level")});
    CHECK(!tab->model()->fieldDisplayFilterEnabled(), "column choice keeps display off while re-extracting");
    // Фильтр по колонке, запущенный посреди расчёта: читает старые поля,
    // и его надо остановить до установки новых.
    FilterRuleSet rules = levelRule(QStringLiteral("Level"), QStringLiteral("ERROR"));
    c.bindRules(rules);
    tab->model()->setFilterRules(rules);
    CHECK(tab->model()->isFiltering(), "filter started during the re-extraction runs in the pool");
    CHECK(waitFor([&] { return !c.isReextracting(); }, 120000) && finished == 1,
          "re-extraction with a filter finishes");
    CHECK(!filteringAtApply, "filters stopped before the new fields go in (invariant 4)");
    CHECK(settled(*tab) && tab->model()->rowCount() == errorCount(records),
          QStringLiteral("filter re-applied on the new fields (%1)").arg(tab->model()->rowCount()));
    CHECK(tab->model()->fieldDisplayFilterEnabled() && tab->model()->visibleFieldIndexes() == QVector<int>{2},
          "column display back on");
    bool levels = true;
    for (int row = 0; row < 50 && row < tab->model()->rowCount(); ++row)
        levels = levels && tab->model()->data(tab->model()->index(row), Qt::DisplayRole)
                               .toString().trimmed() == QLatin1String("ERROR");
    CHECK(levels, "rows show fields of the new schema");

    // Смена схемы посреди расчёта отменяет прежний: записи не тронуты.
    c.selectSchema(indexOf(c.schemas(), QStringLiteral("A")));
    CHECK(c.isReextracting(), "third switch re-extracts");
    c.selectSchema(indexOf(c.schemas(), QStringLiteral("B")));
    CHECK(waitFor([&] { return !c.isReextracting(); }, 120000), "superseded re-extraction settles");
    CHECK(settled(*tab) && tab->model()->rowCount() == errorCount(records) && tab->parserPattern() == b,
          "superseding schema wins");
    // Вытеснение синхронным путём («Filter blocks» выключен) — расчёт
    // объявлен законченным (окно прячет прогресс).
    c.selectSchema(indexOf(c.schemas(), QStringLiteral("A")));
    CHECK(c.isReextracting(), "fourth switch re-extracts");
    finished = 0;
    filteringAtApply = false;
    c.setExtractionEnabled(false);
    CHECK(!c.isReextracting() && finished == 1, "superseded re-extraction announces its end");
    CHECK(settled(*tab) && !tab->model()->fieldDisplayFilterEnabled(), "Filter blocks off");
    c.setExtractionEnabled(true);
    CHECK(c.isReextracting(), "fifth switch re-extracts");
    finished = 0;
    c.cancelReextraction();
    CHECK(!c.isReextracting() && finished == 1, "cancel stops the re-extraction and announces it");
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);

    checkMigrationAndBuiltins();
    checkColumnChoice();
    checkSessionSchema();

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "нет временного каталога\n");
        return 2;
    }
    const QDir dir(tmp.path());
    checkViews(dir, false);
    checkViews(dir, true);
    checkBackgroundReextraction(dir);

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("field_schema: OK\n");
    return 0;
}
