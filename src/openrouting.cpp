#include "openrouting.h"

#include "stdinspooler.h"

#include <QDir>
#include <QFileInfo>

namespace OpenRouting {

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
