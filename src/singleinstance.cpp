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
    // упавшего процесса QLockFile снимает сам.
    QLockFile claimLock(QDir(QDir::tempPath()).filePath(m_socketName + QStringLiteral(".lock")));
    const bool locked = claimLock.tryLock(3000);
    // Блокировку держит другой запуск дольше 3 с (завис посреди захвата):
    // занимать ключ мимо неё нельзя — передать файлы, если первичный уже
    // есть, иначе отдельное окно. Файл блокировки не создаётся вовсе (права
    // на временный каталог) — захват без неё, как до Q-20.
    const bool mayClaim = locked || claimLock.error() != QLockFile::LockFailedError;

    switch (probePrimary()) {
    case Probe::Answers: // первичный уже работает — передаём ему
        m_role = Role::Forward;
        return;
    case Probe::Unsure:  // сокет есть, но не ответил: возможно, живой и занятой
        return;          // первичный — его ключ не отбираем, окно отдельное
    case Probe::Absent:
        break;
    }
    if (!mayClaim)
        return;
    // Никто не слушает — занимаем. removeServer() подчищает сокет,
    // оставшийся после аварийного завершения (Unix: соединение отклонено).
    QLocalServer::removeServer(m_socketName);
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server->listen(m_socketName))
        return; // не удалось занять имя — работаем отдельным окном
    m_role = Role::Primary;

    connect(m_server, &QLocalServer::newConnection, this, [this]() {
        while (QLocalSocket* client = m_server->nextPendingConnection()) {
            // Канал не закрываем: его закрывает отправитель, дождавшись
            // своей записи (см. «Ловушки» в заголовке, S-15).
            connect(client, &QLocalSocket::readyRead, this, [this, client]() {
                while (client->canReadLine())
                    emit filesReceived(decodeFiles(client->readLine()));
            });
            connect(client, &QLocalSocket::disconnected,
                    client, &QLocalSocket::deleteLater);
        }
    });
}

SingleInstance::Probe SingleInstance::probePrimary() const
{
    QLocalSocket probe;
    probe.connectToServer(m_socketName);
    if (probe.waitForConnected(200)) {
        probe.abort();
        return Probe::Answers;
    }
    // Нет имени или соединение отклонено — первичного нет (или он упал и
    // оставил сокет). Таймаут и прочее — не знаем: возможно, живой, но
    // занятой первичный.
    const auto error = probe.error();
    return error == QLocalSocket::ServerNotFoundError || error == QLocalSocket::ConnectionRefusedError
               ? Probe::Absent : Probe::Unsure;
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
