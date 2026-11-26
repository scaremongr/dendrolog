#include "aischemaclient.h"

#include "schemaproposal.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

// ---------------------------------------------------------------------------
// AiConsent
// ---------------------------------------------------------------------------

AiConsent AiConsent::grant(const QByteArray& body)
{
    AiConsent consent;
    consent.m_digest = QCryptographicHash::hash(body, QCryptographicHash::Sha256);
    return consent;
}

bool AiConsent::covers(const QByteArray& body) const
{
    return isValid() && m_digest == QCryptographicHash::hash(body, QCryptographicHash::Sha256);
}

// ---------------------------------------------------------------------------
// Провайдеры
// ---------------------------------------------------------------------------

QString AiProvider::errorMessage(const QByteArray& body) const
{
    Q_UNUSED(body);
    return QString();
}

ClaudeProvider::ClaudeProvider(const QString& model, const QUrl& endpoint)
    : m_model(model)
    , m_endpoint(endpoint)
{
}

QUrl ClaudeProvider::defaultEndpoint()
{
    return QUrl(QStringLiteral("https://%1/v1/messages").arg(QLatin1String(SchemaProposal::kDefaultHost)));
}

QList<QPair<QByteArray, QByteArray>> ClaudeProvider::headers() const
{
    SchemaProposal::RequestOptions options;
    options.model = m_model;
    return SchemaProposal::requestHeaders(options);
}

QString ClaudeProvider::errorMessage(const QByteArray& body) const
{
    // {"type":"error","error":{"type":"…","message":"…"}} — текст сервера.
    const QJsonObject error = QJsonDocument::fromJson(body).object().value(QStringLiteral("error")).toObject();
    const QString message = error.value(QStringLiteral("message")).toString().left(300);
    const QString type = error.value(QStringLiteral("type")).toString();
    if (message.isEmpty())
        return type;
    return type.isEmpty() ? message : QStringLiteral("%1 (%2)").arg(message, type);
}

// ---------------------------------------------------------------------------
// AiSchemaClient
// ---------------------------------------------------------------------------

AiSchemaClient::AiSchemaClient(QObject* parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
    , m_timeout(new QTimer(this))
    , m_retryTimer(new QTimer(this))
{
    qRegisterMetaType<AiSchemaClient::Result>();
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, &AiSchemaClient::onTimeout);
    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, [this]() {
        m_retryPending = false;
        startAttempt();
    });
}

AiSchemaClient::~AiSchemaClient()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
    }
}

void AiSchemaClient::setProvider(std::shared_ptr<const AiProvider> provider)
{
    m_provider = std::move(provider);
}

bool AiSchemaClient::send(const QByteArray& body, AiConsent& consent)
{
    Result refused;
    if (isRunning()) {
        refused.status = Result::Status::Busy;
        refused.message = tr("A request is already running.");
    } else if (!m_config.enabled || !m_provider) {
        refused.status = Result::Status::Disabled;
        refused.message = tr("AI suggestions are turned off (Settings → AI).");
    } else if (m_config.apiKey.trimmed().isEmpty()) {
        refused.status = Result::Status::NoKey;
        refused.message = tr("No API key is set (Settings → AI).");
    } else if (!consent.covers(body)) {
        refused.status = Result::Status::NoConsent;
        refused.message = tr("Nothing was sent: the request was not confirmed.");
    }
    // Согласие — на одну отправку: гасится и при отказе, и при отправке.
    consent = AiConsent();
    if (refused.status != Result::Status::Cancelled) {
        finishLater(refused);
        return false;
    }
    m_body = body;
    m_attempts = 0;
    m_cancelled = false;
    startAttempt();
    return true;
}

void AiSchemaClient::cancel()
{
    if (m_retryPending) {
        m_retryTimer->stop();
        m_retryPending = false;
        Result result;
        result.status = Result::Status::Cancelled;
        result.message = tr("Cancelled.");
        result.attempts = m_attempts;
        finish(result);
        return;
    }
    if (m_reply) {
        m_cancelled = true;
        m_reply->abort();   // onReplyFinished — синхронно, итог Cancelled
    }
}

