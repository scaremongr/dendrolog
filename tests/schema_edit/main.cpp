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
//   7. Графическая правка (U-28) — то же окно без текста шаблона: незнакомый
//      лог разбит на поля сам при открытии; блоки полей, раскрашенные строки
//      и карточка поля согласованы (щелчок по участку строки и по блоку);
//      имя и «где кончается» — из карточки; Make Field — выделением в строке
//      (кнопка у выделения, Ctrl+M), дата и уровень получают свои имена;
//      Detect Fields; скрытая часть строки — снова колонкой; Advanced свёрнут
//      и раскрывается на ошибке шаблона; не подошедшие строки — по кнопке.

#include "appsettings.h"
#include "fieldschemacontroller.h"
#include "LogListView.h"
#include "logmodel.h"
#include "logviewwidget.h"
#include "linetemplate.h"
#include "schemaedit.h"
#include "schemaeditorwindow.h"
#include "schemafieldcard.h"
#include "schemafieldstrip.h"
#include "schemafit.h"
#include "schemalinepreview.h"
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
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRandomGenerator>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTableView>
#include <QTableWidget>
#include <QTest>
#include <QTextBlock>
#include <QToolButton>
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
    // Таблица — второй вид (U-28): первым идут раскрашенные строки.
    auto* views = w.findChild<QTabWidget*>(QStringLiteral("sampleTabs"));
    check(views && views->count() == 2 && views->currentIndex() == 0, QStringLiteral("window: Lines, then Table"));
    if (views)
        views->setCurrentIndex(1);

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


// ---------------------------------------------------------------------------
// 7. Графическая правка (U-28)
// ---------------------------------------------------------------------------

// Заливка участка [start, start + length) блока №line раскрашенных строк.
QColor previewFill(SchemaLinePreview* preview, int line, int start, int length)
{
    const QTextBlock block = preview->document()->findBlockByNumber(line);
    QColor found;
    for (const QTextEdit::ExtraSelection& sel : preview->extraSelections()) {
        if (sel.cursor.selectionStart() == block.position() + start
            && sel.cursor.selectionEnd() == block.position() + start + length)
            found = sel.format.background().color();
    }
    return found;
}

int fieldItems(const SchemaFieldStrip* strip)
{
    int n = 0;
    for (const SchemaFieldStrip::Item& item : strip->items())
        n += item.type == SchemaFieldStrip::Item::Type::Field && item.column >= 0 ? 1 : 0;
    return n;
}

