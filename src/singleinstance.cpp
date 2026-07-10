#include "singleinstance.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonArray>
#include <QJsonDocument>

namespace {

// Кадр протокола: одна JSON-строка (массив путей) + '\n'.
QByteArray encodeFiles(const QStringList& files)
{
    return QJsonDocument(QJsonArray::fromStringList(files))
               .toJson(QJsonDocument::Compact) + '\n';
}

QStringList decodeFiles(const QByteArray& line)
{
    QStringList out;
    const QJsonDocument doc = QJsonDocument::fromJson(line);
    for (const auto& v : doc.array())
        out << v.toString();
    return out;
}

} // namespace

SingleInstance::SingleInstance(const QString& appKey, QObject* parent)
    : QObject(parent)
{
    m_socketName = appKey + QLatin1Char('-') +
                   qEnvironmentVariable("USERNAME", qEnvironmentVariable("USER"));

    // Пробуем подключиться к уже работающему первичному экземпляру.
    QLocalSocket probe;
    probe.connectToServer(m_socketName);
    if (probe.waitForConnected(200)) {
        m_secondary = true;
        return;
    }

    // Никто не слушает — становимся первичным. removeServer() подчищает
    // сокет, оставшийся после аварийного завершения (актуально для Unix).
    QLocalServer::removeServer(m_socketName);
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server->listen(m_socketName)) {
        // Не удалось занять имя (гонка запусков) — работаем как обычное
        // независимое окно, это безопасный fallback.
        return;
    }

    connect(m_server, &QLocalServer::newConnection, this, [this]() {
        while (QLocalSocket* client = m_server->nextPendingConnection()) {
            connect(client, &QLocalSocket::readyRead, this, [this, client]() {
                if (!client->canReadLine())
                    return;
                emit filesReceived(decodeFiles(client->readLine()));
                client->disconnectFromServer();
            });
            connect(client, &QLocalSocket::disconnected,
                    client, &QLocalSocket::deleteLater);
        }
    });
}

bool SingleInstance::sendToPrimary(const QStringList& files) const
{
    QLocalSocket socket;
    socket.connectToServer(m_socketName);
    if (!socket.waitForConnected(500))
        return false;
    socket.write(encodeFiles(files));
    const bool ok = socket.waitForBytesWritten(1000);
    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState)
        socket.waitForDisconnected(500);
    return ok;
}
