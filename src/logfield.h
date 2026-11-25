#ifndef LOGFIELD_H
#define LOGFIELD_H

#include <QString>
#include <QStringView>
#include <QVector>

namespace KeyedLine {
QString unescape(QStringView escaped, bool keepControls);       // keyedline.h
qsizetype unescapedSize(QStringView escaped, bool keepControls);
}

// ============================================================
// FieldSpan — an (offset, length) pair into the owning QString.
//
// Stored as plain integers so that LogEntry can be moved in memory
// without invalidating any pointers.  Call view(msg) to get a
// zero-copy QStringView when you need to read the value.
// ============================================================

struct FieldSpan {
    int start  = -1; ///< Offset in QChars; -1 = field not present
    /// Length in QChars; the top bit marks an ESCAPED value (F-4: a JSON or
    /// logfmt string with backslash sequences, kept as in the file). Read
    /// through size()/isEscaped(), write through set().
    int length =  0;

    static constexpr quint32 kEscapedBit = 0x80000000u;

    bool isValid() const noexcept { return start >= 0; }
    int size() const noexcept { return int(quint32(length) & ~kEscapedBit); }
    bool isEscaped() const noexcept { return (quint32(length) & kEscapedBit) != 0; }
    void set(int offset, int size, bool escaped = false) noexcept
    {
        start = offset;
        length = int(quint32(size) | (escaped ? kEscapedBit : 0u));
    }

    /// Returns a zero-copy view into \a msg — the RAW text (escape
    /// sequences as in the file); value() decodes.
    /// The view remains valid as long as \a msg is alive and unmodified.
    QStringView view(const QString& msg) const noexcept {
        return isValid() ? QStringView(msg).mid(start, size()) : QStringView{};
    }
    /// То же для текста, доступного только как view (индексный бэкенд).
    QStringView view(QStringView msg) const noexcept {
        return isValid() ? msg.mid(start, size()) : QStringView{};
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
    /// То же для текста, доступного только как view (индексный бэкенд).
    QStringView get(int i, QStringView msg) const noexcept {
        return (i >= 0 && i < spans.size()) ? spans[i].view(msg) : QStringView{};
    }

    /// Value of field \a i as the user sees it (F-4): an escaped value is
    /// decoded (KeyedLine::unescape), any other is copied as is. Readers that
    /// show or match a field value go through here, not get(): get() returns
    /// the raw text with escape sequences. displayLine — for a one-line view
    /// (formatDisplayText): control characters stay escape sequences.
    QString value(int i, QStringView msg, bool displayLine = false) const;
    /// value(i, msg, displayLine).size() without building the string.
    qsizetype valueSize(int i, QStringView msg, bool displayLine = false) const
    {
        if (i < 0 || i >= spans.size() || !spans[i].isValid())
            return 0;
        return spans[i].isEscaped() ? KeyedLine::unescapedSize(spans[i].view(msg), displayLine)
                                    : spans[i].size();
    }
    /// True when field \a i holds an escaped value.
    bool isEscaped(int i) const noexcept {
        return i >= 0 && i < spans.size() && spans[i].isEscaped();
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

inline QString LogEntryFields::value(int i, QStringView msg, bool displayLine) const
{
    if (i < 0 || i >= spans.size() || !spans[i].isValid())
        return QString();
    const FieldSpan& span = spans[i];
    return span.isEscaped() ? KeyedLine::unescape(span.view(msg), displayLine)
                            : span.view(msg).toString();
}

#endif // LOGFIELD_H
