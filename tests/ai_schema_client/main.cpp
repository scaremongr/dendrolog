// Клиент нейро-подсказчика (F-17, шаг 5; docs/design/F-17.md, раздел 5) на
// локальном QTcpServer — наружу ни одного запроса:
//
//   1. Заголовки и тело: POST /v1/messages, x-api-key, anthropic-version,
//      anthropic-beta, content-type; тело байт в байт.
//   2. Ответы: успех; refusal и max_tokens (HTTP 200 — разбирает
//      SchemaProposal::parseReply); 401 (ключа нет в сообщении); 429 с
//      retry-after → повтор через столько секунд → успех; 429 до исчерпания
//      повторов; «x-should-retry: false»; 529 → успех; 500 до исчерпания;
//      обрыв соединения; тайм-аут; отмена посреди запроса и посреди паузы.
//   3. Без включения, без ключа, без согласия, с согласием на другое тело и
//      повторно с тем же согласием — ни одного соединения.
//   4. SecretStore: ключ сохраняется и читается; в каталоге настроек его нет
//      открытым текстом (ini с настройками AI — без ключа); на Unix файл
//      0600 в каталоге 0700, в portable-режиме и при подмене каталога
//      настроек ключ не сохраняется; на Windows файл — DPAPI, не открытый
//      текст.

#include "aischemaclient.h"
#include "appsettings.h"
#include "schemaproposal.h"
#include "secretstore.h"

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QNetworkProxy>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

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

bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 20000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

const QString kKey = QStringLiteral("sk-ant-test-0123456789abcdef");

// ---------------------------------------------------------------------------
// Локальный HTTP-сервер: по очереди отдаёт заданные ответы.
// ---------------------------------------------------------------------------
class FakeServer : public QObject {
public:
    struct Response {
        enum class Mode { Answer, Drop, Hang };
        Mode mode = Mode::Answer;
        int status = 200;
        QList<QPair<QByteArray, QByteArray>> headers;
        QByteArray body;
    };
    struct Request {
        QByteArray method;
        QByteArray path;
        QHash<QByteArray, QByteArray> headers;   // имена в нижнем регистре
        QByteArray body;
    };

    FakeServer()
    {
        m_server.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket* socket = m_server.nextPendingConnection()) {
                ++connections;
                m_buffers.insert(socket, QByteArray());
                QObject::connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { onRead(socket); });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1/v1/messages").arg(m_server.serverPort())); }

