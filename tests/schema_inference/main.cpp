// Вывод структуры по строкам (F-17, шаг 4; docs/design/F-17.md, раздел 6):
//
//   1. Корпус из 30 форматов (правдоподобные строки, записанные в тесте
//      генераторами с фиксированным зерном):
//      • 9 встроенных: выведенный шаблон даёт те же Timestamp (время), Level
//        и Message, что встроенный шаблон (resources/templates), на ≥ 95 %
//        строк, принятых встроенным;
//      • 6 «трудных границ» (раздел 2): форма следом, padded, width,
//        постоянное число слов, закрытый набор многословных названий —
//        поле нужного вида со значениями-названиями на ≥ 95 % строк;
//        открытый набор — ничего не разбито, заметка NoBoundary;
//      • 15 «диких» форматов: покрытие ≥ 0,95 и ожидаемые виды полей.
//      Печатается таблица; известные неудачи закреплены (kKnownFailures):
//      изменение в любую сторону — провал. Готово — ≥ 26 из 30 и все шесть
//      «трудных границ».
//   2. Отказы данными: нет строк, JSON / logfmt, проза без структуры.
//   3. Заметка «два вида строк».
//   4. Результат всегда компилируется: фаззинг случайными строками и
//      строками случайных шаблонов.
//   5. Линейность: счётчик шагов на 2000 строк ≤ 5 × на 500.
//
// Каталог встроенных шаблонов — define DENDROLOG_TEMPLATES_SOURCE_DIR.

#include "builtinsamples.h"
#include "linetemplate.h"
#include "schemafit.h"
#include "schemainference.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QSet>

#include <atomic>
#include <cstdio>
#include <functional>

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

using Kind = LineTemplateElement::Kind;
using SchemaInference::Note;

// ---------------------------------------------------------------------------
// Генератор правдоподобных строк.
// ---------------------------------------------------------------------------

class Gen {
public:
    explicit Gen(quint32 seed) : m_rng(seed) {}

    int num(int lo, int hi) { return lo + int(m_rng.bounded(quint32(hi - lo + 1))); }
    bool chance(double p) { return m_rng.generateDouble() < p; }
    template <typename T>
    const T& pick(const QVector<T>& v) { return v[num(0, int(v.size()) - 1)]; }
    QString pickS(const QStringList& v) { return v[num(0, int(v.size()) - 1)]; }

    // Время: секунды от начала суток 2026-11-18, идут вперёд.
    void tick() { m_sec += num(0, 3); m_ms = num(0, 999); }
    int hh() const { return 9 + (m_sec / 3600) % 12; }
    int mm() const { return (m_sec / 60) % 60; }
    int ss() const { return m_sec % 60; }
    int ms() const { return m_ms; }
    QString clock() const
    {
        return QStringLiteral("%1:%2:%3").arg(hh(), 2, 10, QLatin1Char('0'))
            .arg(mm(), 2, 10, QLatin1Char('0')).arg(ss(), 2, 10, QLatin1Char('0'));
    }
    QString msPart() const { return QStringLiteral("%1").arg(m_ms, 3, 10, QLatin1Char('0')); }
    QString isoDate() const { return QStringLiteral("2026-11-18"); }

    QString ip()
    {
        return QStringLiteral("%1.%2.%3.%4").arg(pickS({ "10", "192", "172", "203" }))
            .arg(num(0, 255)).arg(num(0, 255)).arg(num(1, 254));
    }
    QString hex(int len)
    {
        QString s;
        for (int i = 0; i < len; ++i)
            s += QLatin1Char("0123456789abcdef"[num(0, 15)]);
        return s;
    }
    QString user() { return pickS({ "alice", "bob", "carol", "dave", "erin", "frank", "grace", "heidi", "ivan", "judy" }); }
    QString word() { return pickS({ "orders", "users", "billing", "catalog", "search", "reports", "inventory", "audit", "metrics", "sessions", "uploads", "events" }); }
    QString path() { return QStringLiteral("/var/lib/app/%1/%2.dat").arg(word()).arg(num(1, 999)); }

    // Сообщение приложения: ~45 шаблонов, разные первые слова.
    QString message()
    {
        switch (num(0, 44)) {
        case 0: return QStringLiteral("Started application in %1.%2 seconds").arg(num(1, 9)).arg(num(100, 999));
        case 1: return QStringLiteral("Stopping scheduler gracefully");
        case 2: return QStringLiteral("Received request from %1").arg(ip());
        case 3: return QStringLiteral("Cache miss for key %1").arg(word());
        case 4: return QStringLiteral("Connection pool resized to %1").arg(num(5, 50));
        case 5: return QStringLiteral("User %1 logged in").arg(user());
        case 6: return QStringLiteral("User %1 logged out after %2 minutes").arg(user()).arg(num(1, 90));
        case 7: return QStringLiteral("Failed to send invoice 2026-%1 - retrying").arg(num(1000, 9999));
        case 8: return QStringLiteral("Retry %1 of 5 for job %2").arg(num(1, 5)).arg(word());
        case 9: return QStringLiteral("Order %1 processed in %2 ms").arg(num(1000, 99999)).arg(num(1, 900));
        case 10: return QStringLiteral("Payment declined for order %1").arg(num(1000, 99999));
        case 11: return QStringLiteral("Scheduled task cleanup completed");
        case 12: return QStringLiteral("Loading configuration from %1").arg(path());
        case 13: return QStringLiteral("Listening on port %1").arg(num(1024, 9999));
        case 14: return QStringLiteral("Shutting down executor service");
        case 15: return QStringLiteral("Database migration finished");
        case 16: return QStringLiteral("Health check passed");
        case 17: return QStringLiteral("Slow query detected, took %1 ms").arg(num(500, 9000));
        case 18: return QStringLiteral("Session %1 expired").arg(hex(16));
        case 19: return QStringLiteral("Token refreshed for client %1").arg(word());
        case 20: return QStringLiteral("Request timed out after %1 seconds").arg(num(5, 60));
        case 21: return QStringLiteral("Invalid input rejected: %1").arg(word());
        case 22: return QStringLiteral("File %1 not found").arg(path());
        case 23: return QStringLiteral("Disk usage at %1 percent").arg(num(50, 99));
        case 24: return QStringLiteral("New connection accepted");
        case 25: return QStringLiteral("Worker thread %1 started").arg(num(1, 32));
        case 26: return QStringLiteral("Message queue depth is %1").arg(num(0, 5000));
        case 27: return QStringLiteral("Sent %1 notifications").arg(num(1, 500));
        case 28: return QStringLiteral("Cleaning up temporary files");
        case 29: return QStringLiteral("Reindexing collection %1").arg(word());
        case 30: return QStringLiteral("Backup written to %1").arg(path());
        case 31: return QStringLiteral("Authentication succeeded for %1").arg(user());
        case 32: return QStringLiteral("Rate limit exceeded by %1").arg(ip());
        case 33: return QStringLiteral("Feature flag %1 enabled").arg(word());
        case 34: return QStringLiteral("Exported %1 records to CSV").arg(num(1, 100000));
        case 35: return QStringLiteral("Garbage collection took %1 ms").arg(num(1, 400));
        case 36: return QStringLiteral("Certificate expires in %1 days").arg(num(1, 365));
        case 37: return QStringLiteral("Unable to reach upstream %1").arg(ip());
        case 38: return QStringLiteral("Processing batch %1 of %2").arg(num(1, 20)).arg(num(20, 40));
        case 39: return QStringLiteral("Metrics flushed");
        case 40: return QStringLiteral("Transaction %1 committed").arg(hex(12));
        case 41: return QStringLiteral("Index rebuilt successfully");
        case 42: return QStringLiteral("Webhook delivered to %1").arg(word());
        case 43: return QStringLiteral("Lock acquired on resource %1").arg(word());
        default: return QStringLiteral("Heartbeat from node %1").arg(word());
        }
    }

