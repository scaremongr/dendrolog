#include "filechangedetector.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QHash>

bool FileChangeDetector::fingerprintAt(const QString& filePath, qint64 endOffset, quint64& outHash)
{
    if (endOffset < 0)
        return false;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    if (file.size() < endOffset)
        return false; // file is shorter than the region we need — prefix changed

    const qint64 regionLen = qMin(endOffset, kBoundaryBytes);
    const qint64 start = endOffset - regionLen;
    if (start > 0 && !file.seek(start))
        return false;

    const QByteArray region = file.read(regionLen);
    if (region.size() != regionLen)
        return false;

    // Deterministic within a process run (anchors are never persisted to disk).
    outHash = qHash(region, 0);
    return true;
}

FileChangeDetector::Anchor FileChangeDetector::capture(const QString& filePath, qint64 consumedBytes)
{
    Anchor anchor;
    anchor.consumedBytes = consumedBytes;
    fingerprintAt(filePath, consumedBytes, anchor.fingerprint); // leaves 0 on failure
    return anchor;
}

FileChangeDetector::Change FileChangeDetector::classify(const QString& filePath, const Anchor& anchor)
{
    const QFileInfo info(filePath);
    if (!info.exists())
        return Change::Replaced;

    const qint64 currentSize = info.size();
    if (currentSize < anchor.consumedBytes)
        return Change::Replaced; // truncated

    // The boundary region ending at the old offset must be byte-identical for the
    // growth (or unchanged size) to be a genuine append rather than a rewrite.
    quint64 currentBoundary = 0;
    if (!fingerprintAt(filePath, anchor.consumedBytes, currentBoundary))
        return Change::Replaced;

    if (currentBoundary != anchor.fingerprint)
        return Change::Replaced; // same offset, different bytes → rewritten in place

    return (currentSize > anchor.consumedBytes) ? Change::Appended : Change::Unchanged;
}
