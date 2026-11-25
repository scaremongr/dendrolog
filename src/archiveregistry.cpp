#include "archiveregistry.h"

#include "spooldir.h"

#include <QDir>
#include <QFileInfo>

static QString cleanAbsolute(const QString& path)
{
    return QDir::cleanPath(QFileInfo(QDir::fromNativeSeparators(path)).absoluteFilePath());
}

ArchiveRegistry& ArchiveRegistry::instance()
{
    static ArchiveRegistry registry;
    return registry;
}

void ArchiveRegistry::add(const QString& directory, const QString& archivePath,
                          const QHash<QString, QString>& members, std::shared_ptr<SpoolItem> item)
{
    m_entries.insert(cleanAbsolute(directory), Entry{ archivePath, members, std::move(item) });
}

void ArchiveRegistry::remove(const QString& directory)
{
    m_entries.remove(cleanAbsolute(directory));
}

const ArchiveRegistry::Entry* ArchiveRegistry::entryFor(const QString& path, QString* relative) const
{
    if (m_entries.isEmpty() || path.isEmpty())
        return nullptr;
    const QString clean = cleanAbsolute(path);
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        const QString& dir = it.key();
#ifdef Q_OS_WIN
        const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
        const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
        if (clean.compare(dir, cs) == 0) {
            if (relative)
                relative->clear();
            return &it.value();
        }
        if (clean.size() > dir.size() && clean.startsWith(dir, cs) && clean.at(dir.size()) == u'/') {
            if (relative)
                *relative = clean.mid(dir.size() + 1);
            return &it.value();
        }
    }
    return nullptr;
}

std::optional<ArchiveRegistry::Member> ArchiveRegistry::locate(const QString& path) const
{
    QString relative;
    const Entry* entry = entryFor(path, &relative);
    if (!entry || relative.isEmpty())
        return std::nullopt;
    const auto it = entry->members.constFind(relative);
    if (it == entry->members.cend())
        return std::nullopt;
    return Member{ entry->archivePath, *it };
}

bool ArchiveRegistry::isInsideExtraction(const QString& path) const
{
    return entryFor(path, nullptr) != nullptr;
}

QString ArchiveRegistry::archiveFor(const QString& directory) const
{
    QString relative;
    const Entry* entry = entryFor(directory, &relative);
    return entry && relative.isEmpty() ? entry->archivePath : QString();
}
