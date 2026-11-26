// Мера соответствия схемы строкам SchemaFit и задание SchemaFitJob (F-17,
// шаг 1; docs/design/F-17.md, разделы 1–2).
//
//   1. Таблица «схема × строки → счётчики»: полные, частичные, промахи,
//      loose, запасное правило кандидатов, specificity.
//   2. Схема из одного Message (принимает всё) не выигрывает и не «подходит».
//   3. Корпус встроенных схем (образцы — tests/common/builtinsamples.h, тот
//      же, что у builtin_schemas): каждая встроенная схема первая на своих
//      образцах и «подходит»; на чужих — «не подходит», кроме закреплённых
//      пересечений.
//   4. explain — причина промаха данными.
//   5. Бюджет: по шву часов — проверка каждые 32 строки; настоящий
//      катастрофический регекс — timedOut, проход остановлен раньше конца
//      (решает счётчик строк, а не таймер теста).
//   6. SchemaFitJob на обоих бэкендах = синхронный расчёт по тем же строкам;
//      строки набора — строки файла с их номерами; отмена; reset модели
//      посреди расчёта.
//   7. Одна мера: счётчики редактора схем (подпись образцов и файла) равны
//      счётчикам SchemaFit::evaluate на тех же строках.

#include "builtinsamples.h"
#include "builtinschemas.h"
#include "conversionpatterndialog.h"
#include "logscan.h"
#include "schemafit.h"
#include "testdocuments.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QMap>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QToolButton>
#include <QAction>
#include <QMenu>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdio>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const QString& what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "[FAIL] %s\n", qPrintable(what));
        std::fflush(stderr);
    }
}

using K = PatternBlock::MatchKind;
using testdocs::patternBlock;

QString messageOnlySchema()
{
    PatternDefinition def;
    def.blocks = {patternBlock(K::Remainder, QStringLiteral("Message"))};
    return LogPattern::serializeDefinition(def);
}

QString describe(const SchemaFit::Result& r)
{
    return QStringLiteral("examined %1 candidates %2 full %3 partial %4 missed %5 loose %6 "
                          "spec %7 cov %8 valid %9 timedOut %10")
        .arg(r.examined).arg(r.candidates).arg(r.full).arg(r.partial).arg(r.missed)
        .arg(r.loose).arg(r.specificity(), 0, 'f', 2).arg(r.coverage(), 0, 'f', 3)
        .arg(r.valid).arg(r.timedOut);
}

// ---------------------------------------------------------------------------
// 1. Таблица
// ---------------------------------------------------------------------------

struct Counts {
    int examined, candidates, full, partial, missed, loose;
};

void expectCounts(const QString& name, const SchemaFit::Result& r, const Counts& c)
{
    const bool ok = r.valid && r.examined == c.examined && r.candidates == c.candidates
        && r.full == c.full && r.partial == c.partial && r.missed == c.missed && r.loose == c.loose;
    check(ok, QStringLiteral("%1: %2; ожидалось examined %3 candidates %4 full %5 partial %6 "
                             "missed %7 loose %8")
                  .arg(name, describe(r)).arg(c.examined).arg(c.candidates).arg(c.full)
                  .arg(c.partial).arg(c.missed).arg(c.loose));
}

