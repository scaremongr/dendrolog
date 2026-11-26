// Шаблон строки и интерфейс извлекателя (F-17, шаг 2; docs/design/F-17.md,
// раздел 2):
//
//   1. Таблица «шаблон × строка → поля» по каждому виду поля и правилам 1–6.
//   2. Ошибки компиляции: код, позиция, имя поля.
//   3. Встроенные шаблоны (resources/templates): имена колонок = именам
//      блочных схем, поля на образцах tests/common/builtinsamples.h.
//   4. Конвертер блочной схемы: каждая встроенная ≡ своему шаблону на корпусе.
//   5. Ключевой и прежний извлекатели = LogPattern.
//   6. SchemaFit на интерфейсе: шаблоны «подходят» своим образцам; число
//      разных значений по колонке.
//   7. Фаззинг: шаблон → порождённые строки → поля восстановлены.
//   8. Случайные строки: без падений и за линейное число шагов (счётчик).
//   9. Без аллокаций на строку (glibc, без санитайзера).
//
// Режим замера: line_template --bench [строк] — шаблон против регексной
// схемы на корпусах встроенных форматов (бюджет F-17 — не дороже ×0,35).
//
// Каталоги схем в исходниках — define DENDROLOG_PATTERNS_SOURCE_DIR и
// DENDROLOG_TEMPLATES_SOURCE_DIR.

#include "builtinsamples.h"
#include "fieldextractor.h"
#include "linetemplate.h"
#include "logpattern.h"
#include "schemaconverter.h"
#include "schemafit.h"
#include "builtinschemas.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMap>
#include <QRandomGenerator>

#include <atomic>
#include <cstdio>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Счётчик аллокаций: glibc, без санитайзера (у него свой malloc).
// ---------------------------------------------------------------------------
#if defined(__GLIBC__) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
#define DENDRO_COUNT_ALLOCATIONS 1
namespace {
std::atomic<bool> g_countAllocations{false};
std::atomic<long long> g_allocations{0};
}
extern "C" void* __libc_malloc(size_t);
extern "C" void* __libc_calloc(size_t, size_t);
extern "C" void* __libc_realloc(void*, size_t);
extern "C" void* malloc(size_t size)
{
    if (g_countAllocations.load(std::memory_order_relaxed))
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    return __libc_malloc(size);
}
extern "C" void* calloc(size_t n, size_t size)
{
    if (g_countAllocations.load(std::memory_order_relaxed))
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    return __libc_calloc(n, size);
}
extern "C" void* realloc(void* p, size_t size)
{
    if (g_countAllocations.load(std::memory_order_relaxed))
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    return __libc_realloc(p, size);
}
#endif

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

using Outcome = FieldExtractor::Outcome;
using Fields = QMap<QString, QString>;
using namespace builtinsamples;

const QString kPatternsDir = QStringLiteral(DENDROLOG_PATTERNS_SOURCE_DIR);
const QString kTemplatesDir = QStringLiteral(DENDROLOG_TEMPLATES_SOURCE_DIR);

QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QString outcomeName(Outcome o)
{
    switch (o) {
    case Outcome::Rejected: return QStringLiteral("Rejected");
    case Outcome::Partial: return QStringLiteral("Partial");
    case Outcome::Full: return QStringLiteral("Full");
    }
    return QString();
}

// Непустые поля строки по именам колонок: раскодированные (value) или сырые.
Fields fieldsOf(const QStringList& columns, const QString& line, const LogEntryFields& f, bool raw = false)
{
    Fields m;
    for (int i = 0; i < columns.size(); ++i) {
        const QString v = raw ? f.get(i, line).toString() : f.value(i, line);
        if (f.has(i) && !v.isEmpty())
            m.insert(columns[i], v);
    }
    return m;
}

QString describe(const Fields& m)
{
    QStringList parts;
    for (auto it = m.cbegin(); it != m.cend(); ++it)
        parts << QStringLiteral("%1=«%2»").arg(it.key(), it.value());
    return parts.join(QStringLiteral(", "));
}

// ---------------------------------------------------------------------------
// 1. Таблица «шаблон × строка → поля»
// ---------------------------------------------------------------------------

struct Row {
    const char* what;
    QString tpl;
    QString line;
    Outcome outcome;
    Fields fields;
};

