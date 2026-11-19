// Встроенные схемы полей (resources/patterns/*.json) и их установка.
//
//   1. Каждая встроенная схема на образцах настоящих строк своего формата
//      извлекает поля с ожидаемыми значениями, а строки-продолжения
//      (стек-трейсы, traceback) не принимает за начало записи.
//   2. Файл из тех же образцов, загруженный обоими путями хранилища
//      (LogParser и LogIndexer), даёт одинаковые границы записей и те же поля.
//   3. Установка BuiltinSchemas::install: пустой каталог, повторный запуск,
//      исправленная встроенная схема в новой версии, изменённая и удалённая
//      пользователем схема, чужой файл того же имени, новая схема в новой
//      версии, потерянный манифест.
//   4. Каждый файл схемы перечислен в resources/resources.qrc.
//   5. Метки времени форматов распознаёт LineClassifier (F-10), а схемы,
//      исправленные ради этого после Q-5 (q5/ — их прежние версии),
//      доезжают до пользователя, который их не трогал.
//
// Каталог схем в исходниках передаётся define DENDROLOG_PATTERNS_SOURCE_DIR.

#include "builtinschemas.h"
#include "lineclassifier.h"
#include "logpattern.h"
#include "patternheuristics.h"
#include "testdocuments.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QTemporaryDir>

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

const QString kSourceDir = QStringLiteral(DENDROLOG_PATTERNS_SOURCE_DIR);

// Ожидаемые поля строки: имя -> значение. Поля схемы, которых здесь нет,
// обязаны быть пустыми.
using Fields = QMap<QString, QString>;

struct Sample {
    QString line;
    bool    continuation = false;   // строка-продолжение: схема её не принимает
    Fields  fields;
};

Sample rec(const QString& line, const Fields& fields) { return {line, false, fields}; }
Sample cont(const QString& line) { return {line, true, {}}; }

struct SchemaCase {
    QString file;
    QString name;
    QVector<Sample> samples;
};

