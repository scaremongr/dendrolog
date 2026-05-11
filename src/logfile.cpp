#include "logfile.h"
#include <QFileInfo>

LogFile::LogFile(const QString &path)
    : filePath(path)
{
    QFileInfo fi(path);
    displayName = fi.fileName();
    lastModified = fi.lastModified();
    fileSize = fi.size();
} 