void testTable()
{
    const QString logback = QStringLiteral("{Timestamp:time} [{Thread}] {Level:level} {Logger:word} -{?-?} {Message}");
    const QString spring = QStringLiteral(
        "{Timestamp:time} {Level:level} {PID:int} --- {?[{_}] ?}[{Thread}] {Logger:word} : {Message}");
    const QString component = QStringLiteral(
        "{Ts:time} {Level:level} {Component:of(Order|Order Service|Payment Gateway|DB)} {Message}");

    const QVector<Row> rows = {
        // --- time: все формы классификатора, с дробью и зоной
        { "time ISO с дробью и зоной через пробел", QStringLiteral("{T:time} {M}"),
          QStringLiteral("2026-11-18 13:55:36.123 +01:00 hello"), Outcome::Full,
          { { "T", "2026-11-18 13:55:36.123 +01:00" }, { "M", "hello" } } },
        { "time ISO с T и Z", QStringLiteral("{T:time} {M}"),
          QStringLiteral("2026-11-18T13:55:36.130Z  INFO x"), Outcome::Full,
          { { "T", "2026-11-18T13:55:36.130Z" }, { "M", "INFO x" } } },
        { "time nginx dddd/dd/dd", QStringLiteral("{T:time} {M}"),
          QStringLiteral("2026/11/18 13:55:36 [error] x"), Outcome::Full,
          { { "T", "2026/11/18 13:55:36" }, { "M", "[error] x" } } },
        { "time syslog с днём « 8»", QStringLiteral("{T:time} {M}"),
          QStringLiteral("Nov  8 03:04:05 web01 kernel"), Outcome::Full,
          { { "T", "Nov  8 03:04:05" }, { "M", "web01 kernel" } } },
        { "time access с зоной", QStringLiteral("[{T:time}] {M}"),
          QStringLiteral("[18/Nov/2026:13:55:36 +0000] GET"), Outcome::Full,
          { { "T", "18/Nov/2026:13:55:36 +0000" }, { "M", "GET" } } },
        { "time без даты", QStringLiteral("{T:time} {M}"),
          QStringLiteral("13:55:40.016 [main] x"), Outcome::Full,
          { { "T", "13:55:40.016" }, { "M", "[main] x" } } },
        { "time dd.MM.yyyy", QStringLiteral("{T:time} {M}"),
          QStringLiteral("31.12.2025 23:59:59,5 x"), Outcome::Full,
          { { "T", "31.12.2025 23:59:59,5" }, { "M", "x" } } },
        { "time: не метка — строка не принята (правило 6)", QStringLiteral("{T:time} {M}"),
          QStringLiteral("hello 2026-11-18 13:55:36"), Outcome::Rejected, {} },
        { "правило 6: отступ перед первым элементом", QStringLiteral("{T:time} {M}"),
          QStringLiteral("  2026-11-18 13:55:36 x"), Outcome::Rejected, {} },
        // --- level
        { "level — слово словаря, регистр не важен", QStringLiteral("{L:level} {M}"),
          QStringLiteral("warning disk low"), Outcome::Full, { { "L", "warning" }, { "M", "disk low" } } },
        { "level: не целое слово", QStringLiteral("{L:level} {M}"),
          QStringLiteral("INFOX disk"), Outcome::Rejected, {} },
        // --- числа
        { "int со знаком", QStringLiteral("{N:int} {M}"), QStringLiteral("-42 x"), Outcome::Full,
          { { "N", "-42" }, { "M", "x" } } },
        { "int: за числом буква — не число", QStringLiteral("{N:int} {M}"), QStringLiteral("42abc x"),
          Outcome::Rejected, {} },
        { "num с дробью и порядком", QStringLiteral("{N:num} {M}"), QStringLiteral("3.14e-2 x"), Outcome::Full,
          { { "N", "3.14e-2" }, { "M", "x" } } },
        { "hex с 0x", QStringLiteral("{H:hex} {M}"), QStringLiteral("0xDEADbeef x"), Outcome::Full,
          { { "H", "0xDEADbeef" }, { "M", "x" } } },
        { "hex без префикса", QStringLiteral("{H:hex} {M}"), QStringLiteral("deadbeef x"), Outcome::Full,
          { { "H", "deadbeef" }, { "M", "x" } } },
        { "uuid", QStringLiteral("{U:uuid} {M}"), QStringLiteral("123e4567-e89b-12d3-a456-426614174000 x"),
          Outcome::Full, { { "U", "123e4567-e89b-12d3-a456-426614174000" }, { "M", "x" } } },
        { "uuid: короткая группа", QStringLiteral("{U:uuid} {M}"),
          QStringLiteral("123e4567-e89b-12d3-a456-42661417400 x"), Outcome::Rejected, {} },
        // --- ip
        { "ip IPv4", QStringLiteral("{C:ip} {M}"), QStringLiteral("192.168.1.10 x"), Outcome::Full,
          { { "C", "192.168.1.10" }, { "M", "x" } } },
        { "ip IPv6 с ::", QStringLiteral("{C:ip} {M}"), QStringLiteral("2001:db8::1 x"), Outcome::Full,
          { { "C", "2001:db8::1" }, { "M", "x" } } },
        { "ip ::1", QStringLiteral("{C:ip} {M}"), QStringLiteral("::1 x"), Outcome::Full,
          { { "C", "::1" }, { "M", "x" } } },
        { "ip IPv6 с IPv4 в конце", QStringLiteral("{C:ip} {M}"), QStringLiteral("::ffff:10.0.0.1 x"), Outcome::Full,
          { { "C", "::ffff:10.0.0.1" }, { "M", "x" } } },
        { "ip перед двоеточием", QStringLiteral("{C:ip}: {M}"), QStringLiteral("fe80::1: x"), Outcome::Full,
          { { "C", "fe80::1" }, { "M", "x" } } },
        { "ip: время — не адрес", QStringLiteral("{C:ip} {M}"), QStringLiteral("13:55:36 x"), Outcome::Rejected, {} },
        { "ip: пять групп IPv4", QStringLiteral("{C:ip} {M}"), QStringLiteral("1.2.3.4.5 x"), Outcome::Rejected, {} },
        // --- path
        { "path Unix", QStringLiteral("{P:path} {M}"), QStringLiteral("/var/log/app.log x"), Outcome::Full,
          { { "P", "/var/log/app.log" }, { "M", "x" } } },
        { "path Windows с диском", QStringLiteral("{P:path} {M}"), QStringLiteral("C:\\logs\\a.txt x"), Outcome::Full,
          { { "P", "C:\\logs\\a.txt" }, { "M", "x" } } },
        // --- quoted
        { "quoted: значение без кавычек, экранирование раскодировано", QStringLiteral("{Q:quoted} {M}"),
          QStringLiteral("\"a \\\"b\\\" c\" tail"), Outcome::Full, { { "Q", "a \"b\" c" }, { "M", "tail" } } },
        { "quoted: незакрытая кавычка", QStringLiteral("{Q:quoted} {M}"), QStringLiteral("\"open x"),
          Outcome::Rejected, {} },
        { "quoted пустая", QStringLiteral("{Q:quoted} {N:int}"), QStringLiteral("\"\" 5"), Outcome::Full,
          { { "N", "5" } } },
        { "quoted: путь Windows — «\\t», «\\n» не управляющие (не JSON)", QStringLiteral("{L:level} {P:quoted} {M}"),
          QStringLiteral("ERROR \"C:\\temp\\new\\x.log \\\"a\\\"\" not found"), Outcome::Full,
          { { "L", "ERROR" }, { "P", "C:\\temp\\new\\x.log \"a\"" }, { "M", "not found" } } },
        { "пустое совпадение регекса — не отличительный элемент", QStringLiteral("{A:word}{_:/x?/} [{B}] {M}"),
          QStringLiteral("hello world"), Outcome::Rejected, {} },
        // --- of: многословные значения, берётся самое длинное
        { "of: самое длинное из совпавших", component,
          QStringLiteral("2026-11-18 13:55:36 INFO Order Service Processing order 42 for user 7"), Outcome::Full,
          { { "Ts", "2026-11-18 13:55:36" }, { "Level", "INFO" }, { "Component", "Order Service" },
            { "Message", "Processing order 42 for user 7" } } },
        { "of: многословное значение", component,
          QStringLiteral("2026-11-18 13:55:37 WARN Payment Gateway Retry 2 of 5"), Outcome::Full,
          { { "Ts", "2026-11-18 13:55:37" }, { "Level", "WARN" }, { "Component", "Payment Gateway" },
            { "Message", "Retry 2 of 5" } } },
        { "of: короткое", component, QStringLiteral("2026-11-18 13:55:38 INFO DB Connection pool resized to 20"),
          Outcome::Full,
          { { "Ts", "2026-11-18 13:55:38" }, { "Level", "INFO" }, { "Component", "DB" },
            { "Message", "Connection pool resized to 20" } } },
        { "of: значения нет в списке — остановка, остаток в Message (правило 5)", component,
          QStringLiteral("2026-11-18 13:55:39 INFO Billing charged"), Outcome::Partial,
          { { "Ts", "2026-11-18 13:55:39" }, { "Level", "INFO" }, { "Message", "Billing charged" } } },
        { "of: значение не обрывает слово строки", QStringLiteral("{C:of(DB|DBX)} {M}"),
          QStringLiteral("DBXY x"), Outcome::Rejected, {} },
        { "of: «|» и «)» в значении", QStringLiteral("{C:of(a\\|b|c\\))} {M}"), QStringLiteral("c) x"),
          Outcome::Full, { { "C", "c)" }, { "M", "x" } } },
        // --- word
        { "word до пробела", QStringLiteral("{A:word} {B:word} {M}"), QStringLiteral("web01 sshd started now"),
          Outcome::Full, { { "A", "web01" }, { "B", "sshd" }, { "M", "started now" } } },
        { "word до литерала следом", QStringLiteral("{P:word}[{N:int}]: {M}"), QStringLiteral("sshd[22]: x"),
          Outcome::Full, { { "P", "sshd" }, { "N", "22" }, { "M", "x" } } },
        { "word до литерала за необязательным участком", QStringLiteral("{P:word}{?[{N:int}]?}: {M}"),
          QStringLiteral("kernel: x y"), Outcome::Full, { { "P", "kernel" }, { "M", "x y" } } },
        // --- text: до литерала, правило 2 (без пробелов вокруг)
        { "text до литерала, значение без пробелов", QStringLiteral("[{Thread}] {M}"),
          QStringLiteral("[     main] hi"), Outcome::Full, { { "Thread", "main" }, { "M", "hi" } } },
        { "литерал с пробелами вокруг — целиком (правило 1)", QStringLiteral("{Logger} - {M}"),
          QStringLiteral("my-app.db - started - ok"), Outcome::Full,
          { { "Logger", "my-app.db" }, { "M", "started - ok" } } },
        { "greedy — последнее вхождение", QStringLiteral("{A:greedy} - {B}"), QStringLiteral("a - b - c"),
          Outcome::Full, { { "A", "a - b" }, { "B", "c" } } },
        { "text — первое вхождение", QStringLiteral("{A} - {B}"), QStringLiteral("a - b - c"), Outcome::Full,
          { { "A", "a" }, { "B", "b - c" } } },
        { "text до поля с формой (правило 3)", QStringLiteral("{Ts:time} {Component} {Level:level} {Message}"),
          QStringLiteral("2026-11-18 13:55:36 Order Service INFO Processing order 42"), Outcome::Full,
          { { "Ts", "2026-11-18 13:55:36" }, { "Component", "Order Service" }, { "Level", "INFO" },
            { "Message", "Processing order 42" } } },
        { "text до поля с формой: первое место, где поле сходится",
          QStringLiteral("{A} {N:int} {M}"), QStringLiteral("retry 2 of 5 done"), Outcome::Full,
          { { "A", "retry" }, { "N", "2" }, { "M", "of 5 done" } } },
        { "text до поля с формой вплотную — на границе слова", QStringLiteral("{A}{N:int} {M}"),
          QStringLiteral("abc12x 34 tail"), Outcome::Full, { { "A", "abc12x" }, { "N", "34" }, { "M", "tail" } } },
        { "text: литерала нет — остановка", QStringLiteral("{L:level} {Logger} - {M}"),
          QStringLiteral("INFO no separator here"), Outcome::Partial, { { "L", "INFO" }, { "M", "no separator here" } } },
        // --- padded, width, words
        { "padded — до промежутка из двух пробелов", QStringLiteral("{Ts:time} {Component:padded} {Message}"),
          QStringLiteral("2026-11-18 13:55:36 Order Service   Processing order"), Outcome::Full,
          { { "Ts", "2026-11-18 13:55:36" }, { "Component", "Order Service" }, { "Message", "Processing order" } } },
        { "padded — до табуляции", QStringLiteral("{A:padded} {M}"), QStringLiteral("two words\tthe rest"),
          Outcome::Full, { { "A", "two words" }, { "M", "the rest" } } },
        { "width(N)", QStringLiteral("{Logger:width(10)} {M}"), QStringLiteral("a.b.C      started"), Outcome::Full,
          { { "Logger", "a.b.C" }, { "M", "started" } } },
        { "width(N): строка короче", QStringLiteral("{L:level} {Logger:width(10)} {M}"), QStringLiteral("INFO a.b"),
          Outcome::Partial, { { "L", "INFO" }, { "M", "a.b" } } },
        { "words(N)", QStringLiteral("{Ts:time} {Level:level} {Component:words(2)} {Message}"),
          QStringLiteral("2026-11-18 13:55:37 WARN Payment Gateway Retry 2 of 5"), Outcome::Full,
          { { "Ts", "2026-11-18 13:55:37" }, { "Level", "WARN" }, { "Component", "Payment Gateway" },
            { "Message", "Retry 2 of 5" } } },
        { "words(N) до литерала", QStringLiteral("{C:words(2)}: {M}"), QStringLiteral("Order Service: done"),
          Outcome::Full, { { "C", "Order Service" }, { "M", "done" } } },
        // --- rest
        { "rest без пробелов вокруг", QStringLiteral("{L:level} {M}"), QStringLiteral("INFO   hello  "),
          Outcome::Full, { { "L", "INFO" }, { "M", "hello" } } },
        { "пробел в конце строки — и ни одного", QStringLiteral("{L:level} {M}"), QStringLiteral("INFO"),
          Outcome::Full, { { "L", "INFO" } } },
        // --- необязательный участок (правило 4)
        { "Spring Boot с [app]", spring,
          QStringLiteral("2026-11-18T13:55:36.130Z  INFO 34786 --- [myapp] [           main] o.s.b.App       : Starting"),
          Outcome::Full,
          { { "Timestamp", "2026-11-18T13:55:36.130Z" }, { "Level", "INFO" }, { "PID", "34786" },
            { "Thread", "main" }, { "Logger", "o.s.b.App" }, { "Message", "Starting" } } },
        { "Spring Boot без [app]: участок «съел» бы поток — проверка следующего элемента", spring,
          QStringLiteral("2026-11-18 13:55:38.112  INFO 45469 --- [           main] org.apache.X  : Starting"),
          Outcome::Full,
          { { "Timestamp", "2026-11-18 13:55:38.112" }, { "Level", "INFO" }, { "PID", "45469" },
            { "Thread", "main" }, { "Logger", "org.apache.X" }, { "Message", "Starting" } } },
        { "необязательный: участок сошёлся, следом — нет: его поля пусты (правило 4)",
          QStringLiteral("{L:level} {P:word}{?[{N:int}]?}: {M}"), QStringLiteral("INFO cron[12]x: y"),
          Outcome::Partial, { { "L", "INFO" }, { "P", "cron" }, { "M", "[12]x: y" } } },
        { "необязательный без отличительного элемента — не принята", QStringLiteral("{P:word}{?[{N:int}]?}: {M}"),
          QStringLiteral("cron[x]: y"), Outcome::Rejected, {} },
        { "Logback «--»", logback, QStringLiteral("13:55:40.016 [main] TRACE my-app.startup -- Context initialized"),
          Outcome::Full,
          { { "Timestamp", "13:55:40.016" }, { "Thread", "main" }, { "Level", "TRACE" }, { "Logger", "my-app.startup" },
            { "Message", "Context initialized" } } },
        // --- правило 5: остановка
        { "правило 5: поля слева остаются, остаток — в rest", logback,
          QStringLiteral("2026-03-05 10:00:03.000 restart marker"), Outcome::Partial,
          { { "Timestamp", "2026-03-05 10:00:03.000" }, { "Message", "restart marker" } } },
        { "правило 5: без отличительного элемента — не принята", QStringLiteral("{A:word} - {B:word} - {M}"),
          QStringLiteral("foo bar baz"), Outcome::Rejected, {} },
        { "правило 5: литерал от трёх знаков — отличительный", QStringLiteral("{A:word} --- {B:int} {M}"),
          QStringLiteral("foo --- bar baz"), Outcome::Partial, { { "A", "foo" }, { "M", "bar baz" } } },
        { "шаблон кончился, в строке остался текст", QStringLiteral("{L:level} {N:int}"),
          QStringLiteral("INFO 5 extra"), Outcome::Partial, { { "L", "INFO" }, { "N", "5" } } },
        // --- регекс, поле без имени, фигурные скобки
        { "регекс", QStringLiteral("{Id:/[A-Z]{3}-\\d+/} {M}"), QStringLiteral("ABC-123 x"), Outcome::Full,
          { { "Id", "ABC-123" }, { "M", "x" } } },
        { "регекс с «/» внутри", QStringLiteral("{P:/a\\/b/} {M}"), QStringLiteral("a/b x"), Outcome::Full,
          { { "P", "a/b" }, { "M", "x" } } },
        { "поле без имени", QStringLiteral("{_:word} {M}"), QStringLiteral("skip this"), Outcome::Full,
          { { "M", "this" } } },
        { "фигурные скобки литералом", QStringLiteral("{{{N:int}}} {M}"), QStringLiteral("{42} x"), Outcome::Full,
          { { "N", "42" }, { "M", "x" } } },
        { "только {Message} принимает всё", QStringLiteral("{Message}"), QStringLiteral("  anything at all"),
          Outcome::Full, { { "Message", "anything at all" } } },
    };

    for (const Row& r : rows) {
        const LineTemplate tpl(r.tpl);
        check(tpl.isValid(), QStringLiteral("%1: шаблон «%2» компилируется: %3")
                                 .arg(QString::fromUtf8(r.what), r.tpl, tpl.error().message));
        if (!tpl.isValid())
            continue;
        LogEntryFields f;
        const Outcome o = tpl.match(r.line, &f);
        const Fields got = fieldsOf(tpl.columns(), r.line, f);
        check(o == r.outcome && got == r.fields,
              QStringLiteral("%1: «%2» на «%3» → %4 {%5}, ожидалось %6 {%7}")
                  .arg(QString::fromUtf8(r.what), r.tpl, r.line, outcomeName(o), describe(got),
                       outcomeName(r.outcome), describe(r.fields)));
        // upTo: ответ «принята / нет» тот же, начальные колонки — те же.
        for (int upTo = 0; upTo < tpl.columns().size(); ++upTo) {
            LogEntryFields g;
            const Outcome u = tpl.match(r.line, &g, upTo);
            check(FieldExtractor::accepted(u) == FieldExtractor::accepted(o),
                  QStringLiteral("%1: upTo %2 — тот же ответ «принята»").arg(QString::fromUtf8(r.what)).arg(upTo));
            for (int c = 0; c <= upTo && FieldExtractor::accepted(o); ++c) {
                check(g.value(c, r.line) == f.value(c, r.line),
                      QStringLiteral("%1: upTo %2 — колонка %3 та же").arg(QString::fromUtf8(r.what)).arg(upTo).arg(c));
            }
        }
    }

    // Колонки: поле без имени колонкой не становится; хвост — rest.
    const LineTemplate anon(QStringLiteral("{_:word} {A:int} {} {B}"));
    check(anon.isValid() && anon.columns() == QStringList({ "A", "B" }) && anon.tailColumn() == 1,
          QStringLiteral("колонки шаблона с безымянными полями: %1").arg(anon.columns().join(',')));
    check(TemplateExtractor(QStringLiteral("{Id:/x/} {M}")).cost() == FieldExtractor::Cost::Background
              && TemplateExtractor(QStringLiteral("{L:level} {M}")).cost() == FieldExtractor::Cost::Inline,
          QStringLiteral("с регексом — Background, без — Inline"));
    // Экранированное значение несёт бит, сырой текст — как в строке.
    {
        const LineTemplate q(QStringLiteral("{Q:quoted}"));
        LogEntryFields f;
        const QString line = QStringLiteral("\"a\\\"b\"");
        q.match(line, &f);
        check(f.isEscaped(0) && f.get(0, line) == QLatin1String("a\\\"b") && f.value(0, line) == QLatin1String("a\"b"),
              QStringLiteral("quoted: бит экранирования, сырой и раскодированный текст"));
    }

    // explain: причина данными.
    {
        const TemplateExtractor x(logback);
        const MatchReport ok = x.explain(QStringLiteral("2026-03-05 10:00:00.000 [w1] INFO a.B - hello"));
        check(ok.expect == MatchReport::Expect::Nothing, QStringLiteral("explain: принятая строка — Nothing"));
        const MatchReport br = x.explain(QStringLiteral("2026-03-05 10:00:03.000 restart marker"));
        check(br.partial && br.expect == MatchReport::Expect::Literal && br.literal == QLatin1String("[")
                  && br.stopOffset == 24 && br.found.startsWith(QLatin1String("restart")),
              QStringLiteral("explain: после метки ожидалась «[» (expect %1, literal %2, offset %3)")
                  .arg(int(br.expect)).arg(br.literal).arg(br.stopOffset));
        const MatchReport none = x.explain(QStringLiteral("restart marker"));
        check(!none.partial && none.expect == MatchReport::Expect::Timestamp && none.afterBlock == -1
                  && none.field == QLatin1String("Timestamp"),
              QStringLiteral("explain: ожидалась метка"));
        const TemplateExtractor py(QStringLiteral("{Timestamp:time} - {Logger} - {Level:level} - {Message}"));
        const MatchReport sep = py.explain(QStringLiteral("2026-11-18 13:55:36,123 - no separator after logger"));
        check(sep.expect == MatchReport::Expect::Literal && sep.literal == QLatin1String("-")
                  && sep.field == QLatin1String("Logger"),
              QStringLiteral("explain: текст до литерала, литерала нет — он и ожидался"));
        const TemplateExtractor of(component);
        const MatchReport v = of.explain(QStringLiteral("2026-11-18 13:55:39 INFO Billing charged"));
        check(v.expect == MatchReport::Expect::Value && v.literal.contains(QLatin1String("Order Service")),
              QStringLiteral("explain: ожидалось значение из списка"));
    }
}

