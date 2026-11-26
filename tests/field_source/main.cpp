// ============================================================================
// tests/field_source — ленивые поля (F-17, шаг 3; docs/design/F-17.md,
// раздел 3 и таблица PR, строка 3).
//
// Поля строки — функция (текст, схема): в записях их нет, оба бэкенда
// получают их одним путём (FieldSource / FieldScanner). Проверяется:
//   • поля равны на обоих бэкендах и равны прямому FileSchema::extract;
//   • смена схемы — ни reset, ни прохода (счётчики);
//   • фильтр по полю — число вызовов extract (предфильтр, один разбор на
//     запись), на обоих бэкендах;
//   • правило по полю на многострочных записях: Include оставляет
//     стек-трейс своей записи, Exclude убирает его вместе с записью;
//     дозапись посреди записи; оба бэкенда;
//   • предфильтр не теряет совпадение в значении с экранированием (JSON
//     \uXXXX, quoted шаблона);
//   • колонки нет в схеме вкладки — правило нейтрально; есть — ищет в поле
//     при любом показе;
//   • кэш fieldsFor переживает вставки и фильтр, сбрасывается сменой схемы и
//     сбросом файла; повторная отрисовка — ни одного extract.
//   • схема-шаблон (версия 4) — FieldLayout строит её извлекатель.
// ============================================================================

#include "fieldlayout.h"
#include "fieldsource.h"
#include "filterruleset.h"
#include "indexedlogstore.h"
#include "linetemplate.h"
#include "logmodel.h"
#include "logpattern.h"
#include "testdocuments.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>

#include <cstdio>
#include <functional>

using namespace testdocs;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const QString& what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", qPrintable(what));
    }
}

#define CHECK(cond, what) check((cond), (what))

enum class Backend { Resident, Indexed };

QString name(Backend b) { return b == Backend::Resident ? QStringLiteral("resident") : QStringLiteral("indexed"); }

Document build(Backend b, const QStringList& paths, const QString& schema)
{
    return b == Backend::Resident ? buildResident(paths, schema) : buildIndexed(paths, schema);
}

bool appendTail(Backend b, Document& doc, const QByteArray& bytes, const QString& schema)
{
    const bool ok = b == Backend::Resident ? appendResidentTail(doc, 0, bytes, schema)
                                           : appendIndexedTail(doc, 0, bytes, schema);
    return ok && settle(*doc.model, 0);
}

QString describeFields(const LogEntryFields& f)
{
    QStringList parts;
    for (const FieldSpan& s : f.spans)
        parts << QStringLiteral("%1+%2%3").arg(s.start).arg(s.size()).arg(s.isEscaped() ? "e" : "");
    return parts.join(u',');
}

bool sameFields(const LogEntryFields& a, const LogEntryFields& b)
{
    if (a.isEmpty() && b.isEmpty())
        return true;
    const int n = int(qMax(a.spans.size(), b.spans.size()));
    for (int i = 0; i < n; ++i) {
        const FieldSpan x = i < a.spans.size() ? a.spans[i] : FieldSpan();
        const FieldSpan y = i < b.spans.size() ? b.spans[i] : FieldSpan();
        if (x.isValid() != y.isValid())
            return false;
        if (x.isValid() && (x.start != y.start || x.length != y.length))
            return false;
    }
    return true;
}

FilterRuleSet fieldRule(const QString& field, const QString& text,
                        FilterRule::Action action = FilterRule::Action::Include,
                        bool regex = false)
{
    FilterRuleSet set;
    FilterRule r;
    r.fieldName = field;
    r.text = text;
    r.action = action;
    r.isRegex = regex;
    set.rules = { r };
    return set;
}

// Строки модели: ключ «файл:строка» → текст.
QMap<QString, QString> rowsOf(const LogModel& m)
{
    QMap<QString, QString> out;
    for (int r = 0; r < m.rowCount(); ++r) {
        const auto e = m.entryAt(r);
        if (e)
            out.insert(QStringLiteral("%1:%2").arg(e->sourceFile()->filePath).arg(e->originalLineNumber(), 6, 10, QLatin1Char('0')),
                       e->message());
    }
    return out;
}

