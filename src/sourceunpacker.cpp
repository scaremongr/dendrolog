#include "sourceunpacker.h"

#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QtConcurrent>

namespace {

constexpr qint64 kChunkBytes = 1024 * 1024;
constexpr qint64 kProgressStepBytes = 4 * 1024 * 1024;

QString mib(qint64 bytes)
{
    return QString::number(double(bytes) / (1024.0 * 1024.0), 'f', 1) + QStringLiteral(" MB");
}

} // namespace

SourceUnpacker::SourceUnpacker(QObject* parent)
    : QObject(parent)
    , m_freeSpace(&SourceUnpacker::systemFreeSpace)
{
    qRegisterMetaType<SourceUnpacker::Result>();
    m_pool.setMaxThreadCount(1);
    connect(this, &SourceUnpacker::workerFinished, this, &SourceUnpacker::onWorkerFinished,
            Qt::QueuedConnection);
}

SourceUnpacker::~SourceUnpacker()
{
    m_cancel.store(true);
    m_pool.waitForDone();
}

void SourceUnpacker::setFreeSpaceFunction(FreeSpaceFunction fn)
{
    m_freeSpace = fn ? std::move(fn) : FreeSpaceFunction(&SourceUnpacker::systemFreeSpace);
}

qint64 SourceUnpacker::systemFreeSpace(const QString& dir)
{
    const QStorageInfo info(dir);
    if (!info.isValid() || !info.isReady())
        return -1;
    return info.bytesAvailable();
}

void SourceUnpacker::cancel()
{
    m_cancel.store(true);
}

void SourceUnpacker::waitForFinished()
{
    m_pool.waitForDone();
}

void SourceUnpacker::onWorkerFinished(const Result& result)
{
    emit finished(result);
}