// ---------------------------------------------------------------------------
// 2. Ошибки компиляции
// ---------------------------------------------------------------------------

void testErrors()
{
    using C = LineTemplateError::Code;
    struct E {
        QString tpl;
        C code;
        int position;
        QString field;
    };
    const QVector<E> rows = {
        { QStringLiteral("{Component:text} {Message}"), C::UndefinedEnd, 0, QStringLiteral("Component") },
        { QStringLiteral("{Ts:time} {Component:text} {Message}"), C::UndefinedEnd, 10, QStringLiteral("Component") },
        { QStringLiteral("{A}{B}"), C::UndefinedEnd, 0, QStringLiteral("A") },
        { QStringLiteral("{A:greedy}{B:word}"), C::UndefinedEnd, 0, QStringLiteral("A") },
        // Участок открывает (или пропускает) путь к полю без формы.
        { QStringLiteral("{A}{?{B}?}x"), C::UndefinedEnd, 0, QStringLiteral("A") },
        { QStringLiteral("{Thread:text}{? ?}{Message:rest}"), C::UndefinedEnd, 0, QStringLiteral("Thread") },
        { QStringLiteral("{L:level} {A:text}{?[{N:int}]?}{B:word}"), C::UndefinedEnd, 10, QStringLiteral("A") },
        { QStringLiteral("{Ts:time"), C::UnclosedField, 0, QStringLiteral("Ts") },
        { QStringLiteral("x {A:of(a|b}"), C::UnclosedField, 2, QStringLiteral("A") },
        { QStringLiteral("{R:/abc}"), C::UnclosedField, 0, QStringLiteral("R") },
        { QStringLiteral("{R:/[a-z/}"), C::BadRegex, 0, QStringLiteral("R") },
        { QStringLiteral("abc}"), C::UnmatchedBrace, 3, QString() },
        { QStringLiteral("a {?b"), C::UnclosedOptional, 2, QString() },
        { QStringLiteral("{?a{?b?}?}"), C::NestedOptional, 3, QString() },
        { QStringLiteral("x{??}"), C::EmptyOptional, 1, QString() },
        { QStringLiteral("{L:level} {X:foo}"), C::UnknownKind, 10, QStringLiteral("X") },
        { QStringLiteral("{X:width(0)}"), C::BadArgument, 0, QStringLiteral("X") },
        { QStringLiteral("{X:width}"), C::BadArgument, 0, QStringLiteral("X") },
        { QStringLiteral("{X:words(two)}"), C::BadArgument, 0, QStringLiteral("X") },
        { QStringLiteral("{X:of(a||b)}"), C::BadArgument, 0, QStringLiteral("X") },
        { QStringLiteral("{X:int(3)}"), C::BadArgument, 0, QStringLiteral("X") },
        { QStringLiteral("{X:/[/} {M}"), C::BadRegex, 0, QStringLiteral("X") },
        { QStringLiteral("{M:rest} - {L:level}"), C::RestNotLast, 0, QStringLiteral("M") },
        { QStringLiteral("{A:word} {a:int}"), C::DuplicateName, 9, QStringLiteral("a") },
        { QString(), C::Empty, 0, QString() },
        { QStringLiteral("   "), C::Empty, 0, QString() },
    };
    for (const E& r : rows) {
        const LineTemplate tpl(r.tpl);
        const LineTemplateError& e = tpl.error();
        check(!tpl.isValid() && e.code == r.code && e.position == r.position && e.field == r.field
                  && !e.message.isEmpty() && (r.field.isEmpty() || e.message.contains(r.field)),
              QStringLiteral("ошибка «%1»: код %2 позиция %3 поле «%4» («%5»), ожидалось %6 / %7 / «%8»")
                  .arg(r.tpl).arg(int(e.code)).arg(e.position).arg(e.field, e.message)
                  .arg(int(r.code)).arg(r.position).arg(r.field));
        check(tpl.columns().isEmpty() && tpl.match(QStringLiteral("x"), nullptr) == Outcome::Rejected,
              QStringLiteral("невалидный шаблон «%1» ничего не принимает").arg(r.tpl));
    }
    // Сообщение о трудной границе называет выход.
    const LineTemplate hard(QStringLiteral("{Component:text} {Message}"));
    check(hard.error().message.contains(QLatin1String("only a space follows"))
              && hard.error().message.contains(QLatin1String("list of values")),
          QStringLiteral("сообщение о неопределённом конце: %1").arg(hard.error().message));
    // Виды по умолчанию.
    const LineTemplate defaults(QStringLiteral("{A} {B} [{C}] {D} {E:level} {F}"));
    check(defaults.isValid(), QStringLiteral("виды по умолчанию компилируются: %1").arg(defaults.error().message));
    if (defaults.isValid()) {
        QStringList kinds;
        for (const LineTemplateElement& e : defaults.elements()) {
            if (e.isField())
                kinds << LineTemplate::kindName(e.kind);
        }
        // A — пробел и поле без формы: word; B — пробел и литерал «[»: text.
        check(kinds == QStringList({ "word", "text", "text", "text", "level", "rest" }),
              QStringLiteral("виды по умолчанию: %1").arg(kinds.join(',')));
    }
}

