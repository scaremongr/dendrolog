#include "openrouting.h"

#include "sessionfile.h"
#include "stdinspooler.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

namespace OpenRouting {

static const QString kEncodingOption = QStringLiteral("--encoding");

CommandLine parseCommandLine(const QStringList& arguments)
{
    CommandLine cl;
    bool encodingGiven = false;
    for (qsizetype i = 0; i < arguments.size(); ++i) {
        const QString& arg = arguments.at(i);
        if (arg == QLatin1String("--new-instance")) {
            cl.newInstance = true;
        } else if (arg == QLatin1String("-") || arg == QLatin1String("--stdin")) {
            cl.readStdin = true;
        } else if (arg == kEncodingOption) {
            encodingGiven = true;
            // Имя пропущено: следующий аргумент — флаг («-», «--stdin», …) или
            // существующий файл, а не кодировка, — его не съедаем.
            if (i + 1 < arguments.size()) {
                const QString& next = arguments.at(i + 1);
                const bool looksLikeName = !next.startsWith(QLatin1Char('-'))
                    && (TextEncodings::fromName(next.trimmed()) || !QFileInfo::exists(next));
                if (looksLikeName)
                    cl.encodingName = arguments.at(++i);
            }
        } else if (arg.startsWith(kEncodingOption + QLatin1Char('='))) {
            encodingGiven = true;
            cl.encodingName = arg.mid(kEncodingOption.size() + 1);
        } else {
            cl.paths << arg;
        }
    }
    if (!encodingGiven)
        return cl;

    QStringList known;
    for (const TextEncoding e : TextEncodings::selectable())
        known << TextEncodings::displayName(e);
    const QString knownList = known.join(QStringLiteral(", "));
    if (cl.encodingName.trimmed().isEmpty()) {
        cl.error = QCoreApplication::translate("OpenRouting",
            "--encoding needs an encoding name (%1). Files are opened with automatic "
            "detection.").arg(knownList);
        return cl;
    }
    cl.encoding = TextEncodings::fromName(cl.encodingName.trimmed());
    if (!cl.encoding) {
        cl.error = QCoreApplication::translate("OpenRouting",
            "Unknown encoding \"%1\" in --encoding (known: %2; cp1251, cp866, koi8r, "
            "cp1252 also work). Files are opened with automatic detection.")
            .arg(cl.encodingName, knownList);
    }
    return cl;
}

bool ownsLastSession(const CommandLine& commandLine, bool otherInstanceRunning)
{
    return !commandLine.newInstance && !commandLine.readStdin && !otherInstanceRunning;
}

FileEncoding initialEncoding(const CommandLine& commandLine)
{
    if (!commandLine.encoding)
        return {};
    return FileEncoding().withUserChoice(TextDecoder(*commandLine.encoding));
}

QStringList forwardedArguments(const CommandLine& commandLine)
{
    QStringList out;
    if (!commandLine.encodingName.isEmpty() || !commandLine.error.isEmpty())
        out << kEncodingOption + QLatin1Char('=') + commandLine.encodingName;
    out << absolutePaths(commandLine.paths);
    return out;
}

static QString absoluteClean(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

Plan plan(const QStringList& paths)
{
    Plan result;
    QStringList directories;
    for (const QString& raw : paths) {
        if (raw.isEmpty())
            continue;
        const QString path = absoluteClean(raw);
        const QFileInfo info(path);
        if (info.isDir()) {
            if (!directories.contains(path))
                directories << path;
        } else if (info.isFile() && SessionFile::isSessionFile(path)) {
            if (!result.sessions.contains(path))
                result.sessions << path;
        } else if (info.isFile()) {
            if (!result.files.contains(path))
                result.files << path;
        } else if (!result.missing.contains(path)) {
            result.missing << path;
        }
    }
    if (!directories.isEmpty()) {
        result.scanDirectory = directories.takeFirst();
        result.skippedDirectories = directories;
    }
    return result;
}

QStringList absolutePaths(const QStringList& paths)
{
    QStringList result;
    result.reserve(paths.size());
    for (const QString& path : paths)
        if (!path.isEmpty())
            result << absoluteClean(path);
    return result;
}

} // namespace OpenRouting

bool RecentFiles::accepts(const QString& path)
{
    return !path.isEmpty() && !StdinSpooler::isSpoolFilePath(path);
}

void RecentFiles::setPaths(const QStringList& paths)
{
    m_paths.clear();
    for (const QString& path : paths) {
        if (m_paths.size() >= MaxCount)
            break;
        if (accepts(path) && !m_paths.contains(path))
            m_paths << path;
    }
}

bool RecentFiles::add(const QString& path)
{
    if (!accepts(path))
        return false;
    m_paths.removeAll(path);
    m_paths.prepend(path);
    while (m_paths.size() > MaxCount)
        m_paths.removeLast();
    return true;
}

bool RecentFiles::remove(const QString& path)
{
    return m_paths.removeAll(path) > 0;
}