    QString level() { return pickS({ "INFO", "INFO", "INFO", "INFO", "DEBUG", "DEBUG", "WARN", "ERROR", "TRACE" }); }
    QString javaLogger()
    {
        return pickS({ "com.example.demo.Application", "o.s.web.servlet.DispatcherServlet",
                       "org.hibernate.orm.deprecation", "c.e.billing.InvoiceService",
                       "com.zaxxer.hikari.HikariPool", "o.a.catalina.core.StandardService",
                       "c.e.orders.OrderController", "io.netty.channel.EventLoop" });
    }

private:
    QRandomGenerator m_rng;
    int m_sec = 1000;
    int m_ms = 0;
};

QString pad(const QString& s, int width) { return s.leftJustified(width, QLatin1Char(' ')); }
QString rpad(const QString& s, int width) { return s.rightJustified(width, QLatin1Char(' ')); }

// Строки формата: n записей (генератор строки), иногда — продолжения.
using LineFn = std::function<QString(Gen&)>;
QStringList generate(quint32 seed, int n, const LineFn& fn, const QStringList& continuation = {},
                     double contChance = 0.0)
{
    Gen g(seed);
    QStringList out;
    for (int i = 0; i < n; ++i) {
        g.tick();
        out.append(fn(g));
        if (!continuation.isEmpty() && g.chance(contChance)) {
            for (const QString& c : continuation)
                out.append(c);
        }
    }
    return out;
}

const QStringList kJavaTrace = {
    QStringLiteral("java.lang.IllegalStateException: SMTP server did not respond"),
    QStringLiteral("\tat com.example.billing.MailGateway.send(MailGateway.java:88)"),
    QStringLiteral("\tat com.example.billing.InvoiceService.sendInvoice(InvoiceService.java:131)"),
    QStringLiteral("Caused by: java.net.SocketTimeoutException: Read timed out"),
    QStringLiteral("\t... 12 common frames omitted"),
};
const QStringList kPyTrace = {
    QStringLiteral("Traceback (most recent call last):"),
    QStringLiteral("  File \"/srv/app/worker.py\", line 42, in run"),
    QStringLiteral("    result = job.execute()"),
    QStringLiteral("TimeoutError: job did not finish in 30 s"),
};
const QStringList kDotnetTrace = {
    QStringLiteral("System.InvalidOperationException: Sequence contains no elements"),
    QStringLiteral("   at System.Linq.ThrowHelper.ThrowNoElementsException()"),
    QStringLiteral("   at MyApp.Services.OrderService.GetLatest() in /src/OrderService.cs:line 57"),
};

