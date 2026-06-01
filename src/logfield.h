#ifndef LOGFIELD_H
#define LOGFIELD_H

#include <QString>
#include <QStringView>
#include <QVector>

// ============================================================
// FieldSpan — an (offset, length) pair into the owning QString.
//
// Stored as plain integers so that LogEntry can be moved in memory
// without invalidating any pointers.  Call view(msg) to get a
// zero-copy QStringView when you need to read the value.
// ============================================================

struct FieldSpan {
    int start  = -1; ///< Offset in QChars; -1 = field not present
    int length =  0; ///< Length in QChars

    bool isValid() const noexcept { return start >= 0; }

    /// Returns a zero-copy view into \a msg.
    /// The view remains valid as long as \a msg is alive and unmodified.
    QStringView view(const QString& msg) const noexcept {
        return isValid() ? QStringView(msg).mid(start, length) : QStringView{};
    }
};

// ============================================================
// LogEntryFields — structured fields for one log entry line.
//
// Offsets reference LogEntry::message (same object lifetime).
// Continuation lines in a multiline entry will have isEmpty()==true.
// ============================================================

struct ParsedFieldDefinition {
    QString name;

    bool isValid() const noexcept { return !name.trimmed().isEmpty(); }
};

struct LogEntryFields {
    QVector<FieldSpan> spans;

    void resize(int count) {
        spans.resize(count > 0 ? count : 0);
    }

    int size() const noexcept {
        return spans.size();
    }

    /// Returns the view for field index \a i into \a msg.
    QStringView get(int i, const QString& msg) const noexcept {
        return (i >= 0 && i < spans.size()) ? spans[i].view(msg) : QStringView{};
    }

    bool has(int i) const noexcept {
        return i >= 0 && i < spans.size() && spans[i].isValid();
    }

    /// True when no fields were extracted (e.g. continuation lines).
    bool isEmpty() const noexcept {
        for (const auto& s : spans)
            if (s.isValid()) return false;
        return true;
    }
};

#endif // LOGFIELD_H
