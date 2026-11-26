// Ядро нейро-подсказчика (F-17, шаг 5; docs/design/F-17.md, раздел 5) —
// без сети:
//
//   1. Таблица ответов модели: верный; не компилируется → уточнение с текстом
//      ошибки (и текст ошибки — в теле уточняющего запроса); во втором круге
//      — отказ; слишком длинный; больше 32 полей; тяжёлый /регекс/ (бюджет
//      SchemaFit); не JSON (тело, текст); refusal; max_tokens; без шаблона;
//      низкое покрытие → уточнение с не подошедшими строками (≤ 10);
//      шаблон «{Message}» — не поля.
//   2. Маскировка: таблица (метки и уровни целы; цифры, hex, UUID, почта, IP
//      не выживают; один IP — один адрес) и случайные строки (ни одна исходная
//      серия цифр, hex-слово, адрес почты и IP не выживает; вне меток и
//      документационных адресов цифр, кроме 0, нет).
//   3. Эталон текста запроса (tests/schema_proposal/request-v1.json):
//      тело байт в байт и заголовки; системный текст — его примеры
//      компилируются и разбирают свои строки целиком.
//   4. Выбор строк: кандидаты, без продолжений записи и повторов, не больше
//      заданного числа, обрезка.
//
// Эталон перезаписывается с DENDRO_UPDATE_GOLDEN=1 (осознанная правка
// системного текста или формата запроса).

#include "linetemplate.h"
#include "schemaproposal.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>

#include <cstdio>

#ifndef DENDROLOG_PROPOSAL_GOLDEN
#error "DENDROLOG_PROPOSAL_GOLDEN must point to tests/schema_proposal/request-v1.json"
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

using namespace SchemaProposal;

QVector<SchemaFit::Sample> logbackLines(int count)
{
    static const char* const kLevels[] = {"INFO", "WARN", "ERROR", "DEBUG"};
    static const char* const kThreads[] = {"main", "pool-1-thread-2", "http-nio-8080-exec-1"};
    static const char* const kLoggers[] = {"com.example.App", "com.example.db.Pool", "org.acme.Web"};
    QVector<SchemaFit::Sample> lines;
    QRandomGenerator rng(17);
    for (int i = 0; i < count; ++i) {
        const QString line = QStringLiteral("2026-11-18 13:%1:%2.%3 [%4] %5 %6 - Request %7 took %8 ms")
                                 .arg(10 + i % 50, 2, 10, QLatin1Char('0'))
                                 .arg(i % 60, 2, 10, QLatin1Char('0'))
                                 .arg(rng.bounded(1000), 3, 10, QLatin1Char('0'))
                                 .arg(QLatin1String(kThreads[rng.bounded(3)]))
                                 .arg(QLatin1String(kLevels[rng.bounded(4)]))
                                 .arg(QLatin1String(kLoggers[rng.bounded(3)]))
                                 .arg(rng.bounded(100000))
                                 .arg(rng.bounded(5000));
        lines.append({qint64(lines.size() + 1), line});
        if (i % 7 == 3) {
            lines.append({qint64(lines.size() + 1), QStringLiteral("\tat com.example.App.run(App.java:42)")});
            lines.append({qint64(lines.size() + 1), QStringLiteral("\tat java.base/java.lang.Thread.run(Thread.java:840)")});
        }
    }
    return lines;
}

QByteArray messagesReply(const QJsonObject& inner, const QString& stop = QStringLiteral("end_turn"))
{
    const QJsonObject root{
        {QStringLiteral("id"), QStringLiteral("msg_01")},
        {QStringLiteral("type"), QStringLiteral("message")},
        {QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("model"), QStringLiteral("claude-opus-5-5")},
        {QStringLiteral("stop_reason"), stop},
        {QStringLiteral("content"),
         QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("thinking")},
                                {QStringLiteral("thinking"), QString()}},
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                {QStringLiteral("text"),
                                 QString::fromUtf8(QJsonDocument(inner).toJson(QJsonDocument::Compact))}}}},
    };
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QByteArray templateReply(const QString& tpl, const QString& comment = QStringLiteral("ok"))
{
    return messagesReply(QJsonObject{{QStringLiteral("template"), tpl}, {QStringLiteral("comment"), comment}});
}

