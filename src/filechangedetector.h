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
// a fingerprint of the bytes ending at the read offset. On the next check that
// boundary must be byte-identical for the change to qualify as a real append;
// otherwise the file has been rewritten and must be reloaded from scratch.
//
// All offsets are raw byte offsets (matching QFileInfo::size() and QFile::seek),
// so the fingerprint is computed over raw bytes too.
class FileChangeDetector
{
public:
    enum class Change {
        Unchanged, // size and boundary fingerprint match — nothing to read
        Appended,  // prefix intact and file grew — incremental (append-only) read
        Replaced   // truncated, rewritten, or missing — full reload required
    };

    // Immutable snapshot of the end of the consumed region of a file.
    struct Anchor {
        qint64  consumedBytes = 0; // bytes already parsed (== next read offset)
        quint64 fingerprint   = 0; // hash of the boundary region ending here
    };

    // Capture an anchor at `consumedBytes` of the file. Cheap: a single seek plus
    // a read of at most kBoundaryBytes.
    static Anchor capture(const QString& filePath, qint64 consumedBytes);

    // Classify the current on-disk file against a previously captured anchor.
    static Change classify(const QString& filePath, const Anchor& anchor);

private:
    // Size of the boundary region hashed to fingerprint the prefix. Small enough
    // to read instantly, large enough to make accidental matches impossible.
    static constexpr qint64 kBoundaryBytes = 4096;

    // Hash the up-to-kBoundaryBytes region ending at `endOffset`. Returns false
    // if the file can't be opened or is shorter than `endOffset`.
    static bool fingerprintAt(const QString& filePath, qint64 endOffset, quint64& outHash);
};

#endif // FILECHANGEDETECTOR_H
