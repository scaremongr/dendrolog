#ifndef LOGFIELD_H
#define LOGFIELD_H

#include <QStringView>
#include <array>
#include <cstdint>

// ============================================================
// LogField — structural fields that can be present in a single
// parsed log line (maps to common Log4j/Log4cxx specifiers).
// ============================================================

enum class LogField : int {
    Timestamp  = 0, ///< %d  — date/time string as it appears in the line
    ThreadId   = 1, ///< %t  — thread name or numeric id
    LoggerName = 2, ///< %c  — logger / category name
    Level      = 3, ///< %p  — log level token (INFO, WARN, …)
    Message    = 4, ///< %m  — the actual log message body
    Ndc        = 5, ///< %x  — Nested Diagnostic Context
    SourceFile = 6, ///< %F  — source file name
    SourceLine = 7, ///< %L  — source line number
    Count      = 8
};

inline constexpr int     LogFieldCount   = static_cast<int>(LogField::Count);
inline constexpr uint8_t LogFieldAllMask = (1u << LogFieldCount) - 1;

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

struct LogEntryFields {
    std::array<FieldSpan, LogFieldCount> spans{};

    /// Returns the view for field \a f into \a msg.
    QStringView get(LogField f, const QString& msg) const noexcept {
        return spans[static_cast<int>(f)].view(msg);
    }

    bool has(LogField f) const noexcept {
        return spans[static_cast<int>(f)].isValid();
    }

    /// True when no fields were extracted (e.g. continuation lines).
    bool isEmpty() const noexcept {
        for (const auto& s : spans)
            if (s.isValid()) return false;
        return true;
    }
};

#endif // LOGFIELD_H
