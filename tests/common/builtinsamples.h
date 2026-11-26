// Образцы настоящих строк форматов встроенных схем (resources/patterns):
// поля каждой строки заданы руками, продолжения записей помечены. Общий
// корпус тестов builtin_schemas (поля, границы, моменты) и schema_fit
// (мера соответствия, F-17: каждая встроенная схема первая на своих
// образцах) — чтобы корпуса не разошлись.
#ifndef DENDROLOG_TESTS_BUILTINSAMPLES_H
#define DENDROLOG_TESTS_BUILTINSAMPLES_H

#include "logentry.h"

#include <QDateTime>
#include <QMap>
#include <QString>
#include <QVector>

namespace builtinsamples {

// Ожидаемые поля строки: имя -> значение. Поля схемы, которых здесь нет,
// обязаны быть пустыми.
using Fields = QMap<QString, QString>;

struct Sample {
    QString line;
    bool    continuation = false;   // строка-продолжение: схема её не принимает
    Fields  fields;
    qint64  ms = kNoTimestampMs;    // заданный момент записи (ключевые схемы); нет — не сверяется
};

inline Sample rec(const QString& line, const Fields& fields) { return {line, false, fields}; }
// Запись ключевой схемы с заданным моментом (F-4): время берётся из ключа.
inline Sample krec(const QString& line, const Fields& fields, qint64 ms) { return {line, false, fields, ms}; }
inline Sample cont(const QString& line) { return {line, true, {}}; }

inline qint64 utcMs(const char* iso)
{
    return QDateTime::fromString(QString::fromLatin1(iso), Qt::ISODateWithMs).toMSecsSinceEpoch();
}

inline qint64 localMs(int y, int mo, int d, int h, int mi, int s)
{
    return QDateTime(QDate(y, mo, d), QTime(h, mi, s)).toMSecsSinceEpoch();
}

struct SchemaCase {
    QString file;
    QString name;
    QVector<Sample> samples;
};

inline QVector<SchemaCase> cases()
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

// Форматы строк JSON Lines и logfmt (F-17, шаг 7; до него — встроенные
// ключевые схемы F-4, PR 4): поля и момент — из ключей ролей по умолчанию,
// значения полей — раскодированные (LogEntryFields::value). «file» — имя
// формата («json» / «logfmt»), а не файл схемы.
inline QVector<SchemaCase> formatCases()
{
    QVector<SchemaCase> c;
    c.append({QStringLiteral("json"), QStringLiteral("JSON Lines"), {
        krec(QStringLiteral(R"({"ts":"2026-11-18T13:55:36.123Z","level":"info","msg":"Server started","port":8080})"),
             {{"Timestamp", "2026-11-18T13:55:36.123Z"}, {"Level", "info"}, {"Message", "Server started"}},
             utcMs("2026-11-18T13:55:36.123Z")),
        // pino: эпоха в мс и числовой уровень.
        krec(QStringLiteral(R"({"level":30,"time":1763474136456,"pid":123,"hostname":"web01","msg":"request completed"})"),
             {{"Timestamp", "1763474136456"}, {"Level", "30"}, {"Message", "request completed"}},
             1763474136456LL),
        krec(QStringLiteral(R"({"level":50,"time":1763474137000,"msg":"Failed to open \"C:\\temp\\a.log\"\nretrying","err":{"type":"Error"}})"),
             {{"Timestamp", "1763474137000"}, {"Level", "50"},
              {"Message", QStringLiteral("Failed to open \"C:\\temp\\a.log\"\nretrying")}},
             1763474137000LL),
        cont(QStringLiteral("java.lang.IllegalStateException: boom")),
        cont(QStringLiteral("\tat com.example.Main.run(Main.java:42)")),
        // Serilog CLEF: дробь в 7 знаков и зона.
        krec(QStringLiteral(R"({"@t":"2026-11-18T13:55:38.0000000+01:00","@l":"Warning","@mt":"Disk {Drive} low","Drive":"C:"})"),
             {{"Timestamp", "2026-11-18T13:55:38.0000000+01:00"}, {"Level", "Warning"}, {"Message", "Disk {Drive} low"}},
             utcMs("2026-11-18T12:55:38Z")),
        // structlog: «event», ключи в любом порядке.
        krec(QStringLiteral(R"({"event":"user logged in","level":"debug","timestamp":"2026-11-18T13:55:39.5Z","user":"bob"})"),
             {{"Timestamp", "2026-11-18T13:55:39.5Z"}, {"Level", "debug"}, {"Message", "user logged in"}},
             utcMs("2026-11-18T13:55:39.500Z")),
        // ECS: ключ «log.level» дословно; затем путь во вложенный объект.
        krec(QStringLiteral(R"({"@timestamp":"2026-11-18T13:55:40Z","log.level":"error","message":"db timeout"})"),
             {{"Timestamp", "2026-11-18T13:55:40Z"}, {"Level", "error"}, {"Message", "db timeout"}},
             utcMs("2026-11-18T13:55:40Z")),
        cont(QStringLiteral(R"(  "pretty": "printed")")),
        krec(QStringLiteral(R"({"time":"2026-11-18T13:55:41Z","log":{"level":"warn","logger":"app"},"msg":"cache miss"})"),
             {{"Timestamp", "2026-11-18T13:55:41Z"}, {"Level", "warn"}, {"Message", "cache miss"}},
             utcMs("2026-11-18T13:55:41Z")),
        // json.dumps по умолчанию: кириллица \u-последовательностями, эпоха в секундах.
        krec(QStringLiteral(R"({"ts":1763474142,"level":"info","msg":"\u041f\u0440\u0438\u0432\u0435\u0442"})"),
             {{"Timestamp", "1763474142"}, {"Level", "info"}, {"Message", QStringLiteral("Привет")}},
             1763474142000LL),
    }});

