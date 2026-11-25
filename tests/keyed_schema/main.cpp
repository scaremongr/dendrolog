// Ядро ключевой схемы (F-4, docs/design/F-4.md, PR 1): keyedline, вид схемы
// в LogPattern, экранированные значения полей и их читатели, классификация
// строки по ключам. Эталон — заданные руками поля и моменты; QJsonDocument —
// только дополнительная проба на случайных объектах.
//
// Режим замера: keyed_schema --bench [строк] — цена горячего пути ключевой
// схемы против регексной на тех же записях.
#include "filterruleset.h"
#include "keyedline.h"
#include "lineclassifier.h"
#include "logmodel.h"
#include "logpattern.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTimeZone>

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::fprintf(stderr, "[FAIL] %s\n", qPrintable(QString(msg)));   \
        }                                                                    \
    } while (0)

namespace {

constexpr const char* kAbsent = "<absent>";

LogPattern keyedPattern(KeyedDefinition::Format format, const QStringList& extra = {})
{
    PatternDefinition def;
    KeyedDefinition k;
    k.format = format;
    k.extraColumns = extra;
    def.keyed = k;
    LogPattern p(LogPattern::serializeDefinition(def));
    CHECK(p.isValid() && p.isKeyed(), "keyed schema compiles");
    return p;
}

QString fieldOf(const LogPattern& p, const QString& line, const QString& column)
{
    const int idx = p.fieldNames().indexOf(column);
    if (idx < 0)
        return QStringLiteral("<no column>");
    const LogEntryFields f = p.extractFields(line);
    return f.has(idx) ? f.value(idx, line) : QString::fromLatin1(kAbsent);
}

struct Case {
    const char* line;
    bool recognised;
    // Колонка → ожидаемое значение (kAbsent — поля нет).
    QVector<std::pair<const char*, QString>> fields;
};

void runCorpus(const char* name, const LogPattern& p, const QVector<Case>& cases)
{
    for (const Case& c : cases) {
        const QString line = QString::fromUtf8(c.line);
        const LineMatchResult m = p.matchLine(line);
        CHECK(m.ok == c.recognised,
              QStringLiteral("%1: '%2' recognised=%3, expected %4")
                  .arg(QLatin1String(name), line).arg(m.ok).arg(c.recognised));
        const LogEntryFields fields = p.extractFields(line);
        CHECK(c.recognised || fields.isEmpty(),
              QStringLiteral("%1: '%2' unrecognised line has no fields").arg(QLatin1String(name), line));
        for (const auto& [column, expected] : c.fields) {
            const QString got = fieldOf(p, line, QLatin1String(column));
            CHECK(got == expected, QStringLiteral("%1: '%2' %3 = '%4', expected '%5'")
                                       .arg(QLatin1String(name), line, QLatin1String(column), got,
                                            expected));
        }
    }
}

const QString A = QString::fromLatin1(kAbsent);

void testJsonCorpus()
{
    const LogPattern p = keyedPattern(KeyedDefinition::Format::JsonObject,
                                      {QStringLiteral("user"), QStringLiteral("ctx"),
                                       QStringLiteral("tags"), QStringLiteral("ok")});
    CHECK(p.fieldNames() == QStringList({"Timestamp", "Level", "Message", "user", "ctx", "tags", "ok"}),
          "json: columns are roles + extraColumns");
    const QVector<Case> cases = {
        {R"({"ts":"2026-11-18T10:00:00Z","level":"info","msg":"Request failed","user":42})", true,
         {{"Timestamp", "2026-11-18T10:00:00Z"}, {"Level", "info"}, {"Message", "Request failed"},
          {"user", "42"}}},
        // Ключи в любом порядке, эпоха числом, пробелы вокруг.
        {R"({ "msg" : "m" , "user" : "u", "level":"warn" ,"time": 1700000000123 })", true,
         {{"Timestamp", "1700000000123"}, {"Level", "warn"}, {"Message", "m"}, {"user", "u"}}},
        // Экранирование: \" \\ \/ \n \t \uXXXX и суррогатная пара.
        {R"({"msg":"say \"hi\"\npath C:\\temp\/x \u0430\u0431\u0432 \ud83d\ude00\tend","level":"error"})",
         true,
         {{"Message", QStringLiteral("say \"hi\"\npath C:\\temp/x абв 😀\tend")}, {"Level", "error"}}},
        // Кириллица как есть (UTF-8 файла) — без экранирования.
        {R"({"msg":"Ошибка входа","level":"warn"})", true, {{"Message", QStringLiteral("Ошибка входа")}}},
        // Оборванный объект: пары до обрыва.
        {R"({"ts":"2026-11-18T10:00:00Z","level":"info","msg":"cut her)", true,
         {{"Timestamp", "2026-11-18T10:00:00Z"}, {"Level", "info"}, {"Message", A}}},
        {R"({"ts":"2026-11-18T10:00:00Z","lev)", true, {{"Timestamp", "2026-11-18T10:00:00Z"}, {"Level", A}}},
        {R"({"ctx":{"a":[1,{"b":"}"}]},"msg":"after nested"})", true,
         {{"ctx", R"({"a":[1,{"b":"}"}]})"}, {"Message", "after nested"}}},
        {R"({"ctx":{"a":[1,2)", false, {}},                       // обрыв во вложенном
        {R"({"msg":"esc at end\)", false, {}},                    // обрыв посреди \-последовательности
        {R"({"msg":"x\u04)", false, {}},                          // обрыв посреди \u
        // Вложенный путь log.level и дословный ключ «log.level».
        {R"({"log":{"logger":"x","level":"warn"},"msg":"m","ctx":{"a":[1,{"b":"}"}]}})", true,
         {{"Level", "warn"}, {"ctx", R"({"a":[1,{"b":"}"}]})"}}},
        {R"({"log":{"level":"info"},"log.level":"error","msg":"m"})", true, {{"Level", "error"}}},
        {R"({"log":"flat","msg":"m"})", true, {{"Level", A}}},
        // Повторный ключ — первый; null — поля нет, берётся следующий кандидат.
        {R"({"msg":"first","msg":"second"})", true, {{"Message", "first"}}},
        {R"({"msg":null,"message":"fallback","level":null})", true,
         {{"Message", "fallback"}, {"Level", A}}},
        {R"({"msg":"","message":"non-empty"})", true, {{"Message", "non-empty"}}},
        // Приоритет кандидатов — по списку: ts главнее time, где бы ни стоял.
        {R"({"time":"2020-01-01T00:00:00Z","ts":"2026-11-18T10:00:00Z","msg":"m"})", true,
         {{"Timestamp", "2026-11-18T10:00:00Z"}}},
        // Ключ с обратной косой сравнивается раскодированным.
        {R"({"m\u0073g":"escaped key"})", true, {{"Message", "escaped key"}}},
        // Массив, литералы.
        {R"({"msg":"m","tags":["a","b"],"ok":true,"user":null})", true,
         {{"tags", R"(["a","b"])"}, {"ok", "true"}, {"user", A}}},
        // Не распознаны: отступ, мусор, пусто, нет ключа роли, текст.
        {R"(  {"msg":"indented"})", false, {}},
        {R"(  "ts": "2026-11-18T10:00:00Z",)", false, {}},
        {"java.lang.IllegalStateException: boom", false, {}},
        {"", false, {}},
        {"{}", false, {}},
        {R"({"user":42,"tags":[]})", false, {}},
        {"{not json at all", false, {}},
        {R"(2026-11-18 10:00:00 INFO {"msg":"prefixed"})", false, {}},
    };
    runCorpus("json", p, cases);

    // Признак экранирования: get() — сырой текст, value() — раскодированный,
    // value(displayLine) — управляющие символы остаются последовательностями.
    const QString line = QString::fromUtf8(R"({"msg":"a\"b\nc\u0430","level":"info"})");
    const LogEntryFields f = p.extractFields(line);
    const int msg = KeyedDefinition::kMessageColumn;
    CHECK(f.isEscaped(msg) && !f.isEscaped(KeyedDefinition::kLevelColumn), "escaped bit only on escaped values");
    CHECK(f.get(msg, line) == QLatin1String(R"(a\"b\nc\u0430)"), "get() is the raw text");
    CHECK(f.value(msg, line) == QStringLiteral("a\"b\ncа"), "value() decodes");
    CHECK(f.value(msg, line, true) == QStringLiteral("a\"b\\ncа"), "display value keeps \\n escaped");
    for (const char* raw : {R"(a\q\u12G4\)", R"(\u0001\t\\\"\/x)", R"(😀\r\n)", "\\\\"}) {
        const QString v = QString::fromLatin1(raw);
        for (const bool keep : {false, true})
            CHECK(KeyedLine::unescapedSize(v, keep) == KeyedLine::unescape(v, keep).size(),
                  QStringLiteral("unescapedSize('%1', %2)").arg(v).arg(keep));
    }
    CHECK(KeyedLine::unescape(QStringLiteral(R"(a\q\u12G4)")) == QLatin1String(R"(a\q\u12G4)"),
          "unknown and broken escapes stay as in the file");
    CHECK(f.spans[msg].size() == f.get(msg, line).size() && sizeof(FieldSpan) == 8,
          "FieldSpan stays 8 bytes, size() strips the escaped bit");
}