QVector<SchemaCase> cases()
{
    QVector<SchemaCase> c;

    c.append({QStringLiteral("logback.json"), QStringLiteral("Logback / Log4j"), {
        rec(QStringLiteral("2026-11-18 13:55:36.123 [main] INFO  com.example.demo.Application - Started Application in 2.871 seconds"),
            {{"Timestamp", "2026-11-18 13:55:36.123"}, {"Thread", "main"}, {"Level", "INFO"},
             {"Logger", "com.example.demo.Application"}, {"Message", "Started Application in 2.871 seconds"}}),
        rec(QStringLiteral("2026-11-18 13:55:37,004 [http-nio-8080-exec-1] DEBUG o.s.web.servlet.DispatcherServlet - GET \"/api/orders?id=42\", parameters={masked}"),
            {{"Timestamp", "2026-11-18 13:55:37,004"}, {"Thread", "http-nio-8080-exec-1"}, {"Level", "DEBUG"},
             {"Logger", "o.s.web.servlet.DispatcherServlet"}, {"Message", "GET \"/api/orders?id=42\", parameters={masked}"}}),
        rec(QStringLiteral("2026-11-18 13:55:38.250 [RMI TCP Connection(2)-127.0.0.1] WARN  org.hibernate.orm.deprecation - HHH90000025: MySQL8Dialect does not need to be specified explicitly"),
            {{"Timestamp", "2026-11-18 13:55:38.250"}, {"Thread", "RMI TCP Connection(2)-127.0.0.1"}, {"Level", "WARN"},
             {"Logger", "org.hibernate.orm.deprecation"}, {"Message", "HHH90000025: MySQL8Dialect does not need to be specified explicitly"}}),
        rec(QStringLiteral("2026-11-18 13:55:39.731 [pool-3-thread-7] ERROR c.e.billing.InvoiceService - Failed to send invoice 2026-0042 - retrying"),
            {{"Timestamp", "2026-11-18 13:55:39.731"}, {"Thread", "pool-3-thread-7"}, {"Level", "ERROR"},
             {"Logger", "c.e.billing.InvoiceService"}, {"Message", "Failed to send invoice 2026-0042 - retrying"}}),
        cont(QStringLiteral("java.lang.IllegalStateException: SMTP server did not respond")),
        cont(QStringLiteral("\tat com.example.billing.MailGateway.send(MailGateway.java:88)")),
        cont(QStringLiteral("\tat com.example.billing.InvoiceService.sendInvoice(InvoiceService.java:131)")),
        cont(QStringLiteral("Caused by: java.net.SocketTimeoutException: Read timed out")),
        cont(QStringLiteral("\t... 12 common frames omitted")),
        // Logback BasicConfigurator (1.3+): время без даты, «-%kvp-» без пар.
        rec(QStringLiteral("13:55:40.016 [main] TRACE my-app.startup -- Context initialized"),
            {{"Timestamp", "13:55:40.016"}, {"Thread", "main"}, {"Level", "TRACE"},
             {"Logger", "my-app.startup"}, {"Message", "Context initialized"}}),
    }});

    c.append({QStringLiteral("spring-boot.json"), QStringLiteral("Spring Boot"), {
        rec(QStringLiteral("2026-11-18T13:55:36.130Z  INFO 34786 --- [myapp] [           main] o.s.b.d.f.logexample.MyApplication       : Starting MyApplication using Java 17.0.12 with PID 34786 (/opt/apps/myapp.jar started by myuser in /opt/apps/)"),
            {{"Timestamp", "2026-11-18T13:55:36.130Z"}, {"Level", "INFO"}, {"PID", "34786"}, {"Thread", "main"},
             {"Logger", "o.s.b.d.f.logexample.MyApplication"},
             {"Message", "Starting MyApplication using Java 17.0.12 with PID 34786 (/opt/apps/myapp.jar started by myuser in /opt/apps/)"}}),
        rec(QStringLiteral("2026-11-18T13:55:37.512+01:00  WARN 34786 --- [myapp] [nio-8080-exec-3] o.h.engine.jdbc.spi.SqlExceptionHelper   : SQL Error: 0, SQLState: 08001"),
            {{"Timestamp", "2026-11-18T13:55:37.512+01:00"}, {"Level", "WARN"}, {"PID", "34786"}, {"Thread", "nio-8080-exec-3"},
             {"Logger", "o.h.engine.jdbc.spi.SqlExceptionHelper"}, {"Message", "SQL Error: 0, SQLState: 08001"}}),
        // Spring Boot 2.x и 3.x без spring.application.name: без [app].
        rec(QStringLiteral("2026-11-18 13:55:38.112  INFO 45469 --- [           main] org.apache.catalina.core.StandardEngine  : Starting Servlet Engine: Apache Tomcat/9.0.83"),
            {{"Timestamp", "2026-11-18 13:55:38.112"}, {"Level", "INFO"}, {"PID", "45469"}, {"Thread", "main"},
             {"Logger", "org.apache.catalina.core.StandardEngine"}, {"Message", "Starting Servlet Engine: Apache Tomcat/9.0.83"}}),
        rec(QStringLiteral("2026-11-18 13:55:39.020 ERROR 45469 --- [nio-8080-exec-1] o.a.c.c.C.[.[.[/].[dispatcherServlet]    : Servlet.service() for servlet [dispatcherServlet] threw exception"),
            {{"Timestamp", "2026-11-18 13:55:39.020"}, {"Level", "ERROR"}, {"PID", "45469"}, {"Thread", "nio-8080-exec-1"},
             {"Logger", "o.a.c.c.C.[.[.[/].[dispatcherServlet]"}, {"Message", "Servlet.service() for servlet [dispatcherServlet] threw exception"}}),
        cont(QStringLiteral("java.lang.NullPointerException: Cannot invoke \"String.length()\" because \"name\" is null")),
        cont(QStringLiteral("\tat com.example.web.GreetingController.greet(GreetingController.java:21) ~[classes/:na]")),
        cont(QStringLiteral("\tat java.base/jdk.internal.reflect.DirectMethodHandleAccessor.invoke(DirectMethodHandleAccessor.java:103) ~[na:na]")),
        rec(QStringLiteral("2026-11-18 13:55:40.000 DEBUG 45469 --- [   scheduling-1] c.e.jobs.CleanupJob                      : Removed 0 expired sessions"),
            {{"Timestamp", "2026-11-18 13:55:40.000"}, {"Level", "DEBUG"}, {"PID", "45469"}, {"Thread", "scheduling-1"},
             {"Logger", "c.e.jobs.CleanupJob"}, {"Message", "Removed 0 expired sessions"}}),
    }});

    c.append({QStringLiteral("access-combined.json"), QStringLiteral("nginx / Apache access (combined)"), {
        rec(QStringLiteral("127.0.0.1 - frank [10/Oct/2026:13:55:36 -0700] \"GET /apache_pb.gif HTTP/1.0\" 200 2326 \"http://www.example.com/start.html\" \"Mozilla/4.08 [en] (Win98; I ;Nav)\""),
            {{"Client", "127.0.0.1"}, {"User", "frank"}, {"Time", "10/Oct/2026:13:55:36 -0700"},
             {"Request", "GET /apache_pb.gif HTTP/1.0"}, {"Status", "200"}, {"Bytes", "2326"},
             {"Referer", "http://www.example.com/start.html"}, {"User agent", "Mozilla/4.08 [en] (Win98; I ;Nav)"}}),
        rec(QStringLiteral("203.0.113.42 - - [18/Nov/2026:13:55:36 +0000] \"POST /api/v1/login HTTP/1.1\" 401 64 \"-\" \"curl/8.5.0\""),
            {{"Client", "203.0.113.42"}, {"User", "-"}, {"Time", "18/Nov/2026:13:55:36 +0000"},
             {"Request", "POST /api/v1/login HTTP/1.1"}, {"Status", "401"}, {"Bytes", "64"},
             {"Referer", "-"}, {"User agent", "curl/8.5.0"}}),
        rec(QStringLiteral("2001:db8::1 - - [18/Nov/2026:13:55:37 +0000] \"GET /index.html?q=a%20b HTTP/2.0\" 304 0 \"https://example.org/\" \"Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36\""),
            {{"Client", "2001:db8::1"}, {"User", "-"}, {"Time", "18/Nov/2026:13:55:37 +0000"},
             {"Request", "GET /index.html?q=a%20b HTTP/2.0"}, {"Status", "304"}, {"Bytes", "0"},
             {"Referer", "https://example.org/"},
             {"User agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"}}),
        // Мусорный запрос сканера: nginx экранирует байты как \xNN.
        rec(QStringLiteral("198.51.100.7 - - [18/Nov/2026:13:55:38 +0000] \"\\x16\\x03\\x01\\x00\\xA5\\x01\" 400 157 \"-\" \"-\""),
            {{"Client", "198.51.100.7"}, {"User", "-"}, {"Time", "18/Nov/2026:13:55:38 +0000"},
             {"Request", "\\x16\\x03\\x01\\x00\\xA5\\x01"}, {"Status", "400"}, {"Bytes", "157"},
             {"Referer", "-"}, {"User agent", "-"}}),
        // Пустой запрос и Apache: «-» вместо 0 байт, \" внутри строки.
        rec(QStringLiteral("192.0.2.10 - - [18/Nov/2026:13:55:39 +0100] \"-\" 408 - \"-\" \"Mozilla/5.0 \\\"quoted\\\" agent\""),
            {{"Client", "192.0.2.10"}, {"User", "-"}, {"Time", "18/Nov/2026:13:55:39 +0100"},
             {"Request", "-"}, {"Status", "408"}, {"Bytes", "-"},
             {"Referer", "-"}, {"User agent", "Mozilla/5.0 \\\"quoted\\\" agent"}}),
        // Common Log Format (без referer и user agent) читается той же схемой.
        rec(QStringLiteral("10.1.2.3 - alice [18/Nov/2026:13:55:40 +0000] \"GET /favicon.ico HTTP/1.1\" 404 209"),
            {{"Client", "10.1.2.3"}, {"User", "alice"}, {"Time", "18/Nov/2026:13:55:40 +0000"},
             {"Request", "GET /favicon.ico HTTP/1.1"}, {"Status", "404"}, {"Bytes", "209"}}),
    }});

    c.append({QStringLiteral("nginx-error.json"), QStringLiteral("nginx error"), {
        rec(QStringLiteral("2026/11/18 13:55:36 [error] 1234#1234: *99 open() \"/usr/share/nginx/html/favicon.ico\" failed (2: No such file or directory), client: 10.0.0.1, server: localhost, request: \"GET /favicon.ico HTTP/1.1\", host: \"localhost\""),
            {{"Timestamp", "2026/11/18 13:55:36"}, {"Level", "error"}, {"PID", "1234"}, {"TID", "1234"}, {"Connection", "99"},
             {"Message", "open() \"/usr/share/nginx/html/favicon.ico\" failed (2: No such file or directory), client: 10.0.0.1, server: localhost, request: \"GET /favicon.ico HTTP/1.1\", host: \"localhost\""}}),
        rec(QStringLiteral("2026/11/18 13:55:37 [notice] 1#1: using the \"epoll\" event method"),
            {{"Timestamp", "2026/11/18 13:55:37"}, {"Level", "notice"}, {"PID", "1"}, {"TID", "1"},
             {"Message", "using the \"epoll\" event method"}}),
        rec(QStringLiteral("2026/11/18 13:55:37 [notice] 1#1: start worker processes"),
            {{"Timestamp", "2026/11/18 13:55:37"}, {"Level", "notice"}, {"PID", "1"}, {"TID", "1"},
             {"Message", "start worker processes"}}),
        rec(QStringLiteral("2026/11/18 13:55:38 [warn] 31#31: *1052 an upstream response is buffered to a temporary file /var/cache/nginx/proxy_temp/1/00/0000000001 while reading upstream, client: 172.17.0.1, server: _, request: \"GET /big HTTP/1.1\", upstream: \"http://127.0.0.1:8080/big\", host: \"example.com\""),
            {{"Timestamp", "2026/11/18 13:55:38"}, {"Level", "warn"}, {"PID", "31"}, {"TID", "31"}, {"Connection", "1052"},
             {"Message", "an upstream response is buffered to a temporary file /var/cache/nginx/proxy_temp/1/00/0000000001 while reading upstream, client: 172.17.0.1, server: _, request: \"GET /big HTTP/1.1\", upstream: \"http://127.0.0.1:8080/big\", host: \"example.com\""}}),
        rec(QStringLiteral("2026/11/18 13:55:39 [crit] 31#31: *1060 connect() to unix:/run/php/php8.2-fpm.sock failed (2: No such file or directory) while connecting to upstream"),
            {{"Timestamp", "2026/11/18 13:55:39"}, {"Level", "crit"}, {"PID", "31"}, {"TID", "31"}, {"Connection", "1060"},
             {"Message", "connect() to unix:/run/php/php8.2-fpm.sock failed (2: No such file or directory) while connecting to upstream"}}),
        rec(QStringLiteral("2026/11/18 13:55:40 [alert] 30#30: 4 worker_connections are not enough"),
            {{"Timestamp", "2026/11/18 13:55:40"}, {"Level", "alert"}, {"PID", "30"}, {"TID", "30"},
             {"Message", "4 worker_connections are not enough"}}),
        rec(QStringLiteral("2026/11/18 13:55:41 [emerg] 1#1: bind() to 0.0.0.0:80 failed (98: Address already in use)"),
            {{"Timestamp", "2026/11/18 13:55:41"}, {"Level", "emerg"}, {"PID", "1"}, {"TID", "1"},
             {"Message", "bind() to 0.0.0.0:80 failed (98: Address already in use)"}}),
    }});

    c.append({QStringLiteral("syslog-rfc3164.json"), QStringLiteral("Syslog (RFC 3164)"), {
        rec(QStringLiteral("Nov 18 13:55:36 web01 sshd[2231]: Accepted publickey for deploy from 203.0.113.9 port 52222 ssh2: ED25519 SHA256:abc"),
            {{"Timestamp", "Nov 18 13:55:36"}, {"Host", "web01"}, {"Program", "sshd"}, {"PID", "2231"},
             {"Message", "Accepted publickey for deploy from 203.0.113.9 port 52222 ssh2: ED25519 SHA256:abc"}}),
        rec(QStringLiteral("Nov  8 03:04:05 web01 kernel: [12345.678901] usb 1-1: new high-speed USB device number 2 using xhci_hcd"),
            {{"Timestamp", "Nov  8 03:04:05"}, {"Host", "web01"}, {"Program", "kernel"},
             {"Message", "[12345.678901] usb 1-1: new high-speed USB device number 2 using xhci_hcd"}}),
        rec(QStringLiteral("Nov 18 13:55:37 web01 systemd[1]: Started Session 42 of User deploy."),
            {{"Timestamp", "Nov 18 13:55:37"}, {"Host", "web01"}, {"Program", "systemd"}, {"PID", "1"},
             {"Message", "Started Session 42 of User deploy."}}),
        rec(QStringLiteral("Nov 18 13:55:38 mail postfix/smtpd[10112]: connect from unknown[198.51.100.23]"),
            {{"Timestamp", "Nov 18 13:55:38"}, {"Host", "mail"}, {"Program", "postfix/smtpd"}, {"PID", "10112"},
             {"Message", "connect from unknown[198.51.100.23]"}}),
        rec(QStringLiteral("Nov 18 14:17:01 web01 CRON[3301]: (root) CMD (   cd / && run-parts --report /etc/cron.hourly)"),
            {{"Timestamp", "Nov 18 14:17:01"}, {"Host", "web01"}, {"Program", "CRON"}, {"PID", "3301"},
             {"Message", "(root) CMD (   cd / && run-parts --report /etc/cron.hourly)"}}),
        rec(QStringLiteral("Nov 18 14:20:15 web01 sudo:   deploy : TTY=pts/0 ; PWD=/home/deploy ; USER=root ; COMMAND=/usr/bin/systemctl restart nginx"),
            {{"Timestamp", "Nov 18 14:20:15"}, {"Host", "web01"}, {"Program", "sudo"},
             {"Message", "deploy : TTY=pts/0 ; PWD=/home/deploy ; USER=root ; COMMAND=/usr/bin/systemctl restart nginx"}}),
        rec(QStringLiteral("Nov 18 14:21:00 web01 dhclient[812]: DHCPACK of 10.0.0.15 from 10.0.0.1 (xid=0x5e3a)"),
            {{"Timestamp", "Nov 18 14:21:00"}, {"Host", "web01"}, {"Program", "dhclient"}, {"PID", "812"},
             {"Message", "DHCPACK of 10.0.0.15 from 10.0.0.1 (xid=0x5e3a)"}}),
    }});

    c.append({QStringLiteral("syslog-iso.json"), QStringLiteral("Syslog / journalctl (ISO timestamp)"), {
        // rsyslog RFC3339 (Debian 12 / Ubuntu 24.04 по умолчанию).
        rec(QStringLiteral("2026-11-18T13:55:36.123456+01:00 web01 sshd[2231]: Accepted publickey for deploy from 203.0.113.9 port 52222 ssh2"),
            {{"Timestamp", "2026-11-18T13:55:36.123456+01:00"}, {"Host", "web01"}, {"Program", "sshd"}, {"PID", "2231"},
             {"Message", "Accepted publickey for deploy from 203.0.113.9 port 52222 ssh2"}}),
        rec(QStringLiteral("2026-11-18T13:55:37.000012+01:00 web01 kernel: audit: type=1400 audit(1763470537.000:42): apparmor=\"DENIED\" operation=\"open\""),
            {{"Timestamp", "2026-11-18T13:55:37.000012+01:00"}, {"Host", "web01"}, {"Program", "kernel"},
             {"Message", "audit: type=1400 audit(1763470537.000:42): apparmor=\"DENIED\" operation=\"open\""}}),
        // journalctl -o short-iso.
        rec(QStringLiteral("2026-11-18T13:55:38+0100 web01 systemd[1]: Starting nginx.service - A high performance web server and a reverse proxy server..."),
            {{"Timestamp", "2026-11-18T13:55:38+0100"}, {"Host", "web01"}, {"Program", "systemd"}, {"PID", "1"},
             {"Message", "Starting nginx.service - A high performance web server and a reverse proxy server..."}}),
        rec(QStringLiteral("2026-11-18T13:55:39+0100 web01 nginx[4410]: nginx: [emerg] bind() to 0.0.0.0:80 failed (98: Address already in use)"),
            {{"Timestamp", "2026-11-18T13:55:39+0100"}, {"Host", "web01"}, {"Program", "nginx"}, {"PID", "4410"},
             {"Message", "nginx: [emerg] bind() to 0.0.0.0:80 failed (98: Address already in use)"}}),
        // journalctl -o short-iso-precise.
        rec(QStringLiteral("2026-11-18T13:55:40.512301+01:00 web01 systemd-logind[655]: New session 7 of user deploy."),
            {{"Timestamp", "2026-11-18T13:55:40.512301+01:00"}, {"Host", "web01"}, {"Program", "systemd-logind"}, {"PID", "655"},
             {"Message", "New session 7 of user deploy."}}),
    }});

    c.append({QStringLiteral("serilog-console.json"), QStringLiteral("Serilog (console)"), {
        rec(QStringLiteral("[13:55:36 INF] Starting web host"),
            {{"Timestamp", "13:55:36"}, {"Level", "INF"}, {"Message", "Starting web host"}}),
        rec(QStringLiteral("[13:55:36 DBG] Hosting starting"),
            {{"Timestamp", "13:55:36"}, {"Level", "DBG"}, {"Message", "Hosting starting"}}),
        rec(QStringLiteral("[13:55:37 WRN] Overriding address(es) 'http://localhost:5000'. Binding to endpoints defined via IConfiguration and/or UseKestrel() instead."),
            {{"Timestamp", "13:55:37"}, {"Level", "WRN"},
             {"Message", "Overriding address(es) 'http://localhost:5000'. Binding to endpoints defined via IConfiguration and/or UseKestrel() instead."}}),
        rec(QStringLiteral("[13:55:38 ERR] Unhandled exception processing order {\"OrderId\": 42, \"Total\": 19.99}"),
            {{"Timestamp", "13:55:38"}, {"Level", "ERR"}, {"Message", "Unhandled exception processing order {\"OrderId\": 42, \"Total\": 19.99}"}}),
        cont(QStringLiteral("System.InvalidOperationException: Sequence contains no elements")),
        cont(QStringLiteral("   at System.Linq.ThrowHelper.ThrowNoElementsException()")),
        cont(QStringLiteral("   at Shop.Orders.OrderService.Process(Int32 id) in /src/Shop/Orders/OrderService.cs:line 57")),
        cont(QStringLiteral("--- End of stack trace from previous location ---")),
        rec(QStringLiteral("[13:55:39 FTL] Host terminated unexpectedly"),
            {{"Timestamp", "13:55:39"}, {"Level", "FTL"}, {"Message", "Host terminated unexpectedly"}}),
        rec(QStringLiteral("[13:55:39 VRB] Disposing service provider"),
            {{"Timestamp", "13:55:39"}, {"Level", "VRB"}, {"Message", "Disposing service provider"}}),
    }});

    c.append({QStringLiteral("serilog-file.json"), QStringLiteral("Serilog (file, with date)"), {
        rec(QStringLiteral("2026-11-18 13:55:36.123 +01:00 [INF] Starting web host"),
            {{"Timestamp", "2026-11-18 13:55:36.123 +01:00"}, {"Level", "INF"}, {"Message", "Starting web host"}}),
        rec(QStringLiteral("2026-11-18 13:55:37.004 +01:00 [WRN] Slow request GET /api/orders took 1532 ms"),
            {{"Timestamp", "2026-11-18 13:55:37.004 +01:00"}, {"Level", "WRN"}, {"Message", "Slow request GET /api/orders took 1532 ms"}}),
        rec(QStringLiteral("2026-11-18 13:55:38.250 -05:00 [ERR] Payment provider returned 503"),
            {{"Timestamp", "2026-11-18 13:55:38.250 -05:00"}, {"Level", "ERR"}, {"Message", "Payment provider returned 503"}}),
        cont(QStringLiteral("System.Net.Http.HttpRequestException: Response status code does not indicate success: 503 (Service Unavailable).")),
        cont(QStringLiteral("   at System.Net.Http.HttpResponseMessage.EnsureSuccessStatusCode()")),
        cont(QStringLiteral("   at Shop.Payments.Client.ChargeAsync(Decimal amount) in C:\\src\\Shop\\Payments\\Client.cs:line 31")),
        rec(QStringLiteral("2026-11-18 13:55:39.000 +00:00 [DBG] Cache hit for key user:42"),
            {{"Timestamp", "2026-11-18 13:55:39.000 +00:00"}, {"Level", "DBG"}, {"Message", "Cache hit for key user:42"}}),
        rec(QStringLiteral("2026-11-18 13:55:40.777 +01:00 [FTL] Application start-up failed"),
            {{"Timestamp", "2026-11-18 13:55:40.777 +01:00"}, {"Level", "FTL"}, {"Message", "Application start-up failed"}}),
    }});

    c.append({QStringLiteral("python-logging.json"), QStringLiteral("Python logging"), {
        rec(QStringLiteral("2026-11-18 13:55:36,123 - myapp.db - INFO - Connected to postgres://db:5432/app"),
            {{"Timestamp", "2026-11-18 13:55:36,123"}, {"Logger", "myapp.db"}, {"Level", "INFO"},
             {"Message", "Connected to postgres://db:5432/app"}}),
        rec(QStringLiteral("2026-11-18 13:55:37,501 - __main__ - WARNING - Config key 'timeout' is deprecated - use 'read_timeout'"),
            {{"Timestamp", "2026-11-18 13:55:37,501"}, {"Logger", "__main__"}, {"Level", "WARNING"},
             {"Message", "Config key 'timeout' is deprecated - use 'read_timeout'"}}),
        rec(QStringLiteral("2026-11-18 13:55:38,002 - my-service.worker - ERROR - Task failed"),
            {{"Timestamp", "2026-11-18 13:55:38,002"}, {"Logger", "my-service.worker"}, {"Level", "ERROR"},
             {"Message", "Task failed"}}),
        cont(QStringLiteral("Traceback (most recent call last):")),
        cont(QStringLiteral("  File \"/app/worker.py\", line 42, in run")),
        cont(QStringLiteral("    result = 1 / count")),
        cont(QStringLiteral("ZeroDivisionError: division by zero")),
        rec(QStringLiteral("2026-11-18 13:55:39,900 - urllib3.connectionpool - DEBUG - Starting new HTTPS connection (1): api.example.com:443"),
            {{"Timestamp", "2026-11-18 13:55:39,900"}, {"Logger", "urllib3.connectionpool"}, {"Level", "DEBUG"},
             {"Message", "Starting new HTTPS connection (1): api.example.com:443"}}),
        rec(QStringLiteral("2026-11-18 13:55:40,000 - root - CRITICAL - Shutting down"),
            {{"Timestamp", "2026-11-18 13:55:40,000"}, {"Logger", "root"}, {"Level", "CRITICAL"},
             {"Message", "Shutting down"}}),
    }});

    c.append({QStringLiteral("python-basicconfig.json"), QStringLiteral("Python logging (basicConfig)"), {
        rec(QStringLiteral("WARNING:root:Watch out!"),
            {{"Level", "WARNING"}, {"Logger", "root"}, {"Message", "Watch out!"}}),
        rec(QStringLiteral("INFO:__main__:Started worker pid=4242"),
            {{"Level", "INFO"}, {"Logger", "__main__"}, {"Message", "Started worker pid=4242"}}),
        rec(QStringLiteral("DEBUG:urllib3.connectionpool:https://api.example.com:443 \"GET /v1/items HTTP/1.1\" 200 512"),
            {{"Level", "DEBUG"}, {"Logger", "urllib3.connectionpool"},
             {"Message", "https://api.example.com:443 \"GET /v1/items HTTP/1.1\" 200 512"}}),
        rec(QStringLiteral("ERROR:myapp.jobs:Job 17 failed: timeout"),
            {{"Level", "ERROR"}, {"Logger", "myapp.jobs"}, {"Message", "Job 17 failed: timeout"}}),
        cont(QStringLiteral("Traceback (most recent call last):")),
        cont(QStringLiteral("  File \"/app/jobs.py\", line 88, in run_job")),
        cont(QStringLiteral("    raise TimeoutError(\"timeout\")")),
        cont(QStringLiteral("TimeoutError: timeout")),
        rec(QStringLiteral("CRITICAL:root:Out of disk space"),
            {{"Level", "CRITICAL"}, {"Logger", "root"}, {"Message", "Out of disk space"}}),
    }});

    return c;
}

QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeAll(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}

QString schemaOf(const QString& file, QString* name = nullptr)
{
    QString canonical;
    BuiltinSchemas::parseSchemaFile(readAll(QDir(kSourceDir).filePath(file)),
                                    QString(), name, &canonical, true);
    return canonical;
}

QString describeFields(const LogPattern& pattern, const QString& line, const LogEntryFields& f)
{
    QStringList parts;
    const QStringList names = pattern.fieldNames();
    for (int i = 0; i < names.size(); ++i)
        parts << QStringLiteral("%1=«%2»").arg(names[i], f.get(i, line).toString());
    return parts.join(QStringLiteral(", "));
}

// Поля строки против ожидаемых; пустой результат — совпало.
QString compareFields(const LogPattern& pattern, const QString& line,
                      const LogEntryFields& f, const Fields& expected)
{
    const QStringList names = pattern.fieldNames();
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        if (!names.contains(it.key()))
            return QStringLiteral("в схеме нет поля %1").arg(it.key());
    }
    for (int i = 0; i < names.size(); ++i) {
        const QString got = f.get(i, line).toString();
        const QString want = expected.value(names[i]);
        if (got != want)
            return QStringLiteral("поле %1: «%2», ожидалось «%3»; все поля: %4")
                .arg(names[i], got, want, describeFields(pattern, line, f));
    }
    return QString();
}

