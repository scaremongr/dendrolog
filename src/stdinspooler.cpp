#include "stdinspooler.h"

#include "spooldir.h"

#include <QElapsedTimer>
#include <QFile>

#include <cstdio>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {
std::atomic_int g_inputDescriptorForTesting{-1};
}

void StdinSpooler::setInputDescriptorForTesting(int fd)
{
    g_inputDescriptorForTesting.store(fd);
}

StdinSpooler::StdinSpooler(QObject* parent)
    : QThread(parent)
{
}

StdinSpooler::~StdinSpooler()
{
    requestStop();
    if (!wait(500)) {
        // Поток заблокирован в fread на живом пайпе — прервать нечем.
        // На завершении процесса terminate безопасен: поток не держит
        // блокировок и пишет только в собственный буфер/файл.
        terminate();
        wait(200);
    }
    // Файл удалит SpoolItem, когда его отпустит и вкладка.
    m_spool.reset();
}

bool StdinSpooler::startSpooling(const QString& spoolRoot, QString* errorString)
{
    const std::shared_ptr<SpoolDir> dir = SpoolDir::shared(spoolRoot, errorString);
    if (!dir)
        return false;
    std::shared_ptr<SpoolItem> item = dir->createStreamFile(QStringLiteral("stdin"));
    QFile f(item->path());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorString)
            *errorString = f.errorString();
        return false;
    }
    f.close();
    m_spool = std::move(item);
    m_path = m_spool->path();
    start();
    return true;
}

void StdinSpooler::run()
{
    const int testFd = g_inputDescriptorForTesting.load();
#ifdef Q_OS_WIN
    const int inputFd = testFd >= 0 ? testFd : _fileno(stdin);
    // Иначе Windows-CRT в текстовом режиме портит \r\n и режет на Ctrl+Z.
    _setmode(inputFd, _O_BINARY);
#else
    const int inputFd = testFd >= 0 ? testFd : 0;
#endif

    QFile out(m_path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Append))
        return;

    char buf[64 * 1024];
    qint64 total = 0;
    QElapsedTimer sinceEmit;
    sinceEmit.start();

    while (!m_stop.load(std::memory_order_relaxed)) {
        // Именно read(), НЕ fread: fread блокируется, пока не наберёт весь
        // буфер, и живой поток отдавался бы кусками по 64 КБ. read возвращает
        // столько, сколько есть в пайпе (но хотя бы один байт).
#ifdef Q_OS_WIN
        const int got = _read(inputFd, buf, int(sizeof(buf)));
#else
        const qint64 got = ::read(inputFd, buf, sizeof(buf));
#endif
        if (got <= 0)
            break; // EOF либо ошибка пайпа
        out.write(buf, qint64(got));
        // Сразу в файл: QFile буферизует запись, а следующий read может
        // ждать данных сколько угодно — порция, после которой источник
        // замолчал, иначе не видна поллингу до новых данных или EOF (P-9).
        out.flush();
        total += qint64(got);

        // Свежие данные видны файловому поллингу немедленно, а сигналим не
        // чаще ~3 раз в секунду, чтобы не дёргать GUI.
        if (sinceEmit.elapsed() >= 300) {
            sinceEmit.restart();
            emit bytesAppended(total);
        }
    }

    emit bytesAppended(total);
    emit streamFinished(total);
}