// ---------------------------------------------------------------------------
// 1. Таблица ответов.
// ---------------------------------------------------------------------------
void testReplies()
{
    const QVector<SchemaFit::Sample> lines = logbackLines(200);
    const QString good = QStringLiteral("{Timestamp:time} [{Thread}] {Level:level} {Logger:word} - {Message}");

    // Верный.
    {
        const Reply r = parseReply(templateReply(good, QStringLiteral("Logback")));
        check(r.ok() && r.templateText == good && r.comment == QLatin1String("Logback")
                  && r.model == QLatin1String("claude-opus-5-5"),
              QStringLiteral("верный ответ разобран"));
        const Check c = checkTemplate(r.templateText, r.comment, lines, false);
        check(c.status == Check::Status::Accepted && c.fit.coverage() >= kAcceptCoverage && !c.slow,
              QStringLiteral("верный шаблон принят (%1)").arg(c.fit.coverage()));
        // Перевод строки в конце — обёртка.
        const Check c2 = checkTemplate(good + QStringLiteral("\n"), QString(), lines, false);
        check(c2.status == Check::Status::Accepted && c2.templateText == good,
              QStringLiteral("перевод строки в конце шаблона отброшен"));
    }
    // В ограде ```json — тоже разбирается.
    {
        const QJsonObject root{
            {QStringLiteral("stop_reason"), QStringLiteral("end_turn")},
            {QStringLiteral("content"),
             QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                    {QStringLiteral("text"),
                                     QStringLiteral("```json\n{\"template\":\"{Message}\",\"comment\":\"\"}\n```")}}}}};
        const Reply r = parseReply(QJsonDocument(root).toJson());
        check(r.ok() && r.templateText == QLatin1String("{Message}"), QStringLiteral("JSON в ограде"));
    }
    // Не компилируется → уточнение с текстом ошибки; второй круг — отказ.
    {
        const QString bad = QStringLiteral("{Timestamp:time} {Component:text} {Message}");
        const Check c = checkTemplate(bad, QStringLiteral("first"), lines, false);
        const LineTemplate tpl(bad);
        check(!tpl.isValid(), QStringLiteral("шаблон-ошибка не компилируется"));
        check(c.status == Check::Status::NeedsRefinement && c.refinement.problem.contains(tpl.error().message)
                  && c.refinement.previousTemplate == bad && c.refinement.previousComment == QLatin1String("first"),
              QStringLiteral("не компилируется — уточнение с текстом ошибки: %1").arg(c.refinement.problem));
        const QByteArray body = requestBody({QStringLiteral("a")}, RequestOptions(), c.refinement);
        const QJsonArray messages = QJsonDocument::fromJson(body).object().value(QStringLiteral("messages")).toArray();
        check(messages.size() == 3
                  && messages[1].toObject().value(QStringLiteral("role")).toString() == QLatin1String("assistant")
                  && messages[1].toObject().value(QStringLiteral("content")).toString().contains(QStringLiteral("{Component:text}"))
                  && messages[2].toObject().value(QStringLiteral("content")).toString().contains(tpl.error().message),
              QStringLiteral("уточняющий запрос: прежний шаблон и текст ошибки"));
        check(messages[1].toObject().value(QStringLiteral("content")).toString() == previousAnswerText(c.refinement),
              QStringLiteral("прежний ответ в теле = previousAnswerText (его показывает окно согласия)"));
        const Check f = checkTemplate(bad, QString(), lines, true);
        check(f.status == Check::Status::Rejected && f.message.contains(tpl.error().message),
              QStringLiteral("второй круг: не компилируется — отказ"));
    }
    // Слишком длинный.
    {
        const QString longTpl = QStringLiteral("{Timestamp:time} ") + QString(kMaxTemplateChars, QLatin1Char('x'))
            + QStringLiteral(" {Message}");
        const Reply r = parseReply(templateReply(longTpl));
        const Check c = checkTemplate(r.templateText, r.comment, lines, false);
        check(r.ok() && c.status == Check::Status::Rejected && c.message.contains(QStringLiteral("too long")),
              QStringLiteral("слишком длинный — отказ"));
    }
    // Больше 32 полей.
    {
        QString many;
        for (int i = 0; i < kMaxFields + 1; ++i)
            many += QStringLiteral("{F%1:int} ").arg(i);
        many += QStringLiteral("{Message}");
        const Check c = checkTemplate(many, QString(), lines, false);
        check(c.status == Check::Status::Rejected && c.message.contains(QStringLiteral("fields")),
              QStringLiteral("больше 32 полей — отказ"));
    }
    // Тяжёлый /регекс/: каждая строка упирается в предел PCRE2 — бюджет.
    {
        QVector<SchemaFit::Sample> slow;
        for (int i = 0; i < 4000; ++i)
            slow.append({i + 1, QStringLiteral("2026-11-18 13:55:36 ") + QString(40, QLatin1Char('a')) + QStringLiteral("!")});
        const QString heavy = QStringLiteral("{Timestamp:time} {Bad:/(\\w|\\w\\w)+\\d/}{Message}");
        check(LineTemplate(heavy).isValid() && LineTemplate(heavy).hasRegex(),
              QStringLiteral("тяжёлый регекс компилируется"));
        SchemaFit::Budget budget;
        budget.maxMs = 50;
        const Check c = checkTemplate(heavy, QString(), slow, false, &budget);
        check(c.status == Check::Status::Rejected && c.fit.timedOut && c.message.contains(QStringLiteral("slow")),
              QStringLiteral("тяжёлый /регекс/ — отказ по бюджету"));
        // Лёгкий регекс принимается с пометкой «медленный».
        const Check light = checkTemplate(
            QStringLiteral("{Timestamp:time} [{Thread:/[\\w-]+/}] {Level:level} {Logger:word} - {Message}"),
            QString(), lines, false);
        check(light.status == Check::Status::Accepted && light.slow, QStringLiteral("лёгкий регекс — принят, slow"));
    }
    // Не JSON: тело и текст.
    {
        check(parseReply("<html>502</html>").status == Reply::Status::NotJson, QStringLiteral("тело не JSON"));
        const QJsonObject root{
            {QStringLiteral("stop_reason"), QStringLiteral("end_turn")},
            {QStringLiteral("content"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                               {QStringLiteral("text"), QStringLiteral("Sure! Here")}}}}};
        check(parseReply(QJsonDocument(root).toJson()).status == Reply::Status::NotJson,
              QStringLiteral("текст ответа не JSON"));
        check(parseReply(messagesReply(QJsonObject{{QStringLiteral("comment"), QStringLiteral("x")}})).status
                  == Reply::Status::NoTemplate,
              QStringLiteral("JSON без template"));
        check(parseReply(messagesReply(QJsonObject{{QStringLiteral("template"), 5}})).status == Reply::Status::NoTemplate,
              QStringLiteral("template не строка"));
    }
    // refusal и max_tokens — до чтения содержимого (оно может быть пустым).
    {
        QJsonObject root{{QStringLiteral("stop_reason"), QStringLiteral("refusal")},
                         {QStringLiteral("stop_details"),
                          QJsonObject{{QStringLiteral("type"), QStringLiteral("refusal")},
                                      {QStringLiteral("explanation"), QStringLiteral("policy")}}},
                         {QStringLiteral("content"), QJsonArray()}};
        const Reply r = parseReply(QJsonDocument(root).toJson());
        check(r.status == Reply::Status::Refused && r.message.contains(QStringLiteral("declined"))
                  && r.message.contains(QStringLiteral("policy")),
              QStringLiteral("refusal"));
        const Reply t = parseReply(templateReply(QStringLiteral("{Message}")).replace("end_turn", "max_tokens"));
        check(t.status == Reply::Status::Truncated && t.templateText.isEmpty(), QStringLiteral("max_tokens"));
    }
    // Низкое покрытие → уточнение с не подошедшими строками (≤ 10).
    {
        QVector<SchemaFit::Sample> mixed = logbackLines(60);
        for (int i = 0; i < 60; ++i)
            mixed.append({mixed.size() + 1, QStringLiteral("2026-11-18 14:00:%1 ERROR something odd happened %2")
                                                 .arg(i % 60, 2, 10, QLatin1Char('0'))
                                                 .arg(i)});
        const Check c = checkTemplate(good, QString(), mixed, false);
        check(c.status == Check::Status::NeedsRefinement && c.refinement.failedLines.size() == kRefineLines
                  && c.refinement.problem.contains(QStringLiteral("%")),
              QStringLiteral("низкое покрытие — уточнение (%1 строк)").arg(c.refinement.failedLines.size()));
        const Check f = checkTemplate(good, QString(), mixed, true);
        check(f.status == Check::Status::Accepted && f.fit.coverage() < kAcceptCoverage,
              QStringLiteral("второй круг: компилирующийся шаблон показан с тем покрытием, что есть"));
        const QString text = refinementText(c.refinement);
        check(text.contains(QStringLiteral("something odd happened")), QStringLiteral("строки уточнения в тексте"));
    }
    // «{Message}» принимает всё, но полей не даёт.
    {
        const Check c = checkTemplate(QStringLiteral("{Message}"), QString(), lines, false);
        check(c.status == Check::Status::NeedsRefinement, QStringLiteral("{Message} — уточнение"));
        const Check f = checkTemplate(QStringLiteral("{Message}"), QString(), lines, true);
        check(f.status == Check::Status::Rejected, QStringLiteral("{Message} во втором круге — отказ"));
    }
}

