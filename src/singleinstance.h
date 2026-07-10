#pragma once

#include <QObject>
#include <QStringList>

class QLocalServer;

// ---------------------------------------------------------------------------
// SingleInstance — «один экземпляр приложения на пользователя».
//
// Первый запуск занимает именованный QLocalServer и становится первичным.
// Повторный запуск обнаруживает занятое имя (isSecondary() == true), передаёт
// свои аргументы-файлы первичному экземпляру через sendToPrimary() и выходит.
// Первичный экземпляр получает их сигналом filesReceived().
//
// Имя сокета включает имя пользователя, чтобы сессии разных пользователей на
// одной машине (терминальный сервер) не конфликтовали.
// ---------------------------------------------------------------------------
class SingleInstance : public QObject
{
    Q_OBJECT

public:
    explicit SingleInstance(const QString& appKey, QObject* parent = nullptr);

    // true, если первичный экземпляр уже работает.
    bool isSecondary() const { return m_secondary; }

    // Передать список файлов первичному экземпляру.
    // Возвращает false, если тот не ответил за разумное время.
    bool sendToPrimary(const QStringList& files) const;

signals:
    // Первичный экземпляр: вторичный запуск передал файлы (список может быть
    // пустым — тогда нужно просто поднять окно).
    void filesReceived(const QStringList& files);

private:
    QString       m_socketName;
    QLocalServer* m_server = nullptr;
    bool          m_secondary = false;
};