void testTable()
{
    const LogPattern schema(testdocs::buildSchema());   // Timestamp [Thread] Level - Message
    const LogPattern message(messageOnlySchema());
    check(schema.isValid() && message.isValid(), QStringLiteral("таблица: схемы валидны"));

    const QStringList lines = {
        QStringLiteral("2026-03-05 10:00:00.000 [w1] INFO - hello"),            // полная
        QStringLiteral("2026-03-05 10:00:01.000 [w2] WARN - second"),           // полная
        QStringLiteral("java.lang.IllegalStateException: boom"),                // продолжение
        QStringLiteral("\tat com.example.Foo.bar(Foo.java:12)"),                // продолжение
        QString(),                                                              // пустая
        QStringLiteral("   "),                                                  // пустая
        QStringLiteral("2026-03-05 10:00:02.000 [w1] ERROR no dash here"),      // деградация
        QStringLiteral("2026-03-05 10:00:03.000 restart marker"),               // ведущая метка
        QStringLiteral("10:00:04.000 time only, no date"),                      // F-11: кандидат
    };
    if (qEnvironmentVariableIsSet("SCHEMA_FIT_DEBUG"))
        for (const QString& l : lines) {
            const LineMatchResult m = schema.matchLine(l);
            std::fprintf(stderr, "  ok %d matched %d unparsed %d skipped %d..%d | %s\n", m.ok,
                         m.matchedBlockCount, m.unparsedStart, m.skippedFrom, m.skippedTo, qPrintable(l));
        }
    const SchemaFit::Result r = SchemaFit::evaluate(schema, lines);
    std::fprintf(stderr, "таблица/логическая схема: %s\n", qPrintable(describe(r)));
    // Кандидаты: три полные (разделитель «-» после уровня необязателен —
    // уровень сам себя ограничивает), ведущая метка и время без даты —
    // частичные (сошлась только метка, остальное — в Message).
    expectCounts(QStringLiteral("логическая схема"), r, {7, 5, 3, 2, 0, 0});
    check(r.filled.size() == 4 && r.filled[0] == 5 && r.filled[1] == 3 && r.filled[3] == 5,
          QStringLiteral("filled по колонкам: Timestamp и Message у пяти, Thread у трёх"));
    check(r.misses.size() == 2, QStringLiteral("misses: две частичные — %1").arg(r.misses.size()));
    if (r.misses.size() == 2) {
        check(r.misses[0].partial && r.misses[0].text == lines[7] && r.misses[0].afterBlock == 0
                  && r.misses[0].stopOffset == 24,
              QStringLiteral("первая в misses — «restart marker»: после метки, с позиции 24"));
        check(r.misses[1].partial && r.misses[1].text == lines[8], QStringLiteral("вторая — время без даты"));
    }
    check(qAbs(r.coverage() - 4.0 / 5) < 1e-9, QStringLiteral("покрытие (3 + 0,5·2) / 5"));
    check(qAbs(r.specificity() - 11.0 / 5) < 1e-9,
          QStringLiteral("specificity: (3·3 + 1 + 1) / 5 = %1").arg(r.specificity()));

    // Константа в начале: строки без неё — промахи.
    PatternDefinition prefixed;
    PatternBlock app = patternBlock(K::ConstantText, QString());
    app.customRegex = QStringLiteral("APP:");
    prefixed.blocks = {app, patternBlock(K::Timestamp, QStringLiteral("Timestamp")),
                       patternBlock(K::Level, QStringLiteral("Level")),
                       patternBlock(K::Remainder, QStringLiteral("Message"))};
    const LogPattern prefixedPattern(LogPattern::serializeDefinition(prefixed));
    check(prefixedPattern.isValid(), QStringLiteral("схема с константой валидна"));
    const QStringList appLines = {
        QStringLiteral("APP: 2026-03-05 10:00:00.000 INFO started"),
        QStringLiteral("APP: 2026-03-05 10:00:01.000 WARN slow"),
        QStringLiteral("2026-03-05 10:00:02.000 ERROR other component"),
        QStringLiteral("  continuation"),
    };
    if (qEnvironmentVariableIsSet("SCHEMA_FIT_DEBUG"))
        for (const QString& l : appLines) {
            const LineMatchResult m = prefixedPattern.matchLine(l);
            std::fprintf(stderr, "  ok %d matched %d unparsed %d skipped %d..%d | %s\n", m.ok,
                         m.matchedBlockCount, m.unparsedStart, m.skippedFrom, m.skippedTo, qPrintable(l));
        }
    const SchemaFit::Result a = SchemaFit::evaluate(prefixedPattern, appLines);
    expectCounts(QStringLiteral("константа в начале"), a, {4, 3, 2, 0, 1, 0});
    check(a.misses.size() == 1 && !a.misses[0].partial && a.misses[0].afterBlock == -1
              && a.misses[0].text == appLines[2],
          QStringLiteral("промах без константы: ни одного блока"));
    check(SchemaFit::verdict(a) == SchemaFit::Verdict::Doubtful,
          QStringLiteral("2 из 3 — «сомнительно» (%1)").arg(describe(a)));

    // Схема из одного Message принимает всё: продолжения — loose.
    const SchemaFit::Result m = SchemaFit::evaluate(message, lines);
    expectCounts(QStringLiteral("один Message"), m, {7, 5, 5, 0, 0, 2});
    check(m.specificity() == 0.0, QStringLiteral("один Message: specificity 0"));
    check(SchemaFit::verdict(m) == SchemaFit::Verdict::NoFit,
          QStringLiteral("один Message не «подходит» при покрытии 100 %"));

    // Только записи в правильном формате — «подходит».
    const QStringList clean = {lines[0], lines[1], lines[2], lines[3]};
    const SchemaFit::Result c = SchemaFit::evaluate(schema, clean);
    expectCounts(QStringLiteral("чистые строки"), c, {4, 2, 2, 0, 0, 0});
    check(SchemaFit::verdict(c) == SchemaFit::Verdict::Fits, QStringLiteral("чистые строки — Fits"));

    // Запасное правило кандидатов: строк с метками меньше 20 % — кандидаты
    // все непустые без отступа.
    PatternDefinition py;
    py.blocks = {patternBlock(K::Level, QStringLiteral("Level"), QString(), QString(), QStringLiteral(":")),
                 patternBlock(K::TextUntilSeparator, QStringLiteral("Logger"), QString(), QString(),
                              QStringLiteral(":")),
                 patternBlock(K::Remainder, QStringLiteral("Message"))};
    const LogPattern pyPattern(LogPattern::serializeDefinition(py));
    check(pyPattern.isValid(), QStringLiteral("схема WARNING:root:… валидна"));
    const QStringList pyLines = {
        QStringLiteral("WARNING:root:disk is almost full"),
        QStringLiteral("INFO:app.web:started"),
        QStringLiteral("  File \"app.py\", line 3, in <module>"),
        QStringLiteral("ERROR:app.db:connection refused"),
        QStringLiteral("free text without structure"),
    };
    const SchemaFit::Corpus pyCorpus = SchemaFit::corpus(pyLines);
    check(pyCorpus.fallbackRule && pyCorpus.candidateCount == 3,
          QStringLiteral("запасное правило: 3 кандидата — без отступа и со словом уровня (%1)")
              .arg(pyCorpus.candidateCount));
    // Слов уровня нет — кандидаты все строки без отступа.
    const SchemaFit::Corpus plainCorpus = SchemaFit::corpus(QStringList{
        QStringLiteral("{\"ts\":1700000000,\"msg\":\"a\"}"), QStringLiteral("{\"ts\":1700000001,\"msg\":\"b\"}"),
        QStringLiteral("  indented"), QStringLiteral("tail text")});
    check(plainCorpus.fallbackRule && plainCorpus.candidateCount == 3,
          QStringLiteral("запасное правило без уровней: 3 кандидата (%1)").arg(plainCorpus.candidateCount));
    const SchemaFit::Result p = SchemaFit::evaluate(pyPattern, pyCorpus);
    std::fprintf(stderr, "таблица/python: %s\n", qPrintable(describe(p)));
    // Строки со словом уровня — не меньше 20 %: кандидаты — только они.
    expectCounts(QStringLiteral("запасное правило"), p, {5, 3, 3, 0, 0, 0});

    // Лог с длинными трассами: записей с метками меньше 20 %, строк со словом
    // уровня — тоже; строки трасс без отступа — не кандидаты (остаются
    // кандидаты основного правила), продолжения — не промахи.
    const QStringList traces = {
        QStringLiteral("2026-03-05 10:00:00.000 [main] ERROR - fail"),
        QStringLiteral("java.lang.IllegalStateException: boom"),
        QStringLiteral("\tat a.B.c(B.java:1)"), QStringLiteral("\tat a.B.d(B.java:2)"),
        QStringLiteral("\tat a.B.e(B.java:3)"), QStringLiteral("Caused by: java.io.IOException: x"),
        QStringLiteral("\tat a.B.f(B.java:4)"), QStringLiteral("\t... 5 more"),
    };
    const SchemaFit::Corpus traceCorpus = SchemaFit::corpus(traces);
    check(!traceCorpus.fallbackRule && traceCorpus.candidateCount == 1,
          QStringLiteral("трассы: кандидат один — запись с меткой (%1)").arg(traceCorpus.candidateCount));
    const SchemaFit::Result t = SchemaFit::evaluate(schema, traceCorpus);
    expectCounts(QStringLiteral("трассы"), t, {8, 1, 1, 0, 0, 0});

    // Подпись процентов: десятые вниз, без ошибки double.
    check(SchemaFit::percent(0.29) == QStringLiteral("29") && SchemaFit::percent(0.57) == QStringLiteral("57")
              && SchemaFit::percent(0.9996) == QStringLiteral("99.9") && SchemaFit::percent(1.0) == QStringLiteral("100")
              && SchemaFit::percent(0.125) == QStringLiteral("12.5"),
          QStringLiteral("percent: 29 / 57 / 99.9 / 100 / 12.5 — %1 %2 %3")
              .arg(SchemaFit::percent(0.29), SchemaFit::percent(0.57), SchemaFit::percent(0.9996)));

    // Проход, дошедший до конца, годен, сколько бы ни шёл: бюджет только
    // останавливает (31 строка — ни одной проверки посреди прохода).
    {
        qint64 fake = 0;
        SchemaFit::Budget slowClock;
        slowClock.maxMs = 1;
        slowClock.clock = [&fake]() { return fake += 1000; };
        QStringList few;
        for (int i = 0; i < 31; ++i)
            few << lines[0];
        const SchemaFit::Result done = SchemaFit::evaluate(schema, few, slowClock);
        check(done.valid && !done.timedOut, QStringLiteral("досчитанный проход не помечается timedOut"));
    }

    // Невалидная схема — результат не годен.
    check(!SchemaFit::evaluate(LogPattern(), lines).valid, QStringLiteral("невалидная схема — !valid"));
    // Без строк — нет кандидатов, покрытие 0, вердикт «не подходит».
    const SchemaFit::Result empty = SchemaFit::evaluate(schema, QStringList());
    check(empty.valid && empty.candidates == 0 && SchemaFit::verdict(empty) == SchemaFit::Verdict::NoFit,
          QStringLiteral("пустой набор строк"));
}

