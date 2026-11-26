// ============================================================================
// field_reextraction — фоновое переизвлечение полей при смене схемы не пишет
// в записи до apply(): фоновые фильтры, запущенные в это время, читают старые
// поля без гонки, а отмена оставляет записи целиком в старой схеме. Схема —
// на запись (F-5, PR 2): слитая вкладка переизвлекает каждый файл своей
// схемой, поля — в колонках вкладки.
// ============================================================================

#include "fieldreextraction.h"
#include "testdocuments.h"

#include <QTemporaryDir>
#include <cstdio>

static int failures = 0;
#define CHECK(condition, label) do { \
    if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

static bool sameFields(const LogEntryFields& a, const LogEntryFields& b)
{
    if (a.size() != b.size())
        return false;
    for (int i = 0; i < a.size(); ++i) {
        if (a.spans[i].start != b.spans[i].start || a.spans[i].length != b.spans[i].length)
            return false;
    }
    return true;
}

// Одна схема на всё — как вкладка с одной схемой (колонки тождественны).
static FileSchemaSet onlySchema(const LogPattern& pattern)
{
    FieldLayout layout;
    FileSchemaSet set;
    set.fallback = layout.schemaFor(pattern);
    return set;
}

// Поля каждой записи совпадают с ожидаемыми.
static bool allFields(const QVector<std::shared_ptr<LogEntry>>& entries,
                      const std::function<LogEntryFields(const LogEntry&)>& expected)
{
    for (const auto& entry : entries) {
        if (!sameFields(entry->fields(), expected(*entry)))
            return false;
    }
    return true;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    if (!temporary.isValid())
        return 1;
    const QDir dir(temporary.path());

    // Схема A — Timestamp [Thread] Level - Message; схема B — Timestamp и остаток.
    const QString schemaA = testdocs::buildSchema();
    PatternDefinition definitionB;
    definitionB.blocks = {
        testdocs::patternBlock(PatternBlock::MatchKind::Timestamp, QStringLiteral("Timestamp")),
        testdocs::patternBlock(PatternBlock::MatchKind::Remainder, QStringLiteral("Rest")),
    };
    const auto patternA = std::make_shared<const LogPattern>(schemaA);
    const auto patternB = std::make_shared<const LogPattern>(LogPattern::serializeDefinition(definitionB));

    // Больше порога асинхронного фильтра (100k): фильтр вкладки идёт в пуле,
    // параллельно с переизвлечением.
    constexpr int kLines = 120000;
    QByteArray bytes;
    bytes.reserve(kLines * 56);
    for (int i = 0; i < kLines; ++i) {
        bytes += "2026-03-05 10:00:00.000 [worker-" + QByteArray::number(1 + i % 2)
               + "] INFO - message " + QByteArray::number(i) + '\n';
    }
    const QString path = testdocs::writeFile(dir, "fields.log", bytes);
    auto doc = testdocs::buildResident({path}, schemaA);
    CHECK(doc.errors.isEmpty(), "load resident fixture");
    const QVector<std::shared_ptr<LogEntry>> entries = doc.model->residentEntriesForFieldMutation();
    CHECK(entries.size() == kLines, "all lines loaded");
    const auto byA = [&](const LogEntry& e) { return patternA->extractFields(e.message()); };
    const auto byB = [&](const LogEntry& e) { return patternB->extractFields(e.message()); };
    CHECK(allFields(entries, byA), "entries start with schema A fields");

    // Фильтр по полю Thread схемы A — читает поля записей.
    FilterRuleSet threadRule;
    FilterRule rule;
    rule.text = QStringLiteral("worker-1");
    rule.fieldName = QStringLiteral("Thread");
    threadRule.rules.append(rule);
    threadRule.bindFields(patternA->fieldNames(), true);

    // 1. Пока задание идёт, записи не меняются: фоновый фильтр, запущенный в
    //    это время, видит старые поля и считает правильно.
    {
        FieldReextraction extraction(entries, onlySchema(*patternB));
        bool finished = false;
        QObject::connect(&extraction, &FieldReextraction::finished, [&] { finished = true; });
        CHECK(!extraction.isReady(), "not ready before start");
        extraction.apply(); // до старта — ничего
        CHECK(allFields(entries, byA), "apply before start changes nothing");

        extraction.start();
        doc.model->setFilterRules(threadRule);
        CHECK(testdocs::waitFor([&] { return finished; }), "extraction finishes");
        CHECK(testdocs::settle(*doc.model), "filter settles during extraction");
        CHECK(doc.model->rowCount() == kLines / 2, "filter over old fields is correct");
        CHECK(allFields(entries, byA), "workers never write to the entries");
        CHECK(extraction.isReady(), "ready after finished");

        // 2. apply() ставит новые поля — владелец перед этим останавливает
        //    фоновые фильтры.
        doc.model->cancelPendingFilter(true);
        extraction.apply();
        CHECK(allFields(entries, byB), "apply installs the new fields");
        CHECK(!extraction.isReady(), "apply is one-shot");
        extraction.apply();
        CHECK(allFields(entries, byB), "second apply changes nothing");
    }
    testdocs::pump(50); // старые спаны освобождаются в пуле

    // 3. Отмена оставляет записи целиком в прежней схеме — не наполовину.
    {
        FieldReextraction extraction(entries, onlySchema(*patternA));
        bool finished = false;
        QObject::connect(&extraction, &FieldReextraction::finished, [&] { finished = true; });
        extraction.start();
        extraction.cancel();
        testdocs::pump(100);
        CHECK(!finished, "cancelled extraction does not report finished");
        CHECK(!extraction.isReady(), "cancelled extraction is not ready");
        extraction.apply();
        CHECK(allFields(entries, byB), "cancelled extraction leaves entries intact");
    }

    // 4. Уничтожение идущего задания тоже безопасно и записи не трогает.
    {
        auto extraction = std::make_unique<FieldReextraction>(entries, onlySchema(*patternA));
        extraction->start();
        extraction.reset();
        CHECK(allFields(entries, byB), "destroyed extraction leaves entries intact");
    }

    // 5. Фильтр после apply и перепривязки правил видит новые поля: в схеме B
    //    поля Thread нет, правило по нему непригодно, а по Rest — работает.
    {
        FieldReextraction extraction(entries, onlySchema(*patternA));
        bool finished = false;
        QObject::connect(&extraction, &FieldReextraction::finished, [&] { finished = true; });
        extraction.start();
        CHECK(testdocs::waitFor([&] { return finished; }), "second extraction finishes");
        doc.model->cancelPendingFilter(true);
        extraction.apply();
        CHECK(allFields(entries, byA), "schema A restored");
        doc.model->setFilterRules(threadRule);
        CHECK(testdocs::settle(*doc.model), "filter settles after apply");
        CHECK(doc.model->rowCount() == kLines / 2, "filter over re-extracted fields is correct");
    }

    // 6. Схема на запись (F-5): слитая вкладка из трёх файлов — схема A, схема
    //    B (Timestamp, Rest) и без схемы. Каждая запись переизвлекается схемой
    //    своего файла в колонки вкладки (Rest — пятая, после колонок A), у
    //    файла без схемы поля снимаются; до apply() записи не тронуты.
    {
        constexpr int kOther = 6000; // всего больше порога асинхронного фильтра
        QByteArray other;
        QByteArray plain;
        for (int i = 0; i < kOther; ++i) {
            other += "2026-03-05 10:00:01.000 rest " + QByteArray::number(i) + " [worker-1]\n";
            plain += "2026-03-05 10:00:02.000 [worker-1] INFO - plain " + QByteArray::number(i) + '\n';
        }
        const QString otherPath = testdocs::writeFile(dir, "other.log", other);
        const QString plainPath = testdocs::writeFile(dir, "plain.log", plain);
        testdocs::Document merged;
        merged.model = std::make_unique<LogModel>();
        testdocs::addResidentFiles(merged, {path}, schemaA);
        testdocs::addResidentFiles(merged, {otherPath}, LogPattern::serializeDefinition(definitionB));
        testdocs::addResidentFiles(merged, {plainPath}, QString());
        CHECK(merged.errors.isEmpty(), "load merged fixture");
        CHECK(merged.layout.columns
                  == QStringList({"Timestamp", "Thread", "Level", "Message", "Rest"}),
              "tab columns: union by name in schema order");
        const FileSchemaSet schemas = merged.model->fileSchemas();
        const FileSchemaPtr schemaOfA = schemas.forPath(path);
        const FileSchemaPtr schemaOfB = schemas.forPath(otherPath);
        CHECK(schemaOfA && schemaOfA->identity && schemaOfB && !schemaOfB->identity
                  && !schemas.forPath(plainPath),
              "file schemas: A identity, B mapped, plain none");
        const auto byFile = [&](const LogEntry& e) {
            const FileSchema* schema = schemas.forFile(e.sourceFile().get());
            return schema ? schema->extractFields(e.message()) : LogEntryFields();
        };
        const QVector<std::shared_ptr<LogEntry>> all = merged.model->residentEntriesForFieldMutation();
        CHECK(all.size() == kLines + 2 * kOther, "merged lines loaded");
        CHECK(allFields(all, byFile), "load put every file's fields in tab columns");
        // B: Timestamp — колонка 0, Rest — колонка 4, остальных нет.
        bool mappedB = true;
        for (const auto& e : all) {
            if (e->sourceFile()->filePath != otherPath)
                continue;
            mappedB = mappedB && e->fields().has(0) && e->fields().has(4) && !e->fields().has(1)
                && e->fields().get(4, e->message()).startsWith(QStringLiteral("rest "));
        }
        CHECK(mappedB, "schema B fields land in Timestamp and Rest columns");

        // Сбить поля всем записям схемой A, затем вернуть пофайловые.
        for (const auto& e : all)
            e->setFields(schemaOfA->extractFields(e->message()));
        FieldReextraction extraction(all, schemas);
        bool finished = false;
        QObject::connect(&extraction, &FieldReextraction::finished, [&] { finished = true; });
        extraction.start();
        CHECK(testdocs::waitFor([&] { return finished; }), "per-record extraction finishes");
        const auto byA = [&](const LogEntry& e) { return schemaOfA->extractFields(e.message()); };
        CHECK(allFields(all, byA), "per-record workers never write to the entries");
        merged.model->cancelPendingFilter(true);
        extraction.apply();
        CHECK(allFields(all, byFile), "apply installs each file's own schema fields");

        // Правило по колонке Rest (только у B) — «не содержит» на строках A и
        // файла без схемы; по Thread (только у A) — «не содержит» на B.
        FilterRuleSet restRule;
        FilterRule r;
        r.text = QStringLiteral("rest");
        r.fieldName = QStringLiteral("Rest");
        restRule.rules.append(r);
        restRule.bindFields(merged.layout.columns, true);
        merged.model->setFilterRules(restRule);
        CHECK(testdocs::settle(*merged.model), "Rest filter settles");
        CHECK(merged.model->rowCount() == kOther, "Rest rule keeps only schema B lines");
        FilterRuleSet threadOnly;
        r.text = QStringLiteral("worker-1");
        r.fieldName = QStringLiteral("Thread");
        threadOnly.rules = {r};
        threadOnly.bindFields(merged.layout.columns, true);
        merged.model->setFilterRules(threadOnly);
        CHECK(testdocs::settle(*merged.model), "Thread filter settles");
        CHECK(merged.model->rowCount() == kLines / 2,
              "Thread rule: schema A worker-1 only (B has worker-1 in Rest, plain has no fields)");
    }

    std::fprintf(stdout, "Field re-extraction: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