void testLogfmtCorpus()
{
    const LogPattern p = keyedPattern(KeyedDefinition::Format::Logfmt,
                                      {QStringLiteral("user"), QStringLiteral("dur")});
    const QVector<Case> cases = {
        {R"(ts=2026-11-18T10:00:00Z level=info msg="Request failed" user=42)", true,
         {{"Timestamp", "2026-11-18T10:00:00Z"}, {"Level", "info"}, {"Message", "Request failed"},
          {"user", "42"}}},
        {R"(level=warn msg="say \"hi\" C:\\temp" dur=1.5s)", true,
         {{"Message", QStringLiteral("say \"hi\" C:\\temp")}, {"dur", "1.5s"}}},
        {R"(msg=hello world level=info)", true, {{"Message", "hello"}, {"Level", "info"}}},
        {R"(log.level=error msg=x)", true, {{"Level", "error"}}},   // в logfmt «log.level» — имя ключа
        {R"(level= msg=x user=)", true, {{"Level", A}, {"user", A}, {"Message", "x"}}},
        {R"(time=1700000000 lvl=30 message=x)", true, {{"Timestamp", "1700000000"}, {"Level", "30"}}},
        {R"(msg="multi\nline" level=debug)", true, {{"Message", QStringLiteral("multi\nline")}}},
        {R"(level=info msg="unterminated)", true, {{"Level", "info"}, {"Message", A}}},
        {R"(user=1 other=2)", false, {}},                 // нет ключа роли
        {"Caused by: x=1", false, {}},                   // первый токен не пара
        {"  level=info msg=indented", false, {}},
        {"=x msg=1", false, {}},
        {R"("msg"=x)", false, {}},
        {"plain text line", false, {}},
    };
    runCorpus("logfmt", p, cases);
}