// ---------------------------------------------------------------------------
// 2. better / rank
// ---------------------------------------------------------------------------

void testRank()
{
    const LogPattern schema(testdocs::buildSchema());
    const LogPattern message(messageOnlySchema());
    QStringList lines;
    for (int i = 0; i < 40; ++i)
        lines << QStringLiteral("2026-03-05 10:00:%1.000 [w%2] INFO - line %3")
                     .arg(i % 60, 2, 10, QLatin1Char('0')).arg(i % 3).arg(i);
    lines << QStringLiteral("odd line in the middle");
    const SchemaFit::Corpus corpus = SchemaFit::corpus(lines);

    // Message-only первым в каталоге, но не выигрывает.
    const QVector<SchemaFit::Ranked> ranked = SchemaFit::rank({message, schema}, corpus);
    check(ranked.size() == 2 && ranked[0].index == 1,
          QStringLiteral("rank: логическая схема раньше схемы из одного Message"));
    check(SchemaFit::better(ranked[0].result, ranked[1].result)
              && !SchemaFit::better(ranked[1].result, ranked[0].result),
          QStringLiteral("better несимметричен"));
    // Равные — порядок списка (устойчивость).
    const QVector<SchemaFit::Ranked> same = SchemaFit::rank({schema, schema}, corpus);
    check(same.size() == 2 && same[0].index == 0 && same[1].index == 1,
          QStringLiteral("rank устойчив на равных"));
    check(!SchemaFit::better(same[0].result, same[1].result), QStringLiteral("равные — не лучше"));

    // Округление покрытия до 0,02: при равном покрытии решает specificity.
    SchemaFit::Result a, b;
    a.valid = b.valid = true;
    a.candidates = b.candidates = 100;
    a.full = 95; b.full = 96;
    a.fieldsOnCandidates = 95 * 4; b.fieldsOnCandidates = 96 * 2;
    check(SchemaFit::better(a, b), QStringLiteral("покрытие 0,95 и 0,96 — одна ступень, specificity 4 > 2"));
    b.full = 99; b.fieldsOnCandidates = 99 * 2;
    check(SchemaFit::better(b, a), QStringLiteral("покрытие 0,99 лучше 0,95"));
    // Меньше partial — лучше при прочих равных.
    SchemaFit::Result c = a;
    c.full = 93; c.partial = 4; c.fieldsOnCandidates = 97 * 4;
    a.fieldsOnCandidates = c.fieldsOnCandidates * 95 / 97;
    SchemaFit::Result d = a;
    d.full = 95; d.partial = 0; d.fieldsOnCandidates = 95 * 4;
    SchemaFit::Result e = d;
    e.full = 94; e.partial = 2; e.fieldsOnCandidates = 96 * 4;
    check(SchemaFit::better(d, e), QStringLiteral("равное покрытие и specificity — меньше partial"));
    // Прерванная — в конце.
    SchemaFit::Result slow = b;
    slow.timedOut = true;
    slow.valid = false;
    check(SchemaFit::better(a, slow) && !SchemaFit::better(slow, a), QStringLiteral("timedOut — хуже"));
}