// ---------------------------------------------------------------------------
// 3. Встроенные шаблоны
// ---------------------------------------------------------------------------

QString templateFileFor(const QString& patternFile)
{
    if (patternFile.startsWith(QLatin1String("syslog-")))
        return QStringLiteral("syslog.json");
    return patternFile;
}

bool isKeyedFile(const QString& file)
{
    return file == QLatin1String("json-lines.json") || file == QLatin1String("logfmt.json");
}

LogPattern blockPattern(const QString& file)
{
    QString canonical;
    BuiltinSchemas::parseSchemaFile(readAll(QDir(kPatternsDir).filePath(file)), QString(), nullptr,
                                    &canonical, true);
    return LogPattern(canonical);
}

std::shared_ptr<const LineTemplate> builtinTemplate(const QString& file, LineTemplateSchema* schema = nullptr)
{
    LineTemplateSchema s;
    QString why;
    if (!LineTemplateSchema::read(readAll(QDir(kTemplatesDir).filePath(file)), &s, &why))
        return nullptr;
    if (schema)
        *schema = s;
    return std::make_shared<const LineTemplate>(s.templateText);
}

void testBuiltinTemplates()
{
    QStringList expected;
    for (const SchemaCase& sc : cases()) {
        if (!isKeyedFile(sc.file) && !expected.contains(templateFileFor(sc.file)))
            expected << templateFileFor(sc.file);
    }
    expected.sort();
    const QStringList files = QDir(kTemplatesDir).entryList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name);
    check(files == expected && files.size() == 9,
          QStringLiteral("встроенные шаблоны — девять, по формату: %1").arg(files.join(' ')));

    for (const QString& file : files) {
        LineTemplateSchema s;
        const auto tpl = builtinTemplate(file, &s);
        check(tpl && tpl->isValid() && !s.description.isEmpty(),
              QStringLiteral("%1: файл версии 4 читается, шаблон компилируется: %2")
                  .arg(file, tpl ? tpl->error().message : QStringLiteral("не прочитан")));
        check(tpl && !tpl->hasRegex(), QStringLiteral("%1: без регекса — Inline").arg(file));
    }

    for (const SchemaCase& sc : cases()) {
        if (isKeyedFile(sc.file))
            continue;
        const auto tpl = builtinTemplate(templateFileFor(sc.file));
        if (!tpl || !tpl->isValid())
            continue;
        const LogPattern old = blockPattern(sc.file);
        check(old.isValid() && tpl->columns() == old.fieldNames(),
              QStringLiteral("%1: колонки шаблона %2 = колонкам блочной схемы %3")
                  .arg(sc.file, tpl->columns().join(','), old.fieldNames().join(',')));
        for (const Sample& smp : sc.samples) {
            LogEntryFields f;
            const Outcome o = tpl->match(smp.line, &f);
            if (smp.continuation) {
                check(o == Outcome::Rejected,
                      QStringLiteral("%1: продолжение не принято: %2 → %3").arg(sc.file, smp.line, outcomeName(o)));
                continue;
            }
            // Ожидания builtin_schemas — по СЫРОМУ тексту поля (у блочной схемы
            // экранирования нет); раскодированное — отдельной проверкой ниже.
            const Fields got = fieldsOf(tpl->columns(), smp.line, f, /*raw=*/true);
            Fields want;
            for (auto it = smp.fields.cbegin(); it != smp.fields.cend(); ++it) {
                if (!it.value().isEmpty())
                    want.insert(it.key(), it.value());
            }
            check(o == Outcome::Full && got == want,
                  QStringLiteral("%1: %2\n       → %3 {%4}\n       ожидалось {%5}")
                      .arg(sc.file, smp.line, outcomeName(o), describe(got), describe(want)));
        }
    }

    // Решение шага 2: quoted раскодирует «\"» (у блочной схемы — сырой текст).
    const auto access = builtinTemplate(QStringLiteral("access-combined.json"));
    if (access) {
        const QString line = QStringLiteral(
            "192.0.2.10 - - [18/Nov/2026:13:55:39 +0100] \"-\" 408 - \"-\" \"Mozilla/5.0 \\\"quoted\\\" agent\"");
        LogEntryFields f;
        access->match(line, &f);
        const int ua = int(access->columns().indexOf(QStringLiteral("User agent")));
        check(f.value(ua, line) == QLatin1String("Mozilla/5.0 \"quoted\" agent"),
              QStringLiteral("access: User agent раскодирован: %1").arg(f.value(ua, line)));
    }
}