// ---------------------------------------------------------------------------
// 1. Схемы на образцах
// ---------------------------------------------------------------------------

void testSchemaFiles()
{
    const QStringList files =
        QDir(kSourceDir).entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    QStringList covered;
    for (const SchemaCase& sc : cases())
        covered << sc.file;
    covered.sort();
    check(files == covered,
          QStringLiteral("каждая встроенная схема покрыта образцами: файлы %1, в тесте %2")
              .arg(files.join(QLatin1Char(' ')), covered.join(QLatin1Char(' '))));

    QStringList names;
    for (const QString& file : files) {
        const QJsonObject obj = QJsonDocument::fromJson(readAll(QDir(kSourceDir).filePath(file))).object();
        check(!obj.value(QStringLiteral("description")).toString().trimmed().isEmpty(),
              QStringLiteral("%1: есть описание").arg(file));
        QString name;
        const QString canonical = schemaOf(file, &name);
        check(!canonical.isEmpty(), QStringLiteral("%1: строгая проверка схемы").arg(file));
        check(!name.isEmpty() && !names.contains(name),
              QStringLiteral("%1: имя «%2» задано и уникально").arg(file, name));
        names << name;
    }
}

void testSamples(const SchemaCase& sc)
{
    QString name;
    const QString schema = schemaOf(sc.file, &name);
    check(name == sc.name, QStringLiteral("%1: имя «%2», ожидалось «%3»").arg(sc.file, name, sc.name));
    const LogPattern pattern(schema);
    check(pattern.isValid(), QStringLiteral("%1: схема компилируется").arg(sc.file));
    if (!pattern.isValid())
        return;

    int records = 0;
    for (const Sample& s : sc.samples) {
        const LogEntryFields f = pattern.extractFields(s.line);
        if (s.continuation) {
            check(f.isEmpty(), QStringLiteral("%1: продолжение не принято за запись: %2 → %3")
                                   .arg(sc.file, s.line, describeFields(pattern, s.line, f)));
            continue;
        }
        ++records;
        const QString diff = compareFields(pattern, s.line, f, s.fields);
        check(diff.isEmpty(), QStringLiteral("%1: %2\n       %3").arg(sc.file, s.line, diff));
    }
    check(records >= 5, QStringLiteral("%1: не меньше 5 образцов записей (%2)").arg(sc.file).arg(records));
}