// ---------------------------------------------------------------------------
// 2. Маскировка.
// ---------------------------------------------------------------------------
void testMaskTable()
{
    struct Row {
        const char* line;
        const char* expected;
    };
    const Row rows[] = {
        {"2026-11-18 13:55:36.123 [main] INFO  App - user 4711 logged in",
         "2026-11-18 13:55:36.123 [main] INFO  App - user 0000 logged in"},
        {"Nov 18 13:55:36 host sshd[1234]: Accepted key for bob from 10.20.30.40 port 52211",
         "Nov 18 13:55:36 host sshd[0000]: Accepted key for bob from 192.0.2.1 port 00000"},
        {"ERROR mail to john.doe+x@corp.example.org failed (code 550)",
         "ERROR mail to user@example.com failed (code 000)"},
        {"WARN session 3f2a9c1d7e5b token deadbeefcafe0001 id 123e4567-e89b-12d3-a456-426614174000",
         "WARN session aaaaaaaaaaaa token aaaaaaaaaaaaaaaa id aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa"},
        {"10.0.0.1 -> 10.0.0.2 -> 10.0.0.1:8080", "192.0.2.1 -> 192.0.2.2 -> 192.0.2.1:0000"},
        {"[fe80::1ff:fe23:4567:890a] connected", "[2001:db8::1] connected"},
        {"short hex abc123 and 0x1F stay shaped", "short hex abc000 and 0x0F stay shaped"},
        {"user42 v2.3.4 build#77", "user00 v0.0.0 build#00"},
        // Число из одних цифр (эпоха, id) — цифрами, а не hex.
        {"id=1700000000123 took 5 ms pid 12345678", "id=0000000000000 took 0 ms pid 00000000"},
    };
    for (const Row& row : rows) {
        const QString got = maskLine(QString::fromUtf8(row.line));
        check(got == QString::fromUtf8(row.expected),
              QStringLiteral("маска «%1»: «%2», ожидалось «%3»")
                  .arg(QString::fromUtf8(row.line), got, QString::fromUtf8(row.expected)));
    }
    // Один IP — один адрес в наборе строк; разные — разные.
    const QStringList prepared = prepareLines({QStringLiteral("from 10.1.1.1"), QStringLiteral("from 10.1.1.2"),
                                               QStringLiteral("again 10.1.1.1")},
                                              MaskOptions());
    check(prepared == QStringList({QStringLiteral("from 192.0.2.1"), QStringLiteral("from 192.0.2.2"),
                                   QStringLiteral("again 192.0.2.1")}),
          QStringLiteral("один IP — один адрес на набор: %1").arg(prepared.join(QLatin1String(" | "))));
    // Общая таблица IP на запрос: образцы и строки уточнения согласны.
    {
        QHash<QString, QString> map;
        const QStringList a = prepareLines({QStringLiteral("a 10.9.9.9"), QStringLiteral("b 10.8.8.8")}, MaskOptions(), &map);
        const QStringList b = prepareLines({QStringLiteral("c 10.8.8.8")}, MaskOptions(), &map);
        check(a.value(1).mid(2) == b.value(0).mid(2) && a.value(0).mid(2) != b.value(0).mid(2),
              QStringLiteral("общая таблица IP: один адрес в образцах и уточнении"));
    }
    // Без маски — строки как есть (только обрезка).
    MaskOptions none;
    none.mask = false;
    none.cutChars = 10;
    check(prepareLines({QStringLiteral("10.1.1.1 something long")}, none) == QStringList{QStringLiteral("10.1.1.1 s")},
          QStringLiteral("без маски — обрезка до 10"));
    none.cut = false;
    check(prepareLines({QStringLiteral("10.1.1.1 something long")}, none)
              == QStringList{QStringLiteral("10.1.1.1 something long")},
          QStringLiteral("без маски и обрезки — как есть"));
}

