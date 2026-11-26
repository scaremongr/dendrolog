#ifndef AISCHEMACLIENT_H
#define AISCHEMACLIENT_H

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

// ============================================================================
// Нейро-подсказчик, сеть (F-17, шаг 5; docs/design/F-17.md, раздел 5).
//
// AiConsent — согласие пользователя на ОДНУ отправку ровно этого тела
// запроса: его выдаёт окно согласия (AiConsentDialog) после «Send», клиент
// сверяет SHA-256 тела и гасит согласие после отправки. Тело, которое
// пользователь не видел, или второе отправление того же согласия — ни одного
// соединения. Галочки «больше не спрашивать» нет.
//
// AiSchemaClient — POST тела запроса провайдеру (QNetworkAccessManager, на
// GUI-потоке, без своих потоков). Провайдер — интерфейс AiProvider (адрес,
// заголовки, ключ); первая реализация — Claude Messages API, адрес — параметр
// (шов для теста: локальный сервер). Ничего не отправляет, пока подсказчик
// выключен (Settings → AI), нет ключа или нет согласия. Тайм-аут на запрос
// целиком (180 с по умолчанию), отмена; 429, 529 и 5xx — до двух повторов
// (пауза — retry-after, иначе 1 с, 2 с; «x-should-retry: false» запрещает
// повтор); 401 — «ключ не принят»; прочие 4xx — текст ошибки сервера.
//
// Приватность: ключ — только в заголовке запроса, не в сообщениях об ошибке и
// не в журналах; тела запросов и ответов не логируются. Итог — сигнал
// finished один раз на send (queued не нужен: всё на GUI-потоке).
// ============================================================================

class AiConsent {
public:
    AiConsent() = default;
    /// Согласие на отправку ровно этого тела (зовёт окно согласия).
    static AiConsent grant(const QByteArray& body);
    bool covers(const QByteArray& body) const;
    bool isValid() const { return !m_digest.isEmpty(); }

private:
    friend class AiSchemaClient;
    QByteArray m_digest;
};

class AiProvider {
public:
    virtual ~AiProvider() = default;
    virtual QUrl endpoint() const = 0;
    /// Заголовки запроса, кроме ключа.
    virtual QList<QPair<QByteArray, QByteArray>> headers() const = 0;
    /// Заголовок ключа («x-api-key»).
    virtual QByteArray keyHeader() const = 0;
    /// Сообщение об ошибке из тела ответа не-200 (без ключа).
    virtual QString errorMessage(const QByteArray& body) const;
};

/// Claude Messages API. endpoint — https://api.anthropic.com/v1/messages
/// по умолчанию; заголовки — SchemaProposal::requestHeaders(модель).
class ClaudeProvider : public AiProvider {
public:
    explicit ClaudeProvider(const QString& model, const QUrl& endpoint = defaultEndpoint());
    static QUrl defaultEndpoint();
    QUrl endpoint() const override { return m_endpoint; }
    QList<QPair<QByteArray, QByteArray>> headers() const override;
    QByteArray keyHeader() const override { return QByteArrayLiteral("x-api-key"); }
    QString errorMessage(const QByteArray& body) const override;

private:
    QString m_model;
    QUrl m_endpoint;
};

class AiSchemaClient : public QObject {
    Q_OBJECT
public:
    struct Config {
        bool enabled = false;          ///< Settings → AI
        QString apiKey;
        int timeoutMs = 180000;        ///< на запрос целиком (с повторами — на попытку)
        int maxRetries = 2;
        int retryBaseMs = 1000;        ///< без retry-after: 1 с, 2 с
        int maxRetryDelayMs = 60000;   ///< retry-after больше — ждём столько
    };

    struct Result {
        enum class Status {
            Ok,
            Disabled,      ///< подсказчик выключен — соединения не было
            NoConsent,     ///< нет согласия на это тело — соединения не было
            NoKey,         ///< нет ключа — соединения не было
            Busy,          ///< уже идёт запрос
            AuthFailed,    ///< 401
            RateLimited,   ///< 429 после повторов
            Overloaded,    ///< 529 после повторов
            ServerError,   ///< 5xx после повторов
            HttpError,     ///< прочие 4xx
            NetworkError,  ///< обрыв, нет соединения
            TimedOut,
            Cancelled,
        };
        Status status = Status::Cancelled;
        int httpStatus = 0;
        QByteArray body;      ///< тело ответа 200 (иначе пусто)
        QString message;      ///< для человека; ключа в нём нет никогда
        int attempts = 0;     ///< попыток соединения
        bool ok() const { return status == Status::Ok; }
    };

    explicit AiSchemaClient(QObject* parent = nullptr);
    ~AiSchemaClient() override;

    void setProvider(std::shared_ptr<const AiProvider> provider);
    void setConfig(const Config& config) { m_config = config; }
    const Config& config() const { return m_config; }

    /// Отправить тело с согласием пользователя. false — отказано сразу
    /// (Disabled, NoKey, NoConsent, Busy): соединения не было, finished с
    /// этим итогом всё равно придёт (отложенно, из цикла событий).
    bool send(const QByteArray& body, AiConsent& consent);
    void cancel();
    bool isRunning() const;

    /// Сколько раз открывались соединения (для теста «ни одного соединения»).
    int connectionAttempts() const { return m_totalAttempts; }

signals:
    /// Итог запроса. Ответ сети приходит изнутри QNetworkAccessManager::finished
    /// (S-29): удалять клиента из обработчика — только deleteLater, синхронное
    /// удаление освободило бы менеджер посреди его сигнала.
    void finished(const AiSchemaClient::Result& result);
    void retrying(int attempt, int delayMs);

private:
    void startAttempt();
    void onReplyFinished(QNetworkReply* reply);
    void onTimeout();
    void finish(Result result);
    void finishLater(Result result);

    QNetworkAccessManager* m_network = nullptr;
    std::shared_ptr<const AiProvider> m_provider;
    Config m_config;
    QByteArray m_body;
    QPointer<QNetworkReply> m_reply;
    QTimer* m_timeout = nullptr;
    QTimer* m_retryTimer = nullptr;
    bool m_retryPending = false;
    bool m_timedOut = false;
    bool m_cancelled = false;
    int m_attempts = 0;
    int m_totalAttempts = 0;
};

Q_DECLARE_METATYPE(AiSchemaClient::Result)

#endif // AISCHEMACLIENT_H