// ---------------------------------------------------------------------------
// 2. Оба бэкенда: границы записей и поля
// ---------------------------------------------------------------------------

bool expectsTime(const SchemaCase& sc, const Sample& s)
{
    static const QStringList kWithoutTime = {
        QStringLiteral("serilog-console.json"),      // время без даты — F-11
        QStringLiteral("python-basicconfig.json"),   // в формате нет времени вовсе
    };
    return !kWithoutTime.contains(sc.file)
        && !s.fields.value(QStringLiteral("Timestamp")).startsWith(QStringLiteral("13:55:40.016"));
}

void testClassifierTime(const SchemaCase& sc)
{
    const LineClassifier classifier;
    for (const Sample& s : sc.samples) {
        if (s.continuation)
            continue;
        qint64 ms = 0;
        const bool has = classifier.detectTimestampMs(s.line, ms);
        const bool expectedTime = expectsTime(sc, s);
        check(has == expectedTime, QStringLiteral("%1: метка времени %2 распознаётся классификатором: %3")
                                       .arg(sc.file, expectedTime ? QString() : QStringLiteral("НЕ"), s.line));
    }
}

struct RowInfo {
    int     recordId = -1;
    qint64  ms = kNoTimestampMs; // момент записи (logentry.h)
    QString fieldsText;   // поля строки, как их отдаёт хранилище
};

// Строки документа по номеру строки файла (порядок вкладки у бэкендов может
// различаться: резидентный сортирует по времени).
QMap<int, RowInfo> rowsByLine(testdocs::Document& doc, const LogPattern& pattern)
{
    QMap<int, RowInfo> rows;
    const int n = doc.model->rowCount();
    for (int r = 0; r < n; ++r) {
        const std::shared_ptr<LogEntry> e = doc.model->entryAt(r);
        if (!e)
            continue;
        RowInfo info;
        info.recordId = e->logicalEntryId();
        info.ms = e->timestamp().isValid() ? e->timestamp().toMSecsSinceEpoch() : kNoTimestampMs;
        info.fieldsText = e->fields().isEmpty()
            ? QString()
            : describeFields(pattern, e->message(), e->fields());
        rows.insert(e->originalLineNumber(), info);
    }
    return rows;
}

