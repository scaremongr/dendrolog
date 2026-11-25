#include "logfile.h"

#include <QDir>
#include <QFileInfo>

LogFile::LogFile(const QString &path)
    : filePath(path)
{
    QFileInfo fi(path);
    displayName = fi.fileName();
    lastModified = fi.lastModified();
    fileSize = fi.size();
    m_source.path = path;
}

void LogFile::setSource(const LogSource& source)
{
    m_source = source;
    // Разделитель меньше любого печатного символа: члены одного архива идут
    // подряд и сразу за самим архивом, а не вперемешку с соседними путями.
    // У .gz имени внутри нет — ключ равен пути архива.
    if (source.kind != LogSource::Kind::Archive)
        m_archiveOrderKey.clear();
    else if (source.member.isEmpty())
        m_archiveOrderKey = source.path;
    else
        m_archiveOrderKey = source.path + QChar(0x01) + source.member;
}

QString LogFile::displayPath() const
{
    switch (m_source.kind) {
    case LogSource::Kind::File:
        return filePath;
    case LogSource::Kind::Archive:
        // Разделители — как у filePath обычного файла: в нативные переводит
        // место показа.
        if (m_source.member.isEmpty())
            return m_source.path;
        return m_source.path + QStringLiteral(" \u203A ") + m_source.member;
    case LogSource::Kind::Stream:
        return m_source.label.isEmpty() ? displayName : m_source.label;
    }
    return filePath;
}

QString LogFile::nativeDisplayPath() const
{
    if (m_source.kind == LogSource::Kind::Archive && !m_source.member.isEmpty())
        return QDir::toNativeSeparators(m_source.path) + QStringLiteral(" \u203A ") + m_source.member;
    return QDir::toNativeSeparators(displayPath());
}