    c.append({QStringLiteral("logfmt"), QStringLiteral("logfmt"), {
        krec(QStringLiteral(R"(ts=2026-11-18T13:55:36.123Z level=info msg="Server started" port=8080)"),
             {{"Timestamp", "2026-11-18T13:55:36.123Z"}, {"Level", "info"}, {"Message", "Server started"}},
             utcMs("2026-11-18T13:55:36.123Z")),
        // Время без зоны — локальное, как у строки без схемы.
        krec(QStringLiteral(R"(time="2026-11-18 13:55:37" level=warning msg="disk \"/var\" almost full" component=storage)"),
             {{"Timestamp", "2026-11-18 13:55:37"}, {"Level", "warning"}, {"Message", "disk \"/var\" almost full"}},
             localMs(2026, 11, 18, 13, 55, 37)),
        // logrus.
        krec(QStringLiteral(R"(time="2026-11-18T13:55:38+01:00" level=error msg="connection refused" err="dial tcp 10.0.0.1:5432: connect: connection refused")"),
             {{"Timestamp", "2026-11-18T13:55:38+01:00"}, {"Level", "error"}, {"Message", "connection refused"}},
             utcMs("2026-11-18T12:55:38Z")),
        cont(QStringLiteral("goroutine 1 [running]:")),
        cont(QStringLiteral("\tmain.main()")),
        cont(QStringLiteral("  /usr/local/go/src/runtime/panic.go:770 +0x132")),
        // Go slog.
        krec(QStringLiteral(R"(time=2026-11-18T13:55:39.000+00:00 level=DEBUG source=main.go:42 msg="cache miss" key=user:42)"),
             {{"Timestamp", "2026-11-18T13:55:39.000+00:00"}, {"Level", "DEBUG"}, {"Message", "cache miss"}},
             utcMs("2026-11-18T13:55:39Z")),
        // Loki / Prometheus: сообщение после других ключей.
        krec(QStringLiteral(R"(ts=2026-11-18T13:55:40.5Z caller=main.go:12 level=info msg="Starting Loki")"),
             {{"Timestamp", "2026-11-18T13:55:40.5Z"}, {"Level", "info"}, {"Message", "Starting Loki"}},
             utcMs("2026-11-18T13:55:40.500Z")),
        // Эпоха в секундах и «lvl».
        krec(QStringLiteral(R"(t=1763474141 lvl=warn msg="shutting down")"),
             {{"Timestamp", "1763474141"}, {"Level", "warn"}, {"Message", "shutting down"}},
             1763474141000LL),
    }});

    return c;
}

} // namespace builtinsamples

#endif // DENDROLOG_TESTS_BUILTINSAMPLES_H