void testBackends(const SchemaCase& sc, const QDir& tmp)
{
    const QString schema = schemaOf(sc.file);
    const LogPattern pattern(schema);

    QByteArray bytes;
    for (const Sample& s : sc.samples)
        bytes += s.line.toUtf8() + '\n';
    const QString path = testdocs::writeFile(tmp, sc.file + QStringLiteral(".log"), bytes);
    check(!path.isEmpty(), QStringLiteral("%1: файл образцов записан").arg(sc.file));

    testdocs::Document resident = testdocs::buildResident({path}, schema);
    testdocs::Document indexed = testdocs::buildIndexed({path}, schema);
    check(resident.errors.isEmpty() && indexed.errors.isEmpty(),
          QStringLiteral("%1: загрузка без ошибок: %2")
              .arg(sc.file, (resident.errors + indexed.errors).join(QStringLiteral("; "))));

    const QMap<int, RowInfo> res = rowsByLine(resident, pattern);
    const QMap<int, RowInfo> idx = rowsByLine(indexed, pattern);
    check(res.size() == sc.samples.size() && idx.size() == sc.samples.size(),
          QStringLiteral("%1: строк %2 / %3, ожидалось %4")
              .arg(sc.file).arg(res.size()).arg(idx.size()).arg(sc.samples.size()));
    if (res.size() != sc.samples.size() || idx.size() != sc.samples.size())
        return;

    const QList<int> lines = res.keys();
    check(lines == idx.keys(), QStringLiteral("%1: одни и те же номера строк").arg(sc.file));
    for (int i = 0; i < lines.size(); ++i) {
        const Sample& s = sc.samples[i];
        const RowInfo& r = res[lines[i]];
        const RowInfo& x = idx[lines[i]];
        const bool resStarts = i == 0 || r.recordId != res[lines[i - 1]].recordId;
        const bool idxStarts = i == 0 || x.recordId != idx[lines[i - 1]].recordId;
        check(resStarts == !s.continuation,
              QStringLiteral("%1: резидентный, начало записи на строке %2: %3")
                  .arg(sc.file).arg(i + 1).arg(s.line));
        check(idxStarts == !s.continuation,
              QStringLiteral("%1: индексный, начало записи на строке %2: %3")
                  .arg(sc.file).arg(i + 1).arg(s.line));
        check(r.ms == x.ms,
              QStringLiteral("%1: момент строки %2 у бэкендов: %3 | %4")
                  .arg(sc.file).arg(i + 1).arg(r.ms).arg(x.ms));
        if (!s.continuation)
            check(hasTimestampMs(r.ms) == expectsTime(sc, s),
                  QStringLiteral("%1: у записи строки %2 %3момент: %4")
                      .arg(sc.file).arg(i + 1).arg(expectsTime(sc, s) ? QString() : QStringLiteral("нет "), s.line));
        check(r.fieldsText == x.fieldsText,
              QStringLiteral("%1: поля строки %2 у бэкендов: %3 | %4")
                  .arg(sc.file).arg(i + 1).arg(r.fieldsText, x.fieldsText));
        if (!s.continuation) {
            const LogEntryFields f = pattern.extractFields(s.line);
            check(r.fieldsText == describeFields(pattern, s.line, f),
                  QStringLiteral("%1: поля строки %2 в хранилище как у схемы").arg(sc.file).arg(i + 1));
        }
    }
}

// F-12. Без включённого извлечения полей (по умолчанию) границы записей
// решает классификатор: у форматов, где у каждой записи есть метка с датой,
// они обязаны совпасть с границами по схеме, а момент — быть у каждой записи.
// Прежде access-лог и syslog без слов уровня становились одной записью.
// Пустая схема = схема не участвует, как при выключенном Filter blocks.
void testBackendsWithoutExtraction(const SchemaCase& sc, const QDir& tmp)
{
    for (const Sample& s : sc.samples)
        if (!s.continuation && !expectsTime(sc, s))
            return;   // запись без метки отличает только схема (F-11)

    QByteArray bytes;
    for (const Sample& s : sc.samples)
        bytes += s.line.toUtf8() + '\n';
    const QString path = testdocs::writeFile(tmp, sc.file + QStringLiteral(".noschema.log"), bytes);
    testdocs::Document resident = testdocs::buildResident({path}, QString());
    testdocs::Document indexed = testdocs::buildIndexed({path}, QString());
    check(resident.errors.isEmpty() && indexed.errors.isEmpty(),
          QStringLiteral("%1 без схемы: загрузка без ошибок").arg(sc.file));
    const LogPattern none;
    const QMap<int, RowInfo> res = rowsByLine(resident, none);
    const QMap<int, RowInfo> idx = rowsByLine(indexed, none);
    if (res.size() != sc.samples.size() || idx.size() != sc.samples.size()) {
        check(false, QStringLiteral("%1 без схемы: число строк").arg(sc.file));
        return;
    }
    const QList<int> lines = res.keys();
    for (int i = 0; i < lines.size(); ++i) {
        const Sample& s = sc.samples[i];
        const RowInfo& r = res[lines[i]];
        const RowInfo& x = idx[lines[i]];
        const bool resStarts = i == 0 || r.recordId != res[lines[i - 1]].recordId;
        const bool idxStarts = i == 0 || x.recordId != idx[lines[i - 1]].recordId;
        check(resStarts == !s.continuation && idxStarts == !s.continuation,
              QStringLiteral("%1 без схемы: начало записи на строке %2 (резидентный %3, индексный %4): %5")
                  .arg(sc.file).arg(i + 1).arg(resStarts).arg(idxStarts).arg(s.line));
        check(r.ms == x.ms && (s.continuation || hasTimestampMs(r.ms)),
              QStringLiteral("%1 без схемы: момент строки %2: %3 | %4")
                  .arg(sc.file).arg(i + 1).arg(r.ms).arg(x.ms));
    }
}

// Момент записи распознаёт LineClassifier (F-10): ISO, nginx error
// (dddd/dd/dd), access (dd/Mmm/dddd:…), syslog без года. Время без даты
// (Serilog console, Logback HH:mm:ss.SSS) — нет, это F-11; Python
// basicConfig времени не пишет вовсе. Перевернёт ожидание F-11.
// ---------------------------------------------------------------------------
// 3. Установка
// ---------------------------------------------------------------------------

QStringList jsonFiles(const QString& dir)
{
    return QDir(dir).entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
}

// Копия встроенных схем — «версия программы», которую тест меняет.
QString makeSource(const QDir& tmp, const QString& name)
{
    const QString dir = tmp.filePath(name);
    QDir().mkpath(dir);
    for (const QString& f : jsonFiles(kSourceDir))
        QFile::copy(QDir(kSourceDir).filePath(f), QDir(dir).filePath(f));
    return dir;
}

// Меняет в файле схемы значение по JSON-ключу верхнего уровня.
bool patchSchema(const QString& path, const QString& key, const QJsonValue& value)
{
    QJsonObject obj = QJsonDocument::fromJson(readAll(path)).object();
    obj.insert(key, value);
    return writeAll(path, QJsonDocument(obj).toJson());
}

// Добавляет в начало схемы файла линейный префикс — изменение самой схемы.
bool changeSchema(const QString& path, const QString& prefix)
{
    return patchSchema(path, QStringLiteral("linePrefix"), prefix);
}

