#ifndef FILECHANGEDETECTOR_H
#define FILECHANGEDETECTOR_H

#include <QString>
#include <QtGlobal>

// Distinguishes a pure append from a full rewrite/truncation when a watched log
// file changes on disk.
//
// File size alone is ambiguous: a file replaced by a *different, larger* file
// also grows, and naïvely seeking to the old offset would graft unrelated bytes
// onto the end of the view. We therefore anchor the already-consumed prefix with
// two fingerprints: the bytes ending at the read offset (tail) and the start of
// the file (head, Q-19). On the next check both must be byte-identical for the
// change to qualify as a real append; otherwise the file has been rewritten and
// must be reloaded from scratch.
//
// The regions and the hash are those of SourceIdentity (recordid.h): head =
// first min(4 KB, consumed) bytes, tail = up to 4 KB ending at `consumed`,
// RecordFingerprint (SHA-1, 64 bits). For the same consumed size an anchor and
// a SourceIdentity carry equal fingerprints, so "the tab sees the file as
// unchanged" and "bookmarks / file memory see it as Valid" cannot disagree.
//
// All offsets are raw byte offsets (matching QFileInfo::size() and QFile::seek),
// so the fingerprints are computed over raw bytes too (invariant 5).
//
// Cost per check (one stat, as before, plus):
//   • the file's size and mtime are what the anchor last saw, and that mtime
//     was already a few seconds old then — one open, one 4 KB read (tail),
//     exactly as before Q-19: the head is not re-read for a file nothing has
//     touched;
//   • size or mtime differ (append, rewrite, touch), or the stamp is "racy" —
//     the same open, a second 4 KB read (head). Files up to 4 KB consumed: head
//     and tail are one region, read once.
//
// Racy stamps (the rule git uses for its index): an mtime within
// kRacyWindowMs of the moment it was taken may still be shared by a rewrite
// that lands in the same clock tick — coarse kernel clocks, 2 s on FAT, lazy
// metadata on NTFS. Such a stamp proves nothing, so the head is read on every
// check until the stamp is old enough. check() moves the stamp forward once it
// has verified the head of an unchanged file, so a touched file (or a racy
// stamp that has aged) goes back to the one-read check on the next tick.
//
// What is NOT detected (documented limits, not bugs):
//   • an edit only in the middle of the consumed prefix with the same length,
//     head and tail — telling it apart would mean reading the whole file;
//   • a rewrite of only the head that keeps the size AND restores an mtime
//     that was already older than kRacyWindowMs when the anchor saw it:
//     mtime-preserving copies (`cp -p`, archive extraction). The head is
//     checked on the next change of size or mtime.
//   • "same size, different mtime" is deliberately NOT treated as a rewrite by
//     itself: `touch`, a writer that reopens without writing, or NTFS updating
//     mtime late would trigger a full reload of a gigabyte file. A different
//     mtime only makes the check read the head.
class FileChangeDetector
{
public:
    enum class Change {
        Unchanged, // size and both fingerprints match — nothing to read
        Appended,  // prefix intact and file grew — incremental (append-only) read
        Replaced   // truncated, rewritten, or missing — full reload required
    };

    // Immutable snapshot of the consumed region of a file.
    struct Anchor {
        qint64  consumedBytes = 0;  // bytes already parsed (== next read offset)
        quint64 head = 0;           // first min(4 KB, consumedBytes) bytes
        quint64 tail = 0;           // up to 4 KB ending at consumedBytes
        qint64  stampSize = -1;     // file size at capture (may exceed consumedBytes)
        qint64  stampMtimeMs = -1;  // file mtime at capture
        bool    stampRacy = true;   // mtime was too recent to vouch for the head (see above)
        bool    valid = false;      // capture succeeded; an invalid anchor classifies as Replaced
    };

    // mtimes this close to "now" (or in the future) may be shared by a later write.
    static constexpr qint64 kRacyWindowMs = 3000;

    // Capture an anchor at `consumedBytes` of the file: one stat, one open,
    // one read (consumed <= 4 KB) or two reads of at most 4 KB.
    static Anchor capture(const QString& filePath, qint64 consumedBytes);

    // Classify the current on-disk file against a previously captured anchor.
    static Change classify(const QString& filePath, const Anchor& anchor);
    // The same, for an anchor that is kept between checks (tail updates of a
    // tab): when the file is Unchanged and its head was re-read and matched,
    // the anchor's stamp moves to the current size and mtime, so the next
    // check of an untouched file reads only the tail again.
    static Change check(const QString& filePath, Anchor& anchor);

    // Disk accesses made by capture/classify since the start of the process,
    // over all threads — lets tests pin the cost of a check.
    struct IoCounts {
        quint64 opens = 0;
        quint64 reads = 0;
    };
    static IoCounts ioCounts();
};

#endif // FILECHANGEDETECTOR_H