// ---------------------------------------------------------------------------
// Многострочный лог с заданным ответом: у каждой записи поток; стек-трейс —
// без метки (продолжения), иногда с текстом, похожим на поле другого потока.
// ---------------------------------------------------------------------------
struct RecordLog {
    QByteArray bytes;
    // номер строки (с 1) → поток её записи
    QVector<QString> threadOfLine;
};

RecordLog recordLog(int records, quint32 seed)
{
    QRandomGenerator rng(seed);
    RecordLog log;
    log.threadOfLine.append(QString()); // строки с 1
    const char* threads[] = { "main", "worker-1", "io-pool" };
    QDateTime ts = fixtureBaseTime();
    for (int i = 0; i < records; ++i) {
        ts = ts.addMSecs(500 + rng.bounded(500));
        const QString thread = QString::fromLatin1(threads[rng.bounded(3)]);
        const QString line = QStringLiteral("%1 [%2] %3 - request %4\n")
                                 .arg(ts.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")), thread,
                                      i % 5 == 0 ? QStringLiteral("ERROR") : QStringLiteral("INFO"))
                                 .arg(i);
        log.bytes += line.toUtf8();
        log.threadOfLine.append(thread);
        const int frames = int(rng.bounded(4));
        for (int f = 0; f < frames; ++f) {
            // Строка трейса упоминает чужой поток: правило «по всей строке»
            // её бы нашло, правило по полю — нет.
            log.bytes += QStringLiteral("    at Module%1.call [main] line %2\n").arg(f).arg(i).toUtf8();
            log.threadOfLine.append(thread);
        }
    }
    return log;
}

// ---------------------------------------------------------------------------

void testFieldsEqualDirectExtract(const QDir& dir)
{
    const QString path = writeFile(dir, QStringLiteral("equal.log"),
                                   makeLogBytes(600, 17, "\n", fixtureBaseTime(), true, true, true));
    const QString schema = buildSchema();
    Document resident = build(Backend::Resident, {path}, schema);
    Document indexed = build(Backend::Indexed, {path}, schema);
    CHECK(resident.errors.isEmpty() && indexed.errors.isEmpty(), "fixtures load");
    QMap<QString, LogEntryFields> byKey[2];
    for (int k = 0; k < 2; ++k) {
        Document& doc = k == 0 ? resident : indexed;
        const FileSchemaPtr fileSchema = doc.model->fileSchemaFor(path);
        CHECK(fileSchema && fileSchema->isValid(), "schema of the file");
        int mismatches = 0, withFields = 0;
        for (int r = 0; r < doc.model->rowCount(); ++r) {
            const auto e = doc.model->entryAt(r);
            const LogEntryFields got = doc.model->fieldsOf(*e);
            const LogEntryFields direct = fileSchema->extractFields(e->message());
            if (!sameFields(got, direct) && ++mismatches <= 3)
                std::fprintf(stderr, "  %s line %d: %s vs direct %s\n", k ? "indexed" : "resident",
                             e->originalLineNumber(), qPrintable(describeFields(got)),
                             qPrintable(describeFields(direct)));
            withFields += got.isEmpty() ? 0 : 1;
            byKey[k].insert(QString::number(e->originalLineNumber()), got);
        }
        CHECK(mismatches == 0, QStringLiteral("%1: fields = direct extract").arg(k ? "indexed" : "resident"));
        CHECK(withFields >= 600, QStringLiteral("%1: record lines have fields (%2)")
                                     .arg(k ? "indexed" : "resident").arg(withFields));
    }
    CHECK(byKey[0].size() == byKey[1].size(), "both backends have the same lines");
    int diff = 0;
    for (auto it = byKey[0].cbegin(); it != byKey[0].cend(); ++it)
        diff += sameFields(it.value(), byKey[1].value(it.key())) ? 0 : 1;
    CHECK(diff == 0, QStringLiteral("fields equal on both backends (%1 differ)").arg(diff));

    // Колонки показа — те же на обоих бэкендах.
    for (Document* doc : { &resident, &indexed })
        doc->model->setFieldDisplaySelection(true, {2, 3});
    const QMap<QString, QString> shown[2] = {
        [&] { QMap<QString, QString> m; for (int r = 0; r < resident.model->rowCount(); ++r)
                  m.insert(QString::number(resident.model->entryAt(r)->originalLineNumber()),
                           resident.model->data(resident.model->index(r), Qt::DisplayRole).toString());
              return m; }(),
        [&] { QMap<QString, QString> m; for (int r = 0; r < indexed.model->rowCount(); ++r)
                  m.insert(QString::number(indexed.model->entryAt(r)->originalLineNumber()),
                           indexed.model->data(indexed.model->index(r), Qt::DisplayRole).toString());
              return m; }(),
    };
    CHECK(shown[0] == shown[1], "display text with selected fields equal on both backends");
}