void testInstall()
{
    QTemporaryDir tmpDir;
    const QDir tmp(tmpDir.path());
    const QStringList all = jsonFiles(kSourceDir);
    check(all.size() >= 8, QStringLiteral("встроенных схем не меньше 8 (%1)").arg(all.size()));

    // Пустой каталог (его ещё нет): ставится всё.
    const QString v1 = makeSource(tmp, QStringLiteral("v1"));
    const QString target = tmp.filePath(QStringLiteral("config/patterns"));
    BuiltinSchemas::InstallReport r = BuiltinSchemas::install(v1, target);
    check(r.errors.isEmpty(), QStringLiteral("установка без ошибок: %1").arg(r.errors.join(QStringLiteral("; "))));
    check(r.installed == all, QStringLiteral("в пустой каталог ставятся все: %1").arg(r.installed.join(QLatin1Char(' '))));
    check(jsonFiles(target) == all, QStringLiteral("в каталоге все встроенные схемы"));
    check(QFile::exists(QDir(target).filePath(QString::fromLatin1(BuiltinSchemas::kManifestFileName))),
          QStringLiteral("манифест записан"));
    for (const QString& f : all)
        check(readAll(QDir(target).filePath(f)) == readAll(QDir(v1).filePath(f)),
              QStringLiteral("%1 скопирован байт в байт").arg(f));

    // Повторный запуск — ничего не меняется.
    r = BuiltinSchemas::install(v1, target);
    check(!r.changed() && r.declined.isEmpty() && r.errors.isEmpty(),
          QStringLiteral("повторный запуск ничего не делает"));

    // Пользователь изменил одну схему, удалил другую, переформатировал третью.
    const QString edited = QStringLiteral("logback.json");
    const QString removed = QStringLiteral("serilog-console.json");
    const QString reformatted = QStringLiteral("nginx-error.json");
    const QString untouched = QStringLiteral("python-logging.json");
    check(changeSchema(QDir(target).filePath(edited), QStringLiteral("APP ")), QStringLiteral("правка схемы"));
    const QByteArray editedBytes = readAll(QDir(target).filePath(edited));
    check(QFile::remove(QDir(target).filePath(removed)), QStringLiteral("удаление схемы"));
    {
        const QString p = QDir(target).filePath(reformatted);
        const QJsonDocument doc = QJsonDocument::fromJson(readAll(p));
        check(writeAll(p, doc.toJson(QJsonDocument::Compact)), QStringLiteral("переформатирование схемы"));
    }

    // Тот же выпуск: изменённая остаётся, удалённая не воскресает.
    r = BuiltinSchemas::install(v1, target);
    check(!r.changed() && r.errors.isEmpty(), QStringLiteral("правки пользователя не трогаются тем же выпуском"));
    check(readAll(QDir(target).filePath(edited)) == editedBytes, QStringLiteral("изменённая схема цела"));
    check(!QFile::exists(QDir(target).filePath(removed)), QStringLiteral("удалённая схема не воскресла"));

    // Новый выпуск: исправлены все четыре схемы, плюс одна новая.
    const QString v2 = makeSource(tmp, QStringLiteral("v2"));
    for (const QString& f : {edited, removed, reformatted, untouched})
        check(changeSchema(QDir(v2).filePath(f), QStringLiteral("> ")), QStringLiteral("исправление %1 в v2").arg(f));
    const QString added = QStringLiteral("zz-new-format.json");
    QFile::copy(QDir(kSourceDir).filePath(untouched), QDir(v2).filePath(added));
    check(patchSchema(QDir(v2).filePath(added), QStringLiteral("name"), QStringLiteral("New format")),
          QStringLiteral("новая схема в v2"));

    const QString oldUntouched = schemaOf(untouched);
    r = BuiltinSchemas::install(v2, target);
    check(r.errors.isEmpty(), QStringLiteral("обновление без ошибок: %1").arg(r.errors.join(QStringLiteral("; "))));
    check(r.installed == QStringList{added}, QStringLiteral("новая схема выпуска ставится: %1").arg(r.installed.join(QLatin1Char(' '))));
    check(r.updated == QStringList({reformatted, untouched}),
          QStringLiteral("обновляются только нетронутые (и переформатированная): %1").arg(r.updated.join(QLatin1Char(' '))));
    check(readAll(QDir(target).filePath(untouched)) == readAll(QDir(v2).filePath(untouched)),
          QStringLiteral("нетронутая схема заменена исправленной"));
    check(readAll(QDir(target).filePath(edited)) == editedBytes, QStringLiteral("изменённая пользователем не перезаписана"));
    check(!QFile::exists(QDir(target).filePath(removed)), QStringLiteral("удалённая не воскресла в новом выпуске"));
    bool replacedOk = false;
    for (const auto& pr : r.replaced)
        replacedOk |= pr.first == oldUntouched && pr.second != oldUntouched && !pr.second.isEmpty();
    check(r.replaced.size() == 2 && replacedOk, QStringLiteral("замены сообщены парами старая → новая"));

    r = BuiltinSchemas::install(v2, target);
    check(!r.changed(), QStringLiteral("после обновления повторный запуск ничего не делает"));

    // Каталог, где у пользователя уже есть свои схемы: файл того же имени и
    // схема с тем же отображаемым именем в файле с другим именем.
    const QString target2 = tmp.filePath(QStringLiteral("config2/patterns"));
    QDir().mkpath(target2);
    const QByteArray ownBytes = "{\"name\": \"My logback\", \"blocks\": [{\"name\": \"Message\", \"kind\": \"remainder\"}]}";
    check(writeAll(QDir(target2).filePath(QStringLiteral("logback.json")), ownBytes), QStringLiteral("своя схема logback.json"));
    QString pyName;
    schemaOf(untouched, &pyName);
    const QByteArray sameNameBytes = QStringLiteral("{\"name\": \"%1\", \"blocks\": [{\"name\": \"Message\", \"kind\": \"remainder\"}]}")
                                         .arg(pyName).toUtf8();
    check(writeAll(QDir(target2).filePath(QStringLiteral("mine.json")), sameNameBytes), QStringLiteral("своя схема с тем же именем"));
    // Ровно встроенная (например, импортированная раньше вручную).
    QFile::copy(QDir(v1).filePath(reformatted), QDir(target2).filePath(reformatted));

    r = BuiltinSchemas::install(v1, target2);
    check(r.errors.isEmpty(), QStringLiteral("установка рядом со своими без ошибок"));
    check(r.declined == QStringList({edited, untouched}),
          QStringLiteral("свои файл и имя отклоняют встроенную: %1").arg(r.declined.join(QLatin1Char(' '))));
    check(!r.installed.contains(reformatted), QStringLiteral("совпадающий файл не переписывается"));
    check(r.installed.size() == all.size() - 3, QStringLiteral("остальные ставятся"));
    check(readAll(QDir(target2).filePath(QStringLiteral("logback.json"))) == ownBytes, QStringLiteral("свой logback.json цел"));
    check(!QFile::exists(QDir(target2).filePath(untouched)), QStringLiteral("дубль имени не создан"));

    // Следующий выпуск: отклонённые так и остаются, совпадавшая обновляется.
    r = BuiltinSchemas::install(v2, target2);
    check(readAll(QDir(target2).filePath(QStringLiteral("logback.json"))) == ownBytes,
          QStringLiteral("свой logback.json цел и после обновления"));
    check(!QFile::exists(QDir(target2).filePath(untouched)), QStringLiteral("отклонённая не появилась в новом выпуске"));
    check(r.updated.contains(reformatted), QStringLiteral("принятая совпадающая схема обновляется"));

    // Потерян манифест: изменённая пользователем не перезаписывается,
    // удалённая возвращается.
    QFile::remove(QDir(target).filePath(QString::fromLatin1(BuiltinSchemas::kManifestFileName)));
    r = BuiltinSchemas::install(v2, target);
    check(readAll(QDir(target).filePath(edited)) == editedBytes,
          QStringLiteral("без манифеста изменённая схема не перезаписана"));
    check(r.installed.contains(removed), QStringLiteral("без манифеста удалённая возвращается"));

    // Каталог назначения нельзя создать — ошибка, без исключений и падений.
    const QString blocker = tmp.filePath(QStringLiteral("file-not-dir"));
    writeAll(blocker, "x");
    r = BuiltinSchemas::install(v1, blocker + QStringLiteral("/patterns"));
    check(!r.errors.isEmpty() && !r.changed(), QStringLiteral("недоступный каталог — ошибка в отчёте"));
}