void testKeyValuePairs()
{
    const LogPattern p = keyedPattern(KeyedDefinition::Format::JsonObject);
    const QString line = QString::fromUtf8(
        R"({"ts":1,"msg":"a\tb","n\u0061me":"x","obj":{"k":[1,2]},"nil":null,"ok":false})");
    const auto pairs = p.keyValuePairs(line);
    const QVector<std::pair<QString, QString>> expected = {
        {"ts", "1"}, {"msg", "a\tb"}, {"name", "x"}, {"obj", R"({"k":[1,2]})"},
        {"nil", "null"}, {"ok", "false"},
    };
    CHECK(pairs == expected, "keyValuePairs: all top-level keys, decoded, nested as text");
    const LogPattern lf = keyedPattern(KeyedDefinition::Format::Logfmt);
    const auto lfPairs = lf.keyValuePairs(QStringLiteral(R"(a=1 b="x \"y\"" flag c=)"));
    const QVector<std::pair<QString, QString>> lfExpected = {{"a", "1"}, {"b", "x \"y\""}, {"c", ""}};
    CHECK(lfPairs == lfExpected, "keyValuePairs: logfmt");
    CHECK(keyedPattern(KeyedDefinition::Format::JsonObject).keyValuePairs(u"plain").isEmpty(),
          "keyValuePairs: unrecognised line has none");
}

// Случайный объект → строка через QJsonDocument → наши значения равны его.
void testFuzzAgainstQJson()
{
    QRandomGenerator rng(20261125);
    const QStringList keys = {"msg", "user", "path", "note", "a.b", "x"};
    LogPattern p = keyedPattern(KeyedDefinition::Format::JsonObject, {"user", "path", "note", "x"});
    const QString alphabet = QStringLiteral("ab \"\\/\n\t\r\x01яё😀{}[],:=");
    int checked = 0;
    for (int iter = 0; iter < 3000; ++iter) {
        QJsonObject obj;
        QHash<QString, QString> expect;
        for (const QString& key : keys) {
            if (rng.bounded(3) == 0)
                continue;
            switch (rng.bounded(4)) {
            case 0: {
                QString v;
                const int len = int(rng.bounded(12));
                for (int i = 0; i < len; ++i) {
                    const int at = int(rng.bounded(int(alphabet.size())));
                    if (alphabet[at].isHighSurrogate()) {
                        v += alphabet.mid(at, 2);
                    } else if (alphabet[at].isLowSurrogate()) {
                        v += alphabet.mid(at - 1, 2);
                    } else {
                        v += alphabet[at];
                    }
                }
                obj.insert(key, v);
                expect.insert(key, v.isEmpty() ? A : v);
                break;
            }
            case 1: {
                const qint64 n = qint64(rng.bounded(2000000)) - 1000000;
                obj.insert(key, n);
                expect.insert(key, QString::number(n));
                break;
            }
            case 2:
                obj.insert(key, QJsonValue::Null);
                expect.insert(key, A);
                break;
            default: {
                QJsonObject inner;
                inner.insert(QStringLiteral("k"), QStringLiteral("}\""));
                inner.insert(QStringLiteral("arr"), QJsonArray{1, "]", QJsonObject{}});
                obj.insert(key, inner);
                expect.insert(key, QString::fromUtf8(QJsonDocument(inner).toJson(QJsonDocument::Compact)));
                break;
            }
            }
        }
        const QString line = QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        for (const QString& column : {"Message", "user", "path", "note", "x"}) {
            const QString key = column == QLatin1String("Message") ? QStringLiteral("msg") : column;
            const QString got = fieldOf(p, line, column);
            const QString want = expect.value(key, A);
            // Строка без ключа роли не распознана — тогда полей нет вовсе.
            const bool recognised = p.matchLine(line).ok;
            const QString wantSeen = recognised ? want : A;
            CHECK(got == wantSeen, QStringLiteral("fuzz: %1 in %2 = '%3', expected '%4'")
                                       .arg(column, line, got, wantSeen));
            ++checked;
        }
        // Длина для раскладки view = длина значения, построенного целиком.
        const LogEntryFields f = p.extractFields(line);
        for (int i = 0; i < f.size(); ++i)
            for (const bool displayLine : {false, true})
                CHECK(f.valueSize(i, line, displayLine) == f.value(i, line, displayLine).size(),
                      QStringLiteral("fuzz: valueSize of column %1 (display %2) in %3")
                          .arg(i).arg(displayLine).arg(line));
        // Пары верхнего уровня — все ключи объекта.
        const auto pairs = p.keyValuePairs(line);
        CHECK(pairs.size() == obj.size(), QStringLiteral("fuzz: pair count in %1").arg(line));
    }
    std::printf("fuzz: %d values against QJsonDocument\n", checked);
}

// Читатели значения: фильтр по полю видит раскодированное, правило по всей
// строке — сырой текст; строка view — раскодированное однострочное.
void testReaders()
{
    const LogPattern p = keyedPattern(KeyedDefinition::Format::JsonObject);
    const QString line = QString::fromUtf8(
        R"({"level":"error","msg":"open \"C:\\temp\"\n\u0444\u0430\u0439\u043b"})");
    const LogEntryFields fields = p.extractFields(line);

    const auto ruleOn = [&](const QString& field, const QString& text) {
        FilterRuleSet set;
        FilterRule r;
        r.text = text;
        r.fieldName = field;
        set.rules = {r};
        set.bindFields(p.fieldNames(), true);
        return set.matchesLine(line, fields);
    };
    CHECK(ruleOn("Message", QStringLiteral("\"C:\\temp\"")), "filter by field: decoded quotes and backslash");
    CHECK(ruleOn("Message", QStringLiteral("файл")), "filter by field: \\u Cyrillic found as Cyrillic");
    CHECK(!ruleOn("Message", QStringLiteral("\\u0444")), "filter by field: escape sequence is not in the value");
    CHECK(ruleOn("Level", QStringLiteral("ERROR")), "filter by field: Level");
    CHECK(ruleOn(QString(), QStringLiteral("\\u0444")), "rule without field: raw line text");
    {
        FilterRuleSet set;
        FilterRule r;
        r.text = QStringLiteral("^open \"C:\\\\temp\"\\n");
        r.isRegex = true;
        r.fieldName = QStringLiteral("Message");
        set.rules = {r};
        set.bindFields(p.fieldNames(), true);
        CHECK(set.matchesLine(line, fields), "regex filter by field on decoded value");
    }

    const QVector<int> visible = {KeyedDefinition::kLevelColumn, KeyedDefinition::kMessageColumn};
    const QString shown = LogModel::formatDisplayText(line, fields, true, visible);
    CHECK(shown == QStringLiteral("error open \"C:\\temp\"\\nфайл"),
          QStringLiteral("display text decoded, one line: '%1'").arg(shown));
    CHECK(LogModel::formatDisplayText(line, fields, false, visible) == line, "fields off: raw line");
    const QString junk = QStringLiteral("not json");
    CHECK(LogModel::formatDisplayText(junk, p.extractFields(junk), true, visible) == junk,
          "unrecognised line shown raw");
}