// Смена схемы — O(1): ни reset, ни прохода; кэш сбрасывается; повторная
// отрисовка — из кэша.
void testSchemaSwitchAndCache(Backend b, const QDir& dir)
{
    const QString label = name(b);
    const QString path = writeFile(dir, QStringLiteral("switch-%1.log").arg(label),
                                   makeLogBytes(3000, 5, "\n", fixtureBaseTime(), false, true, true));
    Document doc = build(b, {path}, QString());
    CHECK(doc.errors.isEmpty(), label + ": loads");
    LogModel& m = *doc.model;
    FieldSource& source = m.fieldSource();
    int resets = 0, removed = 0, inserted = 0, changed = 0;
    QObject::connect(&m, &QAbstractItemModel::modelReset, [&] { ++resets; });
    QObject::connect(&m, &QAbstractItemModel::rowsRemoved, [&] { ++removed; });
    QObject::connect(&m, &QAbstractItemModel::rowsInserted, [&] { ++inserted; });
    QObject::connect(&m, &QAbstractItemModel::dataChanged, [&] { ++changed; });
    m.setFieldDisplaySelection(true, {2});
    changed = 0;
    source.resetStats();
    const quint64 gen = source.generation();

    // Схема на модели — как LogViewWidget::applySchemaToModel.
    FieldLayout layout;
    FileSchemaSet set;
    set.fallback = layout.schemaFor(buildSchema());
    m.setAvailableFields(layout.columns);
    changed = 0;
    m.setFileSchemas(set);
    CHECK(resets == 0 && removed == 0 && inserted == 0,
          QStringLiteral("%1: schema switch — no reset/remove/insert (%2/%3/%4)")
              .arg(label).arg(resets).arg(removed).arg(inserted));
    CHECK(source.stats().guiExtracts == 0 && source.stats().scanExtracts == 0,
          label + ": schema switch — no pass over the lines");
    CHECK(source.generation() == gen + 1, label + ": generation grows");
    CHECK(changed == 1, QStringLiteral("%1: one repaint of the visible rows (%2)").arg(label).arg(changed));
    CHECK(!m.isFiltering(), label + ": no filter without field rules");

    // Отрисовка «экрана» (60 строк) — не больше extract, чем строк; повтор — 0.
    const int screen = 60;
    for (int r = 0; r < screen; ++r)
        m.data(m.index(r), Qt::DisplayRole);
    const qint64 first = source.stats().guiExtracts;
    CHECK(first > 0 && first <= screen, QStringLiteral("%1: screen paint — %2 extract calls for %3 rows")
                                            .arg(label).arg(first).arg(screen));
    for (int r = 0; r < screen; ++r)
        m.data(m.index(r), Qt::DisplayRole);
    CHECK(source.stats().guiExtracts == first, label + ": repaint — no extract calls");
    QString levelOfRow10 = m.data(m.index(10), Qt::DisplayRole).toString();

    // Кэш переживает фильтр (строки другие, ключ — файл и номер строки)…
    m.setLogLevelFilter({LogLevel::Error, LogLevel::Fatal, LogLevel::Warn, LogLevel::Info});
    CHECK(settle(m, 0), label + ": level filter settles");
    const qint64 hitsBefore = source.stats().cacheHits;
    for (int r = 0; r < m.rowCount() && r < screen; ++r)
        m.data(m.index(r), Qt::DisplayRole);
    CHECK(source.stats().cacheHits > hitsBefore, label + ": cache hits after a filter");
    m.setLogLevelFilter({});
    CHECK(settle(m, 0), label + ": filter off");
    // …и дозапись.
    const qint64 extractsBefore = source.stats().guiExtracts;
    // Схема дозаписи пустая: схемы модели здесь ставит тест (fallback), а
    // testdocs::fileSchema на непустую поставил бы файлу свою — смена схемы.
    CHECK(appendTail(b, doc, makeLogBytes(20, 99, "\n", fixtureBaseTime().addDays(1), false, true, true),
                     QString()),
          label + ": append");
    for (int r = 0; r < screen; ++r)
        m.data(m.index(r), Qt::DisplayRole);
    CHECK(source.stats().guiExtracts == extractsBefore, label + ": cache survives an append");
    CHECK(m.data(m.index(10), Qt::DisplayRole).toString() == levelOfRow10, label + ": same field shown");

    // Сброс строк файла — кэш пуст.
    m.invalidateFileRows(path);
    CHECK(source.cachedLines() == 0, label + ": file reset clears the cache");
    // Смена схемы — кэш пуст, поля новой схемы.
    for (int r = 0; r < screen; ++r)
        m.data(m.index(r), Qt::DisplayRole);
    CHECK(source.cachedLines() > 0, label + ": cache filled again");
    PatternDefinition def;
    def.blocks = { patternBlock(PatternBlock::MatchKind::Timestamp, QStringLiteral("Timestamp")),
                   patternBlock(PatternBlock::MatchKind::Remainder, QStringLiteral("Rest")) };
    FileSchemaSet other;
    other.fallback = layout.schemaFor(LogPattern::serializeDefinition(def));
    m.setAvailableFields(layout.columns);
    m.setFileSchemas(other);
    CHECK(source.cachedLines() == 0, label + ": schema switch clears the cache");
    const int restColumn = int(layout.columns.indexOf(QStringLiteral("Rest")));
    m.setFieldDisplaySelection(true, {restColumn});
    const auto e10 = m.entryAt(10);
    const QString shown = m.data(m.index(10), Qt::DisplayRole).toString();
    CHECK(e10 && shown == e10->message().mid(24),
          QStringLiteral("%1: fields of the new schema shown ('%2')").arg(label, shown));
}

