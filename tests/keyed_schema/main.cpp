// Ядро ключевой схемы (F-4, docs/design/F-4.md, PR 1): keyedline, вид схемы
// в LogPattern, экранированные значения полей и их читатели, классификация
// строки по ключам. Эталон — заданные руками поля и моменты; QJsonDocument —
// только дополнительная проба на случайных объектах.
//
// Режим замера: keyed_schema --bench [строк] — цена горячего пути ключевой
// схемы против регексной на тех же записях.
#include "../common/fieldmatch.h"
#include "filterruleset.h"
#include "keyedline.h"
#include "lineclassifier.h"
#include "lineformat.h"
#include "fieldextractor.h"
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

// Ключевая часть — формат строк файла (F-17, шаг 7): колонки — KeyedExtractor
// по ключам формата, классификация — LineClassifier::classifyLine с
// LineFormatReader. Прежде это была ключевая схема (LogPattern::keyed).
struct KeyedSchema {
    LineFormat format;
    LineFormatReader reader;
    KeyedExtractor extractor;

    explicit KeyedSchema(const KeyedDefinition& k)
        : format(LineFormat::structured(k.format == KeyedDefinition::Format::Logfmt
                                            ? LineFormat::Kind::Logfmt : LineFormat::Kind::Json, k))
        , reader(format)
        , extractor(format.keys)
    {
    }
    QStringList fieldNames() const { return extractor.fields(); }
    bool recognises(QStringView line) const
    {
        return extractor.extract(line, nullptr) != FieldExtractor::Outcome::Rejected;
    }
    LogEntryFields extractFields(QStringView line) const
    {
        LogEntryFields f;
        if (extractor.extract(line, &f) == FieldExtractor::Outcome::Rejected)
            return LogEntryFields();
        return f;
    }
    QVector<std::pair<QString, QString>> keyValuePairs(QStringView line) const
    {
        return KeyedLine::keyValuePairs(line, format.keys.format);
    }
    bool scanKeyed(QStringView line, KeyedLine::Hits* hits, int needed = -1) const
    {
        return reader.lookup.scan(line, hits, needed);
    }
};

KeyedSchema keyedPattern(KeyedDefinition::Format format, const QStringList& extra = {})
{
    KeyedDefinition k;
    k.format = format;
    k.extraColumns = extra;
    return KeyedSchema(k);
}

QString fieldOf(const KeyedSchema& p, const QString& line, const QString& column)
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