    QList<Response> script;
    Response fallback;            // когда script кончился
    QList<Request> requests;
    int connections = 0;

private:
    void onRead(QTcpSocket* socket)
    {
        QByteArray& buf = m_buffers[socket];
        buf += socket->readAll();
        const qsizetype headerEnd = buf.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;
        Request req;
        const QList<QByteArray> lines = buf.left(headerEnd).split('\n');
        const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
        req.method = first.value(0);
        req.path = first.value(1);
        for (int i = 1; i < lines.size(); ++i) {
            const qsizetype colon = lines[i].indexOf(':');
            if (colon > 0)
                req.headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
        }
        const int length = req.headers.value("content-length").toInt();
        if (buf.size() < headerEnd + 4 + length)
            return;
        req.body = buf.mid(headerEnd + 4, length);
        buf.remove(0, headerEnd + 4 + length);
        requests.append(req);

        const Response resp = script.isEmpty() ? fallback : script.takeFirst();
        if (resp.mode == Response::Mode::Drop) {
            socket->abort();
            return;
        }
        if (resp.mode == Response::Mode::Hang)
            return;
        QByteArray out = "HTTP/1.1 " + QByteArray::number(resp.status) + " X\r\n";
        out += "content-type: application/json\r\n";
        for (const auto& h : resp.headers)
            out += h.first + ": " + h.second + "\r\n";
        out += "content-length: " + QByteArray::number(resp.body.size()) + "\r\n";
        out += "connection: close\r\n\r\n";
        out += resp.body;
        socket->write(out);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    QHash<QTcpSocket*, QByteArray> m_buffers;
};

FakeServer::Response answer(int status, const QByteArray& body = QByteArray(),
                            QList<QPair<QByteArray, QByteArray>> headers = {})
{
    FakeServer::Response r;
    r.status = status;
    r.body = body;
    r.headers = std::move(headers);
    return r;
}

QByteArray okBody(const QString& stop = QStringLiteral("end_turn"))
{
    return QStringLiteral(
               "{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-opus-5-5\","
               "\"stop_reason\":\"%1\",\"content\":[{\"type\":\"text\",\"text\":"
               "\"{\\\"template\\\":\\\"{Timestamp:time} {Level:level} {Message}\\\",\\\"comment\\\":\\\"ok\\\"}\"}]}")
        .arg(stop)
        .toUtf8();
}

QByteArray errorBody(const char* type, const char* message)
{
    return QByteArray("{\"type\":\"error\",\"error\":{\"type\":\"") + type + "\",\"message\":\"" + message + "\"}}";
}

struct Run {
    std::unique_ptr<AiSchemaClient> client;
    QList<AiSchemaClient::Result> results;
    QList<int> retryDelays;
};

std::unique_ptr<Run> makeClient(const FakeServer& server, AiSchemaClient::Config config = {})
{
    auto run = std::make_unique<Run>();
    run->client = std::make_unique<AiSchemaClient>();
    run->client->setProvider(std::make_shared<ClaudeProvider>(QStringLiteral("claude-opus-5-5"), server.url()));
    if (config.apiKey.isEmpty())
        config.apiKey = kKey;
    config.enabled = true;
    if (config.retryBaseMs == 1000)
        config.retryBaseMs = 10;
    run->client->setConfig(config);
    Run* raw = run.get();
    QObject::connect(raw->client.get(), &AiSchemaClient::finished, raw->client.get(),
                     [raw](const AiSchemaClient::Result& r) { raw->results.append(r); });
    QObject::connect(raw->client.get(), &AiSchemaClient::retrying, raw->client.get(),
                     [raw](int, int delay) { raw->retryDelays.append(delay); });
    return run;
}

AiSchemaClient::Result sendAndWait(Run& run, const QByteArray& body)
{
    AiConsent consent = AiConsent::grant(body);
    run.client->send(body, consent);
    if (!waitFor([&]() { return !run.results.isEmpty(); }, 30000)) {
        check(false, QStringLiteral("итог не пришёл"));
        return {};
    }
    return run.results.takeFirst();
}

QByteArray sampleBody()
{
    return SchemaProposal::requestBody({QStringLiteral("2026-11-18 13:55:36 INFO hello")},
                                       SchemaProposal::RequestOptions());
}

// ---------------------------------------------------------------------------
void testHeadersAndSuccess()
{
    FakeServer server;
    server.script = {answer(200, okBody())};
    auto run = makeClient(server);
    const QByteArray body = sampleBody();
    const AiSchemaClient::Result r = sendAndWait(*run, body);
    check(r.ok() && r.httpStatus == 200 && r.attempts == 1, QStringLiteral("успех: %1").arg(r.message));
    check(server.requests.size() == 1, QStringLiteral("один запрос"));
    if (server.requests.isEmpty())
        return;
    const FakeServer::Request& req = server.requests.first();
    check(req.method == "POST" && req.path == "/v1/messages", QStringLiteral("POST /v1/messages"));
    check(req.headers.value("x-api-key") == kKey.toUtf8(), QStringLiteral("x-api-key"));
    check(req.headers.value("anthropic-version") == "2026-10-21", QStringLiteral("anthropic-version"));
    check(req.headers.value("anthropic-beta") == "server-side-fallback-2026-07-01", QStringLiteral("anthropic-beta"));
    check(req.headers.value("content-type") == "application/json", QStringLiteral("content-type"));
    check(!req.headers.contains("authorization"), QStringLiteral("без Authorization"));
    check(req.body == body, QStringLiteral("тело байт в байт"));
    const SchemaProposal::Reply reply = SchemaProposal::parseReply(r.body);
    check(reply.ok() && reply.templateText == QLatin1String("{Timestamp:time} {Level:level} {Message}"),
          QStringLiteral("ответ разобран"));
}

void testRefusalAndMaxTokens()
{
    FakeServer server;
    server.script = {answer(200, okBody(QStringLiteral("refusal"))), answer(200, okBody(QStringLiteral("max_tokens")))};
    auto run = makeClient(server);
    const AiSchemaClient::Result a = sendAndWait(*run, sampleBody());
    check(a.ok() && SchemaProposal::parseReply(a.body).status == SchemaProposal::Reply::Status::Refused,
          QStringLiteral("refusal — HTTP 200, разбор: отклонено"));
    const AiSchemaClient::Result b = sendAndWait(*run, sampleBody());
    check(b.ok() && SchemaProposal::parseReply(b.body).status == SchemaProposal::Reply::Status::Truncated,
          QStringLiteral("max_tokens — оборван"));
}

void testErrors()
{
    // 401: без повторов, ключа в сообщении нет (даже если сервер его повторил).
    {
        FakeServer server;
        server.script = {answer(401, errorBody("authentication_error", "invalid x-api-key sk-ant-test-0123456789abcdef"))};
        auto run = makeClient(server);
        const AiSchemaClient::Result r = sendAndWait(*run, sampleBody());
        check(r.status == AiSchemaClient::Result::Status::AuthFailed && r.attempts == 1 && server.requests.size() == 1,
              QStringLiteral("401 — ключ не принят, без повторов"));
        check(!r.message.contains(kKey) && r.message.contains(QStringLiteral("401")),
              QStringLiteral("401: ключа нет в сообщении: %1").arg(r.message));
    }
    // 429 с retry-after: 1 → пауза 1 с → успех.
    {
        FakeServer server;
        server.script = {answer(429, errorBody("rate_limit_error", "slow down"), {{"retry-after", "1"}}),
                         answer(200, okBody())};
        auto run = makeClient(server);
        QElapsedTimer timer;
        timer.start();
        const AiSchemaClient::Result r = sendAndWait(*run, sampleBody());
        check(r.ok() && r.attempts == 2 && run->retryDelays == QList<int>{1000} && timer.elapsed() >= 900,
              QStringLiteral("429 с retry-after → повтор через 1 с → успех (%1 мс)").arg(timer.elapsed()));
    }
    // Огромный retry-after — пауза не больше предела (без переполнения).
    {
        FakeServer server;
        server.script = {answer(429, errorBody("rate_limit_error", "slow down"), {{"retry-after", "3000000"}}),
                         answer(200, okBody())};
        AiSchemaClient::Config config;
        config.maxRetryDelayMs = 200;
        auto run = makeClient(server, config);
        const AiSchemaClient::Result r = sendAndWait(*run, sampleBody());
        check(r.ok() && run->retryDelays == QList<int>{200},
              QStringLiteral("retry-after 3000000 → пауза = предел 200 мс"));
    }
    // 429 до исчерпания повторов (retry-after: 0).
    {
        FakeServer server;
        for (int i = 0; i < 3; ++i)
            server.script.append(answer(429, errorBody("rate_limit_error", "slow down"), {{"retry-after", "0"}}));
        auto run = makeClient(server);
        const AiSchemaClient::Result r = sendAndWait(*run, sampleBody());
        check(r.status == AiSchemaClient::Result::Status::RateLimited && r.attempts == 3 && server.requests.size() == 3,
              QStringLiteral("429: два повтора, затем отказ (%1 попыток)").arg(r.attempts));
    }
    // x-should-retry: false запрещает повтор.
    {
        FakeServer server;
        server.script = {answer(529, errorBody("overloaded_error", "busy"), {{"x-should-retry", "false"}})};
        auto run = makeClient(server);
        const AiSchemaClient::Result r = sendAndWait(*run, sampleBody());
        check(r.status == AiSchemaClient::Result::Status::Overloaded && r.attempts == 1,
              QStringLiteral("x-should-retry: false — без повтора"));
    }
    // 529 → успех (пауза без retry-after — retryBaseMs).
    {
        FakeServer server;
        server.script = {answer(529, errorBody("overloaded_error", "busy")), answer(200, okBody())};
        auto run = makeClient(server);
        const AiSchemaClient::Result r = sendAndWait(*run, sampleBody());
        check(r.ok() && r.attempts == 2 && run->retryDelays == QList<int>{10}, QStringLiteral("529 → успех"));
    }
    // 500 до исчерпания.
    {
        FakeServer server;
        for (int i = 0; i < 3; ++i)
            server.script.append(answer(500, errorBody("api_error", "oops")));
        auto run = makeClient(server);
        const AiSchemaClient::Result r = sendAndWait(*run, sampleBody());
        check(r.status == AiSchemaClient::Result::Status::ServerError && r.attempts == 3
                  && r.message.contains(QStringLiteral("oops")),
              QStringLiteral("500: повторы, затем ошибка сервера"));
    }
    // 400 — текст сервера, без повторов.
    {
        FakeServer server;
        server.script = {answer(400, errorBody("invalid_request_error", "model: unknown"))};
        auto run = makeClient(server);
        const AiSchemaClient::Result r = sendAndWait(*run, sampleBody());
        check(r.status == AiSchemaClient::Result::Status::HttpError && r.attempts == 1
                  && r.message.contains(QStringLiteral("model: unknown")),
              QStringLiteral("400 — текст сервера: %1").arg(r.message));
    }
    // Обрыв.
    {
        FakeServer server;
        // Каждый запрос — обрыв: Qt (6.8.3) сам повторяет POST после обрыва
        // до первого байта ответа, этот повтор клиенту не виден.
        FakeServer::Response drop;
        drop.mode = FakeServer::Response::Mode::Drop;
        server.fallback = drop;
        auto run = makeClient(server);
        const AiSchemaClient::Result r = sendAndWait(*run, sampleBody());
        check(r.status == AiSchemaClient::Result::Status::NetworkError && r.attempts == 1 && !r.message.contains(kKey),
              QStringLiteral("обрыв соединения: статус %1, попыток %2, «%3»")
                  .arg(int(r.status)).arg(r.attempts).arg(r.message));
    }
    // Тайм-аут.
    {
        FakeServer server;
        FakeServer::Response hang;
        hang.mode = FakeServer::Response::Mode::Hang;
        server.script = {hang};
        AiSchemaClient::Config config;
        config.timeoutMs = 300;
        auto run = makeClient(server, config);
        const AiSchemaClient::Result r = sendAndWait(*run, sampleBody());
        check(r.status == AiSchemaClient::Result::Status::TimedOut && r.attempts == 1, QStringLiteral("тайм-аут"));
    }
    // Отмена посреди запроса.
    {
        FakeServer server;
        FakeServer::Response hang;
        hang.mode = FakeServer::Response::Mode::Hang;
        server.script = {hang};
        auto run = makeClient(server);
        const QByteArray body = sampleBody();
        AiConsent consent = AiConsent::grant(body);
        run->client->send(body, consent);
        check(waitFor([&]() { return !server.requests.isEmpty(); }), QStringLiteral("запрос дошёл до сервера"));
        run->client->cancel();
        check(waitFor([&]() { return !run->results.isEmpty(); }), QStringLiteral("итог после отмены"));
        check(!run->results.isEmpty() && run->results.first().status == AiSchemaClient::Result::Status::Cancelled
                  && !run->client->isRunning(),
              QStringLiteral("отмена посреди запроса"));
    }
    // Отмена посреди паузы перед повтором.
    {
        FakeServer server;
        server.script = {answer(529, errorBody("overloaded_error", "busy"), {{"retry-after", "30"}})};
        auto run = makeClient(server);
        const QByteArray body = sampleBody();
        AiConsent consent = AiConsent::grant(body);
        run->client->send(body, consent);
        check(waitFor([&]() { return !run->retryDelays.isEmpty(); }), QStringLiteral("пауза перед повтором"));
        run->client->cancel();
        check(waitFor([&]() { return !run->results.isEmpty(); }) && run->results.first().status
                  == AiSchemaClient::Result::Status::Cancelled && server.requests.size() == 1,
              QStringLiteral("отмена посреди паузы — повтора нет"));
    }
}

void testNoConnection()
{
    FakeServer server;
    server.script = {answer(200, okBody())};
    const QByteArray body = sampleBody();
    const auto expect = [&](Run& run, AiSchemaClient::Result::Status status, const QString& what,
                            AiConsent consent) {
        run.client->send(body, consent);
        check(waitFor([&]() { return !run.results.isEmpty(); }), what + QStringLiteral(": итог"));
        check(!run.results.isEmpty() && run.results.takeFirst().status == status, what + QStringLiteral(": статус"));
    };
    {
        auto run = makeClient(server);
        AiSchemaClient::Config config = run->client->config();
        config.enabled = false;
        run->client->setConfig(config);
        expect(*run, AiSchemaClient::Result::Status::Disabled, QStringLiteral("выключен"), AiConsent::grant(body));
    }
    {
        auto run = makeClient(server);
        AiSchemaClient::Config config = run->client->config();
        config.apiKey = QStringLiteral("  ");
        run->client->setConfig(config);
        expect(*run, AiSchemaClient::Result::Status::NoKey, QStringLiteral("без ключа"), AiConsent::grant(body));
    }
    {
        auto run = makeClient(server);
        expect(*run, AiSchemaClient::Result::Status::NoConsent, QStringLiteral("без согласия"), AiConsent());
        expect(*run, AiSchemaClient::Result::Status::NoConsent, QStringLiteral("согласие на другое тело"),
               AiConsent::grant(body + " "));
        // Согласие — на одну отправку: тот же объект второй раз не годится.
        AiConsent once = AiConsent::grant(body);
        AiConsent copy = once;
        run->client->send(body, once);
        check(!once.isValid(), QStringLiteral("согласие погашено отправкой"));
        check(waitFor([&]() { return !run->results.isEmpty(); }) && run->results.takeFirst().ok(),
              QStringLiteral("с согласием — отправлено"));
        check(server.connections == 1, QStringLiteral("одно соединение"));
        once = AiConsent();
        expect(*run, AiSchemaClient::Result::Status::NoConsent, QStringLiteral("погашенное согласие"), once);
        Q_UNUSED(copy);
    }
    check(server.connections == 1 && server.requests.size() == 1,
          QStringLiteral("кроме согласованной отправки — ни одного соединения (%1)").arg(server.connections));
}

// ---------------------------------------------------------------------------
QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool dirContains(const QString& dir, const QByteArray& needle)
{
    QDirIterator it(dir, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext())
        if (readAll(it.next()).contains(needle))
            return true;
    return false;
}

void testSecretStore()
{
    QTemporaryDir root;
    check(root.isValid(), QStringLiteral("временный каталог"));
    const QString configDir = root.filePath(QStringLiteral("config"));
    QDir().mkpath(configDir);
    AppSettings::overrideConfigDirForTesting(configDir);
    AppSettings& settings = AppSettings::instance();
    settings.load();
    check(!settings.aiEnabled() && settings.aiModel() == QLatin1String(SchemaProposal::kDefaultModel),
          QStringLiteral("подсказчик выключен по умолчанию, модель по умолчанию"));
    settings.setAiEnabled(true);
    settings.setAiModel(QStringLiteral("claude-sonnet-5-5"));
    settings.save();
    settings.setAiEnabled(false);
    settings.load();
    check(settings.aiEnabled() && settings.aiModel() == QLatin1String("claude-sonnet-5-5"),
          QStringLiteral("настройки AI переживают перезапуск"));

#ifdef Q_OS_WIN
    // Windows: хранилище приложения — DPAPI в каталоге настроек.
    SecretStore store = SecretStore::forApplication();
    check(store.kind() == SecretStore::Kind::SystemProtected, QStringLiteral("Windows — DPAPI"));
#else
    // Unix: при подмене каталога настроек (и в portable-режиме) ключ не
    // сохраняется; сохраняемое хранилище — вне каталога настроек.
    check(!SecretStore::forApplication().isAvailable(),
          QStringLiteral("portable / подменённый каталог — ключ не сохраняется"));
    SecretStore store(root.filePath(QStringLiteral("data/private")), SecretStore::platformKind());
#endif
    check(!store.hasKey(), QStringLiteral("ключа ещё нет"));
    QString error;
    check(store.writeKey(kKey, &error), QStringLiteral("ключ сохранён: %1").arg(error));
    QString back;
    check(store.readKey(&back) && back == kKey, QStringLiteral("ключ прочитан"));
    check(!readAll(store.path()).contains(kKey.toUtf8()) || store.kind() == SecretStore::Kind::PrivateFile,
          QStringLiteral("DPAPI — не открытый текст"));
    check(!dirContains(configDir, kKey.toUtf8()), QStringLiteral("в каталоге настроек ключа открытым текстом нет"));
    check(!readAll(AppSettings::iniFilePath()).contains("sk-ant"), QStringLiteral("в ini ключа нет"));
#ifndef Q_OS_WIN
    const QFileInfo info(store.path());
    check(info.permissions() == (QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser
                                 | QFileDevice::WriteUser),
          QStringLiteral("файл ключа 0600"));
    const QFileInfo dirInfo(QFileInfo(store.path()).absolutePath());
    check(!(dirInfo.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::ExeOther)),
          QStringLiteral("каталог ключа 0700"));
    check(!store.path().startsWith(configDir), QStringLiteral("файл ключа вне каталога настроек"));
#endif
    // Действующий ключ: переменная окружения главнее.
    qputenv("ANTHROPIC_API_KEY", "sk-env");
    SecretStore::Source source = SecretStore::Source::None;
    check(SecretStore::effectiveKey(store, &source) == QLatin1String("sk-env")
              && source == SecretStore::Source::Environment,
          QStringLiteral("ANTHROPIC_API_KEY главнее"));
    qunsetenv("ANTHROPIC_API_KEY");
    check(SecretStore::effectiveKey(store, &source) == kKey && source == SecretStore::Source::Stored,
          QStringLiteral("затем сохранённый"));
    check(store.removeKey(&error) && !store.hasKey() && !QFileInfo::exists(store.path()),
          QStringLiteral("ключ удалён"));
    SecretStore::setSessionKey(QStringLiteral("sk-session"));
    check(SecretStore::effectiveKey(store, &source) == QLatin1String("sk-session")
              && source == SecretStore::Source::Session,
          QStringLiteral("затем ключ на этот запуск"));
    SecretStore::setSessionKey(QString());
    check(SecretStore::effectiveKey(store, &source).isEmpty() && source == SecretStore::Source::None,
          QStringLiteral("нет ключа"));
    // Испорченный файл (Windows: чужой DPAPI) не даёт ключа и не падает.
    {
        QFile f(store.path());
        QDir().mkpath(QFileInfo(store.path()).absolutePath());
        if (f.open(QIODevice::WriteOnly))
            f.write(store.kind() == SecretStore::Kind::SystemProtected ? QByteArray("garbage") : QByteArray("   "));
        f.close();
        QString broken;
        check(!store.readKey(&broken) && broken.isEmpty(), QStringLiteral("испорченный файл — ключа нет"));
        store.removeKey();
    }
}

} // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qunsetenv("ANTHROPIC_API_KEY");
    QApplication app(argc, argv);
    // Только локальный сервер: системный прокси окружения не участвует.
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    std::printf("1. заголовки, тело, успех\n");
    testHeadersAndSuccess();
    testRefusalAndMaxTokens();
    std::printf("2. ошибки, повторы, тайм-аут, отмена\n");
    testErrors();
    std::printf("3. без согласия — ни одного соединения\n");
    testNoConnection();
    std::printf("4. хранилище ключа\n");
    testSecretStore();
    std::printf("%d проверок, %d провалов\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