// Правило по полю: решение по первой строке записи действует на всю запись;
// вызовов extract — не больше строк, где есть образец; дозапись посреди записи.
void testRecordRule(Backend b, const QDir& dir)
{
    const QString label = name(b);
    const RecordLog log = recordLog(400, 7);
    // Последняя запись дописывается частями: первая строка в файле,
    // продолжения — дозаписью (запись посреди файла уже не первая в проходе).
    const QString path = writeFile(dir, QStringLiteral("records-%1.log").arg(label), log.bytes);
    const QString schema = buildSchema();
    Document doc = build(b, {path}, schema);
    CHECK(doc.errors.isEmpty(), label + ": loads");
    LogModel& m = *doc.model;
    const int threadColumn = int(doc.layout.columns.indexOf(QStringLiteral("Thread")));
    CHECK(threadColumn == 1, label + ": Thread column");

    const auto expectRows = [&](const QVector<QString>& threadOfLine, bool include,
                                const QString& what) {
        QSet<int> want, got;
        for (int line = 1; line < threadOfLine.size(); ++line)
            if ((threadOfLine[line] == QLatin1String("main")) == include)
                want.insert(line);
        for (int r = 0; r < m.rowCount(); ++r)
            got.insert(m.entryAt(r)->originalLineNumber());
        CHECK(got == want, QStringLiteral("%1: %2 — %3 rows, expected %4")
                               .arg(label, what).arg(got.size()).arg(want.size()));
    };

    int mainLines = 0;
    for (int line = 1; line < log.threadOfLine.size(); ++line)
        mainLines += log.threadOfLine[line] == QLatin1String("main");

    FilterRuleSet include = fieldRule(QStringLiteral("Thread"), QStringLiteral("main"));
    include.bindFields(doc.layout.columns);
    m.fieldSource().resetStats();
    m.setFilterRules(include);
    CHECK(settle(m, 0), label + ": include settles");
    expectRows(log.threadOfLine, true, QStringLiteral("Include keeps the stack trace of its record"));
    // Один разбор на запись и только у записей, чья первая строка содержит
    // образец (строки трейса с «[main]» — продолжения, их не разбирают).
    int headsWithText = 0;
    {
        const QStringList lines = QString::fromUtf8(log.bytes).split(u'\n');
        for (int line = 1; line < log.threadOfLine.size(); ++line) {
            const QString& t = lines[line - 1];
            if (!t.startsWith(u' ') && t.contains(QLatin1String("main")))
                ++headsWithText;
        }
    }
    const qint64 extracts = m.fieldSource().stats().scanExtracts;
    CHECK(extracts <= headsWithText,
          QStringLiteral("%1: extract calls %2 <= record heads containing the text %3")
              .arg(label).arg(extracts).arg(headsWithText));
    CHECK(extracts > 0, label + ": the field is parsed");

    FilterRuleSet exclude = fieldRule(QStringLiteral("Thread"), QStringLiteral("main"),
                                      FilterRule::Action::Exclude);
    exclude.bindFields(doc.layout.columns);
    m.setFilterRules(exclude);
    CHECK(settle(m, 0), label + ": exclude settles");
    expectRows(log.threadOfLine, false, QStringLiteral("Exclude removes the stack trace with its record"));

    // Регекс по полю — тот же ответ.
    FilterRuleSet regex = fieldRule(QStringLiteral("Thread"), QStringLiteral("^ma.n$"),
                                    FilterRule::Action::Include, true);
    regex.bindFields(doc.layout.columns);
    m.setFilterRules(regex);
    CHECK(settle(m, 0), label + ": regex settles");
    expectRows(log.threadOfLine, true, QStringLiteral("regex rule on a field, by record"));

    // Правило по всей строке остаётся построчным: «[main]» в трейсе чужих
    // записей находится.
    FilterRuleSet line = fieldRule(QString(), QStringLiteral("[main]"));
    line.bindFields(doc.layout.columns);
    m.setFilterRules(line);
    CHECK(settle(m, 0), label + ": line rule settles");
    int traceRows = 0;
    for (int r = 0; r < m.rowCount(); ++r)
        traceRows += m.entryAt(r)->message().startsWith(u' ') ? 1 : 0;
    CHECK(traceRows > 0 && m.rowCount() > traceRows, label + ": whole-line rule stays per line");

    // AND: поле записи и текст строки — «трейсы записей main».
    FilterRuleSet both = fieldRule(QStringLiteral("Thread"), QStringLiteral("main"));
    FilterRule tail;
    tail.text = QStringLiteral("    at ");
    both.rules << tail;
    both.bindFields(doc.layout.columns);
    m.setFilterRules(both);
    CHECK(settle(m, 0), label + ": AND settles");
    bool allTrace = true;
    for (int r = 0; r < m.rowCount(); ++r) {
        const auto e = m.entryAt(r);
        allTrace = allTrace && e->message().startsWith(u' ')
                   && log.threadOfLine.value(e->originalLineNumber()) == QLatin1String("main");
    }
    CHECK(allTrace && m.rowCount() > 0, label + ": field rule AND line rule");

    // Дозапись посреди записи: первая строка записи — в файле, продолжения
    // приходят дозаписью (её первая строка в проход не попадает).
    m.setFilterRules(include);
    CHECK(settle(m, 0), label + ": include again");
    QVector<QString> threads = log.threadOfLine;
    QDateTime ts = fixtureBaseTime().addDays(1);
    for (const QString& thread : { QStringLiteral("main"), QStringLiteral("io-pool") }) {
        const QByteArray head = QStringLiteral("%1 [%2] INFO - tail record\n")
                                    .arg(ts.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")), thread)
                                    .toUtf8();
        ts = ts.addSecs(1);
        CHECK(appendTail(b, doc, head, schema), label + ": append a record head");
        threads.append(thread);
        // Продолжения — отдельной порцией.
        QByteArray trace;
        for (int f = 0; f < 3; ++f) {
            trace += QStringLiteral("    at Tail%1.call [main]\n").arg(f).toUtf8();
            threads.append(thread);
        }
        CHECK(appendTail(b, doc, trace, schema), label + ": append continuation lines");
    }
    expectRows(threads, true, QStringLiteral("continuation appended mid-record follows its record (Include)"));
    m.setFilterRules(exclude);
    CHECK(settle(m, 0), label + ": exclude again");
    expectRows(threads, false, QStringLiteral("continuation appended mid-record follows its record (Exclude)"));
    // Ещё одна порция продолжений к последней записи (io-pool) при Exclude.
    QByteArray more;
    for (int f = 0; f < 2; ++f) {
        more += QStringLiteral("    at More%1.call [main]\n").arg(f).toUtf8();
        threads.append(QStringLiteral("io-pool"));
    }
    CHECK(appendTail(b, doc, more, schema), label + ": append more continuation lines");
    expectRows(threads, false, QStringLiteral("second appended portion under Exclude"));
    Q_UNUSED(mainLines);
}

// Колонки нет в схеме вкладки — правило нейтрально; есть — ищет в поле при
// любом показе (галочка показа полей правил не меняет).
void testMissingColumnAndDisplay(Backend b, const QDir& dir)
{
    const QString label = name(b);
    const RecordLog log = recordLog(120, 3);
    const QString path = writeFile(dir, QStringLiteral("missing-%1.log").arg(label), log.bytes);
    Document doc = build(b, {path}, buildSchema());
    LogModel& m = *doc.model;
    const int all = m.rowCount();

    FilterRuleSet missing = fieldRule(QStringLiteral("NoSuchColumn"), QStringLiteral("main"));
    missing.bindFields(doc.layout.columns);
    CHECK(missing.fieldMissing(0) && missing.usableRuleCount() == 0, label + ": missing column — neutral rule");
    m.setFilterRules(missing);
    CHECK(settle(m, 0) && m.rowCount() == all, label + ": neutral rule hides nothing");

    for (const bool shown : { false, true }) {
        m.setFieldDisplaySelection(shown, shown ? QVector<int>{2} : QVector<int>{});
        FilterRuleSet thread = fieldRule(QStringLiteral("Thread"), QStringLiteral("main"));
        thread.bindFields(doc.layout.columns);
        m.setFilterRules(thread);
        CHECK(settle(m, 0), label + ": field rule settles");
        bool ok = m.rowCount() > 0;
        for (int r = 0; r < m.rowCount(); ++r)
            ok = ok && log.threadOfLine.value(m.entryAt(r)->originalLineNumber()) == QLatin1String("main");
        CHECK(ok, QStringLiteral("%1: field rule searches the field with display %2")
                      .arg(label, shown ? "on" : "off"));
    }

    // Нет схемы у файла — правило по полю «не содержит» (Include скрывает всё).
    Document plain = build(b, {path}, QString());
    FilterRuleSet thread = fieldRule(QStringLiteral("Thread"), QStringLiteral("main"));
    thread.bindFields(doc.layout.columns);
    plain.model->setFilterRules(thread);
    CHECK(settle(*plain.model, 0) && plain.model->rowCount() == 0,
          label + ": file without a schema — field rule finds nothing");
}

// Предфильтр по строке не теряет совпадение в значении с экранированием.
void testPrefilterEscapes(Backend b, const QDir& dir)
{
    const QString label = name(b);
    // JSON Lines: значение с \u — кириллица, которой нет в сырой строке.
    {
        QByteArray bytes;
        bytes += R"({"ts":"2026-03-05T10:00:00Z","level":"info","msg":"open файл"})" "\n";
        bytes += R"({"ts":"2026-03-05T10:00:01Z","level":"info","msg":"plain text"})" "\n";
        bytes += R"({"ts":"2026-03-05T10:00:02Z","level":"error","msg":"say \"hi\" there"})" "\n";
        const QString path = writeFile(dir, QStringLiteral("escaped-%1.jsonl").arg(label), bytes);
        PatternDefinition def;
        KeyedDefinition keyed;
        keyed.format = KeyedDefinition::Format::JsonObject;
        def.keyed = keyed;
        const QString schema = LogPattern::serializeDefinition(def);
        Document doc = build(b, {path}, schema);
        CHECK(doc.errors.isEmpty() && doc.model->rowCount() == 3, label + ": JSON loads");
        FilterRuleSet cyr = fieldRule(QStringLiteral("Message"), QStringLiteral("файл"));
        cyr.bindFields(doc.layout.columns);
        doc.model->setFilterRules(cyr);
        CHECK(settle(*doc.model, 0) && doc.model->rowCount() == 1,
              QStringLiteral("%1: \\u value found by the field rule (%2 rows)").arg(label).arg(doc.model->rowCount()));
        FilterRuleSet quote = fieldRule(QStringLiteral("Message"), QStringLiteral("say \"hi\""));
        quote.bindFields(doc.layout.columns);
        doc.model->setFilterRules(quote);
        CHECK(settle(*doc.model, 0) && doc.model->rowCount() == 1, label + ": \\\" value found");
        FilterRuleSet exclude = fieldRule(QStringLiteral("Message"), QStringLiteral("файл"),
                                          FilterRule::Action::Exclude);
        exclude.bindFields(doc.layout.columns);
        doc.model->setFilterRules(exclude);
        CHECK(settle(*doc.model, 0) && doc.model->rowCount() == 2, label + ": \\u value excluded");
    }
    // Шаблон с quoted: «\"» раскодируется.
    {
        QByteArray bytes;
        bytes += "2026-03-05 10:00:00.000 INFO \"Mozilla \\\"x\\\" agent\" first\n";
        bytes += "2026-03-05 10:00:01.000 INFO \"curl\" second\n";
        const QString path = writeFile(dir, QStringLiteral("quoted-%1.log").arg(label), bytes);
        const QString schema = LineTemplateSchema::definition(
            QStringLiteral("{Timestamp:time} {Level:level} {Agent:quoted} {Message}"));
        Document doc = build(b, {path}, schema);
        CHECK(doc.errors.isEmpty() && doc.model->rowCount() == 2, label + ": template log loads");
        CHECK(doc.layout.columns == (QStringList{QStringLiteral("Timestamp"), QStringLiteral("Level"),
                                                 QStringLiteral("Agent"), QStringLiteral("Message")}),
              label + ": template columns: " + doc.layout.columns.join(u','));
        FilterRuleSet agent = fieldRule(QStringLiteral("Agent"), QStringLiteral("Mozilla \"x\""));
        agent.bindFields(doc.layout.columns);
        doc.model->setFilterRules(agent);
        CHECK(settle(*doc.model, 0) && doc.model->rowCount() == 1,
              QStringLiteral("%1: quoted value found (%2 rows)").arg(label).arg(doc.model->rowCount()));
        const auto e = doc.model->entryAt(0);
        CHECK(e && doc.model->fieldsOf(*e).value(2, e->message()) == QStringLiteral("Mozilla \"x\" agent"),
              label + ": quoted value decoded");
    }
}