void testClassifyKeyed()
{
    const LineClassifier classifier;
    const LogPattern p = keyedPattern(KeyedDefinition::Format::JsonObject);
    const qint64 tenUtc =
        QDateTime(QDate(2026, 11, 18), QTime(10, 0), QTimeZone::UTC).toMSecsSinceEpoch();

    // pino: эпоха мс и числовой уровень; слово «error» в сообщении — не уровень.
    KeyedClassification c = classifier.classifyKeyed(
        p, u"{\"level\":30,\"time\":1700000000123,\"msg\":\"retry after error\"}");
    CHECK(c.matched && c.tsFromKey && c.tsMs == 1700000000123LL && c.levelFromKey
              && c.level == LogLevel::Info && c.isPrimary(),
          "classify: pino epoch ms and numeric level from keys");
    c = classifier.classifyKeyed(p, u"{\"ts\":\"2026-11-18T10:00:00Z\",\"severity\":\"WARNING\",\"msg\":\"x\"}");
    CHECK(c.matched && c.tsMs == tenUtc && c.level == LogLevel::Warn, "classify: ISO and word from keys");
    c = classifier.classifyKeyed(p, u"{\"@t\":\"2026-11-18T10:00:00\\u005a\",\"@l\":\"err\",\"@m\":\"x\"}");
    CHECK(c.matched && c.tsFromKey && c.tsMs == tenUtc && c.level == LogLevel::Error,
          "classify: escaped value decoded before reading");
    // Ключа нет или значение не читается — эвристика по строке.
    c = classifier.classifyKeyed(p, u"{\"msg\":\"at 2026-11-18T10:00:00Z ERROR happened\"}");
    CHECK(c.matched && !c.tsFromKey && c.hasTs && c.tsMs == tenUtc && !c.levelFromKey
              && c.hasLevel && c.level == LogLevel::Error && c.isPrimary(),
          "classify: no ts/level keys -> heuristics on the line");
    c = classifier.classifyKeyed(p, u"{\"ts\":\"soon\",\"level\":\"Information\",\"msg\":\"warn x\"}");
    CHECK(c.matched && !c.tsFromKey && !c.hasTs && !c.levelFromKey && c.level == LogLevel::Warn,
          "classify: unreadable values -> heuristics");
    // Не распознана — как без схемы: продолжение, если нет ведущей метки.
    c = classifier.classifyKeyed(p, u"    at com.example.Foo(Foo.java:42)");
    CHECK(!c.matched && !c.hasTs && !c.isPrimary(), "classify: stack frame is a continuation");
    c = classifier.classifyKeyed(p, u"2026-11-18 10:00:00 plain line");
    CHECK(!c.matched && c.hasTs && c.leading && c.isPrimary(), "classify: plain leading timestamp line");
    // Не ключевая схема — только эвристика.
    LogPattern plain(LogPattern::serializeDefinition(PatternDefinition{
        QString(), {PatternBlock{QStringLiteral("Message"), PatternBlock::MatchKind::Remainder}}, {}}));
    c = classifier.classifyKeyed(plain, u"{\"level\":30,\"msg\":\"error\"}");
    CHECK(!c.matched && c.level == LogLevel::Error, "classify: regex schema -> heuristics only");
    // Время без даты (F-11) в нераспознанной строке — через контекст файла.
    ClockDates clock;
    c = classifier.classifyKeyed(p, u"13:55:40.016 [main] INFO x", {}, &clock);
    CHECK(!c.matched && c.hasTs && clock.matched, "classify: clock time with file context");
    c = classifier.classifyKeyed(p, u"{\"ts\":1700000000,\"msg\":\"x\"}", {}, &clock);
    CHECK(c.tsFromKey && !clock.matched, "classify: key timestamp resets clock.matched");
}