// ---------------------------------------------------------------------------
// 3. Корпус встроенных схем
// ---------------------------------------------------------------------------

QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// Известные пересечения: (схема, корпус) → вердикт, отличный от NoFit.
// Пересечение закреплено: изменилось — тест падает и таблицу правят руками.
const QMap<QPair<QString, QString>, SchemaFit::Verdict>& knownOverlaps()
{
    // Syslog RFC 3164 и Syslog ISO — одна и та же цепочка блоков (метку
    // любой формы читает блок Timestamp): подходят друг другу. Их Host /
    // Program / PID разбирают и строку nginx error («[error] 1234#1234:»),
    // но там выигрывает nginx error (rank — своя первая). JSON Lines: метка
    // внутри объекта — якорь, найденный где угодно, — три строки из пяти
    // «разобраны» целиком; не подходит, но «сомнительно».
    static const QMap<QPair<QString, QString>, SchemaFit::Verdict> overlaps = {
        {{QStringLiteral("syslog-iso.json"), QStringLiteral("syslog-rfc3164.json")}, SchemaFit::Verdict::Fits},
        {{QStringLiteral("syslog-rfc3164.json"), QStringLiteral("syslog-iso.json")}, SchemaFit::Verdict::Fits},
        {{QStringLiteral("syslog-rfc3164.json"), QStringLiteral("nginx-error.json")}, SchemaFit::Verdict::Fits},
        {{QStringLiteral("syslog-iso.json"), QStringLiteral("nginx-error.json")}, SchemaFit::Verdict::Fits},
        {{QStringLiteral("syslog-rfc3164.json"), QStringLiteral("json-lines.json")}, SchemaFit::Verdict::Doubtful},
        {{QStringLiteral("syslog-iso.json"), QStringLiteral("json-lines.json")}, SchemaFit::Verdict::Doubtful},
    };
    return overlaps;
}

const char* verdictName(SchemaFit::Verdict v)
{
    switch (v) {
    case SchemaFit::Verdict::Fits: return "Fits";
    case SchemaFit::Verdict::Doubtful: return "Doubtful";
    case SchemaFit::Verdict::NoFit: return "NoFit";
    }
    return "?";
}

void testBuiltinCorpus()
{
    const QString dir = QStringLiteral(DENDROLOG_PATTERNS_SOURCE_DIR);
    QVector<LogPattern> schemas;
    QStringList files;
    for (const builtinsamples::SchemaCase& sc : builtinsamples::cases()) {
        QString name, canonical;
        BuiltinSchemas::parseSchemaFile(readAll(QDir(dir).filePath(sc.file)), sc.file, &name, &canonical);
        schemas.append(LogPattern(canonical));
        files.append(sc.file);
        check(schemas.constLast().isValid(), QStringLiteral("встроенная схема %1 валидна").arg(sc.file));
    }
    // Схема из одного Message в начале каталога — не мешает никому.
    schemas.prepend(LogPattern(messageOnlySchema()));
    files.prepend(QStringLiteral("(message only)"));

    const QVector<builtinsamples::SchemaCase> all = builtinsamples::cases();
    for (int ci = 0; ci < all.size(); ++ci) {
        const builtinsamples::SchemaCase& sc = all[ci];
        QStringList lines;
        for (const builtinsamples::Sample& s : sc.samples)
            lines.append(s.line);
        const SchemaFit::Corpus corpus = SchemaFit::corpus(lines);
        const QVector<SchemaFit::Ranked> ranked = SchemaFit::rank(schemas, corpus);
        const int own = ci + 1;
        // Своя — первая; одинаковое определение (Syslog RFC 3164 и ISO —
        // одна цепочка блоков) делит место, порядок решает каталог.
        const auto ownAt = std::find_if(ranked.begin(), ranked.end(),
                                        [own](const SchemaFit::Ranked& r) { return r.index == own; });
        check(!ranked.isEmpty() && ownAt != ranked.end()
                  && (ranked[0].index == own || !SchemaFit::better(ranked[0].result, ownAt->result)),
              QStringLiteral("корпус %1: первой стала %2 (%3)")
                  .arg(sc.file, files.value(ranked.isEmpty() ? -1 : ranked[0].index),
                       ranked.isEmpty() ? QString() : describe(ranked[0].result)));
        for (const SchemaFit::Ranked& rk : ranked) {
            const SchemaFit::Verdict v = SchemaFit::verdict(rk.result);
            if (rk.index == own) {
                check(v == SchemaFit::Verdict::Fits,
                      QStringLiteral("корпус %1: своя схема — %2 (%3)")
                          .arg(sc.file, QLatin1String(verdictName(v)), describe(rk.result)));
                continue;
            }
            const SchemaFit::Verdict expected =
                knownOverlaps().value({files[rk.index], sc.file}, SchemaFit::Verdict::NoFit);
            if (v != expected)
                std::fprintf(stderr, "    {{QStringLiteral(\"%s\"), QStringLiteral(\"%s\")}, SchemaFit::Verdict::%s},\n",
                             qPrintable(files[rk.index]), qPrintable(sc.file), verdictName(v));
            check(v == expected,
                  QStringLiteral("корпус %1: чужая схема %2 — %3, ожидалось %4 (%5)")
                      .arg(sc.file, files[rk.index], QLatin1String(verdictName(v)),
                           QLatin1String(verdictName(expected)), describe(rk.result)));
        }
    }
}