void runCorpus(const char* name, const KeyedSchema& p, const QVector<Case>& cases)
{
    for (const Case& c : cases) {
        const QString line = QString::fromUtf8(c.line);
        const bool ok = p.recognises(line);
        CHECK(ok == c.recognised,
              QStringLiteral("%1: '%2' recognised=%3, expected %4")
                  .arg(QLatin1String(name), line).arg(ok).arg(c.recognised));
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
    const KeyedSchema p = keyedPattern(KeyedDefinition::Format::JsonObject,
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
    const KeyedSchema p = keyedPattern(KeyedDefinition::Format::Logfmt,
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
    const KeyedSchema p = keyedPattern(KeyedDefinition::Format::JsonObject);
    const QString line = QString::fromUtf8(
        R"({"ts":1,"msg":"a\tb","n\u0061me":"x","obj":{"k":[1,2]},"nil":null,"ok":false})");
    const auto pairs = p.keyValuePairs(line);
    const QVector<std::pair<QString, QString>> expected = {
        {"ts", "1"}, {"msg", "a\tb"}, {"name", "x"}, {"obj", R"({"k":[1,2]})"},
        {"nil", "null"}, {"ok", "false"},
    };
    CHECK(pairs == expected, "keyValuePairs: all top-level keys, decoded, nested as text");
    const KeyedSchema lf = keyedPattern(KeyedDefinition::Format::Logfmt);
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
    KeyedSchema p = keyedPattern(KeyedDefinition::Format::JsonObject, {"user", "path", "note", "x"});
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
            const bool recognised = p.recognises(line);
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
    const KeyedSchema p = keyedPattern(KeyedDefinition::Format::JsonObject);
    const QString line = QString::fromUtf8(
        R"({"level":"error","msg":"open \"C:\\temp\"\n\u0444\u0430\u0439\u043b"})");
    const LogEntryFields fields = p.extractFields(line);

    const auto ruleOn = [&](const QString& field, const QString& text) {
        FilterRuleSet set;
        FilterRule r;
        r.text = text;
        r.fieldName = field;
        set.rules = {r};
        set.bindFields(p.fieldNames());
        return testfields::matchesOwnRecord(set, testfields::scannerFor(p.format.fieldDefinition()), line);
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
        set.bindFields(p.fieldNames());
        CHECK(testfields::matchesOwnRecord(set, testfields::scannerFor(p.format.fieldDefinition()), line),
              "regex filter by field on decoded value");
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
    const KeyedSchema p = keyedPattern(KeyedDefinition::Format::JsonObject);
    const qint64 tenUtc =
        QDateTime(QDate(2026, 11, 18), QTime(10, 0), QTimeZone::UTC).toMSecsSinceEpoch();

    // pino: эпоха мс и числовой уровень; слово «error» в сообщении — не уровень.
    LineClassification c = classifier.classifyLine(&p.reader, u"{\"level\":30,\"time\":1700000000123,\"msg\":\"retry after error\"}");
    CHECK(c.matched && c.tsFromKey && c.tsMs == 1700000000123LL && c.levelFromKey
              && c.level == LogLevel::Info && c.isPrimary(),
          "classify: pino epoch ms and numeric level from keys");
    c = classifier.classifyLine(&p.reader, u"{\"ts\":\"2026-11-18T10:00:00Z\",\"severity\":\"WARNING\",\"msg\":\"x\"}");
    CHECK(c.matched && c.tsMs == tenUtc && c.level == LogLevel::Warn, "classify: ISO and word from keys");
    c = classifier.classifyLine(&p.reader, u"{\"@t\":\"2026-11-18T10:00:00\\u005a\",\"@l\":\"err\",\"@m\":\"x\"}");
    CHECK(c.matched && c.tsFromKey && c.tsMs == tenUtc && c.level == LogLevel::Error,
          "classify: escaped value decoded before reading");
    // Ключа нет или значение не читается — эвристика по строке.
    c = classifier.classifyLine(&p.reader, u"{\"msg\":\"at 2026-11-18T10:00:00Z ERROR happened\"}");
    CHECK(c.matched && !c.tsFromKey && c.hasTs && c.tsMs == tenUtc && !c.levelFromKey
              && c.hasLevel && c.level == LogLevel::Error && c.isPrimary(),
          "classify: no ts/level keys -> heuristics on the line");
    c = classifier.classifyLine(&p.reader, u"{\"ts\":\"soon\",\"level\":\"Information\",\"msg\":\"warn x\"}");
    CHECK(c.matched && !c.tsFromKey && !c.hasTs && !c.levelFromKey && c.level == LogLevel::Warn,
          "classify: unreadable values -> heuristics");
    // Не распознана — как без схемы: продолжение, если нет ведущей метки.
    c = classifier.classifyLine(&p.reader, u"    at com.example.Foo(Foo.java:42)");
    CHECK(!c.matched && !c.hasTs && !c.isPrimary(), "classify: stack frame is a continuation");
    c = classifier.classifyLine(&p.reader, u"2026-11-18 10:00:00 plain line");
    CHECK(!c.matched && c.hasTs && c.leading && c.isPrimary(), "classify: plain leading timestamp line");
    // logfmt: голая метка ISO — текст, а не число (S-17): момент из ключа,
    // а не самая левая метка строки; голое число — эпоха, как раньше.
    const KeyedSchema lf = keyedPattern(KeyedDefinition::Format::Logfmt);
    c = classifier.classifyLine(&lf.reader, u"start=2026-11-18T09:00:00Z ts=2026-11-18T10:00:00Z level=info msg=x");
    CHECK(c.matched && c.tsFromKey && c.tsMs == tenUtc && !c.leading,
          "classify (logfmt): bare ISO value is read from its key");
    c = classifier.classifyLine(&lf.reader, u"ts=1700000000123 level=30 msg=x");
    CHECK(c.tsFromKey && c.tsMs == 1700000000123LL && c.levelFromKey && c.level == LogLevel::Info,
          "classify (logfmt): bare epoch and numeric level");
    c = classifier.classifyLine(&lf.reader, u"ts=-1.5 level=5xx msg=x");
    CHECK(c.tsFromKey && c.tsMs == -1500LL && !c.levelFromKey,
          "classify (logfmt): bare negative fraction is an epoch, 5xx is not a level");
    // Голое значение — Number только в форме, которую читает epochFromValue.
    const QString bare = QStringLiteral("ts=1700000000 n=-12.5 e=1e3 d=2026-11-18 a=5xx b=-x c=1.");
    KeyedLine::PairReader reader(bare, KeyedDefinition::Format::Logfmt);
    QVector<KeyedLine::ValueKind> kinds;
    for (KeyedLine::Pair q; reader.next(&q);)
        kinds << q.kind;
    using VK = KeyedLine::ValueKind;
    CHECK(kinds == QVector<VK>({VK::Number, VK::Number, VK::Literal, VK::Literal, VK::Literal,
                                VK::Literal, VK::Literal}),
          "logfmt: bare Number only for [-]digits[.digits]");
    // Формат Plain — только эвристика.
    c = classifier.classifyLine(nullptr, u"{\"level\":30,\"msg\":\"error\"}", {}, nullptr);
    CHECK(!c.matched && c.level == LogLevel::Error, "classify: plain text -> heuristics only");
    // Время без даты (F-11) в нераспознанной строке — через контекст файла.
    ClockDates clock;
    c = classifier.classifyLine(&p.reader, u"13:55:40.016 [main] INFO x", {}, &clock);
    CHECK(!c.matched && c.hasTs && clock.matched, "classify: clock time with file context");
    c = classifier.classifyLine(&p.reader, u"{\"ts\":1700000000,\"msg\":\"x\"}", {}, &clock);
    CHECK(c.tsFromKey && !clock.matched, "classify: key timestamp resets clock.matched");
}

// Классификация без полей (индексатор, счётчики) кончает разбор строки,
// когда у метки и уровня найден первый ключ-кандидат (S-17). Ответ обязан
// быть тем же, что у полного разбора с полями: заданные случаи (поздний
// главный ключ, повторный, null, путь, ключ с обратной косой) и случайные
// строки обоих форматов из пула ключей в случайном порядке.
bool sameClassification(const LineClassification& a, const LineClassification& b)
{
    return a.matched == b.matched && a.hasTs == b.hasTs && a.tsMs == b.tsMs
        && a.leading == b.leading && a.hasLevel == b.hasLevel && a.level == b.level
        && a.tsFromKey == b.tsFromKey && a.levelFromKey == b.levelFromKey;
}

void checkEarlyExit(const LineClassifier& classifier, const KeyedSchema& p, const QString& line,
                    const char* what)
{
    const LineClassifier::YearReference ref =
        LineClassifier::yearReference(QDate(2026, 11, 18));
    LogEntryFields fields;
    const LineClassification full = classifier.classifyLine(&p.reader, line, ref, nullptr, &fields);
    const LineClassification quick = classifier.classifyLine(&p.reader, line, ref, nullptr, nullptr);
    CHECK(sameClassification(full, quick),
          QStringLiteral("early exit (%1): classification without fields = with fields: %2")
              .arg(QLatin1String(what), line));
    KeyedLine::Hits all, two;
    const bool okAll = p.scanKeyed(line, &all);
    const bool okTwo = p.scanKeyed(line, &two, 2);
    bool same = okAll == okTwo && all.size() == two.size();
    for (int c = 0; same && c < 2; ++c)
        same = all[c].start == two[c].start && all[c].length == two[c].length
            && all[c].escaped == two[c].escaped && all[c].kind == two[c].kind;
    CHECK(same, QStringLiteral("early exit (%1): scan of two columns = full scan for them: %2")
                    .arg(QLatin1String(what), line));
}

void testEarlyExit()
{
    const LineClassifier classifier;
    const KeyedSchema json = keyedPattern(KeyedDefinition::Format::JsonObject, {QStringLiteral("user")});
    const KeyedSchema lf = keyedPattern(KeyedDefinition::Format::Logfmt, {QStringLiteral("user")});
    const LineClassifier::YearReference ref =
        LineClassifier::yearReference(QDate(2026, 11, 18));
    const qint64 elevenUtc =
        QDateTime(QDate(2026, 11, 18), QTime(11, 0), QTimeZone::UTC).toMSecsSinceEpoch();

    // «time» — второй кандидат: «ts» дальше по строке главнее, разбор идёт до него.
    LineClassification c = classifier.classifyLine(
        &json.reader, u"{\"time\":\"2026-11-18T10:00:00Z\",\"msg\":\"x\",\"level\":\"info\",\"ts\":\"2026-11-18T11:00:00Z\",\"lvl\":\"error\"}",
        ref, nullptr, nullptr);
    CHECK(c.matched && c.tsFromKey && c.tsMs == elevenUtc && c.level == LogLevel::Info,
          "early exit: a later first-candidate key wins over an earlier second one");
    c = classifier.classifyLine(
        &lf.reader, u"time=2026-11-18T10:00:00Z lvl=error msg=x level=warn ts=2026-11-18T11:00:00Z",
        ref, nullptr, nullptr);
    CHECK(c.matched && c.tsMs == elevenUtc && c.level == LogLevel::Warn,
          "early exit (logfmt): a later first-candidate key wins over an earlier second one");
    // Первый кандидат с null — не найден, разбор идёт дальше.
    c = classifier.classifyLine(&json.reader, u"{\"ts\":null,\"level\":\"info\",\"time\":\"2026-11-18T11:00:00Z\"}",
                                ref, nullptr, nullptr);
    CHECK(c.tsFromKey && c.tsMs == elevenUtc, "early exit: null first candidate does not stop the scan");

    const QStringList fixed = {
        QStringLiteral("{\"ts\":\"2026-11-18T10:00:00Z\",\"level\":\"info\",\"msg\":\"a\",\"user\":1}"),
        QStringLiteral("{\"msg\":\"a\",\"level\":\"warn\",\"ts\":1700000000}"),
        QStringLiteral("{\"level\":\"info\",\"level\":\"error\",\"ts\":1,\"ts\":2}"),
        QStringLiteral("{\"log\":{\"level\":\"error\"},\"ts\":\"2026-11-18T10:00:00Z\",\"level\":\"debug\"}"),
        QStringLiteral("{\"log\":{\"level\":\"error\"},\"ts\":\"2026-11-18T10:00:00Z\"}"),
        QStringLiteral("{\"t\\u0073\":\"2026-11-18T10:00:00Z\",\"le\\u0076el\":\"warn\"}"),
        QStringLiteral("{\"ts\":\"2026-11-18T10:00:00Z\",\"level\":\"info\",\"msg\":\"unterminated"),
        QStringLiteral("{\"user\":1}"),
        QStringLiteral("    at Foo.bar(Foo.java:1)"),
        QStringLiteral("2026-11-18 10:00:00 ERROR plain"),
    };
    for (const QString& line : fixed)
        checkEarlyExit(classifier, json, line, "json");
    const QStringList fixedLf = {
        QStringLiteral("ts=2026-11-18T10:00:00Z level=info msg=\"a b\" user=1"),
        QStringLiteral("msg=a level=warn ts=1700000000"),
        QStringLiteral("level=info level=error ts=1 ts=2"),
        QStringLiteral("level= ts=2026-11-18T10:00:00Z level=error"),
        QStringLiteral("ts=\"2026-11-18T10:00:00Z\" level=\"in\\\"fo\" msg=x"),
        QStringLiteral("ts=2026-11-18T10:00:00Z level=info msg=\"unterminated"),
        QStringLiteral("user=1 other=2"),
        QStringLiteral("Caused by: x=1"),
        QStringLiteral("2026-11-18 10:00:00 ERROR plain"),
    };
    for (const QString& line : fixedLf)
        checkEarlyExit(classifier, lf, line, "logfmt");

    // Случайные строки: ключи ролей и чужие, повторы, null / пустые значения.
    QRandomGenerator rng(20261126);
    const QStringList keys = {"ts", "time", "@t", "level", "lvl", "severity", "msg", "message",
                              "user", "log", "x"};
    const QStringList values = {"2026-11-18T10:00:00Z", "1700000000123", "info", "ERROR", "30",
                                "hello world", "", "null"};
    for (int iter = 0; iter < 4000; ++iter) {
        const int count = 1 + int(rng.bounded(6));
        QStringList jsonPairs, lfPairs;
        for (int i = 0; i < count; ++i) {
            const QString key = keys[int(rng.bounded(int(keys.size())))];
            const QString value = values[int(rng.bounded(int(values.size())))];
            const bool isNull = value == QLatin1String("null");
            const bool number = !value.isEmpty() && value[0].isDigit() && !value.contains(u'-');
            if (key == QLatin1String("log") && rng.bounded(2))
                jsonPairs << QStringLiteral("\"log\":{\"level\":\"%1\"}").arg(value);
            else
                jsonPairs << QStringLiteral("\"%1\":%2").arg(
                    key, isNull || number ? value : QStringLiteral("\"%1\"").arg(value));
            lfPairs << QStringLiteral("%1=%2").arg(
                key, value.contains(u' ') ? QStringLiteral("\"%1\"").arg(value) : value);
        }
        checkEarlyExit(classifier, json, QStringLiteral("{%1}").arg(jsonPairs.join(u',')), "json fuzz");
        checkEarlyExit(classifier, lf, lfPairs.join(u' '), "logfmt fuzz");
    }
}

// Формат строк (F-17, шаг 7) вместо ключевой схемы: выбор пользователя
// сохраняется JSON-ом (TabSource, память файла, сессия) и читается обратно;
// ключевая схема прежних версий читается своим запасным блоком «Message» и
// узнаётся как снятая (LogPattern::isRetiredKeyedDefinition).
void testSerialization()
{
    KeyedDefinition k;
    k.format = KeyedDefinition::Format::Logfmt;
    k.timeKeys = {"when"};
    k.extraColumns = {"logger", "trace_id"};
    k.epochUnit = KeyedDefinition::EpochUnit::Nanos;
    k.numericLevels = KeyedDefinition::NumericLevels::Python;
    const LineFormat f = LineFormat::structured(LineFormat::Kind::Logfmt, k, LineFormat::Origin::User);
    LineFormat back;
    CHECK(LineFormat::fromJson(f.toJson(), &back) && back == f, "line format round-trips");
    CHECK(f.readingSignature() != LineFormat::structured(LineFormat::Kind::Logfmt).readingSignature(),
          "keys change the reading signature");
    LineFormat noColumns = f;
    noColumns.keys.extraColumns.clear();
    CHECK(noColumns.readingSignature() == f.readingSignature(), "columns do not change the reading");
    KeyedDefinition fromDef;
    CHECK(LineFormat::keysFromFieldDefinition(f.fieldDefinition(), &fromDef) && fromDef == f.keys,
          "field definition carries the keys");
    LineFormat plain = LineFormat::plain(LineFormat::Origin::User);
    plain.levelStartsRecord = true;
    CHECK(LineFormat::fromJson(plain.toJson(), &back) && back == plain, "plain with levelStartsRecord round-trips");
    for (const QJsonObject& bad : {QJsonObject{{"kind", "gelf"}}, QJsonObject{{"kind", "json"}, {"epochUnit", "days"}},
                                   QJsonObject{{"kind", "json"}, {"timeKeys", "ts"}}, QJsonObject{}}) {
        LineFormat d;
        CHECK(!LineFormat::fromJson(bad, &d), QStringLiteral("refused line format: %1")
                  .arg(QString::fromUtf8(QJsonDocument(bad).toJson(QJsonDocument::Compact))));
    }

    // Ключевая схема прежних версий (F-4): «keyed» отбрасывается, схема — Message.
    QJsonObject legacy{{"version", 3}, {"type", "dynamic-block-schema"}, {"linePrefix", ""},
                       {"blocks", QJsonArray{QJsonObject{{"name", "Message"}, {"kind", "remainder"}}}},
                       {"keyed", QJsonObject{{"format", "json"}}}};
    const QString text = QString::fromUtf8(QJsonDocument(legacy).toJson());
    const LogPattern old(text);
    CHECK(old.isValid() && old.fieldNames() == QStringList{"Message"},
          "old keyed schema: a valid schema with one Message column");
    CHECK(LogPattern::isRetiredKeyedDefinition(text) && !LogPattern::isRetiredKeyedDefinition(LogPattern::serializeDefinition(old.definition())),
          "old keyed schema is recognised as retired; its re-serialization is not");
    const QString line = QStringLiteral("level=info msg=x");
    CHECK(old.extractFields(line).get(0, line) == line, "fallback Message is the whole line");
}

// Цена горячего пути: classifyKeyed + extractFields на JSON-строках против
// регексной схемы (эвристика метки и уровня + extractFields) на тех же
// записях в текстовом виде. Цель F-4 — не дороже ×1,2.
struct Cost {
    qint64 keyedNs = 0, regexNs = 0, heuristicNs = 0;
    qint64 classifyNs = 0;   ///< Только classifyKeyed — путь индексатора (поля по требованию).
    qint64 scanNs = 0;       ///< Только разбор пар и поиск метки и уровня (scanKeyed по двум колонкам — путь индексатора).
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

    KeyedDefinition k;
    k.extraColumns = {"thread", "logger"};
    const KeyedSchema keyed(k);

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
            const LineClassification c = classifier.classifyLine(&keyed.reader, line, ref, nullptr);
            sink += c.tsMs + int(c.level) + keyed.extractFields(line).size();
        }
    });
    cost.classifyNs = best([&] {
        for (const QString& line : jsonLines) {
            const LineClassification c = classifier.classifyLine(&keyed.reader, line, ref, nullptr);
            sink += c.tsMs + int(c.level);
        }
    });
    cost.scanNs = best([&] {
        KeyedLine::Hits hits;
        for (const QString& line : jsonLines) {
            keyed.scanKeyed(line, &hits, 2);   // как classifyLine без полей (S-17)
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
    testEarlyExit();
    testSerialization();
    testCost();
    if (g_failures == 0)
        std::fprintf(stderr, "keyed_schema: all checks passed\n");
    else
        std::fprintf(stderr, "keyed_schema: %d check(s) FAILED\n", g_failures);
    return g_failures;
}
