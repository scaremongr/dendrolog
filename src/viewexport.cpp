#include "viewexport.h"
#include "logmodel.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTextStream>
#include <QTimer>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <filesystem>

namespace {

std::filesystem::path nativePath(const QString& path)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(QFile::encodeName(path).constData());
#endif
}

ViewExport::Result identityError(const std::error_code& ec)
{
    // System messages come in the local 8-bit encoding, not UTF-8: MSVC
    // formats them in the ANSI code page (cp1251 on a Russian Windows).
    const std::string message = ec.message();
    return {ViewExport::Error::IdentityCheck,
            QString::fromLocal8Bit(message.data(), qsizetype(message.size()))};
}

ViewExport::Result checkDestination(const QString& destination,
                                   const QStringList& sourcePaths)
{
    const QFileInfo targetInfo(destination);
    const QString absoluteTarget = targetInfo.absoluteFilePath();
    // Resolve using Qt first: QSaveFile also follows Windows .lnk shortcuts,
    // which std::filesystem alone treats as ordinary, unrelated files.
    const QString canonicalTarget = targetInfo.canonicalFilePath();
    const auto target = nativePath(canonicalTarget.isEmpty() ? absoluteTarget : canonicalTarget);
    std::error_code ec;
    const bool targetExists = std::filesystem::exists(target, ec);
    if (ec)
        return identityError(ec);

    for (const QString& sourcePath : sourcePaths) {
        const QFileInfo sourceInfo(sourcePath);
        const QString absoluteSource = sourceInfo.absoluteFilePath();
        const QString canonicalSource = sourceInfo.canonicalFilePath();
        // Also protect a temporarily missing source (e.g. during rotation).
        if (absoluteSource == absoluteTarget
            || (!canonicalTarget.isEmpty() && canonicalSource == canonicalTarget))
            return {ViewExport::Error::SourceFile, sourcePath};
        if (!targetExists)
            continue;

        const auto source = nativePath(canonicalSource.isEmpty() ? absoluteSource : canonicalSource);
        const bool sourceExists = std::filesystem::exists(source, ec);
        if (ec)
            return identityError(ec);
        if (!sourceExists)
            continue;

        // Filesystem identity handles hard links, symlinks and case aliases,
        // without assuming that every Windows directory is case-insensitive.
        const bool same = std::filesystem::equivalent(target, source, ec);
        if (ec)
            return identityError(ec);
        if (same)
            return {ViewExport::Error::SourceFile, sourcePath};
    }
    return {};
}

} // namespace

ViewExport::Result ViewExport::writeRows(QIODevice& output, const RowSource& rows,
                                       const std::atomic_bool* cancel)
{
    QTextStream stream(&output);
    stream.setEncoding(QStringConverter::Utf8);
    Result result;
    qint64 written = 0;
    rows([&](QStringView text) {
        if (cancel && (++written & 0xFF) == 0 && cancel->load(std::memory_order_relaxed)) {
            result = {Error::Cancelled, QString()};
            return false;
        }
        stream << text << '\n';
        if (stream.status() != QTextStream::Ok) {
            result = {Error::Write, output.errorString()};
            return false;
        }
        return true;
    });
    if (!result.ok())
        return result;
    if (cancel && cancel->load(std::memory_order_relaxed))
        return {Error::Cancelled, QString()};
    stream.flush();
    if (stream.status() != QTextStream::Ok)
        return {Error::Write, output.errorString()};
    return {};
}

ViewExport::Result ViewExport::writeRows(QIODevice& output, int rowCount,
                                       const RowText& rowText)
{
    return writeRows(output, [&](const RowSink& sink) {
        for (int row = 0; row < rowCount; ++row) {
            if (!sink(rowText(row)))
                return;
        }
    });
}