// ---------------------------------------------------------------------------
// 4. explain
// ---------------------------------------------------------------------------

void testExplain()
{
    const LogPattern schema(testdocs::buildSchema());
    const PatternDefinition& def = schema.definition();
    const auto reasonOf = [&](const QString& line) {
        return SchemaFit::explain(def, line, schema.matchLine(line));
    };
    using E = SchemaFit::Reason::Expect;

    const SchemaFit::Reason ok = reasonOf(QStringLiteral("2026-03-05 10:00:00.000 [w1] INFO - hello"));
    check(ok.expect == E::Nothing, QStringLiteral("explain: принятая строка — Nothing"));

    // После метки ожидалась обёртка «[» поля Thread.
    const SchemaFit::Reason bracket = reasonOf(QStringLiteral("2026-03-05 10:00:03.000 restart marker"));
    check(bracket.afterBlock == 0 && bracket.expectedBlock == 1 && bracket.expect == E::Literal
              && bracket.literal == QStringLiteral("[") && bracket.stopOffset == 24
              && bracket.found == QStringLiteral("restart marker"),
          QStringLiteral("explain: после метки — «[», в строке «%1» (after %2, expect %3, literal %4)")
              .arg(bracket.found).arg(bracket.afterBlock).arg(int(bracket.expect)).arg(bracket.literal));

    // Разделитель-текст после поля: Logger до « - », в строке его нет.
    PatternDefinition sepDef;
    sepDef.blocks = {patternBlock(K::Timestamp, QStringLiteral("Timestamp")),
                     patternBlock(K::Level, QStringLiteral("Level")),
                     patternBlock(K::TextUntilSeparator, QStringLiteral("Logger"), QString(), QString(),
                                  QStringLiteral(" - ")),
                     patternBlock(K::Remainder, QStringLiteral("Message"))};
    const LogPattern sep(LogPattern::serializeDefinition(sepDef));
    const QString sepLine = QStringLiteral("2026-03-05 10:00:00.000 INFO component without dash");
    const LineMatchResult sepMatch = sep.matchLine(sepLine);
    const SchemaFit::Reason afterLevel = SchemaFit::explain(sep.definition(), sepLine, sepMatch);
    check(sepMatch.ok && afterLevel.afterBlock == 1 && afterLevel.expectedBlock == 2
              && afterLevel.expect == E::Literal && afterLevel.literal == QStringLiteral(" - ")
              && afterLevel.found == QStringLiteral("component without dash"),
          QStringLiteral("explain: после Level поле Logger до « - », разделителя в строке нет"));

    // Ни один блок не сошёлся — ожидалась метка с начала строки.
    const SchemaFit::Reason none = reasonOf(QStringLiteral("restart marker"));
    check(none.afterBlock == -1 && none.expectedBlock == 0 && none.expect == E::Timestamp
              && none.stopOffset == 0 && none.found == QStringLiteral("restart marker"),
          QStringLiteral("explain: строка без метки — ожидалась метка (after %1, expect %2)")
              .arg(none.afterBlock).arg(int(none.expect)));

    // Ключевая схема: строка не запись.
    PatternDefinition keyedDef;
    keyedDef.keyed = KeyedDefinition();
    keyedDef.blocks = {patternBlock(K::Remainder, QStringLiteral("Message"))};
    const LogPattern keyed(LogPattern::serializeDefinition(keyedDef));
    check(keyed.isKeyed(), QStringLiteral("ключевая схема по умолчанию валидна"));
    const QString plain = QStringLiteral("plain text line");
    const SchemaFit::Reason rec = SchemaFit::explain(keyed.definition(), plain, keyed.matchLine(plain));
    check(rec.expect == E::Record && rec.found == plain, QStringLiteral("explain: ключевая — Record"));
}

// ---------------------------------------------------------------------------
// 5. Бюджет
// ---------------------------------------------------------------------------

