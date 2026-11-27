// Правка шаблона на строке (F-17, шаг 10; docs/design/F-17.md, разделы 2 и 8).
//
//   1. Make Field: «шаблон + образцы + выделение → новый шаблон» по каждой
//      гипотезе конца поля — литерал сразу за выделением, форма самого
//      выделения, форма следом, промежуток из пробелов, ширина, число слов,
//      закрытый набор значений; выделение без свидетельства — список из
//      одного значения и сообщение.
//   2. Выбор: подтверждённая свидетельством раньше, затем покрытие, затем
//      меньше разных значений нового поля (таблица better и сценарий, где
//      более раннее свидетельство проигрывает по покрытию).
//   3. «Ends at»: у каждого пункта — итог на образцах, текущий вид отмечен,
//      недоступные названы словами.
//   4. Простые операции: имя, Hide, Remove, Merge with Next, Add Value…
//      (значения — по не подошедшим строкам).
//   5. Результат всегда компилируется — на случайных выделениях, колонках и
//      операциях над встроенными шаблонами и их образцами.
//   6. Окно SchemaEditorWindow: Make Field выделением в ячейке, Other
//      Variant, меню заголовка (Ends at, Merge with Next, Remove, Hide,
//      Rename, Add Value…), клавиатура; слияние и удаление обратимы
//      (QUndoStack); самое долгое событие GUI-потока при пересчёте ≤ 50 мс
//      (под санитайзером — печать).

#include "appsettings.h"
#include "fieldschemacontroller.h"
#include "LogListView.h"
#include "logmodel.h"
#include "logviewwidget.h"
#include "linetemplate.h"
#include "schemaedit.h"
#include "schemaeditorwindow.h"
#include "schemafit.h"
#include "schemasamplemodel.h"

#include "builtinsamples.h"

#include <QAbstractItemDelegate>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSettings>
#include <QTemporaryDir>
#include <QTableView>
#include <QTest>
#include <QUndoStack>

#include <QThread>

#include <cstdio>
#include <functional>

namespace {

int g_failures = 0;
int g_checks = 0;

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

#if !defined(NDEBUG)
constexpr bool kSanitized = true;
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

bool waitFor(const std::function<bool()>& pred, int ms = 60000)
{
    QElapsedTimer t;
    t.start();
    while (!pred()) {
        if (t.elapsed() > ms)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return true;
}

void check(bool ok, const QString& what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "[FAIL] %s\n", qPrintable(what));
        std::fflush(stderr);
    }
}

using SchemaEdit::Evidence;
using Samples = QVector<SchemaFit::Sample>;

SchemaEdit::Options opts()
{
    SchemaEdit::Options o;
    o.budget.maxMs = 0;   // без предела времени: под санитайзером оценка медленнее
    return o;
}

class Gen {
public:
    explicit Gen(quint32 seed) : m_rng(seed) {}
    int num(int lo, int hi) { return lo + int(m_rng.bounded(quint32(hi - lo + 1))); }
    QString pick(const QStringList& v) { return v[num(0, int(v.size()) - 1)]; }
    QString ts()
    {
        m_sec += num(0, 3);
        return QStringLiteral("2026-11-18 %1:%2:%3")
            .arg(9 + (m_sec / 3600) % 12, 2, 10, QLatin1Char('0'))
            .arg((m_sec / 60) % 60, 2, 10, QLatin1Char('0'))
            .arg(m_sec % 60, 2, 10, QLatin1Char('0'));
    }
    QString level() { return pick({"INFO", "INFO", "INFO", "WARN", "ERROR", "DEBUG"}); }
    QString words(int n)
    {
        static const QStringList kWords = {
            "processing", "order", "for", "user", "started", "finished", "request", "timeout", "retry",
            "connection", "pool", "resized", "cache", "miss", "hit", "queue", "drained", "token", "refresh",
            "payment", "accepted", "declined", "invoice", "sent", "mail", "delivered", "batch", "flushed",
            "session", "opened", "closed", "lock", "acquired", "released", "disk", "full", "job", "scheduled",
        };
        QStringList out;
        for (int i = 0; i < n; ++i)
            out << (num(0, 4) == 0 ? QString::number(num(1, 9999)) : pick(kWords));
        return out.join(QLatin1Char(' '));
    }

private:
    QRandomGenerator m_rng;
    int m_sec = 0;
};

Samples samplesOf(const QStringList& lines)
{
    Samples out;
    for (int i = 0; i < lines.size(); ++i)
        out.append(SchemaFit::Sample{i + 1, lines[i]});
    return out;
}

bool compiles(const QString& text)
{
    return !text.isEmpty() && LineTemplate(text).isValid();
}

// Выделение по тексту: первое вхождение sel в строке.
SchemaEdit::MakeFieldResult makeFieldOn(const QString& tpl, const QString& line, const QString& sel,
                                        const Samples& samples)
{
    const qsizetype at = line.indexOf(sel);
    if (at < 0) {
        check(false, QStringLiteral("selection «%1» not in «%2»").arg(sel, line));
        return {};
    }
    return SchemaEdit::makeField(tpl, line, at, sel.size(), samples, opts());
}

void expectBest(const char* what, const SchemaEdit::MakeFieldResult& r, Evidence evidence,
                const QString& expectedTemplate)
{
    const QString name = QString::fromLatin1(what);
    check(r.ok(), name + QStringLiteral(": no variants (%1)").arg(r.error));
    if (!r.ok())
        return;
    const auto& v = r.variants.first();
    check(v.evidence == evidence, name + QStringLiteral(": best evidence %1, expected %2 (%3)")
                                             .arg(int(v.evidence)).arg(int(evidence)).arg(v.templateText));
    check(v.supported, name + QStringLiteral(": best variant is not supported"));
    check(!r.noEvidence, name + QStringLiteral(": noEvidence"));
    if (!expectedTemplate.isEmpty())
        check(v.templateText == expectedTemplate,
              name + QStringLiteral(": template «%1», expected «%2»").arg(v.templateText, expectedTemplate));
    for (const auto& x : r.variants)
        check(compiles(x.templateText), name + QStringLiteral(": variant does not compile: ") + x.templateText);
    std::printf("  %-14s %s\n", what, qPrintable(v.templateText));
    for (int i = 1; i < r.variants.size(); ++i)
        std::printf("  %-14s   other: %s [%s%s]\n", "", qPrintable(r.variants[i].templateText),
                    qPrintable(r.variants[i].summary()), r.variants[i].supported ? "" : ", no evidence");
}