ViewExport::Result ViewExport::save(const QString& destination,
                                  const QStringList& sourcePaths,
                                  const RowSource& rows, const std::atomic_bool* cancel)
{
    Result result = checkDestination(destination, sourcePaths);
    if (!result.ok())
        return result;

    QSaveFile output(destination);
    // Never fall back to truncating the destination if a temporary file cannot
    // be created. The existing result must survive any failed export.
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Text))
        return {Error::Open, output.errorString()};

    result = writeRows(output, rows, cancel);
    if (!result.ok()) {
        output.cancelWriting();
        return result;
    }
    // A long export can overlap external changes to the destination.
    result = checkDestination(destination, sourcePaths);
    if (!result.ok()) {
        output.cancelWriting();
        return result;
    }
    if (!output.commit())
        return {Error::Commit, output.errorString()};
    return {};
}

ViewExport::Result ViewExport::save(const QString& destination,
                                  const QStringList& sourcePaths,
                                  int rowCount, const RowText& rowText)
{
    return save(destination, sourcePaths, [&](const RowSink& sink) {
        for (int row = 0; row < rowCount; ++row) {
            if (!sink(rowText(row)))
                return;
        }
    });
}

// ---------------------------------------------------------------------------
// ViewExportJob
// ---------------------------------------------------------------------------

ViewExportJob::ViewExportJob(QObject* parent)
    : QObject(parent)
    , m_progressTimer(new QTimer(this))
{
    m_progressTimer->setInterval(200);
    connect(m_progressTimer, &QTimer::timeout, this, [this]() {
        if (m_progress)
            emit progress(m_progress->load(std::memory_order_relaxed));
    });
}

ViewExportJob::~ViewExportJob()
{
    // QSaveFile в воркере должен закончить (отменить) запись до выхода.
    cancel(/*wait=*/true);
}

bool ViewExportJob::start(const LogModel& model, const QString& destination,
                          const QStringList& sourcePaths)
{
    if (isRunning())
        return false;
    // Снапшот строк и выбора колонок — на момент старта: дальше модель можно
    // менять, фильтровать и даже закрывать, экспорт пишет то, что было видно.
    const LogScanSnapshot snapshot = model.scanSnapshot(/*filteredOnly=*/true);
    const bool fieldsShown = model.fieldDisplayFilterEnabled();
    const QVector<int> visibleIndexes = model.visibleFieldIndexes();
    const qint64 rows = snapshot.rowCount();
    auto cancelFlag = std::make_shared<std::atomic_bool>(false);
    auto progressValue = std::make_shared<std::atomic<int>>(0);
    m_cancel = cancelFlag;
    m_progress = progressValue;
    m_destination = destination;
    m_rowCount = int(rows);

    m_future = QtConcurrent::run([=]() {
        const ViewExport::RowSource source = [&](const ViewExport::RowSink& sink) {
            qint64 done = 0;
            snapshot.forEachLine(0, [&](qint64 row, const LogEntryMeta&, QStringView text) {
                if ((++done & 0x3FF) == 0)
                    progressValue->store(int(qMin<qint64>(99, done * 100 / qMax<qint64>(1, rows))),
                                         std::memory_order_relaxed);
                if (!fieldsShown)
                    return sink(text);
                // Текст строки валиден только внутри колбэка — без копии.
                const QString raw = QString::fromRawData(text.data(), text.size());
                return sink(LogModel::formatDisplayText(raw, snapshot.fieldsAt(row, raw),
                                                        true, visibleIndexes));
            });
        };
        return ViewExport::save(destination, sourcePaths, source, cancelFlag.get());
    });
    auto* watcher = new QFutureWatcher<ViewExport::Result>(this);
    const int generation = ++m_generation;
    connect(watcher, &QFutureWatcher<ViewExport::Result>::finished, this,
            [this, watcher, generation]() {
                watcher->deleteLater();
                if (generation != m_generation)
                    return;
                m_progressTimer->stop();
                m_cancel.reset();
                m_progress.reset();
                emit finished(watcher->result());
            });
    watcher->setFuture(m_future);
    m_progressTimer->start();
    return true;
}

void ViewExportJob::cancel(bool wait)
{
    if (m_cancel)
        m_cancel->store(true);
    if (wait)
        m_future.waitForFinished();
}

bool ViewExportJob::isRunning() const
{
    return m_future.isStarted() && !m_future.isFinished();
}