// ---------------------------------------------------------------------------
// 4. Конвертер: блочная схема ≡ её шаблону на корпусе
// ---------------------------------------------------------------------------

QStringList corpusLines()
{
    QStringList lines;
    for (const SchemaCase& sc : cases()) {
        for (const Sample& s : sc.samples)
            lines << s.line;
    }
    return lines;
}

void testConverter()
{
    const QStringList all = corpusLines();
    for (const SchemaCase& sc : cases()) {
        const LogPattern old = blockPattern(sc.file);
        const SchemaConverter::Result r = SchemaConverter::fromBlocks(old.definition());
        if (isKeyedFile(sc.file)) {
            check(!r.ok && !r.error.isEmpty(), QStringLiteral("%1: ключевая схема — не шаблон").arg(sc.file));
            continue;
        }
        check(r.ok, QStringLiteral("%1: переведена: %2 («%3»)").arg(sc.file, r.error, r.templateText));
        if (!r.ok)
            continue;
        const LineTemplate tpl(r.templateText);
        check(tpl.isValid() && tpl.columns() == old.fieldNames(),
              QStringLiteral("%1: «%2» компилируется, колонки те же").arg(sc.file, r.templateText));
        if (!tpl.isValid())
            continue;
        std::printf("  %-28s %s\n", qPrintable(sc.file), qPrintable(r.templateText));
        // Свой корпус и все продолжения: принята ли строка и поля — те же.
        QStringList lines;
        for (const Sample& s : sc.samples)
            lines << s.line;
        for (const SchemaCase& other : cases()) {
            for (const Sample& s : other.samples) {
                if (s.continuation)
                    lines << s.line;
            }
        }
        const auto hand = builtinTemplate(templateFileFor(sc.file));
        for (const QString& line : lines) {
            LogEntryFields fo = old.extractFields(line);
            LogEntryFields fn;
            const Outcome o = tpl.match(line, &fn);
            const bool oldAccepted = !fo.isEmpty() || old.matchLine(line).ok;
            const Fields a = fieldsOf(old.fieldNames(), line, fo, true);
            const Fields b = fieldsOf(tpl.columns(), line, fn, true);
            check(oldAccepted == FieldExtractor::accepted(o) && a == b,
                  QStringLiteral("%1: конвертированный ≠ блочной на «%2»\n       блочная %3 {%4}\n       шаблон  %5 {%6}")
                      .arg(sc.file, line).arg(oldAccepted).arg(describe(a), outcomeName(o), describe(b)));
            // И ручной встроенный шаблон: те же поля, что у блочной.
            if (hand && hand->isValid()) {
                LogEntryFields fh;
                const Outcome h = hand->match(line, &fh);
                const Fields c = fieldsOf(hand->columns(), line, fh, true);
                check(oldAccepted == FieldExtractor::accepted(h) && a == c,
                      QStringLiteral("%1: встроенный шаблон ≠ блочной на «%2»\n       блочная {%3}\n       шаблон  {%4}")
                          .arg(sc.file, line, describe(a), describe(c)));
            }
        }
        // Чужие записи (корпус других форматов): принятие не шире прежнего.
        int wider = 0;
        for (const QString& line : all) {
            LogEntryFields fn;
            if (FieldExtractor::accepted(tpl.match(line, &fn)) && !old.matchLine(line).ok)
                ++wider;
        }
        check(wider == 0, QStringLiteral("%1: конвертированный не принимает того, что не принимала блочная (%2)")
                              .arg(sc.file).arg(wider));
    }

    // Простые случаи перевода.
    struct C {
        QString blocks;
        QString tpl;
    };
    PatternDefinition def;
    PatternBlock ts;
    ts.name = QStringLiteral("Timestamp");
    ts.matchKind = PatternBlock::MatchKind::Timestamp;
    PatternBlock th;
    th.name = QStringLiteral("Thread");
    th.matchKind = PatternBlock::MatchKind::TextUntilSeparator;
    th.leadingText = QStringLiteral("[");
    th.closingText = QStringLiteral("]");
    PatternBlock k;
    k.name = QStringLiteral("Kind");
    k.matchKind = PatternBlock::MatchKind::ConstantText;
    k.customRegex = QStringLiteral("GET");
    PatternBlock ig;
    ig.name = QStringLiteral("Skip");
    ig.matchKind = PatternBlock::MatchKind::Integer;
    ig.ignored = true;
    ig.separator = QStringLiteral("|");
    PatternBlock msg;
    msg.name = QStringLiteral("Message");
    msg.matchKind = PatternBlock::MatchKind::Remainder;
    def.blocks = { ts, th, k, ig, msg };
    const SchemaConverter::Result r = SchemaConverter::fromBlocks(def);
    check(r.ok && r.templateText
                      == QLatin1String("{Timestamp:time}{? ?}[{? ?}{Thread:text}{? ?}]{? ?}{Kind:of(GET)}{? ?}"
                                       "{_:int}{? ?}{?|?}{? ?}{Message:rest}"),
          QStringLiteral("перевод: обёртки, константа полем, ignored, необязательный разделитель: %1")
              .arg(r.templateText));
    const LineTemplate t(r.templateText);
    LogEntryFields f;
    const QString line = QStringLiteral("2026-11-18 13:55:36 [ main ]GET 42| hello");
    check(t.match(line, &f) == Outcome::Full
              && fieldsOf(t.columns(), line, f)
                  == Fields({ { "Timestamp", "2026-11-18 13:55:36" }, { "Thread", "main" }, { "Kind", "GET" },
                              { "Message", "hello" } }),
          QStringLiteral("перевод: шаблон разбирает строку"));
    // Text, за которым только пробел и поле без формы, — word.
    PatternDefinition two;
    PatternBlock a;
    a.name = QStringLiteral("A");
    a.matchKind = PatternBlock::MatchKind::TextUntilSeparator;
    a.separator = QStringLiteral(" ");
    PatternBlock b;
    b.name = QStringLiteral("B");
    b.matchKind = PatternBlock::MatchKind::TextUntilSeparator;
    b.separator = QStringLiteral(" ");
    two.blocks = { a, b, msg };
    const SchemaConverter::Result w = SchemaConverter::fromBlocks(two);
    check(w.ok && w.templateText == QLatin1String("{A:word} {B:word} {Message:rest}") && w.notes.size() == 2,
          QStringLiteral("перевод: text без конца → word: %1 (%2)").arg(w.templateText, w.notes.join("; ")));
    // Пробельный регекс-разделитель — пробел шаблона (после самоограниченного
    // блока — промежуток), а не пустой участок.
    {
        PatternDefinition d;
        PatternBlock lv;
        lv.name = QStringLiteral("Level");
        lv.matchKind = PatternBlock::MatchKind::Level;
        lv.separator = QStringLiteral(" ");
        lv.separatorIsRegex = true;
        PatternBlock tx;
        tx.name = QStringLiteral("Thread");
        tx.matchKind = PatternBlock::MatchKind::TextUntilSeparator;
        tx.separator = QStringLiteral(" ");
        tx.separatorIsRegex = true;
        d.blocks = { lv, tx, msg };
        const SchemaConverter::Result rs = SchemaConverter::fromBlocks(d);
        const LineTemplate ts(rs.templateText);
        LogEntryFields fs;
        const QString l = QStringLiteral("INFO t1 hello world");
        check(rs.ok && ts.match(l, &fs) == Outcome::Full
                  && fieldsOf(ts.columns(), l, fs)
                      == Fields({ { "Level", "INFO" }, { "Thread", "t1" }, { "Message", "hello world" } })
                  && fieldsOf(ts.columns(), l, fs) == fieldsOf(LogPattern(LogPattern::serializeDefinition(d))
                                                                   .fieldNames(), l,
                                                               LogPattern(LogPattern::serializeDefinition(d))
                                                                   .extractFields(l)),
              QStringLiteral("перевод: пробельный регекс-разделитель: «%1» %2").arg(rs.templateText, rs.error));
    }
    // Имя колонки, которое шаблон не записал бы буква в букву, — отказ.
    {
        PatternDefinition d;
        PatternBlock u;
        u.name = QStringLiteral("_");
        u.matchKind = PatternBlock::MatchKind::Integer;
        d.blocks = { u, msg };
        check(!SchemaConverter::fromBlocks(d).ok, QStringLiteral("перевод: колонка «_» — отказ"));
        u.name = QStringLiteral("?x");
        d.blocks = { u, msg };
        check(!SchemaConverter::fromBlocks(d).ok, QStringLiteral("перевод: колонка «?x» — отказ"));
    }
}