void AiSchemaClient::startAttempt()
{
    QNetworkRequest request(m_provider->endpoint());
    for (const auto& header : m_provider->headers())
        request.setRawHeader(header.first, header.second);
    request.setRawHeader(m_provider->keyHeader(), m_config.apiKey.trimmed().toUtf8());
    // Переадресация унесла бы ключ на другой адрес — не следуем.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    ++m_attempts;
    ++m_totalAttempts;
    m_timedOut = false;
    m_reply = m_network->post(request, m_body);
    connect(m_reply, &QNetworkReply::finished, this, &AiSchemaClient::onReplyFinished);
    m_timeout->start(qMax(1, m_config.timeoutMs));
}

void AiSchemaClient::onTimeout()
{
    if (m_reply) {
        m_timedOut = true;
        m_reply->abort();
    }
}

void AiSchemaClient::onReplyFinished()
{
    QNetworkReply* reply = m_reply;
    if (!reply)
        return;
    m_reply = nullptr;
    m_timeout->stop();
    reply->deleteLater();

    Result result;
    result.attempts = m_attempts;
    if (m_cancelled) {
        result.status = Result::Status::Cancelled;
        result.message = tr("Cancelled.");
        finish(result);
        return;
    }
    if (m_timedOut) {
        result.status = Result::Status::TimedOut;
        result.message = tr("The server did not answer in %1 s.").arg(qMax(1, m_config.timeoutMs / 1000));
        finish(result);
        return;
    }
    const QVariant statusAttr = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    const QByteArray body = reply->readAll();
    if (!statusAttr.isValid()) {
        result.status = Result::Status::NetworkError;
        result.message = tr("Could not reach %1: %2").arg(m_provider->endpoint().host(), reply->errorString());
        finish(result);
        return;
    }
    const int status = statusAttr.toInt();
    result.httpStatus = status;
    if (status == 200 && reply->error() == QNetworkReply::NoError) {
        result.status = Result::Status::Ok;
        result.body = body;
        finish(result);
        return;
    }
    const QString serverText = m_provider->errorMessage(body);
    const bool retryable = status == 429 || status == 529 || (status >= 500 && status <= 599);
    const bool forbidden = reply->rawHeader("x-should-retry").trimmed().toLower() == "false";
    if (retryable && !forbidden && m_attempts <= m_config.maxRetries) {
        qint64 delay = qint64(m_config.retryBaseMs) << qMin(m_attempts - 1, 20);
        bool ok = false;
        const qint64 retryAfter = reply->rawHeader("retry-after").trimmed().toLongLong(&ok);
        if (ok && retryAfter >= 0)
            delay = qMin<qint64>(retryAfter, qint64(m_config.maxRetryDelayMs) / 1000 + 1) * 1000;
        delay = qBound(qint64(0), delay, qint64(m_config.maxRetryDelayMs));
        m_retryPending = true;
        emit retrying(m_attempts + 1, int(delay));
        m_retryTimer->start(int(delay));
        return;
    }
    if (status == 401) {
        result.status = Result::Status::AuthFailed;
        result.message = tr("The API key was not accepted (401).");
    } else if (status == 429) {
        result.status = Result::Status::RateLimited;
        result.message = tr("Too many requests (429). Try again later.");
    } else if (status == 529) {
        result.status = Result::Status::Overloaded;
        result.message = tr("The service is overloaded (529). Try again later.");
    } else if (status >= 500) {
        result.status = Result::Status::ServerError;
        result.message = tr("Server error (%1).").arg(status);
    } else {
        result.status = Result::Status::HttpError;
        result.message = tr("The request was rejected (%1).").arg(status);
    }
    if (!serverText.isEmpty())
        result.message += QLatin1Char(' ') + serverText;
    // Сервер не повторяет ключ, но страховка дешёвая.
    if (!m_config.apiKey.trimmed().isEmpty())
        result.message.replace(m_config.apiKey.trimmed(), QStringLiteral("…"));
    finish(result);
}

void AiSchemaClient::finish(Result result)
{
    m_body.clear();
    emit finished(result);
}

void AiSchemaClient::finishLater(Result result)
{
    QTimer::singleShot(0, this, [this, result]() { emit finished(result); });
}