// Схема-шаблон в каталоге (версия 4): определение, колонки, разбор.
void testTemplateDefinition()
{
    const QString def = LineTemplateSchema::definition(QStringLiteral("{Timestamp:time} {Level:level} {Message}"));
    CHECK(FieldLayout::isTemplateDefinition(def), "template definition recognised");
    CHECK(!FieldLayout::isTemplateDefinition(buildSchema()), "block schema is not a template");
    FieldLayout layout;
    const FileSchemaPtr s = layout.schemaFor(def);
    CHECK(s && s->isValid() && !s->pattern.isValid(), "template schema: extractor yes, loader pattern no");
    const QString line = QStringLiteral("2026-03-05 10:00:00.000 WARN disk is full");
    const LogEntryFields f = s->extractFields(line);
    CHECK(f.value(1, line) == QStringLiteral("WARN") && f.value(2, line) == QStringLiteral("disk is full"),
          "template schema extracts fields");
    // upTo — только до Level: принята, Level тот же.
    LogEntryFields partial;
    CHECK(FieldExtractor::accepted(s->extract(line, &partial, 1)) && partial.value(1, line) == QStringLiteral("WARN"),
          "extract up to a column");
}

// Режим замера: field_source --bench <файл> [схема]
// Загрузка обоими путями (время, RSS процесса после загрузки резидентной
// вкладки), затем на каждом бэкенде — текстовый фильтр против фильтра по
// полю с тем же образцом: Level (ранняя остановка) и Message (последнее
// поле — весь разбор на строках, прошедших предфильтр). Схема — путь к файлу
// схемы или сама строка; без неё — схема фикстур (buildSchema).
long rssKb(const char* key)
{
    QFile f(QStringLiteral("/proc/self/status"));
    if (!f.open(QIODevice::ReadOnly))
        return -1;
    for (const QByteArray& line : f.readAll().split('\n'))
        if (line.startsWith(key))
            return line.mid(qstrlen(key)).trimmed().split(' ').value(0).toLong();
    return -1;
}