// ---------------------------------------------------------------------------
// 5. Ключевой и прежний извлекатели
// ---------------------------------------------------------------------------

void testOtherExtractors()
{
    for (const SchemaCase& sc : cases()) {
        const LogPattern old = blockPattern(sc.file);
        const LegacyPatternExtractor legacy(old);
        std::unique_ptr<KeyedExtractor> keyed;
        if (old.isKeyed())
            keyed = std::make_unique<KeyedExtractor>(*old.keyed());
        check(legacy.fields() == old.fieldNames() && legacy.tailField() == old.tailFieldIndex()
                  && legacy.isKeyed() == old.isKeyed(),
              QStringLiteral("%1: прежний извлекатель — колонки и хвост схемы").arg(sc.file));
        for (const Sample& s : sc.samples) {
            const LogEntryFields want = old.extractFields(s.line);
            LogEntryFields got;
            const Outcome o = legacy.extract(s.line, &got);
            check(fieldsOf(old.fieldNames(), s.line, want) == fieldsOf(legacy.fields(), s.line, got)
                      && FieldExtractor::accepted(o) == old.matchLine(s.line).ok,
                  QStringLiteral("%1: прежний извлекатель = LogPattern на «%2»").arg(sc.file, s.line));
            if (!keyed)
                continue;
            LogEntryFields k;
            const Outcome ko = keyed->extract(s.line, &k);
            check(fieldsOf(keyed->fields(), s.line, k) == fieldsOf(old.fieldNames(), s.line, want)
                      && (ko == Outcome::Full) == !s.continuation
                      && (s.continuation || k.spans.size() == want.spans.size()),
                  QStringLiteral("%1: ключевой извлекатель = LogPattern на «%2»").arg(sc.file, s.line));
            for (int c = 0; c < k.size() && c < want.size(); ++c)
                check(k.isEscaped(c) == want.isEscaped(c), QStringLiteral("%1: бит экранирования").arg(sc.file));
        }
        if (keyed) {
            check(keyed->explain(QStringLiteral("plain text")).expect == MatchReport::Expect::Record,
                  QStringLiteral("%1: ключевой explain — Record").arg(sc.file));
        }
    }
}

// ---------------------------------------------------------------------------
// 6. SchemaFit на интерфейсе
// ---------------------------------------------------------------------------

void testSchemaFit()
{
    for (const SchemaCase& sc : cases()) {
        QStringList lines;
        for (const Sample& s : sc.samples)
            lines << s.line;
        const LogPattern old = blockPattern(sc.file);
        // Прежний путь (LogPattern) = тот же LogPattern через интерфейс.
        const SchemaFit::Result a = SchemaFit::evaluate(old, lines);
        const SchemaFit::Result b = SchemaFit::evaluate(LegacyPatternExtractor(old), lines);
        check(a.full == b.full && a.partial == b.partial && a.missed == b.missed && a.loose == b.loose
                  && a.filled == b.filled && a.distinct == b.distinct && a.valid && b.valid,
              QStringLiteral("%1: SchemaFit по LogPattern = по LegacyPatternExtractor").arg(sc.file));
        if (isKeyedFile(sc.file)) {
            const SchemaFit::Result k = SchemaFit::evaluate(KeyedExtractor(*old.keyed()), lines);
            check(SchemaFit::verdict(k) == SchemaFit::Verdict::Fits,
                  QStringLiteral("%1: ключевой извлекатель «подходит» своим образцам").arg(sc.file));
            continue;
        }
        const auto tpl = builtinTemplate(templateFileFor(sc.file));
        if (!tpl)
            continue;
        const TemplateExtractor x(tpl);
        const SchemaFit::Result r = SchemaFit::evaluate(x, lines);
        check(SchemaFit::verdict(r) == SchemaFit::Verdict::Fits && r.partial == 0 && r.missed == 0
                  && r.loose == 0,
              QStringLiteral("%1: встроенный шаблон «подходит» своим образцам: %2")
                  .arg(sc.file, SchemaFit::summary(r)));
    }

    // Разные значения по колонке: верная граница — шесть названий, неверная —
    // столько, сколько строк.
    QStringList lines;
    const QStringList names = { "Order Service", "Payment Gateway", "DB", "Auth", "Cache", "Mailer" };
    for (int i = 0; i < 300; ++i)
        lines << QStringLiteral("2026-11-18 13:55:%1 INFO %2 event %3 done")
                     .arg(i % 60, 2, 10, QLatin1Char('0')).arg(names[i % names.size()]).arg(i);
    const TemplateExtractor right(QStringLiteral(
        "{Ts:time} {Level:level} {Component:of(Order Service|Payment Gateway|DB|Auth|Cache|Mailer)} {Message}"));
    const TemplateExtractor wrong(QStringLiteral("{Ts:time} {Level:level} {Component:words(3)} {Message}"));
    const SchemaFit::Result rr = SchemaFit::evaluate(right, lines);
    const SchemaFit::Result rw = SchemaFit::evaluate(wrong, lines);
    check(rr.distinct.value(2) == 6 && rr.distinct.value(1) == 1 && rr.distinct.value(0) == 60
              && rr.distinctText(2) == QLatin1String("6"),
          QStringLiteral("разные значения: Component %1, Level %2, Ts %3")
              .arg(rr.distinct.value(2)).arg(rr.distinct.value(1)).arg(rr.distinct.value(0)));
    check(rw.distinct.value(2) > 50, QStringLiteral("неверная граница — много значений: %1").arg(rw.distinct.value(2)));
    QStringList many;
    for (int i = 0; i < 1500; ++i)
        many << QStringLiteral("INFO value-%1").arg(i);
    const SchemaFit::Result cap = SchemaFit::evaluate(TemplateExtractor(QStringLiteral("{L:level} {V:word}")), many);
    check(cap.distinct.value(1) == SchemaFit::kDistinctCap + 1 && cap.distinctText(1) == QLatin1String("1000+"),
          QStringLiteral("разные значения — точно до 1000, дальше «1000+»: %1").arg(cap.distinctText(1)));

    // explain через SchemaFit — то же, что у извлекателя.
    const TemplateExtractor lb(QStringLiteral("{Timestamp:time} [{Thread}] {Level:level} {Message}"));
    const SchemaFit::Reason reason = SchemaFit::explain(lb, QStringLiteral("2026-03-05 10:00:03.000 restart"));
    check(reason.expect == MatchReport::Expect::Literal && reason.literal == QLatin1String("["),
          QStringLiteral("SchemaFit::explain на извлекателе"));
    // Промахи: элемент и место остановки.
    const SchemaFit::Result mr = SchemaFit::evaluate(lb, QStringList{
        QStringLiteral("2026-03-05 10:00:00.000 [a] INFO x"), QStringLiteral("2026-03-05 10:00:03.000 restart") });
    check(mr.partial == 1 && mr.misses.size() == 1 && mr.misses[0].partial && mr.misses[0].stopOffset == 24
              && mr.misses[0].afterBlock == 1,
          QStringLiteral("промах шаблона: после элемента %1, место %2")
              .arg(mr.misses.value(0).afterBlock).arg(mr.misses.value(0).stopOffset));
}