void testSerialization()
{
    PatternDefinition def;
    KeyedDefinition k;
    k.format = KeyedDefinition::Format::Logfmt;
    k.timeKeys = {"when"};
    k.extraColumns = {"logger", "trace_id"};
    k.epochUnit = KeyedDefinition::EpochUnit::Nanos;
    k.numericLevels = KeyedDefinition::NumericLevels::Python;
    def.keyed = k;
    def.blocks = {PatternBlock{QStringLiteral("Whatever"), PatternBlock::MatchKind::Integer}};
    const QString text = LogPattern::serializeDefinition(def);

    PatternDefinition back;
    CHECK(LogPattern::deserializeDefinition(text, &back), "keyed definition reads back");
    CHECK(back.keyed && *back.keyed == k, "keyed part round-trips");
    CHECK(back.blocks.size() == 1 && back.blocks[0].name == QLatin1String("Message")
              && back.blocks[0].matchKind == PatternBlock::MatchKind::Remainder,
          "keyed schema stores the fallback Message block");
    CHECK(LogPattern::serializeDefinition(back) == text, "serialization is canonical");
    const QJsonObject root = QJsonDocument::fromJson(text.toUtf8()).object();
    CHECK(root.value("version").toInt() == 3, "format version stays 3");

    // Как читает версия без F-4: без «keyed» — регексная схема Message.
    QJsonObject legacy = root;
    legacy.remove(QStringLiteral("keyed"));
    LogPattern old(QString::fromUtf8(QJsonDocument(legacy).toJson()));
    CHECK(old.isValid() && !old.isKeyed() && old.fieldNames() == QStringList{"Message"},
          "without keyed: a valid regex schema with one Message column");
    const QString line = QStringLiteral("level=info msg=x");
    CHECK(old.extractFields(line).get(0, line) == line, "fallback Message is the whole line");

    // Ключевая часть главнее блоков, правленных версией без F-4.
    QJsonObject edited = root;
    QJsonArray blocks;
    blocks.append(QJsonObject{{"name", "Edited"}, {"kind", "integer"}});
    edited.insert(QStringLiteral("blocks"), blocks);
    LogPattern again(QString::fromUtf8(QJsonDocument(edited).toJson()));
    CHECK(again.isKeyed() && again.fieldNames() == k.columnNames(), "keyed wins over edited blocks");

    // Минимальная ключевая часть — умолчания; чужое — отказ.
    QJsonObject minimal = legacy;
    minimal.insert(QStringLiteral("keyed"), QJsonObject{{"format", "json"}});
    PatternDefinition md;
    CHECK(LogPattern::deserializeDefinition(QString::fromUtf8(QJsonDocument(minimal).toJson()), &md)
              && md.keyed && *md.keyed == KeyedDefinition(),
          "keyed with only format -> defaults");
    for (const QJsonValue& bad : {QJsonValue(QJsonObject{{"format", "gelf"}}),
                                  QJsonValue(QJsonObject{{"format", "json"}, {"epochUnit", "days"}}),
                                  QJsonValue(QJsonObject{{"format", "json"}, {"timeKeys", "ts"}}),
                                  QJsonValue(QJsonObject{{"format", "json"}, {"levelKeys", QJsonArray{1}}}),
                                  QJsonValue(QJsonObject{{"format", "json"}, {"numericLevels", "x"}}),
                                  QJsonValue(QStringLiteral("json"))}) {
        QJsonObject o = legacy;
        o.insert(QStringLiteral("keyed"), bad);
        PatternDefinition d;
        CHECK(!LogPattern::deserializeDefinition(QString::fromUtf8(QJsonDocument(o).toJson()), &d, true, false),
              QStringLiteral("refused keyed: %1")
                  .arg(QString::fromUtf8(QJsonDocument(QJsonObject{{"k", bad}}).toJson(QJsonDocument::Compact))));
    }
    // Колонки — по имени: повтор и пустое имя — невалидная схема.
    PatternDefinition dup;
    KeyedDefinition dk;
    dk.extraColumns = {"Level"};
    dup.keyed = dk;
    CHECK(!LogPattern::validateDefinition(dup), "extra column named like a role column is refused");
    dk.extraColumns = {"  "};
    dup.keyed = dk;
    CHECK(!LogPattern::validateDefinition(dup), "empty extra column is refused");
}