void testMaskRandom()
{
    QRandomGenerator rng(4242);
    static const char* const kLevels[] = {"INFO", "WARN", "ERROR", "DEBUG", "TRACE", "FATAL"};
    static const char* const kWords[] = {"request", "user", "id", "took", "for", "from", "order", "x", "-", ":", "|"};
    const auto hexWord = [&](int len) {
        QString s;
        for (int i = 0; i < len; ++i)
            s += QLatin1Char("0123456789abcdef"[rng.bounded(16)]);
        if (!s.contains(QRegularExpression(QStringLiteral("[0-9]"))))
            s[0] = QLatin1Char('7');
        return s;
    };
    int lines = 0;
    for (int n = 0; n < 2000; ++n) {
        QStringList secrets;   // что не должно выжить
        QString line = QStringLiteral("2026-%1-%2T%3:%4:%5.%6Z %7")
                           .arg(1 + rng.bounded(12), 2, 10, QLatin1Char('0'))
                           .arg(1 + rng.bounded(28), 2, 10, QLatin1Char('0'))
                           .arg(rng.bounded(24), 2, 10, QLatin1Char('0'))
                           .arg(rng.bounded(60), 2, 10, QLatin1Char('0'))
                           .arg(rng.bounded(60), 2, 10, QLatin1Char('0'))
                           .arg(rng.bounded(1000), 3, 10, QLatin1Char('0'))
                           .arg(QLatin1String(kLevels[rng.bounded(6)]));
        const QString timestamp = line.section(QLatin1Char(' '), 0, 0);
        const QString level = line.section(QLatin1Char(' '), 1, 1);
        const int tokens = 3 + rng.bounded(10);
        for (int t = 0; t < tokens; ++t) {
            QString tok;
            switch (rng.bounded(7)) {
            case 0: tok = QString::number(10000 + rng.bounded(900000)); secrets << tok; break;
            case 1: tok = hexWord(8 + rng.bounded(24)); secrets << tok; break;
            case 2:
                tok = QStringLiteral("%1.%2.%3.%4").arg(11 + rng.bounded(200)).arg(rng.bounded(256))
                          .arg(rng.bounded(256)).arg(1 + rng.bounded(250));
                secrets << tok;
                break;
            case 3:
                tok = QStringLiteral("u%1.name@host%2.example.net").arg(rng.bounded(1000)).arg(rng.bounded(100));
                secrets << tok;
                break;
            default: tok = QLatin1String(kWords[rng.bounded(11)]); break;
            }
            line += QLatin1Char(' ') + tok;
        }
        const QString masked = maskLine(line);
        ++lines;
        check(masked.startsWith(timestamp + QLatin1Char(' ') + level + QLatin1Char(' ')),
              QStringLiteral("метка и уровень целы: «%1» → «%2»").arg(line, masked));
        for (const QString& s : std::as_const(secrets))
            check(!masked.contains(s), QStringLiteral("«%1» выжило в «%2»").arg(s, masked));
        // Вне метки и документационных адресов цифр, кроме 0, нет.
        QString rest = masked.mid(timestamp.size());
        rest.remove(QRegularExpression(QStringLiteral("(192\\.0\\.2|198\\.51\\.100|203\\.0\\.113)\\.\\d+")));
        check(!rest.contains(QRegularExpression(QStringLiteral("[1-9]"))),
              QStringLiteral("цифры, кроме 0, вне метки: «%1»").arg(masked));
    }
    std::printf("   случайных строк: %d\n", lines);
}

