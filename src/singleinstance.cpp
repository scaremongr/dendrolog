#include "singleinstance.h"

#include <QDir>
#include <QLocalServer>
#include <QLockFile>
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

bool SingleInstance::joinsSingleInstance(const OpenRouting::CommandLine& commandLine)
{
    return !commandLine.newInstance && !commandLine.readStdin;
}

SingleInstance::SingleInstance(const QString& appKey, const OpenRouting::CommandLine& commandLine,
                               QObject* parent)
    : QObject(parent)
{
    m_socketName = appKey + QLatin1Char('-') +
                   qEnvironmentVariable("USERNAME", qEnvironmentVariable("USER"));

    // Отдельное окно ключ не трогает: ни проб, ни сервера.
    if (!joinsSingleInstance(commandLine))
        return;

    // Проба и захват ключа одновременных запусков идут по очереди: иначе оба
    // не нашли бы сервер и оба стали бы первичными (на Unix listen() с
    // UserAccessOption молча заменяет файл чужого живого сокета). Блокировку
    // упавшего процесса QLockFile снимает сам; не дождались (держатель
    // завис) — работаем без неё, как до Q-20.
    QLockFile claimLock(QDir(QDir::tempPath()).filePath(m_socketName + QStringLiteral(".lock")));
    claimLock.tryLock(3000);

    // Первичный уже работает — передаём ему.
    if (primaryAnswers()) {
        m_role = Role::Forward;
        return;
    }
    // Никто не отвечает — занимаем. removeServer() подчищает сокет,
    // оставшийся после аварийного завершения (Unix).
    QLocalServer::removeServer(m_socketName);
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server->listen(m_socketName))
        return; // не удалось занять имя — работаем отдельным окном
    m_role = Role::Primary;

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

bool SingleInstance::primaryAnswers() const
{
    QLocalSocket probe;
    probe.connectToServer(m_socketName);
    if (!probe.waitForConnected(200))
        return false;
    probe.abort();
    return true;
}

bool SingleInstance::sendToPrimary(const QStringList& arguments) const
{
    if (m_role != Role::Forward)
        return false;
    QLocalSocket socket;
    socket.connectToServer(m_socketName);
    if (!socket.waitForConnected(500))
        return false;
    socket.write(encodeFiles(arguments));
    const bool ok = socket.waitForBytesWritten(1000);
    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState)
        socket.waitForDisconnected(500);
    return ok;
}