void testBudget()
{
    const LogPattern schema(testdocs::buildSchema());
    QStringList lines;
    for (int i = 0; i < 1000; ++i)
        lines << QStringLiteral("2026-03-05 10:00:00.000 [w1] INFO - line %1").arg(i);

    // Шов часов: каждое обращение — плюс 1 мс. Проверка каждые 32 строки:
    // 6-я проверка (строка 192) видит 6 мс > 5 мс.
    {
        qint64 fake = 0;
        SchemaFit::Budget budget;
        budget.maxMs = 5;
        budget.clock = [&fake]() { return ++fake; };
        const SchemaFit::Result r = SchemaFit::evaluate(schema, lines, budget);
        check(r.timedOut && !r.valid && r.examined == 192,
              QStringLiteral("бюджет по шву часов: остановка на строке 192 (%1)").arg(describe(r)));
        check(SchemaFit::verdict(r) == SchemaFit::Verdict::NoFit, QStringLiteral("timedOut — NoFit"));
    }
    // Без предела — до конца.
    {
        qint64 fake = 0;
        SchemaFit::Budget budget;
        budget.maxMs = 0;
        budget.clock = [&fake]() { return fake += 1000; };
        const SchemaFit::Result r = SchemaFit::evaluate(schema, lines, budget);
        check(r.valid && !r.timedOut && r.examined == 1000, QStringLiteral("maxMs 0 — без предела"));
    }
    // Флаг отмены.
    {
        std::atomic_bool cancel{true};
        SchemaFit::Budget budget;
        budget.cancel = &cancel;
        const SchemaFit::Result r = SchemaFit::evaluate(schema, lines, budget);
        check(r.cancelled && !r.valid && r.examined == 32, QStringLiteral("отмена — на первой проверке"));
    }
    // Настоящий катастрофический регекс: (\w|\w\w)+\d на строках из букв —
    // каждая строка упирается в предел PCRE2. Время — настоящее, но решает
    // счётчик: проход обязан остановиться задолго до конца набора.
    {
        PatternDefinition def;
        PatternBlock re = patternBlock(K::CustomRegex, QStringLiteral("Bad"));
        re.customRegex = QStringLiteral("(\\w|\\w\\w)+\\d");
        def.blocks = {re, patternBlock(K::Remainder, QStringLiteral("Message"))};
        const LogPattern bad(LogPattern::serializeDefinition(def));
        check(bad.isValid(), QStringLiteral("катастрофическая схема компилируется"));
        QStringList slow;
        for (int i = 0; i < 4000; ++i)
            slow << QString(40, QLatin1Char('a')) + QStringLiteral("!");
        SchemaFit::Budget budget;
        budget.maxMs = 50;
        const SchemaFit::Result r = SchemaFit::evaluate(bad, slow, budget);
        check(r.timedOut && r.examined < slow.size(),
              QStringLiteral("катастрофический регекс — timedOut до конца набора (%1)").arg(describe(r)));
        // rank ставит такую схему в конец.
        const QVector<SchemaFit::Ranked> ranked =
            SchemaFit::rank({bad, LogPattern(messageOnlySchema())}, SchemaFit::corpus(slow.mid(0, 200)), budget);
        check(ranked.size() == 2 && ranked[0].index == 1 && ranked[1].result.timedOut,
              QStringLiteral("rank: схема с timedOut — в конце"));
    }
}

// ---------------------------------------------------------------------------
// 6. SchemaFitJob
// ---------------------------------------------------------------------------

// Счётчик сигнала (Qt Test тест не линкует).
struct Spy {
    int n = 0;
    void hit() { ++n; }
    int count() const { return n; }
    bool isEmpty() const { return n == 0; }
    void clear() { n = 0; }
};

bool sameResult(const SchemaFit::Result& a, const SchemaFit::Result& b)
{
    if (a.examined != b.examined || a.candidates != b.candidates || a.full != b.full
            || a.partial != b.partial || a.missed != b.missed || a.loose != b.loose
            || a.filled != b.filled || a.valid != b.valid || a.misses.size() != b.misses.size()
            || a.fieldsOnCandidates != b.fieldsOnCandidates)
        return false;
    for (int i = 0; i < a.misses.size(); ++i) {
        if (a.misses[i].line != b.misses[i].line || a.misses[i].text != b.misses[i].text)
            return false;
    }
    return true;
}

