#include "filechangedetector.h"

#include "recordid.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>

#include <atomic>
#include <optional>

namespace {

std::atomic<quint64> g_opens{0};
std::atomic<quint64> g_reads{0};

// Fingerprints of the consumed prefix [0, consumed) of an open file: the head
// and the tail region, as SourceIdentity takes them. With `checkHead` false
// only the tail is read (head stays nullopt); up to 4 KB the two are one
// region, read once. nullopt tail — the file is shorter or unreadable.
struct Regions {
    std::optional<quint64> head;
    std::optional<quint64> tail;
};

Regions readRegions(QFile& file, qint64 consumed, bool checkHead)
{
    Regions r;
    const qint64 tailFrom = RecordFingerprint::tailRegionStart(consumed);
    ++g_reads;
    r.tail = RecordFingerprint::ofFileRegion(file, tailFrom, consumed - tailFrom);
    if (!r.tail)
        return r;
    if (tailFrom == 0) {
        r.head = r.tail;
    } else if (checkHead) {
        ++g_reads;
        r.head = RecordFingerprint::ofFileRegion(file, 0, RecordFingerprint::headRegionLength(consumed));
    }
    return r;
}

bool openForRead(QFile& file)
{
    ++g_opens;
    // No QIODevice::Text: fingerprints are over raw bytes (invariant 5).
    return file.open(QIODevice::ReadOnly);
}

bool isRacy(qint64 mtimeMs)
{
    return mtimeMs >= QDateTime::currentMSecsSinceEpoch() - FileChangeDetector::kRacyWindowMs;
}

} // namespace

FileChangeDetector::Anchor FileChangeDetector::capture(const QString& filePath, qint64 consumedBytes)
{
    Anchor anchor;
    anchor.consumedBytes = consumedBytes;
    if (consumedBytes < 0)
        return anchor;
    // Stat before reading: if a writer changes the file in between, the stamp
    // is the older one and the next check reads the head again.
    const QFileInfo info(filePath);
    if (!info.exists())
        return anchor;
    anchor.stampSize = info.size();
    anchor.stampMtimeMs = info.lastModified().toMSecsSinceEpoch();
    anchor.stampRacy = isRacy(anchor.stampMtimeMs);

    QFile file(filePath);
    if (!openForRead(file) || file.size() < consumedBytes)
        return anchor;
    const Regions r = readRegions(file, consumedBytes, /*checkHead=*/true);
    if (!r.head || !r.tail)
        return anchor;
    anchor.head = *r.head;
    anchor.tail = *r.tail;
    anchor.valid = true;
    return anchor;
}

namespace {

FileChangeDetector::Change classifyImpl(const QString& filePath,
                                        const FileChangeDetector::Anchor& anchor,
                                        FileChangeDetector::Anchor* refresh)
{
    using Change = FileChangeDetector::Change;
    const QFileInfo info(filePath);
    if (!info.exists() || !anchor.valid)
        return Change::Replaced;

    const qint64 currentSize = info.size();
    if (currentSize < anchor.consumedBytes)
        return Change::Replaced; // truncated

    QFile file(filePath);
    if (!openForRead(file) || file.size() < anchor.consumedBytes)
        return Change::Replaced;

    // The head is re-read only when something touched the file since the stamp
    // or the stamp is too recent to vouch for it (see the header).
    const qint64 currentMtime = info.lastModified().toMSecsSinceEpoch();
    const bool touched = anchor.stampRacy || currentSize != anchor.stampSize
        || currentMtime != anchor.stampMtimeMs;
    const Regions r = readRegions(file, anchor.consumedBytes, touched);

    // Both regions of the consumed prefix must be byte-identical for the growth
    // (or unchanged size) to be a genuine append rather than a rewrite.
    if (!r.tail || *r.tail != anchor.tail)
        return Change::Replaced; // same offset, different bytes → rewritten in place
    if (r.head && *r.head != anchor.head)
        return Change::Replaced; // start of the file rewritten (Q-19)

    if (currentSize > anchor.consumedBytes)
        return Change::Appended; // the caller re-anchors after reading the tail

    // Unchanged and the head verified against the current stat: that stat is
    // the new stamp (a touch costs one extra read, not one per tick).
    if (refresh && r.head && touched) {
        refresh->stampSize = currentSize;
        refresh->stampMtimeMs = currentMtime;
        refresh->stampRacy = isRacy(currentMtime);
    }
    return Change::Unchanged;
}

} // namespace

FileChangeDetector::Change FileChangeDetector::classify(const QString& filePath, const Anchor& anchor)
{
    return classifyImpl(filePath, anchor, nullptr);
}

FileChangeDetector::Change FileChangeDetector::check(const QString& filePath, Anchor& anchor)
{
    return classifyImpl(filePath, anchor, &anchor);
}

FileChangeDetector::IoCounts FileChangeDetector::ioCounts()
{
    return { g_opens.load(), g_reads.load() };
}