// ---------------------------------------------------------------------------
// 7. Фаззинг: шаблон → порождённые строки → поля восстановлены
// ---------------------------------------------------------------------------

struct Gen {
    QRandomGenerator rng;
    explicit Gen(quint32 seed) : rng(seed) {}
    int below(int n) { return int(rng.bounded(n)); }
    template <typename T>
    const T& pick(const QVector<T>& v) { return v.at(below(int(v.size()))); }

    QString word()
    {
        // Буквы g–z и не слово уровня: не hex, не число, не метка, не уровень.
        static const QVector<QString> words = { "orange", "service", "worker", "pool", "thread", "main",
                                                "queue", "router", "sync", "loop", "jvm", "pyx" };
        return pick(words);
    }
    QString words(int n)
    {
        QStringList w;
        for (int i = 0; i < n; ++i)
            w << word();
        return w.join(QLatin1Char(' '));
    }
    QString token()
    {
        // Слово для word: латиница, цифры, «.» и «_» (без знаков разделителей).
        QString t;
        const QString alphabet = QStringLiteral("abcdefghijklmnopqrstuvwxyz0123456789._");
        const int n = 1 + below(10);
        for (int i = 0; i < n; ++i)
            t += alphabet.at(below(int(alphabet.size())));
        return t;
    }
    QString spaces(int min = 1) { return QString(min + below(3), QLatin1Char(' ')); }
};

void testFuzzRoundTrip()
{
    Gen g(20261126);
    const QVector<QString> times = {
        "2026-11-18 13:55:36.123", "2026-11-18T13:55:36Z", "2026/11/18 13:55:36", "Nov 18 13:55:36",
        "Nov  8 03:04:05", "13:55:40.016", "18/Nov/2026:13:55:36 +0000", "2026-11-18T13:55:36.5+01:00",
    };
    const QVector<QString> levels = { "INFO", "warn", "Error", "DEBUG", "TRACE", "FATAL", "INF", "WRN" };
    const QStringList ofValues = { "Order Service", "Order", "Payment Gateway", "DB", "Auth Svc" };
    // Литералы-разделители: с пробелами вокруг («a» — пробел до, «b» — после).
    struct Sep {
        QString lit;
        bool before;
        bool after;
    };
    const QVector<Sep> seps = { { "|", true, true }, { "-", true, true }, { "::", true, true },
                                { ":", false, true }, { "]", false, true }, { ",", false, true },
                                { "#", false, false }, { "=>", true, true } };

    int templates = 0, lines = 0;
    for (int iter = 0; iter < 2500; ++iter) {
        const int fieldCount = 1 + g.below(6);
        QString tpl;
        QString line;
        QVector<std::pair<QString, QString>> expect;   // колонка → значение
        bool lastWasLiteralGlue = true;
        for (int k = 0; k < fieldCount; ++k) {
            const QString name = QStringLiteral("F%1").arg(k);
            const bool last = k == fieldCount - 1;
            // Вид поля и значение.
            const int kind = g.below(14);
            // Что идёт следом: литерал с пробелами или пробел.
            // text — только до литерала; padded — только до промежутка пробелов.
            const bool glueLiteral = !last && kind != 10 && (kind == 9 || g.below(2) == 0);
            const Sep sep = g.pick(seps);
            QString spec, value, shown;
            switch (kind) {
            case 0: spec = "time"; value = g.pick(times); break;
            case 1: spec = "level"; value = g.pick(levels); break;
            case 2: spec = "int"; value = QString::number(g.below(100000)); break;
            case 3: spec = "hex"; value = QStringLiteral("0x%1").arg(g.rng.generate(), 0, 16); break;
            case 4: spec = "uuid"; value = QStringLiteral("123e4567-e89b-12d3-a456-4266141740%1").arg(10 + g.below(90)); break;
            case 5:
                spec = "ip";
                value = g.below(2) ? QStringLiteral("10.%1.%2.%3").arg(g.below(256)).arg(g.below(256)).arg(g.below(256))
                                   : QStringLiteral("2001:db8::%1").arg(g.below(4096), 0, 16);
                break;
            case 6: {
                spec = "quoted";
                QString raw;
                const int n = g.below(12);
                for (int i = 0; i < n; ++i) {
                    const int c = g.below(6);
                    if (c == 0) { raw += "\\\""; value += '"'; }
                    else if (c == 1) { raw += "\\\\"; value += '\\'; }
                    else if (c == 2) { raw += ' '; value += ' '; }
                    else { const QString w = g.word(); raw += w; value += w; }
                }
                shown = QLatin1Char('"') + raw + QLatin1Char('"');
                break;
            }
            case 7: {
                spec = QStringLiteral("of(") + ofValues.join('|') + QLatin1Char(')');
                value = ofValues.at(g.below(int(ofValues.size())));
                break;
            }
            case 8: spec = "word"; value = g.token(); break;
            case 9: spec = QString(); value = g.words(1 + g.below(3)); break;   // text до литерала
            case 10: spec = "padded"; value = g.words(1 + g.below(3)); break;
            case 11: {
                const int n = 6 + g.below(10);
                spec = QStringLiteral("width(%1)").arg(n);
                value = g.word().left(n);
                shown = value.leftJustified(n, QLatin1Char(' '));
                break;
            }
            case 12: {
                const int n = 1 + g.below(3);
                spec = QStringLiteral("words(%1)").arg(n);
                value = g.words(n);
                break;
            }
            default: spec = "num"; value = QStringLiteral("%1.%2").arg(g.below(1000)).arg(g.below(1000)); break;
            }
            if (last && g.below(3) == 0) {
                spec = "rest";
                value = g.words(1 + g.below(5));
                shown.clear();
            }
            if (shown.isEmpty())
                shown = value;
            Q_UNUSED(lastWasLiteralGlue);
            tpl += QLatin1Char('{') + name + (spec.isEmpty() ? QString() : QLatin1Char(':') + spec) + QLatin1Char('}');
            line += shown;
            expect.append({ name, value });
            if (last)
                break;
            if (glueLiteral) {
                tpl += (sep.before ? QStringLiteral(" ") : QString()) + LineTemplate::escapeLiteral(sep.lit)
                       + (sep.after ? QStringLiteral(" ") : QString());
                line += (sep.before ? g.spaces() : QString()) + sep.lit + (sep.after ? g.spaces() : QString());
                lastWasLiteralGlue = true;
            } else {
                tpl += QLatin1Char(' ');
                line += kind == 10 ? g.spaces(2) : g.spaces();
                lastWasLiteralGlue = false;
            }
        }
        const LineTemplate t(tpl);
        if (!t.isValid()) {
            // Порождённый шаблон с неопределённым концом (text перед пробелом и
            // полем без формы) — ошибка компиляции, это и проверяется.
            check(t.error().code == LineTemplateError::Code::UndefinedEnd,
                  QStringLiteral("фаззинг: «%1» — %2").arg(tpl, t.error().message));
            if (std::getenv("LINE_TEMPLATE_TRACE"))
                std::printf("  не компилируется: %s — %s\n", qPrintable(tpl), qPrintable(t.error().message));
            continue;
        }
        ++templates;
        ++lines;
        LogEntryFields f;
        const Outcome o = t.match(line, &f);
        bool same = o == Outcome::Full;
        QStringList diff;
        for (const auto& [name, value] : expect) {
            const int c = int(t.columns().indexOf(name));
            const QString got = f.value(c, line);
            if (got != value) {
                same = false;
                diff << QStringLiteral("%1: «%2» ≠ «%3»").arg(name, got, value);
            }
        }
        check(same, QStringLiteral("фаззинг: «%1» на «%2» → %3; %4")
                        .arg(tpl, line, outcomeName(o), diff.join(QStringLiteral("; "))));
        if (!same && g_failures > 20)
            break;
    }
    std::printf("  фаззинг: %d шаблонов, %d строк восстановлены\n", templates, lines);
    check(templates > 1500, QStringLiteral("фаззинг: большинство порождённых шаблонов компилируется (%1)").arg(templates));
}

