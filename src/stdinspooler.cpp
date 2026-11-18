#include "stdinspooler.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include <cstdio>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

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
    if (!m_path.isEmpty())
        QFile::remove(m_path);
}

QString StdinSpooler::spoolFilePathFor(qint64 pid)
{
    return QDir::temp().filePath(QStringLiteral("DendroLog-stdin-%1.log").arg(pid));
}

bool StdinSpooler::isSpoolFilePath(const QString& path)
{
    const QFileInfo info(QDir::fromNativeSeparators(path));
    const QString name = info.fileName();
    const QLatin1String prefix("DendroLog-stdin-");
    const QLatin1String suffix(".log");
    if (!name.startsWith(prefix) || !name.endsWith(suffix))
        return false;
    const QStringView pid = QStringView(name).mid(prefix.size(),
                                                  name.size() - prefix.size() - suffix.size());
    if (pid.isEmpty())
        return false;
    for (const QChar ch : pid)
        if (ch < QLatin1Char('0') || ch > QLatin1Char('9'))
            return false;
#ifdef Q_OS_WIN
    const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
    return QDir::cleanPath(info.absolutePath())
               .compare(QDir::cleanPath(QDir::temp().absolutePath()), cs) == 0;
}

bool StdinSpooler::startSpooling()
{
    const QString path = spoolFilePathFor(QCoreApplication::applicationPid());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.close();
    m_path = path;
    start();
    return true;
}

void StdinSpooler::run()
{
#ifdef Q_OS_WIN
    // Иначе Windows-CRT в текстовом режиме портит \r\n и режет на Ctrl+Z.
    _setmode(_fileno(stdin), _O_BINARY);
#endif

    QFile out(m_path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Append))
        return;

    char buf[64 * 1024];
    qint64 total = 0;
    qint64 unflushed = 0;
    QElapsedTimer sinceEmit;
    sinceEmit.start();

    while (!m_stop.load(std::memory_order_relaxed)) {
        // Именно read(), НЕ fread: fread блокируется, пока не наберёт весь
        // буфер, и живой поток отдавался бы кусками по 64 КБ. read возвращает
        // столько, сколько есть в пайпе (но хотя бы один байт).
#ifdef Q_OS_WIN
        const int got = _read(_fileno(stdin), buf, int(sizeof(buf)));
#else
        const qint64 got = ::read(0, buf, sizeof(buf));
#endif
        if (got <= 0)
            break; // EOF либо ошибка пайпа
        out.write(buf, qint64(got));
        total += qint64(got);
        unflushed += qint64(got);

        // Свежие данные становятся видимыми файловому поллингу немедленно,
        // но сигналим не чаще ~3 раз в секунду, чтобы не дёргать GUI.
        if (sinceEmit.elapsed() >= 300) {
            out.flush();
            unflushed = 0;
            sinceEmit.restart();
            emit bytesAppended(total);
        }
    }

    if (unflushed > 0)
        out.flush();
    emit bytesAppended(total);
    emit streamFinished(total);
}