void testJob(const QDir& tmp)
{
    // Лог с многострочными записями; метки монотонны и преамбулы без меток
    // нет — порядок строк обоих бэкендов одинаков (одно-файловая индексная
    // вкладка не сортирует).
    const QByteArray bytes = testdocs::makeLogBytes(6000, 17, "\n", testdocs::fixtureBaseTime(),
                                                    /*withPreamble=*/false, /*trailingNewline=*/true,
                                                    /*monotonic=*/true);
    const QString path = testdocs::writeFile(tmp, QStringLiteral("job.log"), bytes);
    const QStringList physical = testdocs::expectedPhysicalLines(bytes);
    const QVector<LogPattern> schemas = {LogPattern(testdocs::buildSchema()),
                                         LogPattern(messageOnlySchema())};

    QVector<QVector<SchemaFit::Sample>> sampleSets;
    for (int backend = 0; backend < 2; ++backend) {
        const QString label = backend == 0 ? QStringLiteral("резидентный") : QStringLiteral("индексный");
        testdocs::Document doc = backend == 0 ? testdocs::buildResident({path}, QString())
                                              : testdocs::buildIndexed({path}, QString());
        check(doc.errors.isEmpty(), label + QStringLiteral(": загрузка ") + doc.errors.join(QLatin1Char(';')));

        // Синхронный эталон: те же строки, та же функция.
        const QVector<SchemaFit::Sample> samples =
            SchemaFitJob::sampleLines(doc.model->scanSnapshot(false), nullptr);
        sampleSets.append(samples);
        check(samples.size() > SchemaFitJob::kHeadLines && samples.size() <= SchemaFitJob::kHeadLines
                                                                            + SchemaFitJob::kSpreadLines,
              label + QStringLiteral(": строк в наборе %1").arg(samples.size()));
        bool linesOk = true;
        qint64 lastLine = 0;
        for (const SchemaFit::Sample& s : samples) {
            if (s.line <= lastLine || s.line > physical.size() || physical[s.line - 1] != QStringLiteral("%1: %2").arg(s.line).arg(s.text)) {
                if (linesOk)
                    std::fprintf(stderr, "  строка набора %lld (после %lld): «%s» / файл «%s»\n",
                                 s.line, lastLine, qPrintable(s.text),
                                 qPrintable(physical.value(s.line - 1)));
                linesOk = false;
            }
            lastLine = s.line;
        }
        check(linesOk, label + QStringLiteral(": строки набора — строки файла с их номерами, по порядку"));
        check(!samples.isEmpty() && samples.first().line == 1
                  && samples.constLast().line > physical.size() * 9 / 10,
              label + QStringLiteral(": набор — с начала файла и до его конца"));

        // Сравнение без предела времени: под санитайзером и ctest -j бюджет
        // мог бы остановить один из двух проходов (ждать состояние, а не время).
        SchemaFit::Budget noLimit;
        noLimit.maxMs = 0;
        const SchemaFit::Corpus corpus = SchemaFit::corpus(samples);
        QVector<SchemaFit::Result> expected;
        for (const LogPattern& p : schemas)
            expected.append(SchemaFit::evaluate(p, corpus, noLimit));

        SchemaFitJob job;
        job.setModel(doc.model.get());
        Spy finished;
        QObject::connect(&job, &SchemaFitJob::finished, [&finished]() { finished.hit(); });
        job.start(schemas, nullptr, noLimit);
        check(testdocs::waitFor([&]() { return finished.count() > 0; }),
              label + QStringLiteral(": задание закончилось"));
        const SchemaFitJob::Outcome& out = job.outcome();
        check(job.hasOutcome() && out.sampled == samples.size() && out.results.size() == 2,
              label + QStringLiteral(": итог задания"));
        for (int i = 0; i < out.results.size() && i < expected.size(); ++i)
            check(sameResult(out.results[i], expected[i]),
                  label + QStringLiteral(": схема %1 — задание = синхронный расчёт: %2 / %3")
                              .arg(i).arg(describe(out.results[i]), describe(expected[i])));
        check(SchemaFit::verdict(out.results.value(0)) == SchemaFit::Verdict::Fits,
              label + QStringLiteral(": схема фикстуры подходит её файлу (%1)").arg(describe(out.results.value(0))));

        // Отмена: итога нет, finished не приходит.
        finished.clear();
        job.start(schemas);
        job.cancel();
        check(testdocs::waitFor([&]() { return !job.isRunning(); }), label + QStringLiteral(": отмена"));
        QCoreApplication::processEvents();
        check(finished.isEmpty(), label + QStringLiteral(": отменённое задание не доставлено"));

        // Reset модели посреди расчёта — invalidated, итог отброшен.
        Spy invalidated;
        QObject::connect(&job, &SchemaFitJob::invalidated, [&invalidated]() { invalidated.hit(); });
        finished.clear();
        job.start(schemas);
        doc.model->clear();
        check(invalidated.count() == 1 && !job.hasOutcome(),
              label + QStringLiteral(": reset — invalidated, итога нет"));
        check(testdocs::waitFor([&]() { return !job.isRunning(); }), label + QStringLiteral(": reset — расчёт остановлен"));
        QCoreApplication::processEvents();
        check(finished.isEmpty(), label + QStringLiteral(": расчёт до reset не доставлен"));
        // Заново — по новым (пустым) строкам.
        job.start(schemas);
        check(testdocs::waitFor([&]() { return finished.count() > 0; }) && job.outcome().sampled == 0,
              label + QStringLiteral(": после reset — заново, строк нет"));
    }
    if (sampleSets.size() == 2) {
        bool same = sampleSets[0].size() == sampleSets[1].size();
        for (int i = 0; same && i < sampleSets[0].size(); ++i)
            same = sampleSets[0][i].line == sampleSets[1][i].line && sampleSets[0][i].text == sampleSets[1][i].text;
        check(same, QStringLiteral("набор строк одинаков на обоих бэкендах"));
    }
}

} // namespace