// ---------------------------------------------------------------------------
// 3. Эталон текста запроса.
// ---------------------------------------------------------------------------
void testGoldenRequest()
{
    const QStringList raw = {
        QStringLiteral("2026-11-18 13:55:36.123 [main] INFO  com.example.App - Started in 2.3 s (pid 4711)"),
        QStringLiteral("2026-11-18 13:55:37.001 [pool-1-thread-2] WARN  com.example.db.Pool - Retry 2 of 5 for bob@corp.example"),
        QStringLiteral("2026-11-18 13:55:38.500 [http-1] ERROR com.example.Web - 10.0.0.7 GET /api/v1 failed: deadbeef00112233"),
    };
    const QStringList lines = prepareLines(raw, MaskOptions());
    RequestOptions options;
    const QByteArray body = requestBody(lines, options);
    const QByteArray goldenPath = QByteArray(DENDROLOG_PROPOSAL_GOLDEN);
    if (qEnvironmentVariableIsSet("DENDRO_UPDATE_GOLDEN")) {
        QFile out(QString::fromUtf8(goldenPath));
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            out.write(QJsonDocument::fromJson(body).toJson(QJsonDocument::Indented));
            std::printf("   эталон записан: %s\n", goldenPath.constData());
        }
    }
    QFile file(QString::fromUtf8(goldenPath));
    check(file.open(QIODevice::ReadOnly), QStringLiteral("эталон читается"));
    const QJsonDocument golden = QJsonDocument::fromJson(file.readAll());
    check(!golden.isNull() && golden.toJson(QJsonDocument::Compact) == body,
          QStringLiteral("тело запроса = эталону байт в байт:\n%1").arg(QString::fromUtf8(body)));

    const QJsonObject root = QJsonDocument::fromJson(body).object();
    const QString user = root.value(QStringLiteral("messages")).toArray().at(0).toObject()
                             .value(QStringLiteral("content")).toString();
    check(user == userText(lines), QStringLiteral("текст пользователя — userText"));
    check(!user.contains(QStringLiteral("4711")) && !user.contains(QStringLiteral("bob@"))
              && !user.contains(QStringLiteral("10.0.0.7")) && !user.contains(QStringLiteral("deadbeef")),
          QStringLiteral("в запросе нет исходных значений"));
    check(root.value(QStringLiteral("model")).toString() == QLatin1String("claude-opus-5-5")
              && root.value(QStringLiteral("fallbacks")).toString() == QLatin1String("default")
              && root.value(QStringLiteral("output_config")).toObject().value(QStringLiteral("effort")).toString()
                  == QLatin1String("medium")
              && !root.contains(QStringLiteral("thinking")) && !root.contains(QStringLiteral("temperature")),
          QStringLiteral("модель, fallbacks, effort; без thinking и temperature"));
    check(root.value(QStringLiteral("system")).toString() == systemPrompt() && !systemPrompt().isEmpty(),
          QStringLiteral("системный текст — ресурс"));

    const auto headers = requestHeaders(options);
    const auto header = [&](const QByteArray& name) {
        for (const auto& h : headers)
            if (h.first == name)
                return h.second;
        return QByteArray();
    };
    check(header("anthropic-version") == "2026-10-21" && header("anthropic-beta") == "server-side-fallback-2026-07-01"
              && header("content-type") == "application/json" && header("x-api-key").isEmpty(),
          QStringLiteral("заголовки запроса (ключ ставит клиент)"));
    // Модель без серверных запасных — без fallbacks и без беты.
    RequestOptions haiku;
    haiku.model = QStringLiteral("claude-haiku-5-5");
    check(!QJsonDocument::fromJson(requestBody(lines, haiku)).object().contains(QStringLiteral("fallbacks")),
          QStringLiteral("без fallbacks у модели без запасных"));
    bool beta = false;
    for (const auto& h : requestHeaders(haiku))
        beta |= h.first == "anthropic-beta";
    check(!beta, QStringLiteral("без беты у модели без запасных"));

    // Примеры системного текста: шаблоны компилируются и разбирают свои строки.
    const QStringList prompt = systemPrompt().split(QLatin1Char('\n'));
    int examples = 0;
    for (int i = 0; i < prompt.size(); ++i) {
        if (prompt[i] != QLatin1String("Lines:"))
            continue;
        QStringList exampleLines;
        int j = i + 1;
        for (; j < prompt.size() && prompt[j] != QLatin1String("Template:"); ++j)
            exampleLines << prompt[j];
        if (j + 1 >= prompt.size())
            break;
        const LineTemplate tpl(prompt[j + 1]);
        ++examples;
        check(tpl.isValid(), QStringLiteral("пример %1 компилируется: %2").arg(examples).arg(tpl.error().message));
        for (const QString& line : std::as_const(exampleLines)) {
            LogEntryFields fields;
            check(tpl.match(line, &fields) == FieldExtractor::Outcome::Full,
                  QStringLiteral("пример %1 разбирает «%2»").arg(examples).arg(line));
        }
    }
    check(examples == 3, QStringLiteral("в системном тексте три примера (%1)").arg(examples));
}