// Цена горячего пути: classifyKeyed + extractFields на JSON-строках против
// регексной схемы (эвристика метки и уровня + extractFields) на тех же
// записях в текстовом виде. Цель F-4 — не дороже ×1,2.
struct Cost {
    qint64 keyedNs = 0, regexNs = 0, heuristicNs = 0;
    qint64 classifyNs = 0;   ///< Только classifyKeyed — путь индексатора (поля по требованию).
    qint64 scanNs = 0;       ///< Только разбор пар и поиск колонок (LogPattern::scanKeyed).
};

Cost measureCost(int count, int rounds)
{
    QStringList jsonLines, textLines;
    jsonLines.reserve(count);
    textLines.reserve(count);
    const char* levels[] = {"INFO", "WARN", "ERROR", "DEBUG"};
    for (int i = 0; i < count; ++i) {
        const QString ts = QStringLiteral("2026-11-18T10:%1:%2.%3Z")
                               .arg(i / 60000 % 60, 2, 10, QLatin1Char('0'))
                               .arg(i / 1000 % 60, 2, 10, QLatin1Char('0'))
                               .arg(i % 1000, 3, 10, QLatin1Char('0'));
        const QString level = QLatin1String(levels[i % 4]);
        const QString thread = QStringLiteral("worker-%1").arg(i % 7);
        const QString logger = QStringLiteral("com.example.service.Handler%1").arg(i % 13);
        const QString msg = QStringLiteral("Request %1 processed for user %2 in %3 ms")
                                .arg(i).arg(i * 7 % 1000).arg(i % 250);
        jsonLines << QStringLiteral(R"({"ts":"%1","level":"%2","thread":"%3","logger":"%4","msg":"%5"})")
                         .arg(ts, level.toLower(), thread, logger, msg);
        textLines << QStringLiteral("%1 [%2] %3 %4 - %5").arg(ts, thread, level, logger, msg);
    }

    PatternDefinition kd;
    KeyedDefinition k;
    k.extraColumns = {"thread", "logger"};
    kd.keyed = k;
    const LogPattern keyed(LogPattern::serializeDefinition(kd));

    PatternDefinition rd;
    rd.blocks = {
        PatternBlock{QStringLiteral("Timestamp"), PatternBlock::MatchKind::Timestamp},
        PatternBlock{QStringLiteral("Thread"), PatternBlock::MatchKind::TextUntilSeparator,
                     QStringLiteral("["), QStringLiteral("]")},
        PatternBlock{QStringLiteral("Level"), PatternBlock::MatchKind::Level},
        PatternBlock{QStringLiteral("Logger"), PatternBlock::MatchKind::TextUntilSeparator,
                     QString(), QString(), QStringLiteral("-")},
        PatternBlock{QStringLiteral("Message"), PatternBlock::MatchKind::Remainder},
    };
    const LogPattern regex(LogPattern::serializeDefinition(rd));
    CHECK(regex.isValid() && regex.extractFields(textLines[0]).has(4), "cost: regex schema matches its lines");
    CHECK(keyed.extractFields(jsonLines[0]).value(4, jsonLines[0]).startsWith(QLatin1String("com.example")),
          "cost: keyed schema matches its lines");

    const LineClassifier classifier;
    const auto ref = LineClassifier::yearReference(QDate(2026, 11, 20));
    qint64 sink = 0;
    const auto best = [&](auto&& body) {
        qint64 bestNs = -1;
        for (int r = 0; r < rounds; ++r) {
            QElapsedTimer t;
            t.start();
            body();
            const qint64 ns = t.nsecsElapsed();
            bestNs = bestNs < 0 ? ns : qMin(bestNs, ns);
        }
        return bestNs;
    };
    Cost cost;
    // Ключевая схема: одна классификация (вместо пары детекторов) + поля.
    cost.keyedNs = best([&] {
        for (const QString& line : jsonLines) {
            const KeyedClassification c = classifier.classifyKeyed(keyed, line, ref);
            sink += c.tsMs + int(c.level) + keyed.extractFields(line).size();
        }
    });
    cost.classifyNs = best([&] {
        for (const QString& line : jsonLines) {
            const KeyedClassification c = classifier.classifyKeyed(keyed, line, ref);
            sink += c.tsMs + int(c.level);
        }
    });
    cost.scanNs = best([&] {
        KeyedLine::Hits hits;
        for (const QString& line : jsonLines) {
            keyed.scanKeyed(line, &hits);
            sink += hits[0].start;
        }
    });
    // Регексная схема: детекторы по строке + поля регексом (как парсер сегодня).
    cost.regexNs = best([&] {
        for (const QString& line : textLines) {
            qint64 ms = 0;
            LogLevel level = LogLevel::Unknown;
            bool leading = false;
            classifier.detectTimestampMs(line, ms, ref, &leading);
            classifier.detectLogLevel(line, level);
            sink += ms + int(level) + regex.extractFields(line).size();
        }
    });
    // Без схемы — только детекторы на тех же JSON-строках (цель шага 2 — ×1,2 к нему).
    cost.heuristicNs = best([&] {
        for (const QString& line : jsonLines) {
            qint64 ms = 0;
            LogLevel level = LogLevel::Unknown;
            bool leading = false;
            classifier.detectTimestampMs(line, ms, ref, &leading);
            classifier.detectLogLevel(line, level);
            sink += ms + int(level);
        }
    });
    if (sink == 42)
        std::printf(" ");
    return cost;
}