int runBench(const QString& path, QString schema)
{
    if (QFileInfo::exists(schema)) {
        QFile f(schema);
        if (f.open(QIODevice::ReadOnly))
            schema = QString::fromUtf8(f.readAll());
    }
    if (schema.isEmpty())
        schema = buildSchema();
    for (const Backend b : { Backend::Resident, Backend::Indexed }) {
        QElapsedTimer t;
        t.start();
        Document doc = build(b, {path}, schema);
        const qint64 loadMs = t.elapsed();
        std::printf("%s: load %lld ms, %d rows, RSS %ld kB (peak %ld kB)\n", qPrintable(name(b)),
                    (long long)loadMs, doc.model->rowCount(), rssKb("VmRSS:"), rssKb("VmHWM:"));
        LogModel& m = *doc.model;
        const auto timed = [&](const FilterRuleSet& rules, const char* what) {
            FilterRuleSet bound = rules;
            bound.bindFields(doc.layout.columns);
            m.setFilterRules(FilterRuleSet());
            settle(m, 0);
            m.fieldSource().resetStats();
            QElapsedTimer ft;
            ft.start();
            m.setFilterRules(bound);
            settle(m, 0);
            const qint64 ms = ft.elapsed();
            std::printf("  %-34s %6lld ms  rows %8d  extract %lld\n", what, (long long)ms,
                        m.rowCount(), (long long)m.fieldSource().stats().scanExtracts);
            return ms;
        };
        // Образцы — слово уровня и частое слово сообщения.
        for (int rep = 0; rep < 3; ++rep) {
            timed(fieldRule(QString(), QStringLiteral("ERROR")), "text: ERROR");
            timed(fieldRule(QStringLiteral("Level"), QStringLiteral("ERROR")), "field Level: ERROR");
            timed(fieldRule(QString(), QStringLiteral("timeout")), "text: timeout");
            timed(fieldRule(QStringLiteral("Message"), QStringLiteral("timeout")), "field Message: timeout");
            timed(fieldRule(QStringLiteral("Message"), QStringLiteral("o"), FilterRule::Action::Include),
                  "field Message: o (all lines)");
            timed(fieldRule(QString(), QStringLiteral("o")), "text: o (all lines)");
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QCoreApplication app(argc, argv);
    if (argc >= 3 && qstrcmp(argv[1], "--bench") == 0)
        return runBench(QString::fromLocal8Bit(argv[2]),
                        argc >= 4 ? QString::fromLocal8Bit(argv[3]) : QString());
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "no temp dir\n");
        return 1;
    }
    const QDir dir(tmp.path());

    testTemplateDefinition();
    testFieldsEqualDirectExtract(dir);
    for (const Backend b : { Backend::Resident, Backend::Indexed }) {
        testSchemaSwitchAndCache(b, dir);
        testRecordRule(b, dir);
        testMissingColumnAndDisplay(b, dir);
        testPrefilterEscapes(b, dir);
    }

    std::printf("field_source: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