// ---------------------------------------------------------------------------
// 4. Ресурсы
// ---------------------------------------------------------------------------

// Автоопределение схемы (редактор) узнаёт метки новых видов как блок
// Timestamp: syslog в начале строки, access-лог в скобках, nginx error.
void testAutoDetectTimestamp()
{
    const struct { const char* line; int block; } cases[] = {
        { "Nov 18 13:55:36 web01 sshd[2231]: Accepted publickey for deploy", 0 },
        { "Nov  8 03:04:05 web01 kernel: usb 1-1: new device", 0 },
        { "127.0.0.1 - frank [10/Oct/2026:13:55:36 -0700] \"GET /a HTTP/1.0\" 200 2326", -1 },
        { "2026/11/18 13:55:36 [error] 1234#1234: *99 open() failed", 0 },
    };
    for (const auto& c : cases) {
        const QString line = QString::fromUtf8(c.line);
        const PatternDefinition def = PatternHeuristics::suggestSchema(line);
        int found = -1;
        for (int b = 0; b < def.blocks.size(); ++b) {
            if (def.blocks[b].matchKind == PatternBlock::MatchKind::Timestamp) {
                found = b;
                break;
            }
        }
        check(c.block >= 0 ? found == c.block : found > 0,
              QStringLiteral("автоопределение: блок Timestamp (%1): %2").arg(found).arg(line));
    }
    // Блок Timestamp берёт всю метку access-лога вместе с зоной.
    const QRegularExpression re(QRegularExpression::anchoredPattern(PatternHeuristics::timestampRegex()));
    for (const char* t : { "10/Oct/2026:13:55:36 -0700", "18/Nov/2026:13:55:36", "Nov 18 13:55:36",
                           "Nov  8 03:04:05", "2026/11/18 13:55:36", "13:55:36" })
        check(re.match(QString::fromLatin1(t)).hasMatch(),
              QStringLiteral("timestampRegex принимает «%1»").arg(QString::fromLatin1(t)));
    for (const char* t : { "Foo 18 13:55:36", "18/Foo/2026:13:55:36", "Nov 18 13:55",
                           "Nov  18 13:55:36" })
        check(!re.match(QString::fromLatin1(t)).hasMatch(),
              QStringLiteral("timestampRegex не принимает «%1»").arg(QString::fromLatin1(t)));
}

// Схемы, исправленные после Q-5 ради блока Timestamp (F-10): у
// пользователя, который поставил их прежние версии и не трогал, они
// заменяются новыми; изменённая пользователем прежняя версия остаётся.
void testUpgradeFromQ5()
{
    const QString q5Dir = QStringLiteral(DENDROLOG_Q5_PATTERNS_DIR);
    const QStringList fixed = jsonFiles(q5Dir);
    check(fixed == QStringList({QStringLiteral("access-combined.json"),
                                QStringLiteral("syslog-rfc3164.json")}),
          QStringLiteral("прежние версии исправленных схем: %1").arg(fixed.join(QLatin1Char(' '))));
    for (const QString& f : fixed)
        check(readAll(QDir(q5Dir).filePath(f)) != readAll(QDir(kSourceDir).filePath(f)),
              QStringLiteral("%1 в этом выпуске изменена").arg(f));

    QTemporaryDir tmpDir;
    const QDir tmp(tmpDir.path());
    const QString v0 = makeSource(tmp, QStringLiteral("q5"));
    for (const QString& f : fixed) {
        QFile::remove(QDir(v0).filePath(f));
        QFile::copy(QDir(q5Dir).filePath(f), QDir(v0).filePath(f));
    }
    for (const bool userEdited : {false, true}) {
        const QString target = tmp.filePath(userEdited ? QStringLiteral("edited/patterns")
                                                       : QStringLiteral("untouched/patterns"));
        BuiltinSchemas::InstallReport r = BuiltinSchemas::install(v0, target);
        check(r.errors.isEmpty() && r.installed.size() == jsonFiles(kSourceDir).size(),
              QStringLiteral("прежний выпуск установлен"));
        const QString edited = QDir(target).filePath(fixed.first());
        if (userEdited)
            check(changeSchema(edited, QStringLiteral("APP ")), QStringLiteral("правка прежней схемы"));
        const QByteArray editedBytes = readAll(edited);

        r = BuiltinSchemas::install(kSourceDir, target);
        check(r.errors.isEmpty(), QStringLiteral("обновление без ошибок: %1").arg(r.errors.join(QStringLiteral("; "))));
        check(r.installed.isEmpty(), QStringLiteral("новых схем нет"));
        const QStringList expectedUpdated = userEdited ? fixed.mid(1) : fixed;
        check(r.updated == expectedUpdated,
              QStringLiteral("обновлены %1, ожидалось %2")
                  .arg(r.updated.join(QLatin1Char(' ')), expectedUpdated.join(QLatin1Char(' '))));
        for (const QString& f : expectedUpdated) {
            check(readAll(QDir(target).filePath(f)) == readAll(QDir(kSourceDir).filePath(f)),
                  QStringLiteral("%1 заменена исправленной").arg(f));
            QString name;
            const QString canonical = schemaOf(f, &name);
            check(LogPattern(canonical).isValid()
                      && canonical.contains(QStringLiteral("timestamp")),
                  QStringLiteral("%1: поле времени — блок Timestamp").arg(f));
        }
        if (userEdited)
            check(readAll(edited) == editedBytes, QStringLiteral("изменённая пользователем прежняя схема цела"));
    }
}

void testQrc()
{
    const QString qrc = QString::fromUtf8(readAll(QDir(kSourceDir).filePath(QStringLiteral("../resources.qrc"))));
    check(!qrc.isEmpty(), QStringLiteral("resources.qrc читается"));
    for (const QString& f : jsonFiles(kSourceDir))
        check(qrc.contains(QStringLiteral("<file alias=\"%1\">patterns/%1</file>").arg(f)),
              QStringLiteral("%1 есть в resources.qrc под префиксом /patterns").arg(f));
    check(qrc.contains(QStringLiteral("<qresource prefix=\"/patterns\">")), QStringLiteral("префикс /patterns"));
}

// Что предлагает автоопределение на первой записи — для сравнения в отчёте
// (не проверка: встроенная схема должна быть не хуже, это видно глазами).
void printAutodetect(const SchemaCase& sc)
{
    if (qEnvironmentVariableIsEmpty("DENDRO_SHOW_AUTODETECT"))
        return;
    const LogPattern p(LogPattern::serializeDefinition(PatternHeuristics::suggestSchema(sc.samples.first().line)));
    std::fprintf(stderr, "%s: автоопределение: %s\n", qPrintable(sc.file),
                 qPrintable(describeFields(p, sc.samples.first().line, p.extractFields(sc.samples.first().line))));
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testSchemaFiles();
    QTemporaryDir tmpDir;
    for (const SchemaCase& sc : cases()) {
        printAutodetect(sc);
        testSamples(sc);
        testBackends(sc, QDir(tmpDir.path()));
        testBackendsWithoutExtraction(sc, QDir(tmpDir.path()));
        testClassifierTime(sc);
    }
    testInstall();
    testUpgradeFromQ5();
    testAutoDetectTimestamp();
    testQrc();

    std::fprintf(stderr, "builtin_schemas: %d проверок, %d провалов\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