quint64 SourceUnpacker::unpackGzip(const QString& sourcePath, const QString& targetPath)
{
    if (m_busy.load())
        return 0;
    m_busy.store(true);
    m_cancel.store(false);
    const quint64 job = ++m_lastJob;

    // Снимок настроек: воркер не читает члены объекта, кроме флага отмены.
    const FreeSpaceFunction freeSpace = m_freeSpace;
    const qint64 reserve = qMax<qint64>(0, m_reserveBytes);
    const qint64 interval = qMax<qint64>(kChunkBytes, m_checkIntervalBytes);

    // QtConcurrent::run в свой пул, как у LogParser/LogIndexer: передачу
    // данных в воркер видит и слой TSan (tests/sanitizers/tsan_qt_sync.cpp),
    // голый QThreadPool::start(лямбда) — нет.
    (void)QtConcurrent::run(&m_pool, [this, job, sourcePath, targetPath, freeSpace, reserve, interval]() {
        Result r;
        r.job = job;
        r.sourcePath = sourcePath;
        r.targetPath = targetPath;
        const QString targetDir = QFileInfo(targetPath).absolutePath();

        // Занятость снимается сразу после отправки итога: следующее задание
        // можно ставить, не дожидаясь, пока Result дойдёт через очередь.
        const auto finish = [this, &r]() {
            emit workerFinished(r, QPrivateSignal());
            m_busy.store(false);
        };
        const auto sourceFailed = [&r](GzipDevice::Error e, const QString& text) {
            r.error = Error::SourceError;
            r.gzipError = e;
            r.message = text;
        };

        // 1. Источник — gzip? Без сигнатуры целевой файл даже не создаётся.
        const QFileInfo src(sourcePath);
        QFile probe(sourcePath);
        if (!probe.open(QIODevice::ReadOnly)) {
            sourceFailed(GzipDevice::Error::ReadError,
                         GzipDevice::errorText(GzipDevice::Error::ReadError) + QStringLiteral(": ")
                             + probe.errorString());
            finish();
            return;
        }
        if (!GzipDevice::hasSignature(probe.read(2))) {
            sourceFailed(GzipDevice::Error::NotGzip, GzipDevice::errorText(GzipDevice::Error::NotGzip));
            finish();
            return;
        }
        probe.close();
        const qint64 compressedTotal = src.size();

        // Цель — сам источник (тот же путь или ссылка на него): открытие на
        // запись обнулило бы источник, а уборка после ошибки — удалила.
        const QString srcCanonical = src.canonicalFilePath();
        if (!srcCanonical.isEmpty() && QFileInfo(targetPath).canonicalFilePath() == srcCanonical) {
            r.error = Error::WriteFailed;
            r.message = QStringLiteral("Cannot unpack %1 onto itself").arg(sourcePath);
            finish();
            return;
        }

        // 2. Место ДО начала: нижняя оценка распакованного + резерв.
        const qint64 estimate = qMax(compressedTotal, GzipDevice::trailerSize(sourcePath));
        const qint64 freeBefore = freeSpace(targetDir);
        if (freeBefore >= 0 && freeBefore < estimate + reserve) {
            r.error = Error::NoSpace;
            r.requiredBytes = estimate + reserve;
            r.availableBytes = freeBefore;
            r.message = QStringLiteral("Not enough disk space to unpack: at least %1 needed "
                                       "(including %2 reserve), %3 free")
                            .arg(mib(r.requiredBytes), mib(reserve), mib(freeBefore));
            finish();
            return;
        }

        // 3. Распаковка блоками; отмена и место — между блоками.
        GzipDevice gz(sourcePath);
        gz.setAbortFlag(&m_cancel); // отмена — и посреди входа без вывода
        if (!gz.open(QIODevice::ReadOnly)) {
            sourceFailed(gz.error(), gz.errorString());
            finish();
            return;
        }
        QFile out(targetPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            r.error = Error::WriteFailed;
            r.message = QStringLiteral("Cannot create %1: %2").arg(targetPath, out.errorString());
            finish();
            return;
        }

        QByteArray buffer(int(kChunkBytes), Qt::Uninitialized);
        qint64 sinceCheck = 0;
        qint64 lastProgress = 0;
        emit progress(0, compressedTotal);
        for (;;) {
            if (m_cancel.load(std::memory_order_relaxed)) {
                r.error = Error::Cancelled;
                r.message = errorText(Error::Cancelled);
                break;
            }
            const qint64 n = gz.read(buffer.data(), buffer.size());
            if (n < 0) {
                if (gz.error() == GzipDevice::Error::Aborted) {
                    r.error = Error::Cancelled;
                    r.message = errorText(Error::Cancelled);
                } else {
                    sourceFailed(gz.error(), gz.errorString());
                }
                break;
            }
            if (n == 0) {
                if (gz.error() != GzipDevice::Error::None)
                    sourceFailed(gz.error(), gz.errorString());
                break; // конец: все потоки проверены CRC и длиной
            }
            if (out.write(buffer.constData(), n) != n) {
                r.error = Error::WriteFailed;
                r.message = QStringLiteral("Cannot write %1: %2").arg(targetPath, out.errorString());
                break;
            }
            r.bytesWritten += n;
            sinceCheck += n;
            if (sinceCheck >= interval) {
                sinceCheck = 0;
                if (!out.flush()) {
                    r.error = Error::WriteFailed;
                    r.message = QStringLiteral("Cannot write %1: %2").arg(targetPath, out.errorString());
                    break;
                }
                const qint64 freeNow = freeSpace(targetDir);
                if (freeNow >= 0 && freeNow < reserve) {
                    r.error = Error::NoSpace;
                    r.requiredBytes = reserve;
                    r.availableBytes = freeNow;
                    r.message = QStringLiteral("Unpacking stopped: free disk space fell below the "
                                               "%1 reserve after %2 written")
                                    .arg(mib(reserve), mib(r.bytesWritten));
                    break;
                }
            }
            const qint64 done = gz.compressedBytesRead();
            if (done - lastProgress >= kProgressStepBytes) {
                lastProgress = done;
                emit progress(done, compressedTotal);
            }
        }

        if (r.error == Error::None && !out.flush()) {
            r.error = Error::WriteFailed;
            r.message = QStringLiteral("Cannot write %1: %2").arg(targetPath, out.errorString());
        }
        out.close();
        gz.close();
        if (r.error == Error::None) {
            emit progress(compressedTotal, compressedTotal);
        } else {
            // Частичный файл — не результат: не оставлять.
            QFile::remove(targetPath);
        }
        finish();
    });
    return job;
}

QString SourceUnpacker::errorText(Error error)
{
    switch (error) {
    case Error::None: return QString();
    case Error::Cancelled: return QStringLiteral("Unpacking cancelled");
    case Error::SourceError: return QStringLiteral("Cannot unpack the file");
    case Error::NoSpace: return QStringLiteral("Not enough disk space");
    case Error::WriteFailed: return QStringLiteral("Cannot write the unpacked file");
    }
    return QString();
}