void testGraphical(TimingApplication& app)
{
    QTemporaryDir tmp;
    // Свой каталог настроек: в нём схема каталога, которая этот лог не читает.
    AppSettings::overrideConfigDirForTesting(QDir(tmp.path()).filePath(QStringLiteral("config")));
    Gen g(77);
    const QStringList comps = {"Auth", "DB", "Payment Gateway", "Inventory Sync Worker", "Order Service", "Mail Sender"};
    QByteArray bytes;
    for (int i = 0; i < 3000; ++i) {
        if (i == 7 || i == 90) {
            // Запись другого вида (метка в скобках — схема её не прочтёт) с
            // продолжением и запись, прочитанная только до половины.
            bytes += QStringLiteral("[%1] restart requested\n    at com.example.Main.run(Main.java:42)\n").arg(g.ts()).toUtf8();
            bytes += QStringLiteral("%1 | INFO  | shutting down\n").arg(g.ts()).toUtf8();
            continue;
        }
        bytes += QStringLiteral("%1 | %2 | %3 | req-%4 | %5\n")
                     .arg(g.ts(), g.level().leftJustified(5), g.pick(comps).leftJustified(22))
                     .arg(g.num(4096, 65535), 4, 16, QLatin1Char('0'))
                     .arg(g.words(g.num(2, 8)))
                     .toUtf8();
    }
    const QString path = QDir(tmp.path()).filePath(QStringLiteral("orders-service.log"));
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
    check(waitFor([&] { return !tab.isLoading() && tab.model()->rowCount() > 3000 && !tab.model()->isFiltering(); }),
          QStringLiteral("graphical: the tab loads"));
    check(waitFor([&] { return !c.isPicking(&tab); }), QStringLiteral("graphical: the schema pick settles"));
    check(c.shownSchema().isEmpty(), QStringLiteral("graphical: an unknown log gets no schema (%1)")
                                         .arg(c.shownSettings().schemaName));

    SchemaEditorWindow w(&c);
    w.setAsker([](const QString&, bool saveDiscard) {
        return saveDiscard ? SchemaEditorWindow::Answer::Discard : SchemaEditorWindow::Answer::No;
    });
    SchemaFit::Budget noLimit;
    noLimit.maxMs = 0;
    w.setFitBudget(noLimit);
    w.show();
    w.activateWindow();
    QTest::qWaitForWindowExposed(&w);
    const auto settled = [&]() { return !w.isComputing() && w.fileFitReady() && w.endOptionsReady(); };

    auto* strip = w.findChild<SchemaFieldStrip*>(QStringLiteral("fieldStrip"));
    auto* preview = w.findChild<SchemaLinePreview*>(QStringLiteral("linePreview"));
    auto* card = w.findChild<SchemaFieldCard*>(QStringLiteral("fieldCardPanel"));
    auto* views = w.findChild<QTabWidget*>(QStringLiteral("sampleTabs"));
    auto* detect = w.findChild<QPushButton*>(QStringLiteral("detectFieldsBtn"));
    auto* guide = w.findChild<QLabel*>(QStringLiteral("guideLabel"));
    auto* templateEdit = w.findChild<QPlainTextEdit*>(QStringLiteral("templateEdit"));
    auto* nameEdit = w.findChild<QLineEdit*>(QStringLiteral("fieldNameEdit"));
    check(strip && preview && card && views && detect && guide && templateEdit && nameEdit,
          QStringLiteral("graphical: the strip, the lines, the card, Detect Fields and the guide exist"));
    if (!strip || !preview || !card || !views || !detect || !guide || !templateEdit || !nameEdit)
        return;
    check(!w.guideText().isEmpty() && guide->text() == w.guideText() && !detect->isEnabled(),
          QStringLiteral("graphical: without a tab the guide says to open a log: ") + w.guideText());
    const QString foreign = QStringLiteral("Bracketed");
    check(w.newFromTemplate(QStringLiteral("{Timestamp:time} [{Thread}] {Level:level} {Message}"), foreign)
              && w.saveAs(foreign) && c.schemas().size() == 1,
          QStringLiteral("graphical: a schema of the catalogue"));

    // --- Открытие на незнакомом логе: поля определены сами ----------------------
    // Разбиение при открытии идёт на GUI-потоке (вывод структуры по 200 строкам
    // образцов — без извлекателя схемы, инвариант 12 не при чём): цена — здесь.
    QElapsedTimer openTimer;
    openTimer.start();
    w.setView(&tab);
    const qint64 openMs = openTimer.elapsed();
    std::printf("  graphical: opening with detection (200 sample lines, GUI thread) — %lld ms\n", openMs);
    if (!kSanitized)
        check(openMs <= 50, QStringLiteral("graphical: opening with detection %1 ms").arg(openMs));
    check(waitFor(settled), QStringLiteral("graphical: computed"));
    const QString detected = w.templateText();
    QStringList columns = LineTemplate(detected).columns();
    check(w.isDetected() && columns.size() == 5 && columns.first() == QLatin1String("Timestamp")
              && columns.value(1) == QLatin1String("Level") && columns.last() == QLatin1String("Message"),
          QStringLiteral("graphical: the lines are split into fields on opening: ") + detected);
    check(w.savedName().isEmpty() && !w.isModified() && w.schemaName() == QLatin1String("orders-service"),
          QStringLiteral("graphical: a new schema named after the file, closing does not ask (%1)").arg(w.schemaName()));
    check(w.guideText().contains(QLatin1String("by itself")) && w.guideText().contains(QLatin1String("5 fields")),
          QStringLiteral("graphical: the guide says what happened: ") + w.guideText());
    check(!w.isAdvancedShown() && !templateEdit->isVisible(),
          QStringLiteral("graphical: the text template is out of the way (Advanced is collapsed)"));
    check(views->currentWidget() == preview && preview->isVisible(), QStringLiteral("graphical: Lines is the first view"));

    // Блоки: по одному на поле, подпись — словами, без синтаксиса шаблона.
    bool words = fieldItems(strip) == columns.size();
    int literals = 0;
    for (const SchemaFieldStrip::Item& item : strip->items()) {
        if (item.type == SchemaFieldStrip::Item::Type::Literal) {
            literals += item.text == QLatin1String("|") ? 1 : 0;
            continue;
        }
        words = words && !item.caption.isEmpty() && !item.caption.contains(QLatin1Char('{'))
                && !item.caption.contains(QLatin1Char(':')) && item.name == columns.value(item.column);
    }
    check(words && literals == 4, QStringLiteral("graphical: a block per field with the end in words, «|» between them"));
    check(strip->items().value(0).caption == QLatin1String("date and time")
              && strip->items().last().caption == QLatin1String("to the end of the line"),
          QStringLiteral("graphical: captions «%1» … «%2»").arg(strip->items().value(0).caption, strip->items().last().caption));

    // Строки: участок поля залит цветом его блока; не прочитанная — красным,
    // продолжение записи — без полей.
    const QVector<SchemaLinePreview::Line>& lines = preview->lines();
    check(lines.size() >= 190 && lines.first().spans.size() == columns.size()
              && lines.first().text == tab.model()->messageAt(0),
          QStringLiteral("graphical: the lines of the tab, a span per field (%1 lines)").arg(lines.size()));
    bool coloured = !lines.isEmpty();
    if (!lines.isEmpty()) {
        for (const SchemaLinePreview::Span& span : lines.first().spans)
            coloured = coloured && previewFill(preview, 0, span.start, span.length)
                                       == SchemaFieldStrip::fieldColor(span.column, preview->palette());
        const SchemaLinePreview::Span first = lines.first().spans.value(0);
        check(lines.first().text.mid(first.start, first.length).startsWith(QLatin1String("2026-11-18 ")),
              QStringLiteral("graphical: the first span is the timestamp"));
    }
    check(coloured, QStringLiteral("graphical: every field of a line has the colour of its block"));
    int missed = -1, continuation = -1, partial = -1;
    for (int i = 0; i < lines.size(); ++i) {
        if (lines[i].state == SchemaLinePreview::Line::State::Missed && missed < 0)
            missed = i;
        if (lines[i].state == SchemaLinePreview::Line::State::Other && continuation < 0)
            continuation = i;
        if (lines[i].state == SchemaLinePreview::Line::State::Partial && partial < 0)
            partial = i;
    }
    check(missed >= 0 && lines[missed].text.contains(QLatin1String("restart")) && lines[missed].spans.isEmpty()
              && continuation == missed + 1 && lines[continuation].text.contains(QLatin1String("Main.java"))
              && partial == missed + 2 && lines[partial].text.contains(QLatin1String("shutting"))
              && !lines[partial].spans.isEmpty(),
          QStringLiteral("graphical: a line the schema does not read, a continuation and a half-read line are "
                         "told apart (%1, %2, %3)").arg(missed).arg(continuation).arg(partial));
    if (missed >= 0 && partial >= 0) {
        // Красным — обе; не прочитанная гуще.
        QColor missedFill, partialFill;
        const QTextBlock mb = preview->document()->findBlockByNumber(missed);
        const QTextBlock pb = preview->document()->findBlockByNumber(partial);
        for (const QTextEdit::ExtraSelection& sel : preview->extraSelections()) {
            if (!sel.format.boolProperty(QTextFormat::FullWidthSelection))
                continue;
            if (sel.cursor.block() == mb)
                missedFill = sel.format.background().color();
            if (sel.cursor.block() == pb)
                partialFill = sel.format.background().color();
        }
        check(missedFill.isValid() && partialFill.isValid() && missedFill != partialFill
                  && missedFill.red() > missedFill.green() && partialFill.red() > partialFill.green(),
              QStringLiteral("graphical: both are marked red, the unread one stronger"));
    }

    // Каталог: покрытие — только у схем, которые читают лог.
    auto* list = w.findChild<QListWidget*>(QStringLiteral("schemaList"));
    bool noNoise = list && list->count() > 1;
    for (int i = 0; list && i < list->count(); ++i)
        noNoise = noNoise && !list->item(i)->text().contains(QLatin1Char('%'));
    check(noNoise, QStringLiteral("graphical: schemas that do not read the log show no percent"));


    // --- Выбор поля: щелчок по участку строки и по блоку ------------------------
    check(w.selectedColumn() == -1 && !card->field().isValid(), QStringLiteral("graphical: nothing is selected at first"));
    QTest::mouseClick(preview->viewport(), Qt::LeftButton, Qt::NoModifier, preview->spanCenter(0, 2));
    check(w.selectedColumn() == 2 && strip->currentColumn() == 2 && preview->currentColumn() == 2
              && card->field().column == 2 && nameEdit->isVisible() && nameEdit->text() == columns.value(2),
          QStringLiteral("graphical: a click on a field in a line selects it everywhere (%1)").arg(w.selectedColumn()));
    check(waitFor(settled), QStringLiteral("graphical: settled after the selection"));
    QList<QRadioButton*> radios;   // уходящие (deleteLater) — уже без имени
    for (QRadioButton* r : card->findChildren<QRadioButton*>()) {
        if (r->objectName().startsWith(QLatin1String("fieldEnd")))
            radios.append(r);
    }
    int checkedRadios = 0, results = 0;
    for (QRadioButton* r : radios) {
        checkedRadios += r->isChecked() ? 1 : 0;
        const QString id = r->objectName().mid(QStringLiteral("fieldEnd").size());
        if (auto* result = card->findChild<QLabel*>(QStringLiteral("fieldEndResult") + id))
            results += result->text().contains(QStringLiteral(" · ")) && result->text().endsWith(QLatin1Char('%'))
                           && !result->text().contains(QLatin1Char(':')) ? 1 : 0;
    }
    check(radios.size() >= 4 && checkedRadios == 1 && results == radios.size(),
          QStringLiteral("graphical: «Where it ends» — choices with a result each, one is current (%1, %2, %3)")
              .arg(radios.size()).arg(checkedRadios).arg(results));
    auto* values = card->findChild<QLabel*>(QStringLiteral("fieldValues"));
    check(values && values->text().contains(QLatin1String("6 different values"))
              && values->text().contains(QLatin1String("Order Service")),
          QStringLiteral("graphical: the card lists the values of the field: ") + (values ? values->text() : QString()));
    QTest::mouseClick(strip, Qt::LeftButton, Qt::NoModifier, strip->columnRect(1).center());
    check(w.selectedColumn() == 1 && preview->currentColumn() == 1 && nameEdit->text() == QLatin1String("Level"),
          QStringLiteral("graphical: a click on a block selects the field"));
    QTest::keyClick(strip, Qt::Key_Right);
    check(w.selectedColumn() == 2, QStringLiteral("graphical: → moves along the blocks"));

    // --- Имя — из карточки -------------------------------------------------------
    nameEdit->setFocus();
    nameEdit->selectAll();
    QTest::keyClicks(nameEdit, QStringLiteral("Component"));
    QTest::keyClick(nameEdit, Qt::Key_Return);
    columns = LineTemplate(w.templateText()).columns();
    check(columns.value(2) == QLatin1String("Component") && w.isModified() && !w.isDetected(),
          QStringLiteral("graphical: the name in the card renames the field: ") + w.templateText());
    check(strip->items().size() > 4 && w.selectedColumn() == 2 && nameEdit->text() == QLatin1String("Component"),
          QStringLiteral("graphical: the field stays selected after the rename"));
    nameEdit->selectAll();
    QTest::keyClicks(nameEdit, QStringLiteral("Level"));   // занято
    QTest::keyClick(nameEdit, Qt::Key_Return);
    check(LineTemplate(w.templateText()).columns().value(2) == QLatin1String("Component")
              && nameEdit->text() == QLatin1String("Component") && w.editMessage().contains(QLatin1String("Level")),
          QStringLiteral("graphical: a taken name is refused in words: ") + w.editMessage());
    check(waitFor(settled), QStringLiteral("graphical: settled after the rename"));

    // --- «Где кончается» — из карточки -------------------------------------------
    const QVector<SchemaEdit::EndOption> options = w.endOptions(2);
    int other = -1;
    for (int i = 0; i < options.size() && other < 0; ++i) {
        if (options[i].available && !options[i].current && options[i].choice == SchemaEdit::EndOption::Choice::WideGap)
            other = i;
    }
    auto* gap = card->findChild<QRadioButton*>(QStringLiteral("fieldEnd%1").arg(other));
    check(other >= 0 && gap && gap->isEnabled() && !gap->isChecked(), QStringLiteral("graphical: «at a wide gap» is offered"));
    if (gap) {
        const QString before = w.templateText();
        const int undoCount = w.undoStack()->count();
        gap->click();
        check(w.templateText() == options[other].templateText && w.undoStack()->count() == undoCount + 1,
              QStringLiteral("graphical: a choice in the card changes the end of the field: ") + w.templateText());
        check(waitFor(settled), QStringLiteral("graphical: settled after the end"));
        auto* again = card->findChild<QRadioButton*>(QStringLiteral("fieldEnd%1").arg(other));
        check(again && again->isChecked() && strip->items().value(4).caption == QLatin1String("up to a wide gap"),
              QStringLiteral("graphical: the card and the block show the new end"));
        w.undoStack()->undo();
        check(w.templateText() == before, QStringLiteral("graphical: one undo step"));
        check(waitFor(settled), QStringLiteral("graphical: settled after undo"));
    }

    // --- Hide и «Show as a Column» -----------------------------------------------
    auto* hide = card->findChild<QPushButton*>(QStringLiteral("fieldHideBtn"));
    const QString beforeHide = w.templateText();
    if (hide)
        hide->click();
    check(LineTemplate(w.templateText()).columns().size() == 4 && w.selectedColumn() == -1,
          QStringLiteral("graphical: Hide removes the column: ") + w.templateText());
    int hiddenItem = -1;
    for (int i = 0; i < strip->items().size(); ++i) {
        if (strip->items()[i].type == SchemaFieldStrip::Item::Type::Field && strip->items()[i].column < 0)
            hiddenItem = i;
    }
    check(hiddenItem >= 0, QStringLiteral("graphical: the hidden part stays among the blocks"));
    if (hiddenItem >= 0) {
        QTest::mouseClick(strip, Qt::LeftButton, Qt::NoModifier, strip->itemRect(hiddenItem).center());
        auto* show = card->findChild<QPushButton*>(QStringLiteral("fieldShowBtn"));
        check(card->field().isHidden() && show && show->isVisible(), QStringLiteral("graphical: the card offers to show it"));
        if (show)
            show->click();
        check(LineTemplate(w.templateText()).columns().size() == 5 && w.selectedColumn() == 2
                  && nameEdit->isVisible(),
              QStringLiteral("graphical: Show as a Column brings the column back: ") + w.templateText());
    }
    w.setTemplateText(beforeHide);
    check(waitFor(settled), QStringLiteral("graphical: settled after hide and show"));

    // --- Make Field выделением в строке -------------------------------------------
    check(w.newFromTemplate(QStringLiteral("{Timestamp:time} | {Level:level} | {Message}"), QStringLiteral("Coarse")),
          QStringLiteral("graphical: a coarse schema"));
    check(waitFor(settled), QStringLiteral("graphical: the coarse schema is computed"));
    auto* floating = preview->findChild<QToolButton*>(QStringLiteral("previewMakeFieldBtn"));
    check(floating && !floating->isVisible(), QStringLiteral("graphical: no Make Field button without a selection"));
    const QString line0 = preview->lines().value(0).text;
    const QString comp = line0.section(QLatin1Char('|'), 2, 2).trimmed();
    const int compAt = int(line0.indexOf(comp, line0.indexOf(QLatin1Char('|'), line0.indexOf(QLatin1Char('|')) + 1)));
    preview->selectInLine(0, compAt, int(comp.size()));
    check(floating && floating->isVisible() && preview->lineSelection().isValid(),
          QStringLiteral("graphical: a selection in a line brings the Make Field button"));
    app.timing = true;
    app.maxMs = 0;
    if (floating)
        QTest::mouseClick(floating, Qt::LeftButton);
    check(waitFor(settled), QStringLiteral("graphical: Make Field computed"));
    app.timing = false;
    std::printf("  graphical: the longest GUI event of Make Field in a line — %lld ms (%s)\n", app.maxMs,
                qPrintable(app.maxWhat));
    if (!kSanitized)
        check(app.maxMs <= 50, QStringLiteral("graphical: the longest GUI event %1 ms (%2)").arg(app.maxMs).arg(app.maxWhat));
    columns = LineTemplate(w.templateText()).columns();
    check(columns.size() == 4 && w.makeFieldResult().ok() && w.selectedColumn() == 2 && card->field().column == 2,
          QStringLiteral("graphical: the selection became a field, it is selected: ") + w.templateText());
    check(!preview->lineSelection().isValid() && floating && !floating->isVisible(),
          QStringLiteral("graphical: the selection is cleared after Make Field"));
    check(w.editMessage().contains(columns.value(2)) && !w.editMessage().contains(QLatin1Char('{')),
          QStringLiteral("graphical: the message names the field in words: ") + w.editMessage());
    check(preview->lines().value(0).spans.size() == 4, QStringLiteral("graphical: the lines are recoloured"));

    // Дата и уровень получают свои имена; Ctrl+M в строках.
    check(w.newEmpty(), QStringLiteral("graphical: an empty schema"));
    check(waitFor(settled), QStringLiteral("graphical: the empty schema is computed"));
    check(w.guideText().contains(QLatin1String("not split")) && detect->isEnabled() && detect->font().bold(),
          QStringLiteral("graphical: the guide of an unsplit log points at Detect Fields: ") + w.guideText());
    preview->setFocus();
    preview->selectInLine(0, 0, 19);
    QTest::keyClick(preview, Qt::Key_M, Qt::ControlModifier);
    check(waitFor(settled), QStringLiteral("graphical: Make Field of the date computed"));
    check(w.templateText().startsWith(QLatin1String("{Timestamp:time}")),
          QStringLiteral("graphical: a selected date becomes Timestamp: ") + w.templateText());
    const QString afterDate = w.templateText();

    // Нечего делать — словами, шаблон прежний.
    const SchemaLinePreview::Span ts = preview->lines().value(0).spans.value(0);
    check(!w.makeFieldInLine(0, ts.start, ts.length) && w.selectedColumn() == 0 && w.templateText() == afterDate,
          QStringLiteral("graphical: the whole field selected — it is selected instead: ") + w.editMessage());
    check(!w.makeFieldInLine(0, ts.start + 3, ts.length + 4) && w.templateText() == afterDate
              && w.editMessage().contains(QLatin1String("inside one")),
          QStringLiteral("graphical: a selection across fields is refused: ") + w.editMessage());
    int missedNow = -1;
    for (int i = 0; i < preview->lines().size() && missedNow < 0; ++i) {
        if (preview->lines()[i].row < 0)
            missedNow = i;
    }
    if (missedNow >= 0)
        check(!w.makeFieldInLine(missedNow, 0, 4) && w.templateText() == afterDate,
              QStringLiteral("graphical: a line the schema does not read is refused: ") + w.editMessage());
    views->setCurrentIndex(0);
    preview->clearLineSelection();
    check(!w.makeFieldFromSelection() && !w.editMessage().isEmpty() && w.templateText() == afterDate,
          QStringLiteral("graphical: Make Field without a selection explains"));

    // --- Detect Fields --------------------------------------------------------------
    check(w.newEmpty() && waitFor(settled), QStringLiteral("graphical: empty again"));
    const int undoBefore = w.undoStack()->count();
    detect->click();
    check(w.templateText() == detected && w.isDetected() && w.savedName().isEmpty()
              && w.undoStack()->count() == undoBefore + 1,
          QStringLiteral("graphical: Detect Fields splits the lines (an undo step): ") + w.templateText());
    w.undoStack()->undo();
    check(w.templateText() == QLatin1String("{Message}"), QStringLiteral("graphical: undo of Detect Fields"));
    w.undoStack()->redo();
    check(waitFor(settled), QStringLiteral("graphical: settled after Detect Fields"));
    // Схема каталога, которая лог не читает: подсказка и новая схема вместо правки чужой.
    const QString foreignDefinition = c.schemas().value(0).schema;
    check(w.openSchema(foreign) && waitFor(settled), QStringLiteral("graphical: a catalogue schema opens"));
    check(w.guideText().contains(QLatin1String("reads only")) && detect->font().bold(),
          QStringLiteral("graphical: a schema that does not read the log says so: ") + w.guideText());
    detect->click();
    check(w.templateText() == detected && w.savedName().isEmpty() && c.schemas().value(0).schema == foreignDefinition,
          QStringLiteral("graphical: Detect Fields on a catalogue schema makes a new one: ") + w.savedName());
    check(waitFor(settled), QStringLiteral("graphical: settled after the second Detect Fields"));

    // --- Не подошедшие строки — по кнопке -------------------------------------------
    auto* missesToggle = w.findChild<QToolButton*>(QStringLiteral("missesToggle"));
    auto* missesTable = w.findChild<QTableWidget*>(QStringLiteral("missesTable"));
    check(missesToggle && missesTable && missesToggle->isVisible() && !missesTable->isVisible()
              && missesTable->rowCount() > 0 && missesToggle->text().contains(QString::number(missesTable->rowCount())),
          QStringLiteral("graphical: the lines the schema does not read are behind a button: ")
              + (missesToggle ? missesToggle->text() : QString()));
    if (missesToggle)
        missesToggle->click();
    check(missesTable && missesTable->isVisible(), QStringLiteral("graphical: the button shows them"));

    // --- Advanced: раскрывается на ошибке -------------------------------------------
    w.setTemplateText(QStringLiteral("{A:text} {B}"));
    auto* error = w.findChild<QLabel*>(QStringLiteral("templateError"));
    check(w.isAdvancedShown() && templateEdit->isVisible() && error && error->isVisible()
              && w.guideText().contains(QLatin1String("Advanced")) && strip->items().isEmpty(),
          QStringLiteral("graphical: a template error opens Advanced and the guide points there"));
    w.undoStack()->undo();
    check(w.templateText() == detected && fieldItems(strip) == 5, QStringLiteral("graphical: undo brings the blocks back"));

    // --- Таблица — второй вид, выбор общий -------------------------------------------
    check(waitFor(settled), QStringLiteral("graphical: settled before the table"));
    views->setCurrentIndex(1);
    auto* table = w.findChild<QTableView*>(QStringLiteral("sampleTable"));
    if (table && w.sampleModel()->rowCount() > 0) {
        table->setCurrentIndex(w.sampleModel()->index(0, 3));
        check(w.selectedColumn() == 3 && strip->currentColumn() == 3,
              QStringLiteral("graphical: a cell of the table selects the field too"));
    }

    // --- Схема вкладки её лог не читает: редактор показывает разбиение ---------------
    check(w.openSchema(foreign) && !w.isModified(), QStringLiteral("graphical: the catalogue schema again"));
    c.selectSchema(0);
    check(tab.parserPattern() == foreignDefinition, QStringLiteral("graphical: the tab gets the schema that does not fit"));
    w.setView(&tab);
    check(waitFor(settled), QStringLiteral("graphical: settled on the tab with a wrong schema"));
    check(w.isDetected() && w.templateText() == detected && w.savedName().isEmpty() && !w.isModified(),
          QStringLiteral("graphical: instead of a schema that does not read the log — the split lines: ") + w.templateText());
    check(w.guideText().contains(foreign) && w.guideText().contains(QLatin1String("reads only"))
              && w.guideText().contains(QLatin1String("by itself")),
          QStringLiteral("graphical: the guide names the schema of the tab: ") + w.guideText());
    check(c.schemas().value(0).schema == foreignDefinition && tab.parserPattern() == foreignDefinition,
          QStringLiteral("graphical: neither the catalogue nor the tab is changed by that"));
    // Открытая из списка явно — показана как есть.
    check(w.openSchema(foreign) && waitFor(settled) && w.savedName() == foreign && !w.isDetected()
              && w.guideText().contains(QLatin1String("reads only")),
          QStringLiteral("graphical: opened from the list, the schema is shown as it is"));
    // В каталоге есть схема, которая этот лог читает: она главнее догадки.
    detect->click();
    check(w.isDetected() && w.saveAs(QStringLiteral("Orders")) && c.schemas().size() == 2,
          QStringLiteral("graphical: the detected schema is saved to the catalogue"));
    w.setView(&tab);   // у вкладки по-прежнему схема, которая её лог не читает
    check(waitFor(settled), QStringLiteral("graphical: settled with a fitting schema in the catalogue"));
    check(w.savedName() == QLatin1String("Orders") && !w.isDetected() && !w.isModified()
              && w.guideText().contains(QLatin1String("Orders")) && w.guideText().contains(QLatin1String("your schemas")),
          QStringLiteral("graphical: a catalogue schema that reads the log is shown instead of a guess: %1 | %2")
              .arg(w.savedName(), w.guideText()));
    bool percentShown = false;
    for (int i = 0; list && i < list->count(); ++i)
        percentShown = percentShown || (list->item(i)->text().startsWith(QLatin1String("Orders"))
                                        && list->item(i)->text().contains(QLatin1Char('%')));
    check(percentShown, QStringLiteral("graphical: a schema that reads the log shows its percent in the list"));
    check(tab.parserPattern() == foreignDefinition, QStringLiteral("graphical: the tab is not changed until Apply to Tab"));
    w.close();
    w.setView(nullptr);
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
    std::printf("Graphical editing:\n");
    testGraphical(app);

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