// ---------------------------------------------------------------------------
// 8. Случайные строки: без падений, линейно по счётчику шагов
// ---------------------------------------------------------------------------

QStringList stressTemplates()
{
    QStringList t;
    for (const QString& file : QDir(kTemplatesDir).entryList({ QStringLiteral("*.json") }, QDir::Files)) {
        LineTemplateSchema s;
        if (LineTemplateSchema::read(readAll(QDir(kTemplatesDir).filePath(file)), &s))
            t << s.templateText;
    }
    t << QStringLiteral("{A} - {B} - {C} - {D}")
      << QStringLiteral("{A:greedy} - {B:greedy} - {C}")
      << QStringLiteral("{A} {N:int} {B} {Q:quoted} {C} {T:time} {M}")
      << QStringLiteral("{A}{N:int}{B}{H:hex}{C}{P:path}{M}")
      << QStringLiteral("{?[{A}] ?}{?({B}) ?}{?<{C}> ?}{M}")
      << QStringLiteral("{C:of(a|a b|a b c|b)} {D:words(3)} {E:padded} {F:width(5)} {M}")
      << QStringLiteral("{A:word}[{B}]{C:word}({D}){?:{E}:?}{M}")
      << QStringLiteral("{A} {L:level} {B} {I:ip} {C} {U:uuid} {M}");
    return t;
}

void testLinearSteps()
{
    const QStringList templates = stressTemplates();
    QRandomGenerator rng(4242);
    const QString alphabet = QStringLiteral(" \t-[]{}():|\"\\#*/.,0123456789aAbBfFxZ:INFOerr:Nov");
    int lines = 0;
    for (const QString& text : templates) {
        const LineTemplate t(text);
        check(t.isValid(), QStringLiteral("нагрузочный шаблон «%1» компилируется: %2").arg(text, t.error().message));
        if (!t.isValid())
            continue;
        const qint64 elements = t.elements().size();
        LogEntryFields f;
        double worst = 0;
        for (int iter = 0; iter < 1500; ++iter) {
            const int len = int(rng.bounded(600));
            QString line;
            line.reserve(len);
            for (int k = 0; k < len; ++k)
                line += alphabet.at(int(rng.bounded(alphabet.size())));
            qint64 steps = 0;
            t.match(line, &f, -1, nullptr, nullptr, &steps);
            ++lines;
            const double perChar = double(steps) / double(len + 1) / double(elements);
            worst = std::max(worst, perChar);
            check(steps <= 64 * (len + 1) * elements,
                  QStringLiteral("«%1»: %2 шагов на строке из %3 знаков").arg(text).arg(steps).arg(len));
        }
        // Злонамеренные строки: повторы, на которых квадратичный разбор
        // проявился бы — шагов на 4n не больше чем ~4 раза шагов на n.
        const QStringList bricks = { "[", "\"", " - ", "- ", "1", "a ", "::", " (", "{", "a b ", "\\\"", "*" };
        for (const QString& brick : bricks) {
            const auto stepsFor = [&](int reps) {
                QString line = QStringLiteral("2026-11-18 13:55:36 ");
                line += brick.repeated(reps);
                qint64 steps = 0;
                t.match(line, &f, -1, nullptr, nullptr, &steps);
                return steps;
            };
            const qint64 s1 = stepsFor(500), s4 = stepsFor(2000);
            check(s4 <= 5 * s1 + 1000,
                  QStringLiteral("«%1» на повторах «%2»: %3 шагов на 500, %4 на 2000 — не линейно")
                      .arg(text, brick).arg(s1).arg(s4));
        }
        std::printf("  шагов на знак и элемент (худшее): %5.2f  %s\n", worst, qPrintable(text));
    }
    std::printf("  случайных строк: %d\n", lines);
}

// ---------------------------------------------------------------------------
// 9. Без аллокаций на строку
// ---------------------------------------------------------------------------

void testNoAllocations()
{
#ifdef DENDRO_COUNT_ALLOCATIONS
    for (const SchemaCase& sc : cases()) {
        if (isKeyedFile(sc.file))
            continue;
        const auto tpl = builtinTemplate(templateFileFor(sc.file));
        if (!tpl)
            continue;
        const TemplateExtractor x(tpl);
        LogEntryFields f;
        x.extract(sc.samples.first().line, &f);   // out — нужного размера
        long long before = 0, after = 0;
        g_allocations.store(0);
        g_countAllocations.store(true);
        before = g_allocations.load();
        for (int rep = 0; rep < 50; ++rep) {
            for (const Sample& s : sc.samples)
                x.extract(s.line, &f);
        }
        after = g_allocations.load();
        g_countAllocations.store(false);
        check(after == before, QStringLiteral("%1: разбор без аллокаций (%2 за %3 строк)")
                                   .arg(sc.file).arg(after - before).arg(50 * sc.samples.size()));
    }
    // Счётчик сам работает.
    g_allocations.store(0);
    g_countAllocations.store(true);
    void* volatile p = std::malloc(16);
    g_countAllocations.store(false);
    std::free(p);
    check(g_allocations.load() >= 1, QStringLiteral("счётчик аллокаций считает"));
#else
    std::printf("  счётчик аллокаций: не glibc или санитайзер — пропущено\n");
#endif
}

// ---------------------------------------------------------------------------
// Замер
// ---------------------------------------------------------------------------

int runBench(int lineCount)
{
    std::printf("Разбор строки: шаблон против регексной схемы (нс на строку, %d строк на формат)\n", lineCount);
    std::printf("%-30s %10s %10s %8s\n", "формат", "регекс", "шаблон", "доля");
    double sumOld = 0, sumNew = 0;
    for (const SchemaCase& sc : cases()) {
        if (isKeyedFile(sc.file))
            continue;
        const auto tpl = builtinTemplate(templateFileFor(sc.file));
        const LogPattern old = blockPattern(sc.file);
        if (!tpl || !old.isValid())
            continue;
        QStringList lines;
        while (lines.size() < lineCount) {
            for (const Sample& s : sc.samples)
                lines << s.line;
        }
        lines = lines.mid(0, lineCount);
        const TemplateExtractor x(tpl);
        LogEntryFields f;
        qint64 sink = 0;
        // Прогрев и три прогона; медиана.
        const auto timeIt = [&](auto&& fn) {
            QVector<qint64> ns;
            fn();
            for (int r = 0; r < 3; ++r) {
                QElapsedTimer t;
                t.start();
                fn();
                ns << t.nsecsElapsed();
            }
            std::sort(ns.begin(), ns.end());
            return double(ns[1]) / lineCount;
        };
        const double tOld = timeIt([&]() {
            for (const QString& l : lines)
                sink += old.extractFields(l).size();
        });
        const double tNew = timeIt([&]() {
            for (const QString& l : lines)
                sink += int(x.extract(l, &f));
        });
        sumOld += tOld;
        sumNew += tNew;
        std::printf("%-30s %10.0f %10.0f %8.3f\n", qPrintable(sc.file), tOld, tNew, tNew / tOld);
        if (sink == 42)
            std::printf(" ");
    }
    std::printf("%-30s %10.0f %10.0f %8.3f  (бюджет 0.35)\n", "сумма", sumOld, sumNew, sumNew / sumOld);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() >= 2 && args.at(1) == QLatin1String("--bench"))
        return runBench(args.size() >= 3 ? args.at(2).toInt() : 200000);

    std::printf("1. таблица видов и правил\n");
    testTable();
    std::printf("2. ошибки компиляции\n");
    testErrors();
    std::printf("3. встроенные шаблоны\n");
    testBuiltinTemplates();
    std::printf("4. конвертер блочных схем\n");
    testConverter();
    std::printf("5. ключевой и прежний извлекатели\n");
    testOtherExtractors();
    std::printf("6. SchemaFit на интерфейсе\n");
    testSchemaFit();
    std::printf("7. фаззинг шаблон → строки → поля\n");
    testFuzzRoundTrip();
    std::printf("8. случайные строки, линейность\n");
    testLinearSteps();
    std::printf("9. без аллокаций\n");
    testNoAllocations();

    std::printf("%d проверок, %d провалов\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