QStringList builtinSampleLines(const QString& file)
{
    QStringList out;
    for (const builtinsamples::SchemaCase& c : builtinsamples::cases()) {
        if (c.file == file) {
            for (const builtinsamples::Sample& s : c.samples)
                out.append(s.line);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Корпус.
// ---------------------------------------------------------------------------

enum class Group { Builtin, Hard, Wild };

struct Format {
    QString name;
    Group group;
    QStringList lines;
    QString builtinFile;            // Builtin: шаблон resources/templates
    QVector<Kind> expectKinds;      // Wild: виды, которые обязаны быть в шаблоне
    // Hard: ожидаемый вид поля и значения по строкам (пусто — «не разбито»).
    Kind hardKind = Kind::Text;
    int hardCount = 0;              // Width / Words — N
    QStringList hardValues;         // значение поля в строке i (по строкам lines)
    QStringList hardOf;             // Of — ожидаемый набор
    bool expectSplit = true;
};

QStringList kComponents6 = { QStringLiteral("Order Service"), QStringLiteral("Payment Gateway"),
                             QStringLiteral("DB"), QStringLiteral("Inventory Sync Worker"),
                             QStringLiteral("Auth"), QStringLiteral("Mail Sender") };

QVector<Format> corpus()
{
    QVector<Format> fs;
    const int N = 300;

    // ---- 9 встроенных ----------------------------------------------------
    {
        Format f{ QStringLiteral("Logback / Log4j"), Group::Builtin };
        f.builtinFile = QStringLiteral("logback.json");
        f.lines = generate(101, N, [](Gen& g) {
            const QString lvl = g.level();
            const QString thread = g.pickS({ QStringLiteral("main"), QStringLiteral("http-nio-8080-exec-%1").arg(g.num(1, 10)),
                                             QStringLiteral("pool-3-thread-%1").arg(g.num(1, 8)),
                                             QStringLiteral("RMI TCP Connection(2)-127.0.0.1"), QStringLiteral("scheduling-1") });
            return QStringLiteral("%1 %2.%3 [%4] %5 %6 -%7 %8").arg(g.isoDate(), g.clock(), g.msPart(), thread,
                pad(lvl, 5), g.javaLogger(), g.chance(0.02) ? QStringLiteral("-") : QString(), g.message());
        }, kJavaTrace, 0.05) + builtinSampleLines(QStringLiteral("logback.json"));
        fs.append(f);
    }
    {
        Format f{ QStringLiteral("Spring Boot"), Group::Builtin };
        f.builtinFile = QStringLiteral("spring-boot.json");
        f.lines = generate(102, N, [](Gen& g) {
            const QString thread = g.pickS({ QStringLiteral("main"), QStringLiteral("nio-8080-exec-%1").arg(g.num(1, 9)),
                                             QStringLiteral("scheduling-1"), QStringLiteral("task-%1").arg(g.num(1, 4)) });
            const QString app = g.chance(0.6) ? QStringLiteral("[myapp] ") : QString();
            return QStringLiteral("%1T%2.%3Z %4 %5 --- %6[%7] %8 : %9").arg(g.isoDate(), g.clock(), g.msPart(),
                rpad(g.pickS({ "INFO", "INFO", "WARN", "ERROR", "DEBUG" }), 5)).arg(g.num(30000, 30003))
                .arg(app, rpad(thread, 15), pad(g.javaLogger(), 40), g.message());
        }, kJavaTrace, 0.04) + builtinSampleLines(QStringLiteral("spring-boot.json"));
        fs.append(f);
    }
    {
        Format f{ QStringLiteral("nginx / Apache access"), Group::Builtin };
        f.builtinFile = QStringLiteral("access-combined.json");
        f.lines = generate(103, N, [](Gen& g) {
            const QString req = QStringLiteral("%1 /%2/%3 HTTP/1.1").arg(g.pickS({ "GET", "GET", "POST", "PUT", "DELETE" }), g.word()).arg(g.num(1, 999));
            const QString head = QStringLiteral("%1 - %2 [18/Nov/2026:%3 +0000] \"%4\" %5 %6").arg(g.ip(),
                g.chance(0.9) ? QStringLiteral("-") : g.user(), g.clock(), req)
                .arg(g.pickS({ "200", "200", "200", "304", "404", "500" }))
                .arg(g.chance(0.1) ? QStringLiteral("-") : QString::number(g.num(0, 90000)));
            if (g.chance(0.03))
                return head;
            return head + QStringLiteral(" \"%1\" \"%2\"").arg(g.chance(0.5) ? QStringLiteral("-") : QStringLiteral("https://example.org/%1").arg(g.word()),
                g.pickS({ "curl/8.5.0", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
                          "Go-http-client/1.1", "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:121.0) Gecko/20100101 Firefox/121.0" }));
        }) + builtinSampleLines(QStringLiteral("access-combined.json"));
        fs.append(f);
    }
    {
        Format f{ QStringLiteral("nginx error"), Group::Builtin };
        f.builtinFile = QStringLiteral("nginx-error.json");
        f.lines = generate(104, N, [](Gen& g) {
            const QString msg = g.pickS({
                QStringLiteral("open() \"/usr/share/nginx/html/%1.ico\" failed (2: No such file or directory), client: %2, server: localhost").arg(g.word(), g.ip()),
                QStringLiteral("upstream timed out (110: Connection timed out) while reading response header from upstream, client: %1").arg(g.ip()),
                QStringLiteral("connect() failed (111: Connection refused) while connecting to upstream, client: %1").arg(g.ip()),
                QStringLiteral("client intended to send too large body: %1 bytes").arg(g.num(100000, 9000000)),
                QStringLiteral("signal process started"),
                QStringLiteral("using inherited sockets from \"6;\""),
                QStringLiteral("worker process %1 exited with code 0").arg(g.num(100, 9999)) });
            const QString conn = g.chance(0.8) ? QStringLiteral("*%1 ").arg(g.num(1, 99999)) : QString();
            const int pid = g.num(1000, 1003);
            return QStringLiteral("2026/11/18 %1 [%2] %3#%4: %5%6").arg(g.clock(), g.pickS({ "error", "error", "warn", "notice", "info", "crit" }))
                .arg(pid).arg(pid).arg(conn, msg);
        }) + builtinSampleLines(QStringLiteral("nginx-error.json"));
        fs.append(f);
    }
    {
        Format f{ QStringLiteral("Syslog"), Group::Builtin };
        f.builtinFile = QStringLiteral("syslog.json");
        f.lines = generate(105, N, [](Gen& g) {
            const QString prog = g.pickS({ "sshd", "sshd", "CRON", "systemd", "kernel", "postfix/smtpd", "dhclient" });
            const QString pid = prog == QLatin1String("kernel") ? QString() : QStringLiteral("[%1]").arg(g.num(200, 30000));
            const QString msg = g.pickS({
                QStringLiteral("Accepted publickey for %1 from %2 port %3 ssh2").arg(g.user(), g.ip()).arg(g.num(1024, 65000)),
                QStringLiteral("pam_unix(cron:session): session opened for user root(uid=0) by (uid=0)"),
                QStringLiteral("Started Daily apt download activities."),
                QStringLiteral("[%1.%2] usb 1-1: new high-speed USB device number %3 using xhci_hcd").arg(g.num(1, 9999)).arg(g.num(100000, 999999)).arg(g.num(2, 9)),
                QStringLiteral("connect from unknown[%1]").arg(g.ip()),
                QStringLiteral("DHCPREQUEST for %1 on eth0 to %2 port 67").arg(g.ip(), g.ip()),
                QStringLiteral("Disconnected from user %1 %2 port %3").arg(g.user(), g.ip()).arg(g.num(1024, 65000)) });
            return QStringLiteral("Nov 18 %1 %2 %3%4: %5").arg(g.clock(), g.pickS({ "web01", "web02", "db01" }), prog, pid, msg);
        }) + builtinSampleLines(QStringLiteral("syslog-rfc3164.json"));
        fs.append(f);
    }
    {
        Format f{ QStringLiteral("Serilog console"), Group::Builtin };
        f.builtinFile = QStringLiteral("serilog-console.json");
        f.lines = generate(106, N, [](Gen& g) {
            return QStringLiteral("[%1 %2] %3").arg(g.clock(), g.pickS({ "INF", "INF", "INF", "DBG", "WRN", "ERR", "FTL", "VRB" }), g.message());
        }, kDotnetTrace, 0.04) + builtinSampleLines(QStringLiteral("serilog-console.json"));
        fs.append(f);
    }
    {
        Format f{ QStringLiteral("Serilog file"), Group::Builtin };
        f.builtinFile = QStringLiteral("serilog-file.json");
        f.lines = generate(107, N, [](Gen& g) {
            return QStringLiteral("%1 %2.%3 +01:00 [%4] %5").arg(g.isoDate(), g.clock(), g.msPart(),
                g.pickS({ "INF", "INF", "INF", "DBG", "WRN", "ERR" }), g.message());
        }, kDotnetTrace, 0.04) + builtinSampleLines(QStringLiteral("serilog-file.json"));
        fs.append(f);
    }
    {
        Format f{ QStringLiteral("Python logging"), Group::Builtin };
        f.builtinFile = QStringLiteral("python-logging.json");
        f.lines = generate(108, N, [](Gen& g) {
            return QStringLiteral("%1 %2,%3 - %4 - %5 - %6").arg(g.isoDate(), g.clock(), g.msPart(),
                g.pickS({ "root", "app.db", "app.api", "urllib3.connectionpool", "celery.worker" }),
                g.pickS({ "INFO", "INFO", "DEBUG", "WARNING", "ERROR", "CRITICAL" }), g.message());
        }, kPyTrace, 0.04) + builtinSampleLines(QStringLiteral("python-logging.json"));
        fs.append(f);
    }
    {
        Format f{ QStringLiteral("Python basicConfig"), Group::Builtin };
        f.builtinFile = QStringLiteral("python-basicconfig.json");
        f.lines = generate(109, N, [](Gen& g) {
            return QStringLiteral("%1:%2:%3").arg(g.pickS({ "INFO", "INFO", "DEBUG", "WARNING", "ERROR", "CRITICAL" }),
                g.pickS({ "root", "app.db", "app.api", "urllib3.connectionpool", "celery.worker" }), g.message());
        }, kPyTrace, 0.04) + builtinSampleLines(QStringLiteral("python-basicconfig.json"));
        fs.append(f);
    }

    // ---- 6 трудных границ ------------------------------------------------
    const auto hard = [&](const QString& name, quint32 seed, const QStringList& comps,
                          const std::function<QString(Gen&, const QString&)>& line) {
        Format f{ name, Group::Hard };
        Gen g(seed);
        for (int i = 0; i < N; ++i) {
            g.tick();
            const QString comp = comps.isEmpty() ? QString() : g.pickS(comps);
            QString c = comp;
            if (comps.isEmpty()) {
                // Открытый набор: названия не повторяются.
                c = QStringLiteral("%1-%2 %3").arg(g.pickS({ "node", "svc", "edge", "relay" })).arg(g.num(1000, 9999))
                        .arg(g.pickS({ "east", "west", "north", "south", "central" }) + QString::number(g.num(1, 99)));
            }
            f.lines.append(line(g, c));
            f.hardValues.append(c);
        }
        return f;
    };
    const auto ts = [](Gen& g) { return QStringLiteral("%1 %2.%3").arg(g.isoDate(), g.clock(), g.msPart()); };
    {
        Format f = hard(QStringLiteral("Hard: form follows"), 201, kComponents6, [&](Gen& g, const QString& c) {
            return QStringLiteral("%1 %2 %3 %4").arg(ts(g), c, g.level(), g.message());
        });
        f.hardKind = Kind::Text;
        fs.append(f);
    }
    {
        Format f = hard(QStringLiteral("Hard: padded"), 202, kComponents6, [&](Gen& g, const QString& c) {
            return QStringLiteral("%1 %2 %3%4").arg(ts(g), pad(g.level(), 5), pad(c, 24), g.message());
        });
        f.hardKind = Kind::Padded;
        fs.append(f);
    }
    {
        // Ширина 15: «Payment Gateway» — ровно 15 знаков, за ним один пробел.
        QStringList comps = { QStringLiteral("Order Service"), QStringLiteral("Payment Gateway"), QStringLiteral("DB"),
                              QStringLiteral("Inventory Sync"), QStringLiteral("Auth"), QStringLiteral("Mail Sender"),
                              QStringLiteral("Payment Gateway"), QStringLiteral("Payment Gateway") };
        Format f = hard(QStringLiteral("Hard: width"), 203, comps, [&](Gen& g, const QString& c) {
            return QStringLiteral("%1 %2 %3 %4").arg(ts(g), pad(g.level(), 5), pad(c, 15), g.message());
        });
        f.hardKind = Kind::Width;
        f.hardCount = 15;
        fs.append(f);
    }
    {
        const QStringList two = { QStringLiteral("Order Service"), QStringLiteral("Payment Gateway"),
                                  QStringLiteral("Auth Provider"), QStringLiteral("Cache Manager"),
                                  QStringLiteral("Mail Sender"), QStringLiteral("Report Builder") };
        Format f = hard(QStringLiteral("Hard: constant words"), 204, two, [&](Gen& g, const QString& c) {
            return QStringLiteral("%1 %2 %3 %4").arg(ts(g), g.level(), c, g.message());
        });
        f.hardKind = Kind::Words;
        f.hardCount = 2;
        fs.append(f);
    }
    {
        Format f = hard(QStringLiteral("Hard: closed set"), 205, kComponents6, [&](Gen& g, const QString& c) {
            return QStringLiteral("%1 %2 %3 %4").arg(ts(g), g.level(), c, g.message());
        });
        f.hardKind = Kind::Of;
        f.hardOf = kComponents6;
        fs.append(f);
    }
    {
        Format f = hard(QStringLiteral("Hard: open set"), 206, {}, [&](Gen& g, const QString& c) {
            return QStringLiteral("%1 %2 %3 %4").arg(ts(g), g.level(), c, g.message());
        });
        f.expectSplit = false;
        fs.append(f);
    }

    // ---- 15 диких --------------------------------------------------------
    const auto wild = [&](const QString& name, quint32 seed, const QVector<Kind>& kinds, const LineFn& fn,
                          const QStringList& cont = {}, double contChance = 0) {
        Format f{ name, Group::Wild };
        f.expectKinds = kinds;
        f.lines = generate(seed, N, fn, cont, contChance);
        fs.append(f);
    };
    wild(QStringLiteral("Android logcat"), 301, { Kind::Time, Kind::Int }, [](Gen& g) {
        return QStringLiteral("11-18 %1.%2 %3 %4 %5 %6: %7").arg(g.clock(), g.msPart(), rpad(QString::number(g.num(100, 4000)), 5),
            rpad(QString::number(g.num(100, 9000)), 5), g.pickS({ "I", "I", "D", "W", "E", "V" }),
            g.pickS({ "ActivityManager", "PackageManager", "WifiService", "chromium", "OkHttp" }), g.message());
    });
    wild(QStringLiteral("glog / klog"), 302, { Kind::Time, Kind::Int }, [](Gen& g) {
        return QStringLiteral("%1%2 %3.%4%5 %6 %7:%8] %9").arg(g.pickS({ "I", "I", "I", "W", "E" }), QStringLiteral("1118"), g.clock(),
            g.msPart(), QString::number(g.num(100, 999)), rpad(QString::number(g.num(1, 99999)), 7),
            g.pickS({ "server.go", "controller.go", "reflector.go", "main.cc" })).arg(g.num(10, 900)).arg(g.message());
    });
    wild(QStringLiteral("zap console"), 303, { Kind::Time, Kind::Level, Kind::Path }, [](Gen& g) {
        QString line = QStringLiteral("%1T%2.%3Z\t%4\t%5:%6\t%7").arg(g.isoDate(), g.clock(), g.msPart(),
            g.pickS({ "info", "info", "debug", "warn", "error" }), g.pickS({ "server/main.go", "api/handler.go", "store/db.go" }))
            .arg(g.num(10, 400)).arg(g.message());
        if (g.chance(0.5))
            line += QStringLiteral("\t{\"port\": %1}").arg(g.num(1000, 9000));
        return line;
    });
    wild(QStringLiteral("PostgreSQL"), 304, { Kind::Time, Kind::Int }, [](Gen& g) {
        return QStringLiteral("%1 %2.%3 UTC [%4] %5:  %6").arg(g.isoDate(), g.clock(), g.msPart()).arg(g.num(1000, 30000))
            .arg(g.pickS({ "LOG", "LOG", "LOG", "ERROR", "WARNING", "FATAL", "STATEMENT" }),
                 g.pickS({ QStringLiteral("database system is ready to accept connections"),
                          QStringLiteral("checkpoint starting: time"),
                          QStringLiteral("duration: %1.%2 ms  statement: SELECT * FROM orders").arg(g.num(1, 999)).arg(g.num(100, 999)),
                          QStringLiteral("relation \"%1\" does not exist at character %2").arg(g.word()).arg(g.num(10, 99)),
                          QStringLiteral("connection received: host=%1 port=%2").arg(g.ip()).arg(g.num(1024, 65000)),
                          QStringLiteral("automatic vacuum of table \"app.public.%1\"").arg(g.word()) }));
    });
    wild(QStringLiteral("MySQL error"), 305, { Kind::Time, Kind::Int }, [](Gen& g) {
        return QStringLiteral("%1T%2.%3%4Z %5 [%6] [MY-0%7] [%8] %9").arg(g.isoDate(), g.clock(), g.msPart(), QString::number(g.num(100, 999)))
            .arg(g.num(0, 12)).arg(g.pickS({ "System", "Warning", "Note", "ERROR" })).arg(g.num(10000, 13999))
            .arg(g.pickS({ "Server", "InnoDB", "Repl" }), g.message());
    });
    wild(QStringLiteral("Redis"), 306, { Kind::Int, Kind::Time }, [](Gen& g) {
        return QStringLiteral("%1:%2 18 Nov 2026 %3.%4 %5 %6").arg(g.num(1, 9999)).arg(g.pickS({ "M", "M", "S", "C" }), g.clock(),
            g.msPart(), g.pickS({ "*", "*", "#", "-", "." }),
            g.pickS({ QStringLiteral("Ready to accept connections tcp"), QStringLiteral("DB saved on disk"),
                      QStringLiteral("Background saving started by pid %1").arg(g.num(100, 9999)),
                      QStringLiteral("%1 changes in 60 seconds. Saving...").arg(g.num(1, 10000)),
                      QStringLiteral("Connection with replica %1:6379 lost.").arg(g.ip()),
                      QStringLiteral("WARNING overcommit_memory is set to 0!") }));
    });
    wild(QStringLiteral("HAProxy"), 307, { Kind::Time, Kind::Ip, Kind::Int }, [](Gen& g) {
        return QStringLiteral("Nov 18 %1 lb01 haproxy[%2]: %3:%4 [18/Nov/2026:%5.%6] %7 %8/%9 ").arg(g.clock()).arg(g.num(1000, 1003))
            .arg(g.ip()).arg(g.num(1024, 65000)).arg(g.clock(), g.msPart(), g.pickS({ "http-in", "https-in" }),
                 g.pickS({ "app", "static" }), g.pickS({ "web01", "web02" }))
            + QStringLiteral("%1/%2/%3/%4/%5 %6 %7 - - ---- %8/%8/0/0/0 0/0 \"%9\"").arg(g.num(0, 9)).arg(g.num(0, 9)).arg(g.num(0, 9))
                  .arg(g.num(0, 99)).arg(g.num(0, 999)).arg(g.pickS({ "200", "200", "302", "404", "503" })).arg(g.num(100, 90000))
                  .arg(g.num(1, 99)).arg(QStringLiteral("GET /%1 HTTP/1.1").arg(g.word()));
    });
    wild(QStringLiteral("dmesg"), 308, { Kind::Num }, [](Gen& g) {
        const QString t = QStringLiteral("%1.%2").arg(g.num(0, 99999)).arg(g.num(0, 999999), 6, 10, QLatin1Char('0'));
        return QStringLiteral("[%1] %2").arg(rpad(t, 12), g.pickS({
            QStringLiteral("usb 1-1: new high-speed USB device number %1 using xhci_hcd").arg(g.num(2, 9)),
            QStringLiteral("EXT4-fs (sda1): mounted filesystem with ordered data mode"),
            QStringLiteral("Linux version 6.1.0-13-amd64 (debian-kernel@lists.debian.org)"),
            QStringLiteral("e1000e 0000:00:1f.6 eth0: NIC Link is Up 1000 Mbps Full Duplex"),
            QStringLiteral("audit: type=1400 audit(1700315736.%1:%2): apparmor=\"STATUS\"").arg(g.num(100, 999)).arg(g.num(2, 99)),
            QStringLiteral("Out of memory: Killed process %1 (java)").arg(g.num(100, 9999)) }));
    });
    wild(QStringLiteral("log4net"), 309, { Kind::Time, Kind::Int, Kind::Level }, [](Gen& g) {
        return QStringLiteral("%1 %2,%3 [%4] %5 %6 - %7").arg(g.isoDate(), g.clock(), g.msPart()).arg(g.num(1, 40))
            .arg(pad(g.pickS({ "INFO", "INFO", "DEBUG", "WARN", "ERROR", "FATAL" }), 5),
                 g.pickS({ "MyApp.Program", "MyApp.Data.Repository", "MyApp.Web.Startup" }), g.message());
    }, kDotnetTrace, 0.03);
    wild(QStringLiteral("NLog"), 310, { Kind::Time, Kind::Level }, [](Gen& g) {
        return QStringLiteral("%1 %2.%3%4|%5|%6|%7").arg(g.isoDate(), g.clock(), g.msPart(), QString::number(g.num(0, 9)),
            g.pickS({ "INFO", "INFO", "DEBUG", "WARN", "ERROR", "TRACE" }),
            g.pickS({ "MyApp.Program", "MyApp.Data.Repository", "MyApp.Web.Startup" }), g.message());
    }, kDotnetTrace, 0.03);
    wild(QStringLiteral("Apache error"), 311, { Kind::Time }, [](Gen& g) {
        const QString client = g.chance(0.7) ? QStringLiteral("[client %1:%2] ").arg(g.ip()).arg(g.num(1024, 65000)) : QString();
        return QStringLiteral("[Wed Nov 18 %1.%2%3 2026] [%4] [pid %5:tid %6] %7%8").arg(g.clock(), g.msPart(), QString::number(g.num(100, 999)),
            g.pickS({ "core:error", "ssl:warn", "mpm_event:notice", "authz_core:error" })).arg(g.num(1000, 9999)).arg(g.num(100000, 999999))
            .arg(client, g.pickS({ QStringLiteral("AH00037: Symbolic link not allowed or link target not accessible: /var/www/html/%1").arg(g.word()),
                                   QStringLiteral("AH00489: Apache/2.4.58 (Unix) configured -- resuming normal operations"),
                                   QStringLiteral("AH01630: client denied by server configuration: /srv/%1").arg(g.word()) }));
    });
    wild(QStringLiteral("Windows CBS"), 312, { Kind::Time, Kind::Level }, [](Gen& g) {
        return QStringLiteral("%1 %2, %3 %4 %5").arg(g.isoDate(), g.clock(), pad(g.pickS({ "Info", "Info", "Info", "Warning", "Error" }), 21),
            pad(g.pickS({ "CBS", "CSI", "DPX" }), 6), g.message());
    });
    wild(QStringLiteral("Tomcat catalina"), 313, { Kind::Time, Kind::Level }, [](Gen& g) {
        return QStringLiteral("18-Nov-2026 %1.%2 %3 [%4] %5 %6").arg(g.clock(), g.msPart(), g.pickS({ "INFO", "INFO", "WARNING", "SEVERE" }),
            g.pickS({ "main", "http-nio-8080-exec-1", "Catalina-utility-2" }),
            g.pickS({ "org.apache.catalina.startup.Catalina.start", "org.apache.coyote.AbstractProtocol.init",
                      "org.apache.catalina.core.StandardService.startInternal" }), g.message());
    });
    wild(QStringLiteral("Squid access"), 314, { Kind::Num, Kind::Int, Kind::Ip }, [](Gen& g) {
        return QStringLiteral("17003%1.%2 %3 %4 %5/%6 %7 %8 http://%9.example.com/ - HIER_DIRECT/93.184.216.34 text/html")
            .arg(g.num(10000, 99999)).arg(g.msPart()).arg(rpad(QString::number(g.num(1, 9999)), 6)).arg(g.ip(),
                 g.pickS({ "TCP_MISS", "TCP_HIT", "TCP_REFRESH_MODIFIED" }), g.pickS({ "200", "200", "304" }))
            .arg(g.num(100, 90000)).arg(g.pickS({ "GET", "GET", "POST" }), g.word());
    });
    wild(QStringLiteral("RabbitMQ"), 315, { Kind::Time }, [](Gen& g) {
        return QStringLiteral("%1 %2.%3%4+00:00 [%5] <0.%6.0> %7").arg(g.isoDate(), g.clock(), g.msPart(), QString::number(g.num(100, 999)),
            g.pickS({ "info", "info", "warning", "error", "debug" })).arg(g.num(100, 999)).arg(g.message());
    });
    return fs;
}

// Известные неудачи корпуса (печатаемая таблица закрепляет их: формат,
// который начал проходить или перестал, — провал, ожидание меняется
// осознанно). Причины — в docs/design/F-17.md, «Сделано → шаг 4».
const QSet<QString> kKnownFailures = {
};

// ---------------------------------------------------------------------------
// Проверка формата.
// ---------------------------------------------------------------------------

struct Column {
    int index = -1;
    Kind kind = Kind::Text;
    int count = 0;
    QStringList values;
};

QVector<Column> columnsOf(const LineTemplate& t)
{
    QVector<Column> cols(t.columns().size());
    for (const LineTemplateElement& e : t.elements()) {
        if (e.type == LineTemplateElement::Type::Field && e.column >= 0) {
            cols[e.column].index = e.column;
            cols[e.column].kind = e.kind;
            cols[e.column].count = e.count;
            cols[e.column].values = e.values;
        }
    }
    return cols;
}

int columnOfKind(const LineTemplate& t, Kind kind)
{
    for (const Column& c : columnsOf(t)) {
        if (c.kind == kind)
            return c.index;
    }
    return -1;
}

QString readTemplate(const QString& file)
{
    QFile f(QDir(QStringLiteral(DENDROLOG_TEMPLATES_SOURCE_DIR)).filePath(file));
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    LineTemplateSchema schema;
    if (!LineTemplateSchema::read(f.readAll(), &schema))
        return QString();
    return schema.templateText;
}

QString kindsText(const LineTemplate& t)
{
    QStringList out;
    for (const Column& c : columnsOf(t))
        out.append(LineTemplate::kindName(c.kind));
    return out.join(QLatin1Char(','));
}

struct Outcome {
    bool pass = false;
    QString detail;
    double coverage = 0;
    QString templateText;
};

Outcome judge(const Format& f)
{
    Outcome o;
    const SchemaInference::Result r = SchemaInference::infer(f.lines);
    if (!r.ok()) {
        o.detail = QStringLiteral("no variant: ") + r.failureText;
        return o;
    }
    const SchemaInference::Variant& v = r.variants.first();
    o.coverage = v.fit.coverage();
    o.templateText = v.templateText;
    const LineTemplate inferred(v.templateText);
    check(inferred.isValid(), f.name + QStringLiteral(": variant compiles"));
    for (const SchemaInference::Variant& other : r.variants)
        check(LineTemplate(other.templateText).isValid(), f.name + QStringLiteral(": every variant compiles"));
    LogEntryFields got;

    if (f.group == Group::Builtin) {
        const LineTemplate builtin(readTemplate(f.builtinFile));
        check(builtin.isValid(), f.name + QStringLiteral(": built-in template read"));
        const int bTime = columnOfKind(builtin, Kind::Time);
        const int bLevel = int(builtin.columns().indexOf(QStringLiteral("Level")));
        const int bMsg = builtin.tailColumn();
        const int iTime = columnOfKind(inferred, Kind::Time);
        const int iLevel = int(inferred.columns().indexOf(QStringLiteral("Level")));
        const int iMsg = inferred.tailColumn();
        int total = 0, same = 0;
        LogEntryFields want;
        QString firstBad;
        const SchemaFit::Corpus lines = SchemaFit::corpus(f.lines);
        for (int li = 0; li < lines.samples.size(); ++li) {
            if (!lines.candidate[li])
                continue;
            const QString& line = lines.samples[li].text;
            if (!FieldExtractor::accepted(builtin.match(line, &want)))
                continue;
            ++total;
            const bool acc = FieldExtractor::accepted(inferred.match(line, &got));
            bool ok = acc;
            const auto cmp = [&](int b, int i) {
                if (b < 0)
                    return;
                const QString wv = want.value(b, line);
                const QString gv = i >= 0 ? got.value(i, line) : QString();
                ok = ok && wv == gv;
            };
            cmp(bTime, iTime);
            cmp(bLevel, iLevel);
            cmp(bMsg, iMsg);
            if (ok)
                ++same;
            else if (firstBad.isEmpty())
                firstBad = line;
        }
        o.pass = total > 0 && same >= total * 0.95;
        o.detail = QStringLiteral("same T/L/M %1/%2").arg(same).arg(total);
        if (!o.pass && !firstBad.isEmpty())
            o.detail += QStringLiteral(" first differing: ") + firstBad.left(90);
        return o;
    }

    if (f.group == Group::Wild) {
        QVector<Kind> have;
        for (const Column& c : columnsOf(inferred))
            have.append(c.kind);
        bool kinds = true;
        for (Kind k : f.expectKinds) {
            const int at = int(have.indexOf(k));
            if (at < 0) {
                kinds = false;
                break;
            }
            have.remove(at);
        }
        o.pass = kinds && o.coverage >= 0.95;
        o.detail = QStringLiteral("kinds %1%2").arg(kindsText(inferred), kinds ? QString() : QStringLiteral(" (missing)"));
        return o;
    }

    // Трудная граница.
    if (!f.expectSplit) {
        // Не разбито: за Level сразу Message, заметка NoBoundary.
        const int lvl = int(inferred.columns().indexOf(QStringLiteral("Level")));
        const int msg = inferred.tailColumn();
        int good = 0;
        for (int i = 0; i < f.lines.size(); ++i) {
            if (FieldExtractor::accepted(inferred.match(f.lines[i], &got)) && msg >= 0
                && got.value(msg, f.lines[i]).startsWith(f.hardValues[i] + QLatin1Char(' ')))
                ++good;
        }
        bool noted = false;
        for (const Note& n : v.notes)
            noted = noted || (n.kind == Note::Kind::NoBoundary && n.after == QLatin1String("Level"));
        bool split = false;
        for (const SchemaInference::Variant& other : r.variants)
            split = split || other.hardFields > 0;
        o.pass = lvl >= 0 && good >= f.lines.size() * 0.95 && noted && !split;
        o.detail = QStringLiteral("message keeps name %1/%2, note %3, hard fields %4").arg(good).arg(f.lines.size())
                       .arg(noted ? QStringLiteral("yes") : QStringLiteral("no"), split ? QStringLiteral("yes") : QStringLiteral("no"));
        return o;
    }
    // Поле нужного вида, значения — названия.
    int col = -1;
    for (const Column& c : columnsOf(inferred)) {
        if (c.kind == f.hardKind && (f.hardCount == 0 || c.count == f.hardCount)) {
            col = c.index;
            break;
        }
    }
    int good = 0;
    if (col >= 0) {
        for (int i = 0; i < f.lines.size(); ++i) {
            if (FieldExtractor::accepted(inferred.match(f.lines[i], &got)) && got.value(col, f.lines[i]) == f.hardValues[i])
                ++good;
        }
    }
    bool ofOk = true;
    if (f.hardKind == Kind::Of && col >= 0) {
        QStringList vs = columnsOf(inferred)[col].values;
        QStringList want = f.hardOf;
        vs.sort();
        want.sort();
        ofOk = vs == want;
    }
    // Осторожный вариант: до трудной границы, дальше — Message.
    bool cautious = false;
    for (const SchemaInference::Variant& other : r.variants) {
        if (other.kind == SchemaInference::Variant::Kind::Cautious && other.hardFields == 0) {
            const LineTemplate ct(other.templateText);
            cautious = ct.isValid() && ct.tailColumn() >= 0 && ct.columns().contains(QStringLiteral("Timestamp"))
                && ct.columns().size() < inferred.columns().size();
        }
    }
    check(cautious, f.name + QStringLiteral(": cautious variant up to the hard boundary"));
    o.pass = col >= 0 && ofOk && good >= f.lines.size() * 0.95 && v.hardFields > 0
        && v.kind == SchemaInference::Variant::Kind::Full;
    o.detail = QStringLiteral("field %1 %2/%3").arg(col >= 0 ? inferred.columns().at(col) : QStringLiteral("none"))
                   .arg(good).arg(f.lines.size());
    return o;
}

void testCorpus()
{
    const QVector<Format> fs = corpus();
    check(fs.size() == 30, QStringLiteral("30 formats in the corpus"));
    int passed = 0, hardPassed = 0, hardTotal = 0;
    std::printf("  %-24s %-4s %-6s %s\n", "format", "ok", "cover", "template / detail");
    for (const Format& f : fs) {
        const Outcome o = judge(f);
        passed += o.pass ? 1 : 0;
        if (f.group == Group::Hard) {
            ++hardTotal;
            hardPassed += o.pass ? 1 : 0;
        }
        std::printf("  %-24s %-4s %5.3f  %s\n      %s\n", qPrintable(f.name), o.pass ? "yes" : "NO", o.coverage,
                    qPrintable(o.templateText), qPrintable(o.detail));
        check(o.pass != kKnownFailures.contains(f.name),
              QStringLiteral("%1: %2 (known failures: %3)").arg(f.name, o.pass ? QStringLiteral("passes") : QStringLiteral("fails"),
                                                                kKnownFailures.contains(f.name) ? QStringLiteral("listed") : QStringLiteral("not listed")));
    }
    std::printf("  passed %d of %d; hard boundaries %d of %d\n", passed, int(fs.size()), hardPassed, hardTotal);
    check(passed >= 26, QStringLiteral("at least 26 of 30 formats pass"));
    check(hardPassed == hardTotal, QStringLiteral("all hard boundaries pass"));
}

// ---------------------------------------------------------------------------
// Отказы, два вида строк.
// ---------------------------------------------------------------------------

void testFailures()
{
    using SchemaInference::Failure;
    {
        const auto r = SchemaInference::infer(QStringList{});
        check(!r.ok() && r.failure == Failure::NoLines, QStringLiteral("empty input — NoLines"));
        const auto r2 = SchemaInference::infer(QStringList{ QString(), QStringLiteral("   ") });
        check(!r2.ok() && r2.failure == Failure::NoLines, QStringLiteral("blank lines — NoLines"));
    }
    {
        QStringList json;
        Gen g(401);
        for (int i = 0; i < 100; ++i) {
            g.tick();
            json.append(QStringLiteral("{\"time\":\"2026-11-18T%1Z\",\"level\":\"info\",\"msg\":\"%2\"}").arg(g.clock(), g.message()));
        }
        const auto r = SchemaInference::infer(json);
        check(!r.ok() && r.failure == Failure::KeyedLines, QStringLiteral("JSON Lines — KeyedLines"));
        QStringList logfmt;
        for (int i = 0; i < 100; ++i)
            logfmt.append(QStringLiteral("time=2026-11-18T%1Z level=info msg=\"%2\"").arg(g.clock(), g.message()));
        const auto r2 = SchemaInference::infer(logfmt);
        check(!r2.ok() && r2.failure == Failure::KeyedLines, QStringLiteral("logfmt — KeyedLines"));
    }
    {
        // Проза: слова из словаря в случайном порядке — структуры нет.
        const QStringList vocab = QStringLiteral(
            "the a system user request failed started cache order payment node disk file network timeout "
            "retry queue worker thread memory process service config update delete create read write lock "
            "token session client server port host value key index table record batch job task event "
            "metric alert error warning notice debug trace info data stream buffer socket packet frame").split(QLatin1Char(' '));
        QStringList prose;
        Gen g(402);
        for (int i = 0; i < 400; ++i) {
            QStringList w;
            const int len = g.num(3, 12);
            for (int k = 0; k < len; ++k)
                w.append(g.pickS(vocab));
            w[0][0] = w[0][0].toUpper();
            prose.append(w.join(QLatin1Char(' ')));
        }
        const auto r = SchemaInference::infer(prose);
        bool structure = false;
        for (const auto& v : r.variants)
            structure = structure || v.fit.specificity() >= 1.0;
        check(!structure, QStringLiteral("prose — no structure (%1)").arg(r.ok() ? r.variants.first().templateText : r.failureText));
    }
}

void testCancelAndBudget()
{
    using SchemaInference::Failure;
    const QStringList lines = generate(901, 300, [](Gen& g) {
        return QStringLiteral("%1 %2.%3 [main] %4 %5 - %6").arg(g.isoDate(), g.clock(), g.msPart(), pad(g.level(), 5),
                                                              g.javaLogger(), g.message());
    });
    {
        std::atomic_bool cancel{true};
        SchemaInference::Options o;
        o.cancel = &cancel;
        const auto r = SchemaInference::infer(lines, o);
        check(!r.ok() && r.failure == Failure::Cancelled, QStringLiteral("cancel before start — Cancelled"));
    }
    {
        // Отмена посреди оценки: часы бюджета поднимают флаг на первой проверке.
        std::atomic_bool cancel{false};
        SchemaInference::Options o;
        o.cancel = &cancel;
        o.budget.clock = [&cancel]() { cancel.store(true); return qint64(0); };
        const auto r = SchemaInference::infer(lines, o);
        check(!r.ok() && r.failure == Failure::Cancelled, QStringLiteral("cancel during the fit — Cancelled"));
    }
    {
        // Бюджет исчерпан у каждого варианта — TimedOut, а не «нет структуры».
        qint64 now = 0;
        SchemaInference::Options o;
        o.budget.maxMs = 10;
        o.budget.clock = [&now]() { now += 1000; return now; };
        const auto r = SchemaInference::infer(lines, o);
        check(!r.ok() && r.failure == Failure::TimedOut, QStringLiteral("budget exhausted — TimedOut (%1)").arg(int(r.failure)));
    }
}

void testFarBracket()
{
    // Закрывающая скобка дальше четырёх слов — всё равно якорь.
    const QStringList threads = { QStringLiteral("main"), QStringLiteral("a b c d e f"), QStringLiteral("pool worker one two three four five"),
                                  QStringLiteral("x y z w v u t s") };
    QStringList lines;
    QStringList want;
    Gen g(902);
    for (int i = 0; i < 300; ++i) {
        g.tick();
        const QString t = g.pickS(threads);
        lines.append(QStringLiteral("%1 %2 [%3] %4 %5").arg(g.isoDate(), g.clock(), t, g.level(), g.message()));
        want.append(t);
    }
    const auto r = SchemaInference::infer(lines);
    check(r.ok(), QStringLiteral("far bracket: a variant"));
    if (!r.ok())
        return;
    const LineTemplate t(r.variants.first().templateText);
    const int thread = int(t.columns().indexOf(QStringLiteral("Thread")));
    const int level = int(t.columns().indexOf(QStringLiteral("Level")));
    int good = 0;
    LogEntryFields f;
    for (int i = 0; i < lines.size(); ++i) {
        if (FieldExtractor::accepted(t.match(lines[i], &f)) && thread >= 0 && level >= 0 && f.value(thread, lines[i]) == want[i])
            ++good;
    }
    check(good == lines.size(), QStringLiteral("far bracket: Thread and Level (%1, %2/%3)").arg(t.text()).arg(good).arg(lines.size()));
}

void testTwoKinds()
{
    // 70 % — Logback, 30 % — Python logging.
    QStringList lines;
    Gen g(501);
    for (int i = 0; i < 400; ++i) {
        g.tick();
        if (g.chance(0.7))
            lines.append(QStringLiteral("%1 %2.%3 [main] %4 %5 - %6").arg(g.isoDate(), g.clock(), g.msPart(), pad(g.level(), 5),
                                                                        g.javaLogger(), g.message()));
        else
            lines.append(QStringLiteral("%1 %2,%3 - %4 - %5 - %6").arg(g.isoDate(), g.clock(), g.msPart(), QStringLiteral("app.db"),
                                                                     g.pickS({ "INFO", "WARNING", "ERROR" }), g.message()));
    }
    const auto r = SchemaInference::infer(lines);
    check(r.ok(), QStringLiteral("two kinds: a variant"));
    bool noted = false;
    for (const auto& v : r.variants) {
        for (const Note& n : v.notes) {
            if (n.kind == Note::Kind::TwoKinds) {
                noted = true;
                check(LineTemplate(n.otherTemplate).isValid(), QStringLiteral("two kinds: other template compiles"));
                check(n.otherShare > 0.2 && n.otherShare < 0.4,
                      QStringLiteral("two kinds: share of the other kind %1").arg(n.otherShare));
            }
        }
    }
    check(noted, QStringLiteral("two kinds: note TwoKinds (%1)").arg(r.ok() ? r.variants.first().templateText : QString()));
}

// ---------------------------------------------------------------------------
// Фаззинг: результат всегда компилируется.
// ---------------------------------------------------------------------------

void testFuzz()
{
    QRandomGenerator rng(601);
    const QString alphabet = QStringLiteral("abcXYZ019 \t[](){}<>:;,.-_/\\|\"'=*#@!?%{}йж");
    int variants = 0;
    for (int round = 0; round < 300; ++round) {
        QStringList lines;
        const int n = 1 + int(rng.bounded(80u));
        for (int i = 0; i < n; ++i) {
            QString s;
            const int len = int(rng.bounded(60u));
            for (int k = 0; k < len; ++k)
                s += alphabet[int(rng.bounded(quint32(alphabet.size())))];
            lines.append(s);
        }
        const auto r = SchemaInference::infer(lines);
        for (const auto& v : r.variants) {
            ++variants;
            const LineTemplate t(v.templateText);
            check(t.isValid(), QStringLiteral("fuzz random: compiles: %1 — %2").arg(v.templateText, t.error().message));
        }
    }
    // Строки из случайной последовательности токенов (структура есть).
    const QStringList pieces = { QStringLiteral("{ts}"), QStringLiteral("[{lvl}]"), QStringLiteral("{lvl}"), QStringLiteral("{n}"),
                                 QStringLiteral("[{w}]"), QStringLiteral("{w}"), QStringLiteral("-"), QStringLiteral(":"),
                                 QStringLiteral("|"), QStringLiteral("\"{w} {w}\""), QStringLiteral("{ip}"), QStringLiteral("<{n}>"),
                                 QStringLiteral("{w}:{n}"), QStringLiteral("({w})") };
    for (int round = 0; round < 300; ++round) {
        QStringList shape;
        const int parts = 1 + int(rng.bounded(7u));
        for (int k = 0; k < parts; ++k)
            shape.append(pieces[int(rng.bounded(quint32(pieces.size())))]);
        const QString sep = rng.bounded(4u) == 0 ? QStringLiteral("  ") : QStringLiteral(" ");
        Gen g(700 + round);
        QStringList lines;
        for (int i = 0; i < 120; ++i) {
            g.tick();
            QString line;
            for (const QString& p : shape) {
                QString t = p;
                t.replace(QStringLiteral("{ts}"), QStringLiteral("%1 %2").arg(g.isoDate(), g.clock()));
                while (t.contains(QStringLiteral("{lvl}")))
                    t.replace(t.indexOf(QStringLiteral("{lvl}")), 5, g.level());
                while (t.contains(QStringLiteral("{n}")))
                    t.replace(t.indexOf(QStringLiteral("{n}")), 3, QString::number(g.num(0, 99999)));
                while (t.contains(QStringLiteral("{w}")))
                    t.replace(t.indexOf(QStringLiteral("{w}")), 3, g.word());
                t.replace(QStringLiteral("{ip}"), g.ip());
                line += t + sep;
            }
            line += g.message();
            lines.append(line);
        }
        const auto r = SchemaInference::infer(lines);
        for (const auto& v : r.variants) {
            ++variants;
            const LineTemplate t(v.templateText);
            check(t.isValid(), QStringLiteral("fuzz shaped: compiles: %1 — %2").arg(v.templateText, t.error().message));
        }
        if (shape.join(QString()).contains(QLatin1Char('{')))
            check(r.ok(), QStringLiteral("fuzz shaped: a variant for %1").arg(shape.join(sep)));
    }
    std::printf("  %d variants checked\n", variants);
}

// ---------------------------------------------------------------------------
// Линейность по счётчику.
// ---------------------------------------------------------------------------

void testLinear()
{
    const auto lines = [](int n) {
        return generate(801, n, [](Gen& g) {
            return QStringLiteral("%1 %2.%3 [%4] %5 %6 - %7").arg(g.isoDate(), g.clock(), g.msPart(),
                g.pickS({ "main", "worker-1", "worker-2" }), pad(g.level(), 5), g.javaLogger(), g.message());
        });
    };
    const auto small = SchemaInference::infer(lines(500));
    const auto big = SchemaInference::infer(lines(2000));
    std::printf("  steps: 500 lines — %lld, 2000 lines — %lld (x%.2f)\n", (long long)small.steps, (long long)big.steps,
                double(big.steps) / std::max<qint64>(1, small.steps));
    check(small.steps > 0 && big.steps <= small.steps * 5, QStringLiteral("steps grow linearly with lines"));
    // Длинные строки: окно, а не вся строка.
    const auto longLines = [](int len) {
        QStringList out;
        Gen g(802);
        for (int i = 0; i < 300; ++i) {
            g.tick();
            QString tail;
            while (tail.size() < len)
                tail += g.message() + QLatin1Char(' ');
            out.append(QStringLiteral("%1 %2 %3 %4").arg(g.isoDate(), g.clock(), g.level(), tail));
        }
        return out;
    };
    const auto shortL = SchemaInference::infer(longLines(200));
    const auto longL = SchemaInference::infer(longLines(20000));
    std::printf("  steps: 200-char lines — %lld, 20000-char lines — %lld\n", (long long)shortL.steps, (long long)longL.steps);
    check(longL.steps <= shortL.steps * 3, QStringLiteral("steps do not grow with line length (except the fit)"));
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::printf("1. корпус из 30 форматов\n");
    testCorpus();
    std::printf("2. отказы\n");
    testFailures();
    std::printf("3. два вида строк, отмена, бюджет, скобка вдали\n");
    testTwoKinds();
    testCancelAndBudget();
    testFarBracket();
    std::printf("4. фаззинг — всегда компилируется\n");
    testFuzz();
    std::printf("5. линейность\n");
    testLinear();
    std::printf("%d проверок, %d провалов\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