const SchemaEdit::Variant* variantOf(const SchemaEdit::MakeFieldResult& r, Evidence e)
{
    for (const auto& v : r.variants) {
        if (v.evidence == e)
            return &v;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// 1. Гипотезы Make Field
// ---------------------------------------------------------------------------

const QString kHead = QStringLiteral("{Timestamp:time} {Level:level} {Message}");

void testLiteral()
{
    Gen g(11);
    const QStringList threads = {"main", "worker-1", "worker-2", "http-nio-8080-exec-3", "scheduler", "pool-4-thread-1"};
    QStringList lines;
    for (int i = 0; i < 300; ++i)
        lines << QStringLiteral("%1 %2 [%3] %4").arg(g.ts(), g.level(), g.pick(threads), g.words(g.num(2, 7)));
    const QString line = QStringLiteral("2026-11-18 09:00:01 INFO [worker-2] cache miss for user 7");
    const auto r = makeFieldOn(kHead, line, QStringLiteral("worker-2"), samplesOf(lines));
    expectBest("literal", r, Evidence::Literal, QStringLiteral("{Timestamp:time} {Level:level} [{Field1}] {Message}"));
    if (r.ok())
        check(r.variants.first().distinct() == threads.size(),
              QStringLiteral("literal: %1 thread values").arg(r.variants.first().distinct()));
}

void testOwnForm()
{
    Gen g(12);
    QStringList lines;
    for (int i = 0; i < 300; ++i)
        lines << QStringLiteral("%1 %2 pid=%3 %4").arg(g.ts(), g.level()).arg(g.num(100, 99999)).arg(g.words(g.num(2, 6)));
    const QString line = QStringLiteral("2026-11-18 09:00:01 INFO pid=4242 job scheduled");
    const auto r = makeFieldOn(kHead, line, QStringLiteral("4242"), samplesOf(lines));
    expectBest("form", r, Evidence::Form, QStringLiteral("{Timestamp:time} {Level:level} pid={Field1:int} {Message}"));
}

void testFormFollows()
{
    Gen g(13);
    const QStringList comps = {"Auth", "DB", "Payment Gateway", "Inventory Sync Worker", "Order Service", "Mail Sender"};
    QStringList lines;
    for (int i = 0; i < 300; ++i)
        lines << QStringLiteral("%1 %2 %3 %4").arg(g.ts(), g.pick(comps), g.level(), g.words(g.num(2, 7)));
    const QString tpl = QStringLiteral("{Timestamp:time} {Message}");
    const QString line = QStringLiteral("2026-11-18 09:00:01 Order Service WARN retry 2 of 5");
    const auto r = makeFieldOn(tpl, line, QStringLiteral("Order Service"), samplesOf(lines));
    expectBest("form follows", r, Evidence::FormFollows,
               QStringLiteral("{Timestamp:time} {Field1} {Level:level} {Message}"));
}

void testGap()
{
    Gen g(14);
    const QStringList comps = {"auth", "db pool", "payment gateway", "inventory sync worker", "orders", "mail"};
    QStringList lines;
    for (int i = 0; i < 300; ++i)
        lines << QStringLiteral("%1 %2 %3%4%5").arg(g.ts(), g.level(), g.pick(comps), QString(g.num(2, 5), QLatin1Char(' ')),
                                                    g.words(g.num(2, 7)));
    const QString line = QStringLiteral("2026-11-18 09:00:01 INFO payment gateway   token refresh");
    const auto r = makeFieldOn(kHead, line, QStringLiteral("payment gateway"), samplesOf(lines));
    expectBest("gap", r, Evidence::Gap, QStringLiteral("{Timestamp:time} {Level:level} {Field1:padded} {Message}"));
}

void testWidth()
{
    Gen g(15);
    // Колонка в 15 знаков: значения короче добиты пробелами, значение в 14
    // знаков отделено одним пробелом — промежутка «два пробела» у части строк
    // нет, а позиция следующего поля одна.
    const QStringList comps = {"auth", "db", "payment-gw-0001", "inventory-sync", "orders", "mail-sender-01"};
    QStringList lines;
    for (int i = 0; i < 300; ++i) {
        QString c = g.pick(comps);
        lines << QStringLiteral("%1 %2 %3 %4").arg(g.ts(), g.level(), c.leftJustified(15), g.words(g.num(2, 7)));
    }
    const QString line = QStringLiteral("2026-11-18 09:00:01 INFO inventory-sync  token refresh");
    const auto r = makeFieldOn(kHead, line, QStringLiteral("inventory-sync"), samplesOf(lines));
    // payment-gw-0001 — 15 знаков, вплотную к пробелу; mail-sender-01 — 14.
    expectBest("width", r, Evidence::Width, QStringLiteral("{Timestamp:time} {Level:level} {Field1:width(15)} {Message}"));
}

void testWords()
{
    Gen g(16);
    const QStringList comps = {"Order Service", "Payment Gateway", "Mail Sender", "Inventory Sync", "Auth Server",
                               "Report Builder", "Cache Warmer", "Token Keeper"};
    QStringList lines;
    for (int i = 0; i < 300; ++i)
        lines << QStringLiteral("%1 %2 %3 %4").arg(g.ts(), g.level(), g.pick(comps), g.words(g.num(2, 7)));
    const QString line = QStringLiteral("2026-11-18 09:00:01 INFO Report Builder job scheduled");
    const auto r = makeFieldOn(kHead, line, QStringLiteral("Report Builder"), samplesOf(lines));
    expectBest("words", r, Evidence::Words, QStringLiteral("{Timestamp:time} {Level:level} {Field1:words(2)} {Message}"));
    // Набор значений тоже подтверждён — и столько же значений: проще (число
    // слов) — первым.
    if (const auto* v = variantOf(r, Evidence::Values)) {
        check(v->supported, QStringLiteral("words: values variant supported"));
        check(v->distinct() == r.variants.first().distinct(), QStringLiteral("words: same distinct"));
    } else {
        check(false, QStringLiteral("words: values variant missing"));
    }
}

void testValues()
{
    Gen g(17);
    const QStringList comps = {"Auth", "DB", "Payment Gateway", "Inventory Sync Worker", "Order Service", "Mail Sender"};
    QStringList lines;
    for (int i = 0; i < 300; ++i)
        lines << QStringLiteral("%1 %2 %3 %4").arg(g.ts(), g.level(), g.pick(comps), g.words(g.num(2, 7)));
    const QString line = QStringLiteral("2026-11-18 09:00:01 INFO Order Service processing order 42");
    const auto r = makeFieldOn(kHead, line, QStringLiteral("Order Service"), samplesOf(lines));
    expectBest("values", r, Evidence::Values, QString());
    if (!r.ok())
        return;
    const auto& best = r.variants.first();
    check(best.distinct() == comps.size(), QStringLiteral("values: %1 values").arg(best.distinct()));
    check(best.templateText.startsWith(QStringLiteral("{Timestamp:time} {Level:level} {Field1:of(")),
          QStringLiteral("values: ") + best.templateText);
    for (const QString& c : comps)
        check(best.templateText.contains(c), QStringLiteral("values: %1 listed").arg(c));
    // words(2) — тоже вариант, но значений больше («words(2): N values ·
    // listed values: 6 values»).
    const auto* w = variantOf(r, Evidence::Words);
    check(w && !w->supported && w->distinct() > comps.size(),
          QStringLiteral("values: words(2) has more values (%1)").arg(w ? w->summary() : QString()));
    if (w)
        check(w->summary().startsWith(QStringLiteral("words(2): ")), QStringLiteral("summary: ") + w->summary());
    check(best.summary().contains(QStringLiteral("6 values")), QStringLiteral("summary: ") + best.summary());
}

void testNoEvidence()
{
    Gen g(18);
    QStringList lines;
    for (int i = 0; i < 300; ++i)
        lines << QStringLiteral("%1 %2 %3").arg(g.ts(), g.level(), g.words(g.num(3, 9)));
    const QString line = QStringLiteral("2026-11-18 09:00:01 INFO lock acquired for session 77");
    const auto r = makeFieldOn(kHead, line, QStringLiteral("lock acquired"), samplesOf(lines));
    check(r.ok(), QStringLiteral("no evidence: a variant anyway (%1)").arg(r.error));
    if (!r.ok())
        return;
    check(r.noEvidence, QStringLiteral("no evidence: flag"));
    check(r.variants.first().evidence == Evidence::SingleValue, QStringLiteral("no evidence: single value first"));
    check(r.variants.first().templateText
              == QStringLiteral("{Timestamp:time} {Level:level} {Field1:of(lock acquired)} {Message}"),
          QStringLiteral("no evidence: ") + r.variants.first().templateText);
    check(r.message.contains(QStringLiteral("Add Value")), QStringLiteral("no evidence: message ") + r.message);
    for (const auto& v : r.variants) {
        check(!v.supported, QStringLiteral("no evidence: supported ") + v.templateText);
        check(compiles(v.templateText), QStringLiteral("no evidence: compiles ") + v.templateText);
    }
    std::printf("  %-14s %s\n  %-14s %s\n", "no evidence", qPrintable(r.variants.first().templateText), "",
                qPrintable(r.message));
}

void testErrors()
{
    const Samples s = samplesOf({QStringLiteral("2026-11-18 09:00:01 INFO hello world")});
    const QString line = s.first().text;
    // В литерале шаблона.
    auto r = SchemaEdit::makeField(QStringLiteral("{Timestamp:time} [{Level:level}] {Message}"),
                                   QStringLiteral("2026-11-18 09:00:01 [INFO] hello"), 20, 1, s, opts());
    check(!r.ok() && !r.error.isEmpty(), QStringLiteral("error: selection in a literal"));
    // Пустое выделение.
    r = SchemaEdit::makeField(kHead, line, 5, 0, s, opts());
    check(!r.ok() && !r.error.isEmpty(), QStringLiteral("error: empty selection"));
    // Значение целиком.
    r = SchemaEdit::makeField(kHead, line, line.indexOf(QStringLiteral("hello")), 11, s, opts());
    check(!r.ok() && r.error.contains(QStringLiteral("whole value")), QStringLiteral("error: whole value ") + r.error);
    // Шаблон с ошибкой.
    r = SchemaEdit::makeField(QStringLiteral("{Timestamp:time"), line, 0, 4, s, opts());
    check(!r.ok(), QStringLiteral("error: invalid template"));
    // Строка не разобрана.
    r = SchemaEdit::makeField(kHead, QStringLiteral("hello world"), 0, 5, s, opts());
    check(!r.ok(), QStringLiteral("error: line not parsed"));
}

// ---------------------------------------------------------------------------
// 2. Выбор по покрытию и числу значений
// ---------------------------------------------------------------------------

SchemaEdit::Variant fakeVariant(Evidence e, bool supported, int full, int partial, int distinct)
{
    SchemaEdit::Variant v;
    v.evidence = e;
    v.supported = supported;
    v.column = 0;
    v.fit.valid = true;
    v.fit.candidates = 100;
    v.fit.full = full;
    v.fit.partial = partial;
    v.fit.distinct = {distinct};
    return v;
}

void testChoice()
{
    using SchemaEdit::better;
    // Подтверждённая — раньше неподтверждённой, даже с худшим покрытием.
    check(better(fakeVariant(Evidence::Values, true, 80, 0, 6), fakeVariant(Evidence::Literal, false, 100, 0, 6)),
          QStringLiteral("choice: supported first"));
    // Покрытие раньше числа значений и порядка свидетельств.
    check(better(fakeVariant(Evidence::Values, true, 100, 0, 40), fakeVariant(Evidence::Literal, true, 90, 0, 3)),
          QStringLiteral("choice: coverage first"));
    // Покрытие в пределах шага — меньше значений.
    check(better(fakeVariant(Evidence::Values, true, 99, 0, 6), fakeVariant(Evidence::Words, true, 100, 0, 14)),
          QStringLiteral("choice: fewer values within the coverage step"));
    // Всё равно — порядок свидетельств.
    check(better(fakeVariant(Evidence::Words, true, 100, 0, 6), fakeVariant(Evidence::Values, true, 100, 0, 6)),
          QStringLiteral("choice: evidence order on a tie"));
    check(!better(fakeVariant(Evidence::Values, true, 100, 0, 6), fakeVariant(Evidence::Words, true, 100, 0, 6)),
          QStringLiteral("choice: strict"));

    // Сценарий: промежуток из пробелов (раньше по порядку) есть у 96 %
    // строк, у остальных за названием один пробел — набор значений разбирает
    // все строки и выигрывает по покрытию.
    Gen g(21);
    const QStringList comps = {"auth", "db pool", "payment gateway", "inventory sync worker", "orders", "mail"};
    QStringList lines;
    for (int i = 0; i < 300; ++i)
        lines << QStringLiteral("%1 %2 %3%4%5").arg(g.ts(), g.level(), g.pick(comps),
                                                    QString(i % 25 == 0 ? 1 : g.num(2, 4), QLatin1Char(' ')),
                                                    g.words(g.num(2, 6)));
    const QString line = QStringLiteral("2026-11-18 09:00:01 INFO payment gateway   token refresh");
    const auto r = makeFieldOn(kHead, line, QStringLiteral("payment gateway"), samplesOf(lines));
    // Покрытие то же (padded берёт до промежутка или до конца строки), но
    // разных значений у него больше — выбран набор значений.
    expectBest("distinct", r, Evidence::Values, QString());
    const auto* gap = variantOf(r, Evidence::Gap);
    check(gap && gap->supported && gap->distinct() > r.variants.first().distinct(),
          QStringLiteral("distinct: the gap variant is supported, but has more values"));

    // Сценарий: колонка постоянной ширины (раньше по порядку) у 96 % строк, у
    // остальных за значением один пробел — ширина их не разбирает, одно
    // слово — разбирает: ширина проигрывает по покрытию.
    const QStringList names = {"auth", "db", "payment-gw-0001", "inventory-sync", "orders", "mail-sender-01"};
    QStringList aligned;
    for (int i = 0; i < 300; ++i) {
        const QString c = g.pick(names);
        aligned << QStringLiteral("%1 %2 %3 %4").arg(g.ts(), g.level(), i % 25 == 0 ? c : c.leftJustified(15),
                                                     g.words(g.num(2, 6)));
    }
    const QString line2 = QStringLiteral("2026-11-18 09:00:01 INFO inventory-sync  token refresh");
    const auto r2 = makeFieldOn(kHead, line2, QStringLiteral("inventory-sync"), samplesOf(aligned));
    expectBest("coverage", r2, Evidence::Words, QStringLiteral("{Timestamp:time} {Level:level} {Field1:word} {Message}"));
    const auto* width = variantOf(r2, Evidence::Width);
    check(width && width->supported && width->fit.coverage() < r2.variants.first().fit.coverage(),
          QStringLiteral("coverage: the width variant is supported, but parses fewer lines"));
}

// ---------------------------------------------------------------------------
// 3. Ends at
// ---------------------------------------------------------------------------

void testEndOptions()
{
    Gen g(31);
    const QStringList comps = {"Auth", "DB", "Payment Gateway", "Inventory Sync Worker", "Order Service", "Mail Sender"};
    QStringList lines;
    for (int i = 0; i < 300; ++i)
        lines << QStringLiteral("%1 %2 %3 - %4").arg(g.ts(), g.level(), g.pick(comps), g.words(g.num(2, 7)));
    const Samples s = samplesOf(lines);
    const QString tpl = QStringLiteral("{Timestamp:time} {Level:level} {Component} - {Message}");
    const auto options = SchemaEdit::endOptions(tpl, 2, s, opts());
    using C = SchemaEdit::EndOption::Choice;
    QHash<int, SchemaEdit::EndOption> by;
    int currents = 0;
    for (const auto& o : options) {
        by.insert(int(o.choice), o);
        currents += o.current ? 1 : 0;
        if (o.available)
            check(compiles(o.templateText), QStringLiteral("ends at: compiles ") + o.templateText);
        else
            check(!o.reason.isEmpty(), QStringLiteral("ends at: reason for ") + o.label);
        std::printf("  ends at %-28s %s%s\n", qPrintable(o.label), qPrintable(o.summary()), o.current ? "  (current)" : "");
    }
    check(currents == 1, QStringLiteral("ends at: one current (%1)").arg(currents));
    check(by.value(int(C::Literal)).available && by.value(int(C::Literal)).current
              && by.value(int(C::Literal)).label == QStringLiteral("at “-”"),
          QStringLiteral("ends at: literal current"));
    check(by.value(int(C::Literal)).distinct() == comps.size(), QStringLiteral("ends at: literal 6 values"));
    check(by.value(int(C::Values)).available && by.value(int(C::Values)).distinct() == comps.size(),
          QStringLiteral("ends at: values 6"));
    check(by.value(int(C::Words)).available && by.value(int(C::Words)).kind == QStringLiteral("words(2)"),
          QStringLiteral("ends at: words(2)"));
    check(by.value(int(C::Words)).summary().startsWith(QStringLiteral("words(2): ")),
          QStringLiteral("ends at: summary ") + by.value(int(C::Words)).summary());
    check(!by.value(int(C::LineEnd)).available, QStringLiteral("ends at: rest not available in the middle"));
    check(!by.value(int(C::Form)).available, QStringLiteral("ends at: names have no form"));
    // Message в конце: «до конца строки» — текущий.
    const auto last = SchemaEdit::endOptions(tpl, 3, s, opts());
    bool restCurrent = false;
    for (const auto& o : last)
        restCurrent = restCurrent || (o.choice == C::LineEnd && o.current && o.available);
    check(restCurrent, QStringLiteral("ends at: message is rest"));
    // Числа: форма.
    QStringList nums;
    for (int i = 0; i < 100; ++i)
        nums << QStringLiteral("%1 %2 %3 done").arg(g.ts(), g.level()).arg(g.num(1, 99999));
    const auto numOptions = SchemaEdit::endOptions(QStringLiteral("{Timestamp:time} {Level:level} {N:word} {Message}"), 2,
                                                   samplesOf(nums), opts());
    bool form = false;
    for (const auto& o : numOptions)
        form = form || (o.choice == C::Form && o.available && o.kind == QStringLiteral("int"));
    check(form, QStringLiteral("ends at: int form"));
}

// ---------------------------------------------------------------------------
// 4. Простые операции
// ---------------------------------------------------------------------------

void testOperations()
{
    Gen g(41);
    const QStringList comps = {"Auth", "DB", "Payment Gateway", "Order Service"};
    QStringList lines;
    for (int i = 0; i < 200; ++i)
        lines << QStringLiteral("%1 %2 [%3] %4").arg(g.ts(), g.level(), g.pick({"main", "worker-1"}), g.words(g.num(2, 6)));
    const Samples s = samplesOf(lines);
    const QString tpl = QStringLiteral("{Timestamp:time} {Level:level} [{Thread}] {Message}");

    auto e = SchemaEdit::rename(tpl, 2, QStringLiteral("Worker"));
    check(e.ok() && e.templateText == QStringLiteral("{Timestamp:time} {Level:level} [{Worker}] {Message}"),
          QStringLiteral("rename: ") + e.templateText + e.error);
    check(!SchemaEdit::rename(tpl, 2, QStringLiteral("level")).ok(), QStringLiteral("rename: duplicate (case)"));
    check(!SchemaEdit::rename(tpl, 2, QStringLiteral("a:b")).ok(), QStringLiteral("rename: bad char"));
    check(!SchemaEdit::rename(tpl, 2, QStringLiteral(" ")).ok(), QStringLiteral("rename: empty"));

    e = SchemaEdit::hide(tpl, 2);
    check(e.ok() && e.templateText == QStringLiteral("{Timestamp:time} {Level:level} [{_}] {Message}"),
          QStringLiteral("hide: ") + e.templateText + e.error);
    check(LineTemplate(e.templateText).columns() == QStringList({"Timestamp", "Level", "Message"}),
          QStringLiteral("hide: columns"));

    e = SchemaEdit::mergeWithNext(tpl, 2, s, opts());
    check(e.ok() && e.templateText == QStringLiteral("{Timestamp:time} {Level:level} [{Thread}"),
          QStringLiteral("merge: ") + e.templateText + e.error);
    if (e.ok()) {
        LogEntryFields f;
        const LineTemplate t(e.templateText);
        const QString line = QStringLiteral("2026-11-18 09:00:01 INFO [main] hello there");
        t.match(line, &f);
        check(f.value(2, line) == QStringLiteral("main] hello there"), QStringLiteral("merge: value ") + f.value(2, line));
    }
    check(!SchemaEdit::mergeWithNext(tpl, 3, s, opts()).ok(), QStringLiteral("merge: last field"));
    // Через необязательный участок — нельзя разрезать.
    check(!SchemaEdit::mergeWithNext(QStringLiteral("{A:word}{? [{B}]?} {C}"), 1, s, opts()).ok(),
          QStringLiteral("merge: across an optional part"));

    e = SchemaEdit::remove(tpl, 2, s, opts());   // Thread → в Level? предыдущее — Level
    check(e.ok() && LineTemplate(e.templateText).columns() == QStringList({"Timestamp", "Level", "Message"}),
          QStringLiteral("remove: ") + e.templateText + e.error);
    e = SchemaEdit::remove(tpl, 0, s, opts());   // первое — в следующее
    check(e.ok() && LineTemplate(e.templateText).columns() == QStringList({"Level", "Thread", "Message"}),
          QStringLiteral("remove first: ") + e.templateText + e.error);
    check(!SchemaEdit::remove(QStringLiteral("{Message}"), 0, s, opts()).ok(), QStringLiteral("remove: the only field"));
    // Сосед скрыт — колонка уходит, а не остаётся под своим именем.
    e = SchemaEdit::remove(QStringLiteral("{_:time} {Thread:word} {Message}"), 0, s, opts());
    check(e.ok() && LineTemplate(e.templateText).columns() == QStringList({"Message"}),
          QStringLiteral("remove next to a hidden field: ") + e.templateText + e.error);

    // Add Value…: значения из строк, где разбор остановился на поле.
    QStringList vl;
    for (int i = 0; i < 200; ++i)
        vl << QStringLiteral("%1 %2 %3 %4").arg(g.ts(), g.level(), i % 10 == 0 ? QStringLiteral("Billing Engine") : g.pick(comps),
                                                 g.words(g.num(2, 6)));
    const QString ofTpl = QStringLiteral("{Timestamp:time} {Level:level} {Component:of(Auth|DB|Payment Gateway|Order Service)} {Message}");
    const QStringList suggested = SchemaEdit::suggestValues(ofTpl, 2, samplesOf(vl), opts());
    check(!suggested.isEmpty() && suggested.first() == QStringLiteral("Billing Engine"),
          QStringLiteral("suggest: ") + suggested.join(QLatin1Char('|')));
    // Поле в конце строки: значение из нескольких слов до конца строки.
    QStringList tailLines;
    for (int i = 0; i < 60; ++i)
        tailLines << QStringLiteral("%1 %2 %3").arg(g.ts(), g.level(), i % 4 == 0 ? QStringLiteral("lock released")
                                                                                   : QStringLiteral("lock acquired"));
    const QStringList tail = SchemaEdit::suggestValues(
        QStringLiteral("{Timestamp:time} {Level:level} {Event:of(lock acquired)}"), 2, samplesOf(tailLines), opts());
    check(tail.value(0) == QStringLiteral("lock released"), QStringLiteral("suggest at the line end: ") + tail.join(QLatin1Char('|')));
    e = SchemaEdit::addValue(ofTpl, 2, QStringLiteral("Billing Engine"));
    check(e.ok() && e.templateText.contains(QStringLiteral("Order Service|Billing Engine)")),
          QStringLiteral("add value: ") + e.templateText + e.error);
    if (e.ok()) {
        const auto fit = SchemaFit::evaluate(TemplateExtractor(e.templateText), samplesOf(vl));
        check(fit.full == fit.candidates, QStringLiteral("add value: all lines full"));
    }
    check(!SchemaEdit::addValue(ofTpl, 2, QStringLiteral("DB")).ok(), QStringLiteral("add value: duplicate"));
    check(!SchemaEdit::addValue(tpl, 2, QStringLiteral("x")).ok(), QStringLiteral("add value: not a list"));
}

// ---------------------------------------------------------------------------
// 5. Результат всегда компилируется
// ---------------------------------------------------------------------------

void testRoundTrip()
{
    for (const auto& c : builtinsamples::cases()) {
        Q_UNUSED(c);
    }
    const QStringList templates = {
        QStringLiteral("{Timestamp:time} [{Thread}] {Level:level} {Logger:word} -{?-?} {Message}"),
        QStringLiteral("{Timestamp:time} {Level:level} {PID:int} --- {?[{_}] ?}[{Thread}] {Logger:word} : {Message}"),
        QStringLiteral("{Client:ip} {_:word} {User:word} [{Time:time}] {Request:quoted} {Status:int} {Bytes:word}{? {Referer:quoted} {User agent:quoted}?}"),
        QStringLiteral("{Timestamp:time} [{Level:of(debug|info|notice|warn|error|crit|alert|emerg)}] {PID:int}#{TID:int}: {?*{Connection:int} ?}{Message}"),
        QStringLiteral("{Id:/[A-Z]{3}-\\/\\d+/} {Rest:width(4)} {W:words(2)} {{x}} {Message}"),
    };
    for (const QString& t : templates) {
        const LineTemplate tpl(t);
        check(tpl.isValid(), QStringLiteral("round trip: source ") + t);
        const QString again = SchemaEdit::render(SchemaEdit::piecesOf(tpl));
        const LineTemplate back(again);
        check(back.isValid() && back.columns() == tpl.columns(),
              QStringLiteral("round trip: %1 → %2").arg(t, again));
    }
}

QStringList builtinTemplates()
{
    return {
        QStringLiteral("{Timestamp:time} [{Thread}] {Level:level} {Logger:word} -{?-?} {Message}"),
        QStringLiteral("{Timestamp:time} {Level:level} {PID:int} --- {?[{_}] ?}[{Thread}] {Logger:word} : {Message}"),
        QStringLiteral("{Client:ip} {_:word} {User:word} [{Time:time}] {Request:quoted} {Status:int} {Bytes:word}{? {Referer:quoted} {User agent:quoted}?}"),
        QStringLiteral("{Timestamp:time} [{Level:of(debug|info|notice|warn|error|crit|alert|emerg)}] {PID:int}#{TID:int}: {?*{Connection:int} ?}{Message}"),
        QStringLiteral("{Timestamp:time} {Host:word} {Program:word}{?[{PID:int}]?}: {Message}"),
        QStringLiteral("[{Timestamp:time} {Level:level}] {Message}"),
        QStringLiteral("{Timestamp:time} [{Level:level}] {Message}"),
        QStringLiteral("{Timestamp:time} - {Logger} - {Level:level} - {Message}"),
        QStringLiteral("{Level:of(CRITICAL|ERROR|WARNING|INFO|DEBUG)}:{Logger}:{Message}"),
    };
}

void testFuzz()
{
    QStringList all;
    for (const auto& c : builtinsamples::cases()) {
        for (const auto& s : c.samples)
            all << s.line;
    }
    const Samples samples = samplesOf(all);
    const QStringList templates = builtinTemplates();
    QRandomGenerator rng(4242);
    int made = 0, refused = 0, ops = 0;
    for (int iter = 0; iter < 400; ++iter) {
        const QString tpl = templates[int(rng.bounded(quint32(templates.size())))];
        const QString line = all[int(rng.bounded(quint32(all.size())))];
        if (line.isEmpty())
            continue;
        const qsizetype a = rng.bounded(quint32(line.size()));
        const qsizetype len = 1 + rng.bounded(quint32(std::min<qsizetype>(24, line.size() - a)));
        const auto r = SchemaEdit::makeField(tpl, line, a, len, samples, opts());
        if (r.ok()) {
            ++made;
            for (const auto& v : r.variants)
                check(compiles(v.templateText), QStringLiteral("fuzz: %1 [%2,+%3] on %4 → %5")
                                                    .arg(tpl).arg(a).arg(len).arg(line, v.templateText));
            check(!r.message.isEmpty(), QStringLiteral("fuzz: message"));
        } else {
            ++refused;
            check(!r.error.isEmpty(), QStringLiteral("fuzz: refusal without words"));
        }
        // Операции над случайной колонкой.
        const int columns = int(LineTemplate(tpl).columns().size());
        const int c = int(rng.bounded(quint32(columns)));
        const Samples few = samples.mid(0, 40);
        const SchemaEdit::Edit edits[] = {
            SchemaEdit::mergeWithNext(tpl, c, few, opts()),
            SchemaEdit::remove(tpl, c, few, opts()),
            SchemaEdit::hide(tpl, c),
            SchemaEdit::rename(tpl, c, QStringLiteral("Renamed")),
        };
        for (const auto& e : edits) {
            ++ops;
            check(e.ok() ? compiles(e.templateText) : !e.error.isEmpty(),
                  QStringLiteral("fuzz op: %1 col %2 → %3 %4").arg(tpl).arg(c).arg(e.templateText, e.error));
        }
        if (iter % 20 == 0) {
            for (const auto& o : SchemaEdit::endOptions(tpl, c, few, opts()))
                check(o.available ? compiles(o.templateText) : !o.reason.isEmpty(),
                      QStringLiteral("fuzz ends at: %1 col %2 %3").arg(tpl).arg(c).arg(o.label));
        }
    }
    std::printf("  fuzz: %d made, %d refused, %d operations\n", made, refused, ops);
    check(made > 50, QStringLiteral("fuzz: some selections become fields (%1)").arg(made));
}

// Цена расчёта в воркере на выборке файла (2300 строк, как у SchemaFitJob):
// печать, не проверка времени.
void testCost()
{
    Gen g(71);
    const QStringList comps = {"Auth", "DB", "Payment Gateway", "Inventory Sync Worker", "Order Service", "Mail Sender"};
    QStringList lines;
    for (int i = 0; i < 2300; ++i)
        lines << QStringLiteral("%1 %2 [%3] %4 %5").arg(g.ts(), g.level(), g.pick({"main", "worker-1", "worker-2"}),
                                                       g.pick(comps), g.words(g.num(2, 8)));
    const Samples s = samplesOf(lines);
    const QString tpl = QStringLiteral("{Timestamp:time} {Level:level} [{Thread}] {Message}");
    const QString line = QStringLiteral("2026-11-18 09:00:01 INFO [main] Order Service processing order 42");
    QElapsedTimer t;
    t.start();
    const auto r = makeFieldOn(tpl, line, QStringLiteral("Order Service"), s);
    const qint64 make = t.elapsed();
    check(r.ok() && r.variants.first().evidence == Evidence::Values, QStringLiteral("cost: values on 2300 lines"));
    t.restart();
    int options = 0;
    for (int c = 0; c < 4; ++c)
        options += int(SchemaEdit::endOptions(tpl, c, s, opts()).size());
    const qint64 ends = t.elapsed();
    t.restart();
    const auto merged = SchemaEdit::mergeWithNext(tpl, 2, s, opts());
    const qint64 merge = t.elapsed();
    check(merged.ok(), QStringLiteral("cost: merge"));
    std::printf("  cost on %d lines (worker): Make Field %lld ms (%d variants), Ends at of 4 columns %lld ms "
                "(%d items), Merge with Next %lld ms\n",
                int(s.size()), make, int(r.variants.size()), ends, options, merge);
}

// ---------------------------------------------------------------------------
// 6. Окно
// ---------------------------------------------------------------------------

int rowStartingWith(SchemaSampleModel* m, int column, const QString& prefix)
{
    for (int r = 0; r < m->rowCount(); ++r) {
        if (m->rawCell(r, column).startsWith(prefix))
            return r;
    }
    return -1;
}

QAction* menuAction(QMenu* menu, const QString& name)
{
    for (QAction* a : menu->actions()) {
        if (a->objectName() == name)
            return a;
        if (a->menu()) {
            if (QAction* sub = menuAction(a->menu(), name))
                return sub;
        }
    }
    return nullptr;
}

void testWindow(TimingApplication& app)
{
    QTemporaryDir tmp;
    Gen g(61);
    const QStringList comps = {"Auth", "DB", "Payment Gateway", "Inventory Sync Worker", "Order Service", "Mail Sender"};
    QByteArray bytes;
    for (int i = 0; i < 6000; ++i)
        bytes += QStringLiteral("%1 %2 %3 %4\n").arg(g.ts(), g.level(), g.pick(comps), g.words(g.num(2, 8))).toUtf8();
    const QString path = QDir(tmp.path()).filePath(QStringLiteral("unknown.log"));
    {
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write(bytes);
    }
    FieldSchemaController c;
    {
        QSettings ini(AppSettings::iniFilePath(), QSettings::IniFormat);
        ini.beginGroup(QStringLiteral("View"));
        c.load(ini);
    }
    LogViewWidget tab;
    c.configureView(&tab);
    c.setCurrentView(&tab);
    tab.addLogFile(path);
    check(waitFor([&] { return !tab.isLoading() && tab.model()->rowCount() == 6000 && !tab.model()->isFiltering(); }),
          QStringLiteral("window: the tab loads"));

    SchemaEditorWindow w(&c);
    w.setAsker([](const QString&, bool saveDiscard) {
        return saveDiscard ? SchemaEditorWindow::Answer::Discard : SchemaEditorWindow::Answer::No;
    });
    QStringList asked;
    QString answer;
    w.setTextAsker([&](const QString& title, const QString&, const QString& text, const QStringList& items) {
        asked << title + QLatin1Char(':') + text + QLatin1Char(':') + items.join(QLatin1Char('|'));
        return answer;
    });
    SchemaFit::Budget noLimit;
    noLimit.maxMs = 0;
    w.setFitBudget(noLimit);
    w.show();
    w.activateWindow();
    QTest::qWaitForWindowExposed(&w);
    w.setView(&tab);
    w.newFromTemplate(kHead, QStringLiteral("Unknown"));
    const auto settled = [&]() { return !w.isComputing() && w.fileFitReady() && w.endOptionsReady(); };
    check(waitFor(settled), QStringLiteral("window: computed"));
    check(w.editSamples().size() >= 2000, QStringLiteral("window: edit samples — the file sample (%1)")
                                               .arg(w.editSamples().size()));

    auto* table = w.findChild<QTableView*>(QStringLiteral("sampleTable"));
    SchemaSampleModel* m = w.sampleModel();
    const int row = rowStartingWith(m, 2, QStringLiteral("Order Service"));
    check(table && row >= 0, QStringLiteral("window: a row with Order Service"));
    if (!table || row < 0)
        return;

    // Make Field выделением в ячейке: F2 открывает поле, Ctrl+M.
    app.timing = true;
    app.maxMs = 0;
    table->setFocus();
    table->setCurrentIndex(m->index(row, 2));
    QTest::keyClick(table, Qt::Key_F2);
    auto* editor = table->findChild<QLineEdit*>(QStringLiteral("cellSelectEdit"));
    check(editor && editor->isReadOnly() && editor->text().startsWith(QStringLiteral("Order Service")),
          QStringLiteral("window: F2 opens the selection field"));
    if (!editor)
        return;
    editor->setSelection(0, int(QStringLiteral("Order Service").size()));
    QTest::keyClick(editor, Qt::Key_M, Qt::ControlModifier);
    check(waitFor(settled), QStringLiteral("window: Make Field computed"));
    const QString made = w.templateText();
    check(made.startsWith(QStringLiteral("{Timestamp:time} {Level:level} {Field1:of(")) && made.contains(QStringLiteral("Payment Gateway")),
          QStringLiteral("window: Make Field → list of values: ") + made);
    check(w.makeFieldResult().ok() && !w.makeFieldResult().noEvidence, QStringLiteral("window: evidence"));
    auto* message = w.findChild<QLabel*>(QStringLiteral("editMessage"));
    check(message && message->text().contains(QStringLiteral("Field1")) && message->text().contains(QStringLiteral("6 values")),
          QStringLiteral("window: message ") + (message ? message->text() : QString()));
    check(m->columns().value(2) == QStringLiteral("Field1") && m->distinctCount(2) == comps.size(),
          QStringLiteral("window: the new column has 6 values (%1)").arg(m->distinctCount(2)));
    check(m->headerData(2, Qt::Horizontal, Qt::DisplayRole).toString() == QStringLiteral("Field1 · 6"),
          QStringLiteral("window: header ") + m->headerData(2, Qt::Horizontal, Qt::DisplayRole).toString());
    check(m->headerData(2, Qt::Horizontal, Qt::ToolTipRole).toString().contains(QStringLiteral("Order Service")),
          QStringLiteral("window: tooltip lists frequent values"));
    app.timing = false;
    std::printf("  window: the longest GUI event while computing (Make Field, Ends at, samples; %d lines) — %lld ms (%s)\n",
                int(w.editSamples().size()), app.maxMs, qPrintable(app.maxWhat));
    if (!kSanitized)
        check(app.maxMs <= 50, QStringLiteral("window: the longest GUI event %1 ms (%2)").arg(app.maxMs).arg(app.maxWhat));

    // Other Variant — тот же шаг отмены.
    const int undoCount = w.undoStack()->count();
    check(w.makeFieldResult().variants.size() >= 2, QStringLiteral("window: other variants exist"));
    auto* other = w.findChild<QPushButton*>(QStringLiteral("otherVariantBtn"));
    check(other && other->isEnabled(), QStringLiteral("window: Other Variant enabled"));
    if (other)
        other->click();
    check(w.templateText() == w.makeFieldResult().variants.value(1).templateText && w.undoStack()->count() == undoCount,
          QStringLiteral("window: Other Variant replaces the step: ") + w.templateText());
    QTest::keyClick(&w, Qt::Key_M, Qt::ControlModifier | Qt::ShiftModifier);
    check(w.makeFieldResult().variants.size() >= 2
              && w.variantIndex() == 2 % int(w.makeFieldResult().variants.size()),
          QStringLiteral("window: Ctrl+Shift+M"));
    w.undoStack()->undo();
    check(w.templateText() == kHead, QStringLiteral("window: undo Make Field → ") + w.templateText());
    w.undoStack()->redo();
    check(waitFor(settled), QStringLiteral("window: redo computed"));
    check(w.templateText() != kHead, QStringLiteral("window: redo Make Field"));
    // Прямо — первый вариант.
    w.setTemplateText(made);
    check(waitFor(settled), QStringLiteral("window: back to the best variant"));

    // Ends at: меню колонки, итог у каждого пункта.
    std::unique_ptr<QMenu> menu(w.columnMenu(2));
    QMenu* ends = menu->findChild<QMenu*>(QStringLiteral("endsAtMenu"));
    check(ends && !ends->actions().isEmpty(), QStringLiteral("window: Ends at submenu"));
    QAction* words = nullptr;
    int checked = 0;
    if (ends) {
        for (QAction* a : ends->actions()) {
            checked += a->isChecked() ? 1 : 0;
            if (a->text().contains(QStringLiteral("words(2): ")))
                words = a;
        }
    }
    check(checked == 1, QStringLiteral("window: the current end is checked"));
    check(words && words->isEnabled() && words->text().contains(QStringLiteral("values")),
          QStringLiteral("window: «after 2 words — words(2): N values»"));
    if (words)
        words->trigger();
    check(w.templateText().contains(QStringLiteral("{Field1:words(2)}")), QStringLiteral("window: Ends at → words(2): ") + w.templateText());
    w.undoStack()->undo();
    check(w.templateText() == made, QStringLiteral("window: undo Ends at"));
    check(waitFor(settled), QStringLiteral("window: settled after Ends at"));

    // Rename… и Hide из меню.
    answer = QStringLiteral("Component");
    menu.reset(w.columnMenu(2));
    if (QAction* a = menuAction(menu.get(), QStringLiteral("renameFieldAction")))
        a->trigger();
    check(LineTemplate(w.templateText()).columns().value(2) == QStringLiteral("Component"),
          QStringLiteral("window: Rename… → ") + w.templateText());
    check(waitFor(settled), QStringLiteral("window: settled after rename"));

    // Merge with Next — обратимо.
    const QString beforeMerge = w.templateText();
    menu.reset(w.columnMenu(2));
    if (QAction* a = menuAction(menu.get(), QStringLiteral("mergeFieldAction")))
        a->trigger();
    check(waitFor(settled), QStringLiteral("window: merge computed"));
    check(LineTemplate(w.templateText()).columns() == QStringList({"Timestamp", "Level", "Component"}),
          QStringLiteral("window: merged: ") + w.templateText());
    w.undoStack()->undo();
    check(w.templateText() == beforeMerge, QStringLiteral("window: undo merge"));
    w.undoStack()->redo();
    check(LineTemplate(w.templateText()).columns().size() == 3, QStringLiteral("window: redo merge"));
    w.undoStack()->undo();
    check(waitFor(settled), QStringLiteral("window: settled after merge"));

    // Remove — обратимо; клавиатурой (Ctrl+Shift+D на текущей колонке).
    table->setFocus();
    table->setCurrentIndex(m->index(0, 2));
    QTest::keyClick(table, Qt::Key_D, Qt::ControlModifier | Qt::ShiftModifier);
    check(waitFor(settled), QStringLiteral("window: remove computed"));
    check(LineTemplate(w.templateText()).columns() == QStringList({"Timestamp", "Level", "Message"}),
          QStringLiteral("window: removed (text to Level): ") + w.templateText());
    w.undoStack()->undo();
    check(w.templateText() == beforeMerge, QStringLiteral("window: undo remove"));
    check(waitFor(settled), QStringLiteral("window: settled after remove"));

    // Hide клавиатурой.
    table->setCurrentIndex(m->index(0, 2));
    QTest::keyClick(table, Qt::Key_H, Qt::ControlModifier | Qt::ShiftModifier);
    check(LineTemplate(w.templateText()).columns() == QStringList({"Timestamp", "Level", "Message"})
              && w.templateText().contains(QStringLiteral("{_:of(")),
          QStringLiteral("window: hidden: ") + w.templateText());
    w.undoStack()->undo();
    check(waitFor(settled), QStringLiteral("window: settled after hide"));

    // Add Value…: список без одного значения — предложение из строк, где
    // разбор остановился на поле.
    QString shortList = beforeMerge;
    shortList.replace(QStringLiteral("|Mail Sender"), QString()).replace(QStringLiteral("Mail Sender|"), QString());
    w.setTemplateText(shortList);
    check(waitFor(settled), QStringLiteral("window: settled with a shorter list"));
    check(!m->misses().isEmpty() && m->misses().first().reason.contains(QStringLiteral("Add Value")),
          QStringLiteral("window: the miss names Add Value… (%1)")
              .arg(m->misses().isEmpty() ? QString() : m->misses().first().reason));
    check(w.suggestedValues(2).value(0) == QStringLiteral("Mail Sender"),
          QStringLiteral("window: suggested ") + w.suggestedValues(2).join(QLatin1Char('|')));
    answer = QStringLiteral("Mail Sender");
    asked.clear();
    table->setCurrentIndex(m->index(0, 2));
    QTest::keyClick(table, Qt::Key_A, Qt::ControlModifier | Qt::ShiftModifier);
    check(asked.size() == 1 && asked.first().contains(QStringLiteral("Mail Sender")),
          QStringLiteral("window: Add Value… asks with the suggestion ") + asked.join(QLatin1Char(';')));
    check(w.templateText().contains(QStringLiteral("Mail Sender")), QStringLiteral("window: value added ") + w.templateText());

    // Кнопка Make Field: щелчок уводит фокус из поля ячейки — выделение
    // запомнено.
    check(waitFor(settled), QStringLiteral("window: settled before the button"));
    const int row2 = rowStartingWith(m, 3, QString());
    table->setFocus();
    table->setCurrentIndex(m->index(row2, 3));
    QTest::keyClick(table, Qt::Key_F2);
    editor = table->findChild<QLineEdit*>(QStringLiteral("cellSelectEdit"));
    auto* makeButton = w.findChild<QPushButton*>(QStringLiteral("makeFieldBtn"));
    check(editor && makeButton, QStringLiteral("window: cell editor and the button"));
    if (editor && makeButton) {
        const int space = int(editor->text().indexOf(QLatin1Char(' ')));
        editor->setSelection(0, space > 0 ? space : 1);
        const QString beforeButton = w.templateText();
        QTest::mouseClick(makeButton, Qt::LeftButton);
        check(waitFor(settled), QStringLiteral("window: button Make Field computed"));
        check(w.templateText() != beforeButton && LineTemplate(w.templateText()).columns().size() == 5,
              QStringLiteral("window: the button uses the selection: ") + w.templateText() + QLatin1Char(' ') + w.editMessage());
    }
    // Other Variant — только для своей команды: отмена и новая правка его гасят.
    const bool hadVariants = w.makeFieldResult().variants.size() > 1;
    w.undoStack()->undo();
    w.setDescription(QStringLiteral("x"));
    check(!other->isEnabled() && !w.otherVariant(), QStringLiteral("window: Other Variant is off after undo + edit"));
    Q_UNUSED(hadVariants);

    // Сочетания колонки — только в таблице: в поле имени Ctrl+Shift+D не
    // удаляет колонку.
    const QString beforeName = w.templateText();
    if (auto* name = w.findChild<QLineEdit*>(QStringLiteral("nameEdit"))) {
        name->setFocus();
        QTest::keyClick(name, Qt::Key_D, Qt::ControlModifier | Qt::ShiftModifier);
        check(waitFor(settled) && w.templateText() == beforeName, QStringLiteral("window: no column shortcut in Name"));
    }

    // Без выделения Make Field говорит, что делать.
    w.newFromTemplate(kHead, QStringLiteral("Again"));
    check(waitFor(settled), QStringLiteral("window: settled again"));
    w.makeFieldFromSelection();
    check(!w.editMessage().isEmpty() && w.templateText() == kHead, QStringLiteral("window: Make Field without a selection explains"));
    w.close();
}

} // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    TimingApplication app(argc, argv);

    std::printf("Make Field:\n");
    testLiteral();
    testOwnForm();
    testFormFollows();
    testGap();
    testWidth();
    testWords();
    testValues();
    testNoEvidence();
    testErrors();
    std::printf("Choice:\n");
    testChoice();
    std::printf("Ends at:\n");
    testEndOptions();
    testOperations();
    testRoundTrip();
    testFuzz();
    testCost();
    std::printf("Window:\n");
    testWindow(app);

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