void testDialog(const QDir& tmp);   // ниже: окно редактора схем

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTemporaryDir tmpDir;

    testTable();
    testRank();
    testBuiltinCorpus();
    testExplain();
    testBudget();
    testJob(QDir(tmpDir.path()));
    testDialog(QDir(tmpDir.path()));

    std::fprintf(stderr, "schema_fit: %d проверок, %d провалов\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// 7. Одна мера: окно редактора схем
// ---------------------------------------------------------------------------

void testDialog(const QDir& tmp)
{
    const QString dir = QStringLiteral(DENDROLOG_PATTERNS_SOURCE_DIR);
    ConversionPatternDialog::PatternList list;
    int springRow = -1;
    QStringList springLines;
    QString springSchema;
    for (const builtinsamples::SchemaCase& sc : builtinsamples::cases()) {
        SchemaStore::SchemaEntry entry;
        BuiltinSchemas::parseSchemaFile(readAll(QDir(dir).filePath(sc.file)), sc.file, &entry.name,
                                        &entry.schema);
        if (sc.file == QLatin1String("spring-boot.json")) {
            springRow = int(list.size());
            springSchema = entry.schema;
            for (const builtinsamples::Sample& smp : sc.samples)
                springLines << smp.line;
        }
        list.append(entry);
    }
    SchemaStore::SchemaEntry fixture;
    fixture.name = QStringLiteral("Fixture");
    fixture.schema = LogPattern(testdocs::buildSchema()).patternString();
    const int fixtureRow = int(list.size());
    list.append(fixture);

    ConversionPatternDialog dlg(list);
    SchemaFit::Budget noLimit;
    noLimit.maxMs = 0;
    dlg.setFitBudget(noLimit);
    auto* table = dlg.findChild<QTableWidget*>(QStringLiteral("schemaTable"));
    auto* sample = dlg.findChild<QPlainTextEdit*>(QStringLiteral("sampleEdit"));
    auto* status = dlg.findChild<QLabel*>(QStringLiteral("previewStatusLabel"));
    auto* fileLabel = dlg.findChild<QLabel*>(QStringLiteral("fileCoverageLabel"));
    auto* menuBtn = dlg.findChild<QToolButton*>(QStringLiteral("suggestMenuBtn"));
    check(table && sample && status && fileLabel && menuBtn && menuBtn->menu(),
          QStringLiteral("окно: элементы на месте"));
    if (!table || !sample || !status || !fileLabel || !menuBtn || !menuBtn->menu())
        return;
    check(fileLabel->isHidden(), QStringLiteral("окно без вкладки: подписи файла нет"));

    // Образцы: подпись — та же мера на тех же строках.
    table->setCurrentCell(springRow, 0);
    sample->setPlainText(springLines.join(QLatin1Char('\n')));
    const SchemaFit::Result expectedSamples = SchemaFit::evaluate(LogPattern(springSchema), springLines);
    check(testdocs::waitFor([&]() { return sameResult(dlg.sampleFit(), expectedSamples); }, 10000),
          QStringLiteral("окно: счётчики образцов = SchemaFit::evaluate (%1 / %2)")
              .arg(describe(dlg.sampleFit()), describe(expectedSamples)));
    check(status->text().startsWith(SchemaFit::summary(expectedSamples)),
          QStringLiteral("окно: подпись образцов — «%1»").arg(status->text()));

    // Подбор из каталога по образцам: Spring Boot — первым; пункт меню
    // выбирает его в списке.
    // Ранжирование по образцам — в воркере: до итога меню говорит «Checking».
    QVector<SchemaFit::Ranked> bySamples;
    check(testdocs::waitFor([&]() {
              bool pending = true;
              bySamples = dlg.catalogueSuggestions(&pending);
              return !pending;
          }, 10000),
          QStringLiteral("окно: ранжирование каталога по образцам закончилось"));
    check(!bySamples.isEmpty() && bySamples[0].index == springRow,
          QStringLiteral("окно: по образцам первая — Spring Boot"));
    table->setCurrentCell(0, 0);
    emit menuBtn->menu()->aboutToShow();
    QAction* first = menuBtn->menu()->findChild<QAction*>(QStringLiteral("suggestCatalogue0"));
    check(first && first->text().startsWith(list[springRow].name),
          QStringLiteral("окно: первый пункт меню — %1").arg(first ? first->text() : QString()));
    if (first)
        first->trigger();
    check(table->currentRow() == springRow, QStringLiteral("окно: пункт меню выбрал схему в списке"));
    check(menuBtn->menu()->findChild<QAction*>(QStringLiteral("suggestDraft")) != nullptr,
          QStringLiteral("окно: в меню есть черновик по образцам"));

    // Вкладка: образцы — с текущей строки, покрытие — на строках файла.
    const QByteArray bytes = testdocs::makeLogBytes(3000, 5, "\n", testdocs::fixtureBaseTime(),
                                                    false, true, true);
    const QString path = testdocs::writeFile(tmp, QStringLiteral("dialog.log"), bytes);
    for (int backend = 0; backend < 2; ++backend) {
        const QString label = backend == 0 ? QStringLiteral("окно, резидентный") : QStringLiteral("окно, индексный");
        testdocs::Document doc = backend == 0 ? testdocs::buildResident({path}, QString())
                                              : testdocs::buildIndexed({path}, QString());
        table->setCurrentCell(fixtureRow, 0);
        sample->clear();
        dlg.setSource(doc.model.get(), 10);
        const QStringList shown = sample->toPlainText().split(QLatin1Char('\n'));
        check(!shown.isEmpty() && shown.first() == doc.model->messageAt(10),
              label + QStringLiteral(": образцы — с текущей строки вкладки"));
        check(!fileLabel->isHidden(), label + QStringLiteral(": подпись файла видна"));
        check(testdocs::waitFor([&]() { return dlg.fileFitReady(); }, 30000),
              label + QStringLiteral(": покрытие на файле посчитано"));
        const QVector<SchemaFit::Sample> lines =
            SchemaFitJob::sampleLines(doc.model->scanSnapshot(false), nullptr);
        const SchemaFit::Result expected = SchemaFit::evaluate(LogPattern(fixture.schema), lines, noLimit);
        check(sameResult(dlg.fileFit(), expected),
              label + QStringLiteral(": счётчики файла = SchemaFit::evaluate (%1 / %2)")
                          .arg(describe(dlg.fileFit()), describe(expected)));
        check(fileLabel->text().contains(SchemaFit::summary(expected)),
              label + QStringLiteral(": подпись файла — «%1»").arg(fileLabel->text()));
        const QVector<SchemaFit::Ranked> byFile = dlg.catalogueSuggestions();
        check(!byFile.isEmpty() && byFile[0].index == fixtureRow,
              label + QStringLiteral(": по файлу первая — схема фикстуры"));

        // Reset вкладки — проверка заново, по новым строкам.
        doc.model->clear();
        check(!dlg.fileFitReady(), label + QStringLiteral(": reset — прежний итог снят"));
        check(testdocs::waitFor([&]() { return dlg.fileFitReady(); }, 30000)
                  && dlg.fileFit().candidates == 0,
              label + QStringLiteral(": после reset — заново"));
        dlg.setSource(nullptr, -1);
        check(fileLabel->isHidden(), label + QStringLiteral(": без вкладки подпись скрыта"));
    }
}
