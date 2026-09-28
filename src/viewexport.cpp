#include "viewexport.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTextStream>
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
        return {ViewExport::Error::IdentityCheck, QString::fromStdString(ec.message())};

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
            return {ViewExport::Error::IdentityCheck, QString::fromStdString(ec.message())};
        if (!sourceExists)
            continue;

        // Filesystem identity handles hard links, symlinks and case aliases,
        // without assuming that every Windows directory is case-insensitive.
        const bool same = std::filesystem::equivalent(target, source, ec);
        if (ec)
            return {ViewExport::Error::IdentityCheck, QString::fromStdString(ec.message())};
        if (same)
            return {ViewExport::Error::SourceFile, sourcePath};
    }
    return {};
}

} // namespace

ViewExport::Result ViewExport::writeRows(QIODevice& output, int rowCount,
                                       const RowText& rowText)
{
    QTextStream stream(&output);
    stream.setEncoding(QStringConverter::Utf8);
    for (int row = 0; row < rowCount; ++row) {
        stream << rowText(row) << '\n';
        if (stream.status() != QTextStream::Ok)
            return {Error::Write, output.errorString()};
    }
    stream.flush();
    if (stream.status() != QTextStream::Ok)
        return {Error::Write, output.errorString()};
    return {};
}

ViewExport::Result ViewExport::save(const QString& destination,
                                  const QStringList& sourcePaths,
                                  int rowCount, const RowText& rowText)
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

    result = writeRows(output, rowCount, rowText);
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