// ---------------------------------------------------------------------------
// 4. Выбор строк.
// ---------------------------------------------------------------------------
void testPick()
{
    QVector<SchemaFit::Sample> samples = logbackLines(300);
    // Повторы.
    for (int i = 0; i < 50; ++i)
        samples.append({samples.size() + 1, QStringLiteral("2026-11-18 13:00:00.000 [main] INFO A - same")});
    const QStringList picked = pickLines(samples, MaskOptions(), 40);
    check(picked.size() == 40, QStringLiteral("40 строк (%1)").arg(picked.size()));
    check(int(QSet<QString>(picked.begin(), picked.end()).size()) == picked.size(), QStringLiteral("без повторов"));
    bool continuation = false;
    for (const QString& line : picked)
        continuation |= line.startsWith(QLatin1Char('\t'));
    check(!continuation, QStringLiteral("без строк-продолжений"));
    // Голова файла — первые 20 кандидатов по порядку.
    const QString first = maskLine(samples[0].text);
    check(picked.value(0) == first, QStringLiteral("первая строка — начало файла"));
    // Обрезка.
    QVector<SchemaFit::Sample> longLines;
    for (int i = 0; i < 5; ++i)
        longLines.append({i + 1, QStringLiteral("2026-11-18 13:00:0%1 INFO ").arg(i) + QString(1000, QLatin1Char('z'))});
    const QStringList cut = pickLines(longLines, MaskOptions(), 40);
    bool allCut = cut.size() == 5;
    for (const QString& line : cut)
        allCut &= line.size() == kCutChars;
    check(allCut, QStringLiteral("строки обрезаны до 300"));
    check(pickLines({}, MaskOptions()).isEmpty(), QStringLiteral("пусто — пусто"));
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::printf("1. ответы модели\n");
    testReplies();
    std::printf("2. маскировка\n");
    testMaskTable();
    testMaskRandom();
    std::printf("3. эталон запроса\n");
    testGoldenRequest();
    std::printf("4. выбор строк\n");
    testPick();
    std::printf("%d проверок, %d провалов\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