void report(const Cost& c, int count)
{
    std::printf("cost per line (%d lines): keyed classify + fields %.0f ns, regex schema (detectors + fields) %.0f ns,"
                " keyed classify only %.0f ns (of it key scan %.0f ns), detectors only on the same JSON %.0f ns;"
                " keyed / regex = %.2f\n",
                count, double(c.keyedNs) / count, double(c.regexNs) / count,
                double(c.classifyNs) / count, double(c.scanNs) / count, double(c.heuristicNs) / count,
                double(c.keyedNs) / double(c.regexNs));
}

#if !defined(NDEBUG)
constexpr bool kSanitized = true;   // отладочная сборка: наш разбор без оптимизации, pcre2 — с ней
#elif defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kSanitized = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif
#else
constexpr bool kSanitized = false;
#endif

void testCost()
{
    const int count = 50000;
    const Cost c = measureCost(count, 3);
    report(c, count);
    // Санитайзер инструментирует наш сканер, но не pcre2, а отладочная сборка
    // не оптимизирует его — отношение там не показательно; печатается, но не
    // проверяется.
    if (!kSanitized)
        CHECK(c.keyedNs <= c.regexNs * 12 / 10 + 20000000,
              QStringLiteral("cost: keyed %1 ms vs regex schema %2 ms (target <= x1.2)")
                  .arg(c.keyedNs / 1000000).arg(c.regexNs / 1000000));
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc >= 2 && qstrcmp(argv[1], "--bench") == 0) {
        const int count = argc >= 3 ? QByteArray(argv[2]).toInt() : 1000000;
        report(measureCost(count, 5), count);
        return 0;
    }
    testJsonCorpus();
    testLogfmtCorpus();
    testKeyValuePairs();
    testFuzzAgainstQJson();
    testReaders();
    testClassifyKeyed();
    testSerialization();
    testCost();
    if (g_failures == 0)
        std::fprintf(stderr, "keyed_schema: all checks passed\n");
    else
        std::fprintf(stderr, "keyed_schema: %d check(s) FAILED\n", g_failures);
    return g_failures;
}
